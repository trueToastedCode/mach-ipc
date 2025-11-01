#include "mach_ipc.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#define MSG_ECHO (1)
#define MSG_SILENT (2)

static mach_server_t *g_server = NULL;

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
}

void* on_message_with_reply(mach_server_t *server, client_handle_t client,
                            uint32_t msg_type, const void *data, size_t size,
                            size_t *reply_size, void *user_data) {
    (void)user_data;
    void *reply = NULL;
    if (msg_type == MSG_ECHO) {
        printf("Client %u: %.*s\n", client.id, (int)size, (char*)data);
        
        // Echo back
        reply = ipc_alloc(size);
        memcpy(reply, data, size);
        *reply_size = size;

        // also trigger send a silent msg
        const char *silent_message = "Hello from server!";
        printf("Sending: %s\n", silent_message);
        int status = mach_server_send(
            server, client, MSG_SILENT,
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
    
    server_callbacks_t callbacks = {
        .on_client_connected = on_client_connected,
        .on_client_disconnected = on_client_disconnected,
        .on_message = NULL,
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