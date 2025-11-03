#include "stress_test.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>

static volatile int g_running = 1;
static mach_client_t *g_client = NULL;

// Client statistics
typedef struct {
    pthread_mutex_t lock;
    uint32_t pings_sent;
    uint32_t pings_received;
    uint32_t broadcasts_received;
    uint32_t echos_received;
    uint32_t timeouts;
    uint32_t errors;
    uint64_t total_latency_us;
} client_stats_t;

static client_stats_t g_stats = {0};

void signal_handler(int sig) {
    (void)sig;
    printf("\n=== Client Shutting Down ===\n");
    printf("Statistics:\n");
    printf("  Pings Sent: %u\n", g_stats.pings_sent);
    printf("  Pings Received: %u\n", g_stats.pings_received);
    printf("  Broadcasts Received: %u\n", g_stats.broadcasts_received);
    printf("  Echos Received: %u\n", g_stats.echos_received);
    printf("  Timeouts: %u\n", g_stats.timeouts);
    printf("  Errors: %u\n", g_stats.errors);
    if (g_stats.pings_received > 0) {
        printf("  Average Latency: %llu us\n", 
               g_stats.total_latency_us / g_stats.pings_received);
    }
    printf("============================\n");
    g_running = 0;
}

void on_connected(mach_client_t *client, void *data) {
    (void)client;
    (void)data;
    printf("[CONNECT] Connected to server!\n");
}

void on_disconnected(mach_client_t *client, void *data) {
    (void)client;
    (void)data;
    printf("[DISCONNECT] Disconnected from server\n");
    g_running = 0;
}

void on_message(mach_client_t *client, uint32_t msg_type,
                const void *data, size_t size, void *user_data) {
    (void)client;
    (void)user_data;
    
    switch (msg_type) {
        case MSG_TYPE_BROADCAST_MSG:
            pthread_mutex_lock(&g_stats.lock);
            g_stats.broadcasts_received++;
            pthread_mutex_unlock(&g_stats.lock);
            printf("[BROADCAST] Received: %.*s\n", (int)size, (char*)data);
            break;
            
        case MSG_TYPE_ECHO_BACK:
            pthread_mutex_lock(&g_stats.lock);
            g_stats.echos_received++;
            pthread_mutex_unlock(&g_stats.lock);
            printf("[ECHO] Received back: %.*s\n", (int)size, (char*)data);
            break;
            
        default:
            printf("[UNKNOWN] Received message type: %u\n", msg_type);
            break;
    }
}

// Test 1: Rapid ping-pong
void test_ping_flood(mach_client_t *client, int count) {
    printf("\n=== Test 1: Ping Flood (%d messages) ===\n", count);
    
    struct timeval start, end;
    gettimeofday(&start, NULL);
    
    for (int i = 0; i < count && g_running; i++) {
        ping_payload_t ping;
        ping.sequence = i;
        gettimeofday(&end, NULL);
        ping.timestamp = end.tv_sec * 1000000ULL + end.tv_usec;
        ping.client_id = 0;
        
        const void *reply_data = NULL;
        size_t reply_size = 0;
        
        ipc_status_t status = mach_client_send_with_reply(
            client, MSG_ID_PING,
            &ping, sizeof(ping),
            &reply_data, &reply_size,
            2000
        );
        
        pthread_mutex_lock(&g_stats.lock);
        g_stats.pings_sent++;
        
        if (status == STRESS_STATUS_PING_OK && reply_data) {
            g_stats.pings_received++;
            
            ping_payload_t *pong = (ping_payload_t*)reply_data;
            struct timeval now;
            gettimeofday(&now, NULL);
            uint64_t now_us = now.tv_sec * 1000000ULL + now.tv_usec;
            uint64_t latency = now_us - ping.timestamp;
            g_stats.total_latency_us += latency;
            
            if (i % 100 == 0) {
                printf("  Ping %d: %llu us\n", i, latency);
            }
        } else if (status == IPC_ERROR_TIMEOUT) {
            g_stats.timeouts++;
            printf("  Ping %d: TIMEOUT\n", i);
        } else {
            g_stats.errors++;
            printf("  Ping %d: ERROR - %s\n", i, ipc_status_string(status));
        }
        pthread_mutex_unlock(&g_stats.lock);
        
        ply_free((void*)reply_data, reply_size);
    }
    
    gettimeofday(&end, NULL);
    double elapsed = (end.tv_sec - start.tv_sec) + 
                     (end.tv_usec - start.tv_usec) / 1000000.0;
    printf("Completed in %.2f seconds (%.0f msg/s)\n", elapsed, count / elapsed);
}

// Test 2: Heavy payload
void test_heavy_payload(mach_client_t *client, size_t size) {
    printf("\n=== Test 2: Heavy Payload (%zu bytes) ===\n", size);
    
    void *payload = ipc_alloc(size);
    if (!payload) {
        printf("Failed to allocate payload\n");
        return;
    }
    
    // Fill with pattern
    for (size_t i = 0; i < size; i++) {
        ((char*)payload)[i] = (char)(i % 256);
    }
    
    const void *reply_data = NULL;
    size_t reply_size = 0;
    
    struct timeval start, end;
    gettimeofday(&start, NULL);
    
    ipc_status_t status = mach_client_send_with_reply(
        client, MSG_ID_HEAVY_PAYLOAD,
        payload, size,
        &reply_data, &reply_size,
        5000
    );
    
    gettimeofday(&end, NULL);
    double elapsed = (end.tv_sec - start.tv_sec) + 
                     (end.tv_usec - start.tv_usec) / 1000000.0;
    
    if (status == STRESS_STATUS_HEAVY_OK) {
        printf("Success! Round-trip: %.2f ms (%.2f MB/s)\n",
               elapsed * 1000, (size * 2.0) / (1024 * 1024 * elapsed));
    } else {
        printf("Failed: %s\n", ipc_status_string(status));
    }
    
    ipc_free(payload);
    ply_free((void*)reply_data, reply_size);
}

// Test 3: Burst mode (fire-and-forget)
void test_burst_mode(mach_client_t *client, int count) {
    printf("\n=== Test 3: Burst Mode (%d messages) ===\n", count);
    
    struct timeval start, end;
    gettimeofday(&start, NULL);
    
    for (int i = 0; i < count && g_running; i++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Burst message %d", i);
        
        ipc_status_t status = mach_client_send(
            client, MSG_ID_ECHO_BACK,
            msg, strlen(msg) + 1
        );
        
        if (status != IPC_SUCCESS) {
            pthread_mutex_lock(&g_stats.lock);
            g_stats.errors++;
            pthread_mutex_unlock(&g_stats.lock);
        }
    }
    
    gettimeofday(&end, NULL);
    double elapsed = (end.tv_sec - start.tv_sec) + 
                     (end.tv_usec - start.tv_usec) / 1000000.0;
    
    printf("Sent %d messages in %.2f seconds (%.0f msg/s)\n",
           count, elapsed, count / elapsed);
    
    // Wait a bit for echos
    sleep(1);
    
    // Confirm with server
    uint32_t burst_count = count;
    const void *reply_data = NULL;
    size_t reply_size = 0;
    
    ipc_status_t status = mach_client_send_with_reply(
        client, MSG_ID_BURST,
        &burst_count, sizeof(burst_count),
        &reply_data, &reply_size,
        2000
    );
    
    if (status == STRESS_STATUS_BURST_OK && reply_data) {
        uint32_t ack = *(uint32_t*)reply_data;
        printf("Server acknowledged: %u messages\n", ack);
    }
    
    ply_free((void*)reply_data, reply_size);
}

// Test 4: Broadcast request
void test_broadcast(mach_client_t *client) {
    printf("\n=== Test 4: Broadcast Test ===\n");
    
    const char *msg = "Request broadcast";
    ipc_status_t status = mach_client_send(
        client, MSG_ID_BROADCAST_REQ,
        msg, strlen(msg) + 1
    );
    
    if (status == IPC_SUCCESS) {
        printf("Broadcast requested\n");
        sleep(1); // Wait for broadcast
    } else {
        printf("Failed: %s\n", ipc_status_string(status));
    }
}

// Test 5: Timeout handling
void test_timeout(mach_client_t *client) {
    printf("\n=== Test 5: Timeout Handling ===\n");
    
    // Test 1: Should succeed
    uint32_t delay = 500;
    const void *reply_data = NULL;
    size_t reply_size = 0;
    
    printf("Testing 500ms delay (2s timeout)...\n");
    ipc_status_t status = mach_client_send_with_reply(
        client, MSG_ID_TIMEOUT_TEST,
        &delay, sizeof(delay),
        &reply_data, &reply_size,
        2000
    );
    
    if (status == STRESS_STATUS_TIMEOUT_OK) {
        printf("  Success!\n");
    } else {
        printf("  Unexpected: %s\n", ipc_status_string(status));
    }
    ply_free((void*)reply_data, reply_size);
    
    // Test 2: Should timeout
    delay = 3000;
    reply_data = NULL;
    reply_size = 0;
    
    printf("Testing 3s delay (2s timeout)...\n");
    status = mach_client_send_with_reply(
        client, MSG_ID_TIMEOUT_TEST,
        &delay, sizeof(delay),
        &reply_data, &reply_size,
        2000
    );
    
    if (status == IPC_ERROR_TIMEOUT) {
        printf("  Correctly timed out!\n");
        pthread_mutex_lock(&g_stats.lock);
        g_stats.timeouts++;
        pthread_mutex_unlock(&g_stats.lock);
    } else {
        printf("  Unexpected: %s\n", ipc_status_string(status));
    }
    ply_free((void*)reply_data, reply_size);
}

// Test 6: Shared memory (UPSH feature)
void test_shared_memory(mach_client_t *client) {
    printf("\n=== Test 6: Shared Memory ===\n");
    
    size_t size = 1024 * 1024; // 1MB
    void *buffer = ipc_alloc(size);
    if (!buffer) {
        printf("Failed to allocate buffer\n");
        return;
    }
    
    // Fill with pattern
    for (size_t i = 0; i < size; i++) {
        ((char*)buffer)[i] = (char)(i % 256);
    }
    
    const void *reply_data = NULL;
    size_t reply_size = 0;
    
    printf("Sharing %zu bytes...\n", size);
    ipc_status_t status = mach_client_send_with_reply(
        client, MSG_ID_SHARE_MEMORY,
        buffer, size,
        &reply_data, &reply_size,
        5000
    );
    
    if (status == STRESS_STATUS_SHARE_OK && reply_data) {
        uint32_t verified = *(uint32_t*)reply_data;
        printf("Server verified %u bytes\n", verified);
    } else {
        printf("Failed: %s\n", ipc_status_string(status));
    }
    
    ipc_free(buffer);
    ply_free((void*)reply_data, reply_size);
}

// Test 7: Get server statistics
void test_get_stats(mach_client_t *client) {
    printf("\n=== Test 7: Server Statistics ===\n");
    
    const void *reply_data = NULL;
    size_t reply_size = 0;
    
    ipc_status_t status = mach_client_send_with_reply(
        client, MSG_ID_STATS_REQ,
        NULL, 0,
        &reply_data, &reply_size,
        2000
    );
    
    if (status == IPC_SUCCESS && reply_data) {
        stats_payload_t *stats = (stats_payload_t*)reply_data;
        printf("Server Statistics:\n");
        printf("  Total Messages: %u\n", stats->total_messages);
        printf("  Total Bytes: %u\n", stats->total_bytes);
        printf("  Broadcasts: %u\n", stats->broadcasts);
        printf("  Timeouts: %u\n", stats->timeouts);
        printf("  Errors: %u\n", stats->errors);
    } else {
        printf("Failed: %s\n", ipc_status_string(status));
    }
    
    ply_free((void*)reply_data, reply_size);
}

int main(int argc, char *argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    int test_mode = 0; // 0 = all tests, 1+ = specific test
    if (argc > 1) {
        test_mode = atoi(argv[1]);
    }
    
    // Initialize stats
    pthread_mutex_init(&g_stats.lock, NULL);
    
    set_user_ipc_status_string(stress_status_string);
    
    client_callbacks_t callbacks = {
        .on_connected = on_connected,
        .on_disconnected = on_disconnected,
        .on_message = on_message,
        .on_message_with_reply = NULL
    };
    
    g_client = mach_client_create(&callbacks, NULL);
    if (!g_client) {
        fprintf(stderr, "Failed to create client\n");
        return 1;
    }
    
    printf("=== Stress Test Client ===\n");
    printf("Connecting to com.example.stress...\n");
    
    ipc_status_t status = mach_client_connect(g_client, "com.example.stress", 5000);
    if (status != IPC_SUCCESS) {
        fprintf(stderr, "Failed to connect: %s\n", ipc_status_string(status));
        mach_client_destroy(g_client);
        return 1;
    }
    
    // Run tests
    if (test_mode == 0 || test_mode == 1) {
        test_ping_flood(g_client, 1000);
    }
    
    if (test_mode == 0 || test_mode == 2) {
        test_heavy_payload(g_client, 1024 * 1024); // 1MB
        test_heavy_payload(g_client, 10 * 1024 * 1024); // 10MB
    }
    
    if (test_mode == 0 || test_mode == 3) {
        test_burst_mode(g_client, 500);
    }
    
    if (test_mode == 0 || test_mode == 4) {
        test_broadcast(g_client);
    }
    
    if (test_mode == 0 || test_mode == 5) {
        test_timeout(g_client);
    }
    
    if (test_mode == 0 || test_mode == 6) {
        test_shared_memory(g_client);
    }
    
    if (test_mode == 0 || test_mode == 7) {
        test_get_stats(g_client);
    }
    
    printf("\n=== All Tests Complete ===\n");
    printf("Press Ctrl+C to exit or wait for disconnect...\n");
    
    // Keep alive to receive broadcasts/echos
    while (g_running && mach_client_is_connected(g_client)) {
        sleep(1);
    }
    
    mach_client_disconnect(g_client);
    mach_client_destroy(g_client);
    
    pthread_mutex_destroy(&g_stats.lock);
    
    return 0;
}