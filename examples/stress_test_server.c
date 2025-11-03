#include "stress_test.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>

// Server statistics
typedef struct {
    pthread_mutex_t lock;
    uint32_t total_messages;
    uint32_t total_bytes;
    uint32_t broadcasts;
    uint32_t timeouts;
    uint32_t errors;
    uint32_t active_clients;
} server_stats_t;

static mach_server_t *g_server = NULL;
static server_stats_t g_stats = {0};

void signal_handler(int sig) {
    (void)sig;
    printf("\n=== Shutting Down Server ===\n");
    printf("Final Statistics:\n");
    printf("  Total Messages: %u\n", g_stats.total_messages);
    printf("  Total Bytes: %u\n", g_stats.total_bytes);
    printf("  Broadcasts: %u\n", g_stats.broadcasts);
    printf("  Timeouts: %u\n", g_stats.timeouts);
    printf("  Errors: %u\n", g_stats.errors);
    printf("  Active Clients: %u\n", g_stats.active_clients);
    printf("===========================\n");
    
    if (g_server) {
        mach_server_stop(g_server);
    }
}

void on_client_connected(mach_server_t *server, client_handle_t client, void *data) {
    (void)server;
    (void)data;
    
    pthread_mutex_lock(&g_stats.lock);
    g_stats.active_clients++;
    pthread_mutex_unlock(&g_stats.lock);
    
    printf("[CONNECT] Client %u connected (slot %d) - Total: %u\n", 
           client.id, client.slot, g_stats.active_clients);
}

void on_client_disconnected(mach_server_t *server, client_handle_t client, void *data) {
    (void)server;
    (void)data;
    
    pthread_mutex_lock(&g_stats.lock);
    g_stats.active_clients--;
    pthread_mutex_unlock(&g_stats.lock);
    
    printf("[DISCONNECT] Client %u disconnected - Remaining: %u\n", 
           client.id, g_stats.active_clients);
}

void on_message(mach_server_t *server, client_handle_t client,
                uint32_t msg_type, const void *data, size_t size, void *user_data) {
    (void)user_data;
    
    pthread_mutex_lock(&g_stats.lock);
    g_stats.total_messages++;
    g_stats.total_bytes += size;
    pthread_mutex_unlock(&g_stats.lock);
    
    switch (msg_type) {
        case MSG_TYPE_BROADCAST_REQ: {
            // Client requested a broadcast
            const char *msg = "BROADCAST from server!";
            ipc_status_t status = mach_server_broadcast(
                server, MSG_ID_BROADCAST_MSG, msg, strlen(msg) + 1
            );
            
            pthread_mutex_lock(&g_stats.lock);
            g_stats.broadcasts++;
            pthread_mutex_unlock(&g_stats.lock);
            
            if (status == IPC_SUCCESS) {
                printf("[BROADCAST] Sent to all clients (requested by %u)\n", client.id);
            } else {
                printf("[ERROR] Broadcast failed: %s\n", ipc_status_string(status));
                pthread_mutex_lock(&g_stats.lock);
                g_stats.errors++;
                pthread_mutex_unlock(&g_stats.lock);
            }
            break;
        }
        
        case MSG_TYPE_ECHO_BACK: {
            // Echo message back to client
            ipc_status_t status = mach_server_send(
                server, client, MSG_ID_ECHO_BACK, data, size
            );
            if (status != IPC_SUCCESS) {
                printf("[ERROR] Echo back failed: %s\n", ipc_status_string(status));
                pthread_mutex_lock(&g_stats.lock);
                g_stats.errors++;
                pthread_mutex_unlock(&g_stats.lock);
            }
            break;
        }
        
        default:
            printf("[UNKNOWN] Client %u sent unknown message type: %u\n", 
                   client.id, msg_type);
            break;
    }
}

void* on_message_with_reply(mach_server_t *server, client_handle_t client,
                             uint32_t msg_type, const void *data, size_t size,
                             size_t *reply_size, void *user_data, int *reply_status) {
    (void)server;
    (void)user_data;
    
    pthread_mutex_lock(&g_stats.lock);
    g_stats.total_messages++;
    g_stats.total_bytes += size;
    pthread_mutex_unlock(&g_stats.lock);
    
    void *reply = NULL;
    
    switch (msg_type) {
        case MSG_TYPE_PING: {
            // Simple ping-pong
            ping_payload_t *ping = (ping_payload_t*)data;
            
            ping_payload_t *pong = ipc_alloc(sizeof(ping_payload_t));
            if (pong) {
                memcpy(pong, ping, sizeof(ping_payload_t));
                struct timeval tv;
                gettimeofday(&tv, NULL);
                pong->timestamp = tv.tv_sec * 1000000ULL + tv.tv_usec;
                
                reply = pong;
                *reply_size = sizeof(ping_payload_t);
                *reply_status = STRESS_STATUS_PING_OK;
                
                if (ping->sequence % 100 == 0) {
                    printf("[PING] Client %u seq=%u\n", client.id, ping->sequence);
                }
            }
            break;
        }
        
        case MSG_TYPE_HEAVY_PAYLOAD: {
            // Process large payload
            printf("[HEAVY] Client %u sent %zu bytes\n", client.id, size);
            
            // Allocate and return same size
            reply = ipc_alloc(size);
            if (reply) {
                memcpy(reply, data, size);
                *reply_size = size;
                *reply_status = STRESS_STATUS_HEAVY_OK;
            }
            break;
        }
        
        case MSG_TYPE_BURST: {
            // Handle burst test - just acknowledge
            uint32_t *count = (uint32_t*)data;
            printf("[BURST] Client %u completed burst of %u messages\n", 
                   client.id, *count);
            
            uint32_t *ack = ipc_alloc(sizeof(uint32_t));
            if (ack) {
                *ack = *count;
                reply = ack;
                *reply_size = sizeof(uint32_t);
                *reply_status = STRESS_STATUS_BURST_OK;
            }
            break;
        }
        
        case MSG_TYPE_TIMEOUT_TEST: {
            // Intentionally delay to test timeout handling
            uint32_t delay_ms = *(uint32_t*)data;
            printf("[TIMEOUT] Client %u requested %ums delay\n", client.id, delay_ms);
            
            if (delay_ms > 0) {
                usleep(delay_ms * 1000);
            }
            
            uint32_t *result = ipc_alloc(sizeof(uint32_t));
            if (result) {
                *result = delay_ms;
                reply = result;
                *reply_size = sizeof(uint32_t);
                *reply_status = STRESS_STATUS_TIMEOUT_OK;
            }
            
            pthread_mutex_lock(&g_stats.lock);
            g_stats.timeouts++;
            pthread_mutex_unlock(&g_stats.lock);
            break;
        }
        
        case MSG_TYPE_SHARE_MEMORY: {
            // Test shared memory feature (UPSH flag)
            printf("[SHARE] Client %u shared %zu bytes\n", client.id, size);
            
            // Verify we can read the shared memory
            const char *shared_data = (const char*)data;
            size_t verified = 0;
            for (size_t i = 0; i < size && i < 1024; i++) {
                if (shared_data[i] != 0) verified++;
            }
            
            uint32_t *result = ipc_alloc(sizeof(uint32_t));
            if (result) {
                *result = verified;
                reply = result;
                *reply_size = sizeof(uint32_t);
                *reply_status = STRESS_STATUS_SHARE_OK;
            }
            break;
        }
        
        case MSG_TYPE_STATS_REQ: {
            // Return current statistics
            stats_payload_t *stats = ipc_alloc(sizeof(stats_payload_t));
            if (stats) {
                pthread_mutex_lock(&g_stats.lock);
                stats->total_messages = g_stats.total_messages;
                stats->total_bytes = g_stats.total_bytes;
                stats->broadcasts = g_stats.broadcasts;
                stats->timeouts = g_stats.timeouts;
                stats->errors = g_stats.errors;
                pthread_mutex_unlock(&g_stats.lock);
                
                reply = stats;
                *reply_size = sizeof(stats_payload_t);
                *reply_status = IPC_SUCCESS;
                
                printf("[STATS] Sent to client %u\n", client.id);
            }
            break;
        }
        
        default:
            printf("[UNKNOWN] Client %u sent unknown request type: %u\n", 
                   client.id, msg_type);
            *reply_status = IPC_ERROR_INVALID_PARAM;
            break;
    }
    
    if (!reply) {
        pthread_mutex_lock(&g_stats.lock);
        g_stats.errors++;
        pthread_mutex_unlock(&g_stats.lock);
    }
    
    return reply;
}

int main() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Initialize stats
    pthread_mutex_init(&g_stats.lock, NULL);
    
    set_user_ipc_status_string(stress_status_string);
    
    server_callbacks_t callbacks = {
        .on_client_connected = on_client_connected,
        .on_client_disconnected = on_client_disconnected,
        .on_message = on_message,
        .on_message_with_reply = on_message_with_reply
    };
    
    g_server = mach_server_create("com.example.stress", &callbacks, NULL);
    if (!g_server) {
        fprintf(stderr, "Failed to create server\n");
        return 1;
    }
    
    printf("=== Stress Test Server Started ===\n");
    printf("Service: com.example.stress\n");
    printf("Ready for connections...\n");
    printf("===================================\n\n");
    
    ipc_status_t status = mach_server_run(g_server);
    printf("\nServer stopped: %s\n", ipc_status_string(status));
    
    pthread_mutex_destroy(&g_stats.lock);
    mach_server_destroy(g_server);
    return 0;
}