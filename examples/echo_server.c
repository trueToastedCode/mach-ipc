#include "echo.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <inttypes.h>

static mach_server_t *g_server = NULL;

// for this example we will only have one client
pthread_mutex_t shmem_lock;
shared_memory_t *shmem = NULL;

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

    pthread_mutex_lock(&shmem_lock);
    shared_memory_destroy(shmem);
    shmem = NULL;
    pthread_mutex_unlock(&shmem_lock);
}

void on_message(mach_server_t *server, client_handle_t client, 
                mach_port_t *remote_port, uint32_t msg_type, const void *data, size_t size, void *user_data) {
    (void)server;
    (void)client;
    (void)user_data;
    if (msg_type == MSG_TYPE_SILENT) {
        printf("Client: %.*s\n", (int)size, (char*)data);
    }
}

void* on_message_with_reply(mach_server_t *server, client_handle_t client,
                            mach_port_t *remote_port, uint32_t msg_type, const void *data, size_t size,
                            size_t *reply_size, void *user_data, int *reply_status) {
    (void)user_data;
    (void)reply_status;
    void *reply = NULL;

    if (msg_type == MSG_TYPE_SET_ECHO_SHM) {
        pthread_mutex_lock(&shmem_lock);
        if (shmem) {
            *reply_status = IPC_ERROR_INTERNAL;
            pthread_mutex_unlock(&shmem_lock);
            return NULL;
        }
        kern_return_t kr = shared_memory_map(
            *remote_port,
            *((size_t*)data),
            &shmem
        );
        if (kr != KERN_SUCCESS) {
            *reply_status = IPC_ERROR_INTERNAL;
            pthread_mutex_unlock(&shmem_lock);
            return NULL;
        }
        *remote_port = MACH_PORT_NULL; // set it to null because we have taken ownership
        printf("Shared memory with %" PRIu64 " bytes has been mapped!\n", shmem->size);
        pthread_mutex_unlock(&shmem_lock);
        return NULL;
    }

    if (msg_type == MSG_TYPE_ECHO) {
        pthread_mutex_lock(&shmem_lock);
        if (!shmem) {
            *reply_status = IPC_ERROR_INTERNAL;
            pthread_mutex_unlock(&shmem_lock);
            return NULL;
        }
        printf("Client %u: %.*s\n", client.id,
               (int)shared_memory_get_size(shmem), (char*)shared_memory_get_data(shmem));
        const char *echo_message = "Hello from server! Data in shared memory.";
        size_t echo_message_len = strlen(echo_message) + 1;
        snprintf(shared_memory_get_data(shmem), shared_memory_get_size(shmem),
                 "%s", echo_message);
        pthread_mutex_unlock(&shmem_lock);
        *reply_status = ECHO_CUSTOM_STATUS;
        
        // also trigger send a silent msg
        const char *silent_message = "Hello from server!";
        printf("Sending: %s\n", silent_message);
        int status = mach_server_send(
            server, client, MSG_ID_SILENT,
            silent_message, strlen(silent_message) + 1
        );
        if (status != IPC_SUCCESS) {
            printf("Silent msg failed: %s\n", ipc_status_string(status));
        }
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
    
    printf("Echo server started. Press Ctrl+C to stop.\n");
    
    ipc_status_t status = mach_server_run(g_server);
    printf("Server stopped: %s\n", ipc_status_string(status));
    
    mach_server_destroy(g_server);
    return 0;
}