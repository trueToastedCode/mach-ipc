/* ============================================================================
 * Client Implementation
 * ============================================================================ */

#include "internal.h"
#include "log.h"
#include <servers/bootstrap.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

/* ============================================================================
 * MESSAGE HANDLERS
 * ============================================================================ */

static void handle_user_message(
    mach_client_t *client,
    mach_msg_header_t *header,
    internal_payload_t *payload,
    size_t payload_size,
    const void *user_payload,
    size_t user_payload_size
) {
    uint32_t user_msg_type = header->msgh_id & INTERNAL_MSG_TYPE_MASK;
    bool needs_reply = HAS_FEATURE_WACK(header->msgh_id);
    
    LOG_DEBUG_MSG("Received user message: type=%u, needs_reply=%d, has_user_payload=%d",
                 user_msg_type, needs_reply, user_payload != NULL);
    
    // Dispatch to message queue for asynchronous processing
    // Header will be overwritten, can only use copies in async dispatch
    // But payload cleanup has been signaled as being handled here
    // So it stays available without expensive copies
    uint32_t msgh_id = header->msgh_id;
    mach_port_t server_port = client->server_port;
    uint64_t correlation_id = payload->correlation_id;
    int correlation_slot = payload->correlation_slot;
    uint32_t client_id = client->client_id;
    int client_slot = client->client_slot;
    struct timespec user_payload_deadline = payload->user_payload_deadline;
    
    dispatch_async(client->message_queue, ^{
        bool user_payload_is_save =
            has_no_deadline(user_payload_deadline) ||
            !is_deadline_expired(user_payload_deadline, USER_PLY_SAFETY_MS);
        if (needs_reply) {
            // Message with reply
            if (user_payload_is_save) {
                if (client->callbacks.on_message_with_reply) {
                    size_t reply_size = 0;
                    int reply_status = IPC_SUCCESS;
                    void *reply_data = client->callbacks.on_message_with_reply(
                        client,
                        user_msg_type,
                        user_payload,
                        user_payload_size,
                        &reply_size,
                        client->user_data,
                        &reply_status
                    );
                    
                    // Send acknowledgment
                    internal_payload_t ack = (internal_payload_t){
                        .client_id = client_id,
                        .client_slot = client_slot,
                        .status = reply_status
                    };
                    protocol_send_ack(
                        server_port,
                        msgh_id,
                        correlation_id,
                        correlation_slot,
                        &ack,
                        sizeof(ack),
                        reply_data,
                        reply_size
                    );
                    
                    ipc_free(reply_data);
                }
            } else {
                // User payload considered dangerous
                LOG_ERROR_MSG("Message with id=%u and reply rejected because the user payload has reached it's deadline", msgh_id);
                internal_payload_t ack = (internal_payload_t){
                    .client_id = client_id,
                    .client_slot = client_slot,
                    .status = IPC_ERROR_TIMEOUT
                };
                protocol_send_ack(
                    server_port,
                    msgh_id,
                    correlation_id,
                    correlation_slot,
                    &ack,
                    sizeof(ack),
                    NULL,
                    0
                );
            }
        } else {
            // Fire-and-forget message
            if (user_payload_is_save) {
                // User payload considered save
                if (client->callbacks.on_message) {
                    client->callbacks.on_message(
                        client,
                        user_msg_type,
                        user_payload,
                        user_payload_size,
                        client->user_data
                    );
                }
            } else {
                // User payload considered dangerous
                LOG_ERROR_MSG("Message with id=%u ignored because the user payload has reached it's deadline", msgh_id);
            }
        }
        
        // Cleanup payload after processing
        vm_deallocate(mach_task_self(), (vm_address_t)payload, payload_size);
        if (user_payload && user_payload_size) {
            vm_deallocate(mach_task_self(), (vm_address_t)user_payload, user_payload_size);
        }
    });
}

static void handle_death_notification(mach_client_t *client, mach_msg_header_t *header) {
    (void)header;
    
    LOG_INFO_MSG("Server died");
    
    client->connected = 0;
    client->running = 0;
    
    // Notify user
    if (client->callbacks.on_disconnected) {
        client->callbacks.on_disconnected(client, client->user_data);
    }
}

static bool client_message_handler(
    mach_port_t service_port,
    mach_msg_header_t *header,
    internal_payload_t *payload,
    size_t payload_size,
    const void *user_payload,
    size_t user_payload_size,
    void *context
) {
    (void)service_port;
    mach_client_t *client = (mach_client_t*)context;
    
    // Handle native Mach messages
    if (!payload) {
        if (header->msgh_id == MACH_NOTIFY_DEAD_NAME) {
            handle_death_notification(client, header);
        }
        return true;
    }
    
    // Handle our protocol messages
    if (IS_INTERNAL_MSG(header->msgh_id)) {
        
    } else if (IS_EXTERNAL_MSG(header->msgh_id)) {
        handle_user_message(client, header, payload, payload_size, user_payload, user_payload_size);
        return false;  // Payload cleanup handled by async dispatch
    }

    return true;
}

/* ============================================================================
 * RECEIVER THREAD
 * ============================================================================ */

static void* client_receiver_thread(void *arg) {
    mach_client_t *client = (mach_client_t*)arg;
    
    LOG_INFO_MSG("Client receiver thread started");
    
    protocol_receive_loop(
        client->local_port,
        &client->running,
        &client->ack_pool,
        &client->ack_lock,
        client_message_handler,
        client
    );
    
    LOG_INFO_MSG("Client receiver thread stopped");
    return NULL;
}

/* ============================================================================
 * PUBLIC API
 * ============================================================================ */

mach_client_t* mach_client_create(
    const client_callbacks_t *callbacks,
    void *user_data
) {
    mach_client_t *client = calloc(1, sizeof(mach_client_t));
    if (!client) return NULL;
    
    // Initialize resource tracker
    client->resources = resource_tracker_create();
    if (!client->resources) {
        free(client);
        return NULL;
    }
    
    // Initialize pools
    pool_init(&client->ack_pool, MAX_ACKS, sizeof(ack_waiter_t));
    resource_tracker_add(client->resources, RES_TYPE_POOL, &client->ack_pool,
                        (void(*)(void*))pool_free, "ack_pool");
    
    // Create message processing queue
    client->message_queue = dispatch_queue_create("com.ipc.client.messages", 
                                                  DISPATCH_QUEUE_SERIAL);
    if (!client->message_queue) {
        resource_tracker_cleanup_all(client->resources);
        resource_tracker_destroy(client->resources);
        free(client);
        return NULL;
    }
    resource_tracker_add(client->resources, RES_TYPE_QUEUE, &client->message_queue,
                        (void(*)(void*))dispatch_release, "message_queue");
    
    // Initialize lock
    pthread_mutex_init(&client->ack_lock, NULL);
    resource_tracker_add(client->resources, RES_TYPE_MUTEX, &client->ack_lock,
                        (void(*)(void*))pthread_mutex_destroy, "ack_lock");
    
    client->next_correlation_id = 1;
    
    if (callbacks) {
        client->callbacks = *callbacks;
    }
    client->user_data = user_data;
    
    LOG_INFO_MSG("Client created");
    return client;
}

ipc_status_t mach_client_connect(
    mach_client_t *client,
    const char *service_name,
    uint32_t timeout_ms
) {
    if (!client || !service_name) {
        return IPC_ERROR_INVALID_PARAM;
    }
    
    if (client->connected) {
        return IPC_ERROR_INTERNAL;
    }
    
    strncpy(client->service_name, service_name, sizeof(client->service_name) - 1);
    
    // Lookup server
    kern_return_t kr = bootstrap_look_up(
        bootstrap_port,
        service_name,
        &client->server_port
    );
    
    if (kr != KERN_SUCCESS) {
        LOG_ERROR_MSG("Failed to lookup service '%s': %s",
                     service_name, mach_error_string(kr));
        return IPC_ERROR_NOT_CONNECTED;
    }
    
    resource_tracker_add(client->resources, RES_TYPE_PORT, &client->server_port,
                        NULL, "server_port");
    
    // Create local receive port
    kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
                           &client->local_port);
    if (kr != KERN_SUCCESS) {
        LOG_ERROR_MSG("Failed to allocate local port: %s", mach_error_string(kr));
        return IPC_ERROR_INTERNAL;
    }
    
    resource_tracker_add(client->resources, RES_TYPE_PORT, &client->local_port,
                        NULL, "local_port");
    
    // Add send right
    kr = mach_port_insert_right(mach_task_self(), client->local_port,
                               client->local_port, MACH_MSG_TYPE_MAKE_SEND);
    if (kr != KERN_SUCCESS) {
        LOG_ERROR_MSG("Failed to insert send right: %s", mach_error_string(kr));
        return IPC_ERROR_INTERNAL;
    }
    
    // Setup death notification for server
    kr = mach_port_request_notification(
        mach_task_self(),
        client->server_port,
        MACH_NOTIFY_DEAD_NAME,
        1,
        client->local_port,
        MACH_MSG_TYPE_MAKE_SEND_ONCE,
        &(mach_port_t){MACH_PORT_NULL}
    );
    
    if (kr != KERN_SUCCESS) {
        LOG_ERROR_MSG("Failed to setup death notification: %s",
                     mach_error_string(kr));
        return IPC_ERROR_INTERNAL;
    }
    
    // Start receiver thread
    client->running = 1;
    int err = pthread_create(&client->receiver_thread, NULL,
                            client_receiver_thread, client);
    if (err != 0) {
        LOG_ERROR_MSG("Failed to create receiver thread");
        client->running = 0;
        return IPC_ERROR_INTERNAL;
    }
    
    resource_tracker_add(client->resources, RES_TYPE_THREAD, &client->receiver_thread,
                        NULL, "receiver_thread");
    
    // Send connect message
    internal_payload_t payload = (internal_payload_t){
        .client_id = 0,    // Will be assigned by server
        .client_slot = -1, // Will be assigned by server
        .status = IPC_SUCCESS
    };
    
    internal_payload_t *ack_payload = NULL;
    size_t ack_size = 0;
    const void *ack_user_payload = NULL;
    size_t ack_user_size = 0;
    
    kr = protocol_send_with_ack(
        client->server_port,
        client->local_port,
        &client->ack_pool,
        &client->ack_lock,
        &client->next_correlation_id,
        MSG_ID_CONNECT,
        &payload,
        sizeof(payload),
        NULL,
        0,
        &ack_payload,
        &ack_size,
        &ack_user_payload,
        &ack_user_size,
        timeout_ms
    );
    
    if (kr != KERN_SUCCESS) {
        LOG_ERROR_MSG("Connect failed: %s", mach_error_string(kr));
        return kr == KERN_OPERATION_TIMED_OUT ? IPC_ERROR_TIMEOUT : IPC_ERROR_SEND_FAILED;
    }
    
    if (!ack_payload) {
        LOG_ERROR_MSG("Connect ack missing");
        ply_free((void*)ack_user_payload, ack_user_size);
        return IPC_ERROR_INTERNAL;
    }
    
    if (ack_payload->status != IPC_SUCCESS) {
        LOG_ERROR_MSG("Connect rejected by server (status=%d)", ack_payload->status);
        ply_free((void*)ack_payload, ack_size);
        ply_free((void*)ack_user_payload, ack_user_size);
        return ack_payload->status;
    }
    
    client->client_id = ack_payload->client_id;
    client->client_slot = ack_payload->client_slot;
    client->connected = 1;
    
    ply_free((void*)ack_payload, ack_size);
    ply_free((void*)ack_user_payload, ack_user_size);
    
    LOG_INFO_MSG("Connected to server (id=%u, slot=%d)", client->client_id, client->client_slot);
    
    // Notify user
    if (client->callbacks.on_connected) {
        client->callbacks.on_connected(client, client->user_data);
    }
    
    return IPC_SUCCESS;
}

bool mach_client_is_connected(mach_client_t *client) {
    return client && client->connected;
}

ipc_status_t mach_client_send(
    mach_client_t *client,
    uint32_t msg_type,
    const void *data,
    size_t size
) {
    if (!client || !client->connected) {
        return IPC_ERROR_NOT_CONNECTED;
    }
    
    internal_payload_t payload = (internal_payload_t){
        .client_id = client->client_id,
        .client_slot = client->client_slot,
        .status = IPC_SUCCESS
    };
    
    kern_return_t kr = protocol_send_message(
        client->server_port,
        MACH_PORT_NULL,
        MSG_ID_USER(msg_type),
        &payload,
        sizeof(payload),
        data,
        size,
        0
    );
    
    return kr == KERN_SUCCESS ? IPC_SUCCESS : IPC_ERROR_SEND_FAILED;
}

ipc_status_t mach_client_send_with_reply(
    mach_client_t *client,
    uint32_t msg_type,
    const void *data,
    size_t size,
    const void **reply_data,
    size_t *reply_size,
    uint32_t timeout_ms
) {
    if (!client || !client->connected || !reply_data || !reply_size) {
        return IPC_ERROR_INVALID_PARAM;
    }
    
    internal_payload_t payload = (internal_payload_t){
        .client_id = client->client_id,
        .client_slot = client->client_slot,
        .status = IPC_SUCCESS
    };
    
    internal_payload_t *ack_payload = NULL;
    size_t ack_size = 0;
    const void *ack_user_payload = NULL;
    size_t ack_user_size = 0;
    
    kern_return_t kr = protocol_send_with_ack(
        client->server_port,
        MACH_PORT_NULL,
        &client->ack_pool,
        &client->ack_lock,
        &client->next_correlation_id,
        MSG_ID_USER(msg_type),
        &payload,
        sizeof(payload),
        data,
        size,
        &ack_payload,
        &ack_size,
        &ack_user_payload,
        &ack_user_size,
        timeout_ms
    );

    int ack_status;
    bool is_ack_status;

    if (ack_payload && ack_size) {
        ack_status = ack_payload->status;
        is_ack_status = true;
        vm_deallocate(mach_task_self(), (vm_address_t)ack_payload, ack_size);
    } else {
        is_ack_status = false;
    }

    if (ack_user_payload && ack_user_size) {
        if (kr == KERN_SUCCESS) {
            *reply_size = ack_user_size;
            *reply_data = ack_user_payload;
            return is_ack_status ? ack_status : IPC_SUCCESS;
        } else {
            *reply_data = NULL;
            *reply_size = 0;
        }
        vm_deallocate(mach_task_self(), (vm_address_t)ack_user_payload, ack_user_size);
    } else {
        *reply_data = NULL;
        *reply_size = 0;
        if (kr == KERN_SUCCESS) {
            return is_ack_status ? ack_status : IPC_SUCCESS;
        }
    }
    
    return kr == KERN_OPERATION_TIMED_OUT ? IPC_ERROR_TIMEOUT : IPC_ERROR_SEND_FAILED;
}

void mach_client_disconnect(mach_client_t *client) {
    if (!client || !client->connected) return;
    
    LOG_INFO_MSG("Disconnecting from server...");
    
    client->connected = 0;
    client->running = 0;
    
    // Notify user
    if (client->callbacks.on_disconnected) {
        client->callbacks.on_disconnected(client, client->user_data);
    }
}

void mach_client_destroy(mach_client_t *client) {
    if (!client) return;
    
    if (client->connected) {
        mach_client_disconnect(client);
    }
    
    client->running = 0;
    
    // Wait for receiver thread
    if (client->receiver_thread) {
        pthread_join(client->receiver_thread, NULL);
    }
    
    // Drain message queue before cleanup
    if (client->message_queue) {
        dispatch_sync(client->message_queue, ^{});
    }
    
    resource_tracker_destroy(client->resources);
    
    free(client);
}