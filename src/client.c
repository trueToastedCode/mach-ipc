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
    size_t payload_size
) {
    uint32_t user_msg_type = header->msgh_id & INTERNAL_MSG_TYPE_MASK;
    bool needs_reply = HAS_FEATURE_WACK(header->msgh_id);
    
    LOG_DEBUG_MSG("Received user message: type=%u, needs_reply=%d",
                 user_msg_type, needs_reply);
    
    if (needs_reply) {
        // Message with reply
        if (client->callbacks.on_message_with_reply) {
            size_t reply_size = 0;
            void *reply_data = client->callbacks.on_message_with_reply(
                client,
                user_msg_type,
                payload->data,
                payload->data_size,
                &reply_size,
                client->user_data
            );
            
            // Send acknowledgment
            internal_payload_t *ack = create_payload(reply_size);
            if (ack) {
                ack->client_id = client->client_id;
                ack->status = reply_data ? IPC_SUCCESS : IPC_ERROR_INTERNAL;
                if (reply_data && reply_size > 0) {
                    copy_to_payload(ack, reply_data, reply_size);
                }
                
                protocol_send_ack(
                    client->server_port,
                    header->msgh_id,
                    payload->correlation_id,
                    ack,
                    sizeof(internal_payload_t) + reply_size
                );
                
                free_payload(ack);
            }
            
            if (reply_data) {
                ipc_free(reply_data);
            }
        }
    } else {
        // Fire-and-forget message
        if (client->callbacks.on_message) {
            client->callbacks.on_message(
                client,
                user_msg_type,
                payload->data,
                payload->data_size,
                client->user_data
            );
        }
    }
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

static void client_message_handler(
    mach_port_t service_port,
    mach_msg_header_t *header,
    internal_payload_t *payload,
    size_t payload_size,
    void *context
) {
    (void)service_port;
    mach_client_t *client = (mach_client_t*)context;
    
    // Handle native Mach messages
    if (!payload) {
        if (header->msgh_id == MACH_NOTIFY_DEAD_NAME) {
            handle_death_notification(client, header);
        }
        return;
    }
    
    // Handle our protocol messages
    if (IS_INTERNAL_MSG(header->msgh_id)) {
        
    } else if (IS_EXTERNAL_MSG(header->msgh_id)) {
        handle_user_message(client, header, payload, payload_size);
    }
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
    internal_payload_t *payload = create_payload(0);
    if (!payload) {
        return IPC_ERROR_NO_MEMORY;
    }
    
    payload->client_id = 0; // Will be assigned by server
    payload->status = IPC_SUCCESS;
    
    internal_payload_t *ack_payload = NULL;
    size_t ack_size = 0;
    
    kr = protocol_send_with_ack(
        client->server_port,
        client->local_port,
        &client->ack_pool,
        &client->ack_lock,
        &client->next_correlation_id,
        MSG_ID_CONNECT,
        payload,
        sizeof(internal_payload_t),
        &ack_payload,
        &ack_size,
        timeout_ms
    );
    
    free_payload(payload);
    
    if (kr != KERN_SUCCESS) {
        LOG_ERROR_MSG("Connect failed: %s", mach_error_string(kr));
        return kr == KERN_OPERATION_TIMED_OUT ? IPC_ERROR_TIMEOUT : IPC_ERROR_SEND_FAILED;
    }
    
    if (!ack_payload) {
        LOG_ERROR_MSG("Connect ack missing");
        return IPC_ERROR_INTERNAL;
    }
    
    if (ack_payload->status != IPC_SUCCESS) {
        LOG_ERROR_MSG("Connect rejected by server (status=%d)", ack_payload->status);
        vm_deallocate(mach_task_self(), (vm_address_t)ack_payload, ack_size);
        return ack_payload->status;
    }
    
    client->client_id = ack_payload->client_id;
    client->connected = 1;
    
    vm_deallocate(mach_task_self(), (vm_address_t)ack_payload, ack_size);
    
    LOG_INFO_MSG("Connected to server (client_id=%u)", client->client_id);
    
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
    
    internal_payload_t *payload = create_payload(size);
    if (!payload) return IPC_ERROR_NO_MEMORY;
    
    payload->client_id = client->client_id;
    payload->status = IPC_SUCCESS;
    if (data && size > 0) {
        copy_to_payload(payload, data, size);
    }
    
    kern_return_t kr = protocol_send_message(
        client->server_port,
        MACH_PORT_NULL,
        MSG_ID_USER(msg_type),
        payload,
        sizeof(internal_payload_t) + size
    );
    
    free_payload(payload);
    
    return kr == KERN_SUCCESS ? IPC_SUCCESS : IPC_ERROR_SEND_FAILED;
}

ipc_status_t mach_client_send_with_reply(
    mach_client_t *client,
    uint32_t msg_type,
    const void *data,
    size_t size,
    void **reply_data,
    size_t *reply_size,
    uint32_t timeout_ms
) {
    if (!client || !client->connected || !reply_data || !reply_size) {
        return IPC_ERROR_INVALID_PARAM;
    }
    
    internal_payload_t *payload = create_payload(size);
    if (!payload) return IPC_ERROR_NO_MEMORY;
    
    payload->client_id = client->client_id;
    payload->status = IPC_SUCCESS;
    if (data && size > 0) {
        copy_to_payload(payload, data, size);
    }
    
    internal_payload_t *ack_payload = NULL;
    size_t ack_size = 0;
    
    kern_return_t kr = protocol_send_with_ack(
        client->server_port,
        MACH_PORT_NULL,
        &client->ack_pool,
        &client->ack_lock,
        &client->next_correlation_id,
        MSG_ID_USER(msg_type),
        payload,
        sizeof(internal_payload_t) + size,
        &ack_payload,
        &ack_size,
        timeout_ms
    );
    
    free_payload(payload);
    
    if (kr == KERN_SUCCESS && ack_payload) {
        *reply_size = ack_payload->data_size;
        if (*reply_size > 0) {
            *reply_data = ipc_alloc(*reply_size);
            memcpy(*reply_data, ack_payload->data, *reply_size);
        } else {
            *reply_data = NULL;
        }
        vm_deallocate(mach_task_self(), (vm_address_t)ack_payload, ack_size);
        return IPC_SUCCESS;
    }
    
    *reply_data = NULL;
    *reply_size = 0;
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
    
    resource_tracker_cleanup_all(client->resources);
    resource_tracker_destroy(client->resources);
    
    free(client);
}