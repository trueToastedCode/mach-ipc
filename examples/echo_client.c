#include "mach_ipc.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#define MSG_ECHO (1)

static volatile int running = 1;

void signal_handler(int sig) {
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

int main() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    client_callbacks_t callbacks = {
        .on_connected = on_connected,
        .on_disconnected = on_disconnected,
        .on_message = NULL,
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
    
    const char *message = "Hello World!";
    void *reply = NULL;
    size_t reply_size = 0;
    printf("Sending: %s\n", message);
    status = mach_client_send_with_reply(
        client, MSG_ECHO,
        message, strlen(message) + 1,
        &reply, &reply_size,
        2000
    );
    if (status == IPC_SUCCESS && reply) {
        printf("Echo reply: %s\n", (char*)reply);
        ipc_free(reply);
    } else {
        printf("Echo failed: %s\n", ipc_status_string(status));
    }

    sleep(1);
    
    mach_client_disconnect(client);
    mach_client_destroy(client);
    
    return 0;
}