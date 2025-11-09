#include "echo.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <inttypes.h>

static volatile int running = 1;

pthread_mutex_t shmem_lock;
shared_memory_t *shmem = NULL;
size_t shmem_size = 4096;

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

    pthread_mutex_lock(&shmem_lock);
    shared_memory_destroy(shmem);
    shmem = NULL;
    pthread_mutex_unlock(&shmem_lock);
    pthread_mutex_destroy(&shmem_lock);

    running = 0;
}

void on_message(mach_client_t *client, mach_port_t *remote_port, uint32_t msg_type,
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

    pthread_mutex_init(&shmem_lock, NULL);

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
        pthread_mutex_destroy(&shmem_lock);
        return 1;
    }
    
    ipc_status_t status = mach_client_connect(client, "com.example.echo", 5000);
    if (status != IPC_SUCCESS) {
        fprintf(stderr, "Failed to connect: %s\n", ipc_status_string(status));
        mach_client_destroy(client);
        pthread_mutex_destroy(&shmem_lock);
        return 1;
    }

    // make shmem
    pthread_mutex_lock(&shmem_lock);
    kern_return_t kr = shared_memory_create(shmem_size, &shmem);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "Failed to create shared memory: %s\n", mach_error_string(kr));
        pthread_mutex_unlock(&shmem_lock);
        mach_client_destroy(client);
        return 1;
    }
    pthread_mutex_unlock(&shmem_lock);
    printf("[Client] Created shared memory: %zu bytes\n", shmem_size);
    
    // map shmem server side
    pthread_mutex_lock(&shmem_lock);
    if (!shmem) {
        // (disconnected)
        printf("Set echo shm failed because shmem doesnt't exist any more");
        pthread_mutex_unlock(&shmem_lock);
        mach_client_destroy(client);
        return 1;
    }
    status = mach_client_send_with_port_and_reply(
        client,
        shared_memory_get_port(shmem),
        MSG_ID_SET_ECHO_SHM,
        (const void*)&shmem_size,
        sizeof(shmem_size),
        NULL,
        NULL,
        2000
    );
    pthread_mutex_unlock(&shmem_lock);
    if (status != IPC_SUCCESS) {
        printf("Set echo shm failed: %s\n", ipc_status_string(status));
        mach_client_destroy(client);
        return 1;
    }

    // Write data to shared memory
    pthread_mutex_lock(&shmem_lock);
    if (!shmem) {
        // (disconnected)
        printf("Set echo shm failed because shmem doesnt't exist any more");
        pthread_mutex_unlock(&shmem_lock);
        mach_client_destroy(client);
        return 1;
    }
    const char *echo_message = "Hello from client! Data in shared memory.";
    size_t echo_message_len = strlen(echo_message) + 1;
    snprintf(shared_memory_get_data(shmem), shmem_size, "%s", echo_message);
    pthread_mutex_unlock(&shmem_lock);

    // Sent echo event
    status = mach_client_send_with_reply(
        client,
        MSG_ID_ECHO,
        NULL,
        0,
        NULL,
        NULL,
        2000
    );
    if (status != ECHO_CUSTOM_STATUS) {
        printf("Send echo failed: %s\n", ipc_status_string(status));
        mach_client_destroy(client);
        return 1;
    }

    // read back the echo
    pthread_mutex_lock(&shmem_lock);
    if (!shmem) {
        // (disconnected)
        printf("Set echo shm failed because shmem doesnt't exist any more");
        pthread_mutex_unlock(&shmem_lock);
        mach_client_destroy(client);
        return 1;
    }
    printf("Server: %.*s\n", (int)shared_memory_get_size(shmem), (char*)shared_memory_get_data(shmem));
    pthread_mutex_unlock(&shmem_lock);

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