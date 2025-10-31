/* ============================================================================
 * Server Implementation
 * ============================================================================ */

#include "internal.h"
#include "log.h"
#include <servers/bootstrap.h>
#include <string.h>
#include <stdlib.h>

/* ============================================================================
 * CLIENT MANAGEMENT
 * ============================================================================ */

client_info_t* create_client(uint32_t id, mach_port_t port) {
    client_info_t *client = calloc(1, sizeof(client_info_t));
    if (!client) return NULL;
    
    client->id = id;
    client->port = port;
    client->active = true;
    client->death_notif_setup = false;
    
    // Create serial queue for this client
    char queue_name[64];
    snprintf(queue_name, sizeof(queue_name), "com.ipc.client.%u", id);
    client->queue = dispatch_queue_create(queue_name, DISPATCH_QUEUE_SERIAL);
    
    if (!client->queue) {
        free(client);
        return NULL;
    }
    
    snprintf(client->debug_name, sizeof(client->debug_name), "Client-%u", id);
    return client;
}

void destroy_client(client_info_t *client) {
    if (!client) return;
    
    client->active = false;
    
    if (client->queue) {
        dispatch_sync(client->queue, ^{}); // Drain queue
        dispatch_release(client->queue);
        client->queue = NULL;
    }
    
    if (client->port != MACH_PORT_NULL) {
        mach_port_deallocate(mach_task_self(), client->port);
        client->port = MACH_PORT_NULL;
    }
    
    free(client);
}

static bool add_client(mach_server_t *server, client_info_t *client) {
    pthread_mutex_lock(&server->clients_lock);
    
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (server->clients[i] == NULL) {
            server->clients[i] = client;
            server->client_count++;
            pthread_mutex_unlock(&server->clients_lock);
            return true;
        }
    }
    
    pthread_mutex_unlock(&server->clients_lock);
    return false;
}

void remove_client(mach_server_t *server, client_info_t *client) {
    pthread_mutex_lock(&server->clients_lock);
    
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (server->clients[i] == client) {
            server->clients[i] = NULL;
            server->client_count--;
            break;
        }
    }
    
    pthread_mutex_unlock(&server->clients_lock);
}

/* ============================================================================
 * MESSAGE HANDLERS
 * ============================================================================ */

static void handle_connect_request(
    mach_server_t *server,
    mach_msg_header_t *header,
    internal_payload_t *payload,
    size_t payload_size
) {
    (void)payload_size;
    
    mach_port_t client_port = header->msgh_remote_port;
    int status = 0;
    uint32_t client_id = 0;
    client_info_t *client = NULL;
    
    if (client_port == MACH_PORT_NULL) {
        LOG_ERROR_MSG("Connect request with null port");
        status = IPC_ERROR_INVALID_PARAM;
        goto send_reply;
    }
    
    // Check if already connected
    pthread_mutex_lock(&server->clients_lock);
    if (find_client_by_port_locked(server, client_port)) {
        pthread_mutex_unlock(&server->clients_lock);
        LOG_ERROR_MSG("Client already connected");
        status = IPC_ERROR_INTERNAL;
        goto send_reply;
    }
    pthread_mutex_unlock(&server->clients_lock);
    
    // Assign client ID
    pthread_mutex_lock(&server->clients_lock);
    client_id = server->next_client_id++;
    pthread_mutex_unlock(&server->clients_lock);
    
    // Create client
    client = create_client(client_id, client_port);
    if (!client) {
        LOG_ERROR_MSG("Failed to create client");
        status = IPC_ERROR_NO_MEMORY;
        goto send_reply;
    }
    
    // Add to client list
    if (!add_client(server, client)) {
        LOG_ERROR_MSG("Client list is full");
        destroy_client(client);
        client = NULL;
        status = IPC_ERROR_CLIENT_FULL;
        goto send_reply;
    }
    
    // Setup death notification
    kern_return_t kr = mach_port_request_notification(
        mach_task_self(),
        client_port,
        MACH_NOTIFY_DEAD_NAME,
        1,
        server->service_port,
        MACH_MSG_TYPE_MAKE_SEND_ONCE,
        &(mach_port_t){MACH_PORT_NULL}
    );
    
    if (kr != KERN_SUCCESS) {
        LOG_ERROR_MSG("Failed to setup death notification: %s", 
                     mach_error_string(kr));
        remove_client(server, client);
        destroy_client(client);
        client = NULL;
        status = IPC_ERROR_INTERNAL;
        goto send_reply;
    }
    
    client->death_notif_setup = true;
    status = IPC_SUCCESS;
    
send_reply:
    ;
    // Send reply
    internal_payload_t *reply = create_payload(0);
    if (reply) {
        reply->client_id = client_id;
        reply->status = status;
        
        kern_return_t kr = protocol_send_ack(
            client_port,
            header->msgh_id,
            payload->correlation_id,
            reply,
            sizeof(internal_payload_t)
        );
        
        free_payload(reply);
        
        if (kr != KERN_SUCCESS || status != 0) {
            if (client) {
                remove_client(server, client);
                destroy_client(client);
            }
            return;
        }
    }
    
    if (status == IPC_SUCCESS) {
        LOG_INFO_MSG("Client %u connected", client_id);
        
        // Notify user
        if (server->callbacks.on_client_connected) {
            dispatch_async(client->queue, ^{
                server->callbacks.on_client_connected(
                    server,
                    (client_handle_t){.id = client->id, .internal = client},
                    server->user_data
                );
            });
        }
    }
}

static void handle_user_message(
    mach_server_t *server,
    mach_msg_header_t *header,
    internal_payload_t *payload,
    size_t payload_size
) {
    // Find client
    pthread_mutex_lock(&server->clients_lock);
    client_info_t *client = find_client_by_id_locked(server, payload->client_id);
    pthread_mutex_unlock(&server->clients_lock);
    
    if (!client) {
        LOG_ERROR_MSG("Message from unknown client %u", payload->client_id);
        return;
    }
    
    // Extract user message type from msg_id
    uint32_t user_msg_type = header->msgh_id & INTERNAL_MSG_TYPE_MASK;
    bool needs_reply = HAS_FEATURE_WACK(header->msgh_id);
    
    LOG_DEBUG_MSG("User message from client %u: type=%u, needs_reply=%d",
                 client->id, user_msg_type, needs_reply);
    
    // Dispatch to client's queue for sequential processing
    dispatch_async(client->queue, ^{
        if (needs_reply) {
            // Message with reply
            if (server->callbacks.on_message_with_reply) {
                size_t reply_size = 0;
                void *reply_data = server->callbacks.on_message_with_reply(
                    server,
                    (client_handle_t){.id = client->id, .internal = client},
                    user_msg_type,
                    payload->data,
                    payload->data_size,
                    &reply_size,
                    server->user_data
                );
                
                // Send acknowledgment
                internal_payload_t *ack = create_payload(reply_size);
                if (ack) {
                    ack->client_id = client->id;
                    ack->status = reply_data ? IPC_SUCCESS : IPC_ERROR_INTERNAL;
                    if (reply_data && reply_size > 0) {
                        copy_to_payload(ack, reply_data, reply_size);
                    }
                    
                    protocol_send_ack(
                        client->port,
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
            if (server->callbacks.on_message) {
                server->callbacks.on_message(
                    server,
                    (client_handle_t){.id = client->id, .internal = client},
                    user_msg_type,
                    payload->data,
                    payload->data_size,
                    server->user_data
                );
            }
        }
    });
}

static void handle_death_notification(mach_server_t *server, mach_msg_header_t *header) {
    mach_dead_name_notification_t *notif = (mach_dead_name_notification_t*)header;
    
    pthread_mutex_lock(&server->clients_lock);
    client_info_t *client = find_client_by_port_locked(server, notif->not_port);
    pthread_mutex_unlock(&server->clients_lock);
    
    if (client) {
        LOG_INFO_MSG("Client %u died", client->id);
        
        // Notify user
        if (server->callbacks.on_client_disconnected) {
            dispatch_async(client->queue, ^{
                server->callbacks.on_client_disconnected(
                    server,
                    (client_handle_t){.id = client->id, .internal = client},
                    server->user_data
                );
            });
        }
        
        remove_client(server, client);
        destroy_client(client);
    }
}

static void server_message_handler(
    mach_port_t service_port,
    mach_msg_header_t *header,
    internal_payload_t *payload,
    size_t payload_size,
    void *context
) {
    (void)service_port;
    mach_server_t *server = (mach_server_t*)context;
    
    // Handle native Mach messages
    if (!payload) {
        if (header->msgh_id == MACH_NOTIFY_DEAD_NAME) {
            handle_death_notification(server, header);
        }
        return;
    }
    
    // Handle our protocol messages
    if (IS_MSG_TYPE(header->msgh_id, INTERNAL_MSG_TYPE_CONNECT)) {
        handle_connect_request(server, header, payload, payload_size);
    } else {
        handle_user_message(server, header, payload, payload_size);
    }
}

/* ============================================================================
 * PUBLIC API
 * ============================================================================ */

mach_server_t* mach_server_create(
    const char *service_name,
    const server_callbacks_t *callbacks,
    void *user_data
) {
    if (!service_name) return NULL;
    
    mach_server_t *server = calloc(1, sizeof(mach_server_t));
    if (!server) return NULL;
    
    // Initialize resource tracker
    server->resources = resource_tracker_create();
    if (!server->resources) {
        free(server);
        return NULL;
    }
    
    strncpy(server->service_name, service_name, sizeof(server->service_name) - 1);
    
    // Initialize pools
    pool_init(&server->ack_pool, MAX_ACKS, sizeof(ack_waiter_t));
    resource_tracker_add(server->resources, RES_TYPE_POOL, &server->ack_pool, 
                        (void(*)(void*))pool_free, "ack_pool");
    
    // Initialize locks
    pthread_mutex_init(&server->clients_lock, NULL);
    resource_tracker_add(server->resources, RES_TYPE_MUTEX, &server->clients_lock,
                        (void(*)(void*))pthread_mutex_destroy, "clients_lock");
    
    pthread_mutex_init(&server->ack_lock, NULL);
    resource_tracker_add(server->resources, RES_TYPE_MUTEX, &server->ack_lock,
                        (void(*)(void*))pthread_mutex_destroy, "ack_lock");
    
    // Register with bootstrap
    kern_return_t kr = bootstrap_check_in(
        bootstrap_port,
        service_name,
        &server->service_port
    );
    
    if (kr != KERN_SUCCESS) {
        LOG_ERROR_MSG("Bootstrap check-in failed: %s", mach_error_string(kr));
        mach_server_destroy(server);
        return NULL;
    }
    
    resource_tracker_add(server->resources, RES_TYPE_PORT, &server->service_port,
                        NULL, "service_port");
    
    server->next_client_id = 1;
    server->next_correlation_id = 1;
    
    if (callbacks) {
        server->callbacks = *callbacks;
    }
    server->user_data = user_data;
    
    LOG_INFO_MSG("Server created: %s (port %u)", service_name, server->service_port);
    return server;
}

ipc_status_t mach_server_run(mach_server_t *server) {
    if (!server) return IPC_ERROR_INVALID_PARAM;
    
    server->running = 1;
    
    LOG_INFO_MSG("Server running...");
    
    // Run receive loop in current thread
    protocol_receive_loop(
        server->service_port,
        &server->running,
        &server->ack_pool,
        &server->ack_lock,
        server_message_handler,
        server
    );
    
    LOG_INFO_MSG("Server stopped");
    return IPC_SUCCESS;
}

void mach_server_stop(mach_server_t *server) {
    if (!server) return;
    
    LOG_INFO_MSG("Stopping server...");
    server->running = 0;
}

ipc_status_t mach_server_send(
    mach_server_t *server,
    client_handle_t client,
    uint32_t msg_type,
    const void *data,
    size_t size
) {
    if (!server || !IS_VALID_CLIENT(client)) {
        return IPC_ERROR_INVALID_PARAM;
    }
    
    client_info_t *client_info = (client_info_t*)client.internal;
    if (!client_info->active) {
        return IPC_ERROR_NOT_CONNECTED;
    }
    
    internal_payload_t *payload = create_payload(size);
    if (!payload) return IPC_ERROR_NO_MEMORY;
    
    payload->client_id = 0; // Server doesn't have a client ID
    payload->status = IPC_SUCCESS;
    if (data && size > 0) {
        copy_to_payload(payload, data, size);
    }
    
    kern_return_t kr = protocol_send_message(
        client_info->port,
        MACH_PORT_NULL,
        MSG_ID_USER(msg_type),
        payload,
        sizeof(internal_payload_t) + size
    );
    
    free_payload(payload);
    
    return kr == KERN_SUCCESS ? IPC_SUCCESS : IPC_ERROR_SEND_FAILED;
}

ipc_status_t mach_server_send_with_reply(
    mach_server_t *server,
    client_handle_t client,
    uint32_t msg_type,
    const void *data,
    size_t size,
    void **reply_data,
    size_t *reply_size,
    uint32_t timeout_ms
) {
    if (!server || !IS_VALID_CLIENT(client) || !reply_data || !reply_size) {
        return IPC_ERROR_INVALID_PARAM;
    }
    
    client_info_t *client_info = (client_info_t*)client.internal;
    if (!client_info->active) {
        return IPC_ERROR_NOT_CONNECTED;
    }
    
    internal_payload_t *payload = create_payload(size);
    if (!payload) return IPC_ERROR_NO_MEMORY;
    
    payload->client_id = 0;
    payload->status = IPC_SUCCESS;
    if (data && size > 0) {
        copy_to_payload(payload, data, size);
    }
    
    internal_payload_t *ack_payload = NULL;
    size_t ack_size = 0;
    
    kern_return_t kr = protocol_send_with_ack(
        client_info->port,
        MACH_PORT_NULL,
        &server->ack_pool,
        &server->ack_lock,
        &server->next_correlation_id,
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

int mach_server_client_count(mach_server_t *server) {
    if (!server) return 0;
    
    pthread_mutex_lock(&server->clients_lock);
    int count = server->client_count;
    pthread_mutex_unlock(&server->clients_lock);
    
    return count;
}

void mach_server_destroy(mach_server_t *server) {
    if (!server) return;
    
    if (server->running) {
        mach_server_stop(server);
    }
    
    // Disconnect all clients
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (server->clients[i]) {
            destroy_client(server->clients[i]);
            server->clients[i] = NULL;
        }
    }
    
    resource_tracker_cleanup_all(server->resources);
    resource_tracker_destroy(server->resources);
    
    free(server);
}