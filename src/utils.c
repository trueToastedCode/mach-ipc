/* ============================================================================
 * Utility Functions
 * ============================================================================ */

#include "mach_ipc.h"
#include "internal.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

const char* ipc_status_string(ipc_status_t status) {
    switch (status) {
        case IPC_SUCCESS:
            return "Success";
        case IPC_ERROR_INVALID_PARAM:
            return "Invalid parameter";
        case IPC_ERROR_NO_MEMORY:
            return "Out of memory";
        case IPC_ERROR_NOT_CONNECTED:
            return "Not connected";
        case IPC_ERROR_TIMEOUT:
            return "Timeout";
        case IPC_ERROR_SEND_FAILED:
            return "Send failed";
        case IPC_ERROR_INTERNAL:
            return "Internal error";
        case IPC_ERROR_CLIENT_FULL:
            return "Client list full";
        default:
            return "Unknown error";
    }
}

void* ipc_alloc(size_t size) {
    return size ? malloc(size) : NULL;
}

void ipc_free(void *ptr) {
    if (ptr) {
        free(ptr);
    }
}

void ply_free(void *ptr, size_t size) {
    if (ptr && size) {
        vm_deallocate(mach_task_self(), (vm_address_t)ptr, size);
    }
}

ipc_status_t mach_server_broadcast(
    mach_server_t *server,
    uint32_t msg_type,
    const void *data,
    size_t size
) {
    if (!server) return IPC_ERROR_INVALID_PARAM;
    
    // Get snapshot of clients
    client_handle_t clients[MAX_CLIENTS];
    int count = 0;
    
    pthread_mutex_lock(&server->clients_lock);
    for (int i = 0; i < MAX_CLIENTS && count < MAX_CLIENTS; i++) {
        if (server->clients[i] && server->clients[i]->active) {
            clients[count].id = server->clients[i]->id;
            clients[count].internal = server->clients[i];
            count++;
        }
    }
    pthread_mutex_unlock(&server->clients_lock);
    
    // Send to all clients
    ipc_status_t result = IPC_SUCCESS;
    for (int i = 0; i < count; i++) {
        ipc_status_t status = mach_server_send(server, clients[i], msg_type, data, size);
        if (status != IPC_SUCCESS) {
            result = status; // Return last error
        }
    }
    
    return result;
}

void mach_server_disconnect_client(mach_server_t *server, client_handle_t client) {
    if (!server || !IS_VALID_CLIENT(client)) return;
    
    client_info_t *client_info = (client_info_t*)client.internal;
    
    // Notify user before destroying
    if (server->callbacks.on_client_disconnected) {
        dispatch_sync(client_info->queue, ^{
            server->callbacks.on_client_disconnected(server, client, server->user_data);
        });
    }
    
    remove_client(server, client_info);
    destroy_client(client_info);
}