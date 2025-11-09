#include "echo.h"
#include "linear_ts_pool.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <inttypes.h>

static mach_server_t *g_server = NULL;

typedef struct {
    shared_memory_t *shmem;
    uint32_t client_id;
} shmem_pool_entry_t;

LinearTSPool shmem_pool;

void signal_handler(int sig) {
    (void)sig;
    printf("\nShutting down...\n");
    if (g_server) {
        mach_server_stop(g_server);
    }
}

void on_client_connected(mach_server_t *server, client_handle_t client, void *data) {
    (void)server;
    (void)data;
    printf("Client %u connected\n", client.id);
}

void on_client_disconnected(mach_server_t *server, client_handle_t client, void *data) {
    (void)server;
    (void)data;

    printf("Client %u disconnected\n", client.id);

    // Lock the entry before destroying
    if (linear_ts_pool_lock_entry(&shmem_pool, client.slot)) {
        shmem_pool_entry_t *entry = (shmem_pool_entry_t*)linear_ts_pool_get(&shmem_pool, client.slot);
        if (entry && entry->shmem) {
            shared_memory_destroy(entry->shmem);
            entry->shmem = NULL;
        }
        linear_ts_pool_unlock_entry(&shmem_pool, client.slot);
    }
    
    linear_ts_pool_remove(&shmem_pool, client.slot);
}

void on_message(mach_server_t *server, client_handle_t client, 
                mach_port_t *remote_port, uint32_t msg_type, const void *data, size_t size, void *user_data) {
    (void)server;
    (void)client;
    (void)remote_port;
    (void)user_data;
    if (msg_type == MSG_TYPE_SILENT) {
        printf("Client: %.*s\n", (int)size, (char*)data);
    }
}

void* on_message_with_reply(mach_server_t *server, client_handle_t client,
                            mach_port_t *remote_port, uint32_t msg_type, const void *data, size_t size,
                            size_t *reply_size, void *user_data, int *reply_status) {
    (void)server;
    (void)reply_size;
    (void)user_data;
    void *reply = NULL;

    if (msg_type == MSG_TYPE_SET_ECHO_SHM) {
        if (linear_ts_pool_is_active(&shmem_pool, client.slot)) {
            *reply_status = IPC_ERROR_INTERNAL;
            return NULL;
        }
        
        shmem_pool_entry_t entry = {0};
        entry.client_id = client.id;
        
        kern_return_t kr = shared_memory_map(
            *remote_port,
            *((size_t*)data),
            &entry.shmem
        );
        
        if (kr != KERN_SUCCESS) {
            *reply_status = IPC_ERROR_INTERNAL;
            return NULL;
        }
        
        if (!linear_ts_pool_set(&shmem_pool, client.slot, &entry)) {
            shared_memory_destroy(entry.shmem);
            *reply_status = IPC_ERROR_INTERNAL;
            return NULL;
        }
        
        *remote_port = MACH_PORT_NULL;
        printf("Shared memory with %" PRIu64 " bytes has been mapped!\n", entry.shmem->size);
        return NULL;
    }

    if (msg_type == MSG_TYPE_ECHO) {
        // Lock the entry - this blocks only this specific entry
        if (!linear_ts_pool_lock_entry(&shmem_pool, client.slot)) {
            *reply_status = IPC_ERROR_INTERNAL;
            return NULL;
        }
        
        shmem_pool_entry_t *entry = (shmem_pool_entry_t*)linear_ts_pool_get(&shmem_pool, client.slot);
        if (!entry || !entry->shmem) {
            *reply_status = IPC_ERROR_INTERNAL;
            linear_ts_pool_unlock_entry(&shmem_pool, client.slot);
            return NULL;
        }
        
        printf("Client %u: %.*s\n", client.id,
               (int)shared_memory_get_size(entry->shmem), 
               (char*)shared_memory_get_data(entry->shmem));
        
        const char *echo_message = "Hello from server! Data in shared memory.";
        snprintf(shared_memory_get_data(entry->shmem), 
                 shared_memory_get_size(entry->shmem),
                 "%s", echo_message);
        
        *reply_status = ECHO_CUSTOM_STATUS;
        
        // Unlock when done
        linear_ts_pool_unlock_entry(&shmem_pool, client.slot);
    }

    return reply;
}

int main() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    set_user_ipc_status_string(echo_status_string);

    server_callbacks_t callbacks = {
        .on_client_connected = on_client_connected,
        .on_client_disconnected = on_client_disconnected,
        .on_message = on_message,
        .on_message_with_reply = on_message_with_reply
    };
    
    g_server = mach_server_create("com.example.echo", &callbacks, NULL);
    if (!g_server) {
        fprintf(stderr, "Failed to create server\n");
        return 1;
    }

    linear_ts_pool_init(&shmem_pool, mach_server_max_clients(g_server), sizeof(shmem_pool_entry_t));
    
    printf("Echo server started. Press Ctrl+C to stop.\n");
    
    ipc_status_t status = mach_server_run(g_server);
    printf("Server stopped: %s\n", ipc_status_string(status));
    
    linear_ts_pool_free(&shmem_pool);
    mach_server_destroy(g_server);
    return 0;
}