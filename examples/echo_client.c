#include "echo.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

static volatile int running = 1;

void signal_handler(int sig) {
    (void)sig;
    printf("\nDisconnecting...\n");
    running = 0;
}

void on_connected(mach_client_t *client, void *data) {
    (void)client;
    (void)data;
    printf("Connected to server!\n");
}

void on_disconnected(mach_client_t *client, void *data) {
    (void)client;
    (void)data;
    printf("Disconnected from server\n");
    running = 0;
}

void on_message(mach_client_t *client, uint32_t msg_type,
                const void *data, size_t size, void *user_data) {
    (void)client;
    (void)user_data;
    if (msg_type == MSG_TYPE_SILENT) {
        printf("Server: %.*s\n", (int)size, (char*)data);
    }
}

int main() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    set_user_ipc_status_string(echo_status_string);

    client_callbacks_t callbacks = {
        .on_connected = on_connected,
        .on_disconnected = on_disconnected,
        .on_message = on_message,
        .on_message_with_reply = NULL
    };
    
    mach_client_t *client = mach_client_create(&callbacks, NULL);
    if (!client) {
        fprintf(stderr, "Failed to create client\n");
        return 1;
    }
    
    ipc_status_t status = mach_client_connect(client, "com.example.echo", 5000);
    if (status != IPC_SUCCESS) {
        fprintf(stderr, "Failed to connect: %s\n", ipc_status_string(status));
        mach_client_destroy(client);
        return 1;
    }
    
    const char *echo_message = "Hello World!";
    size_t echo_message_len = strlen(echo_message) + 1;
    void *echo_message_buffer = ipc_alloc(echo_message_len);
    if (!echo_message_buffer) {
        fprintf(stderr, "Failed to alloc buffer for echo msg");
        mach_client_destroy(client);
        return 1;
    }
    memcpy(echo_message_buffer, echo_message, echo_message_len);
    const void *echo_reply = NULL;
    size_t echo_reply_size = 0;

    printf("Sending: %s\n", echo_message);

    status = mach_client_send_with_reply(
        client, MSG_ID_ECHO,
        echo_message_buffer, echo_message_len,
        &echo_reply, &echo_reply_size,
        2000
    );
    
    printf("Silent msg status: %s\n", ipc_status_string(status));
    if (status == ECHO_CUSTOM_STATUS && echo_reply) {
        printf("Echo reply: %s\n", (char*)echo_reply);
    } else {
        printf("Echo failed: %s\n", ipc_status_string(status));
    }

    ipc_free(echo_message_buffer);
    ply_free((void*)echo_reply, echo_reply_size);

    sleep(1);

    const char *silent_message = "Hello from client!";
    printf("Sending: %s\n", silent_message);
    status = mach_client_send(
        client, MSG_ID_SILENT,
        silent_message, strlen(silent_message) + 1
    );
    if (status != IPC_SUCCESS) {
        printf("Silent msg failed: %s\n", ipc_status_string(status));
    }

    sleep(1);

    mach_client_disconnect(client);
    mach_client_destroy(client);
    
    return 0;
}