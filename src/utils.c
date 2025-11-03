/* ============================================================================
 * Utility Functions
 * ============================================================================ */

#include "mach_ipc.h"
#include "internal.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

const char *(*user_ipc_status_string)(ipc_status_t) = NULL;

const char* ipc_status_string(ipc_status_t status) {
    if (status >= 1000) {
        const char *st_str = NULL;
        if (user_ipc_status_string) {
            st_str = user_ipc_status_string(status);
        }
        return st_str ? st_str : "Unknown user error";
    }
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

void set_user_ipc_status_string(const char *(*_user_ipc_status_string)(ipc_status_t)) {
    user_ipc_status_string = _user_ipc_status_string;
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

struct timespec calc_deadline(uint64_t timeout_ms) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    struct timespec timeout;

    // convert milliseconds → seconds + nanoseconds
    timeout.tv_sec  = now.tv_sec + (timeout_ms / 1000);
    timeout.tv_nsec = now.tv_nsec + (timeout_ms % 1000) * 1000000ULL;

    // normalize in case tv_nsec >= 1 second
    if (timeout.tv_nsec >= 1000000000L) {
        timeout.tv_sec += 1;
        timeout.tv_nsec -= 1000000000L;
    }

    return timeout;
}

bool is_deadline_expired(struct timespec deadline, uint64_t safety_ms) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    // Safety margin in nanoseconds
    long safety_ns = safety_ms * 1000000L;

    // Deadline + Safety
    struct timespec safe_deadline = deadline;
    safe_deadline.tv_nsec += safety_ns;

    // Normalize tv_nsec >= 1e9
    if (safe_deadline.tv_nsec >= 1000000000L) {
        safe_deadline.tv_sec += safe_deadline.tv_nsec / 1000000000L;
        safe_deadline.tv_nsec = safe_deadline.tv_nsec % 1000000000L;
    }

    // Compare now vs safe_deadline
    if (now.tv_sec > safe_deadline.tv_sec)
        return true;
    if (now.tv_sec == safe_deadline.tv_sec && now.tv_nsec >= safe_deadline.tv_nsec)
        return true;

    return false;
}

bool has_no_deadline(struct timespec deadline) {
    return deadline.tv_sec == 0 && deadline.tv_nsec == 0;
}
