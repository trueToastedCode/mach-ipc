/* ============================================================================
 * Internal Protocol - Implementation Details
 * NOT exposed to framework users
 * ============================================================================ */

#ifndef INTERNAL_H
#define INTERNAL_H

#include <mach/mach.h>
#include <pthread.h>
#include <dispatch/dispatch.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include "mach_ipc.h"
#include "pool.h"
#include "event_framework.h"

/* ============================================================================
 * MACH MESSAGE STRUCTURES
 * ============================================================================ */

/* Out-of-line message structure */
typedef struct {
    mach_msg_header_t header;
    mach_msg_body_t body;
    mach_msg_ool_descriptor_t payload;
    mach_msg_ool_descriptor_t user_payload;
} internal_mach_msg_t;

/* Payload structure sent in OOL descriptor */
typedef struct {
    uint32_t client_id;         // Client identifier
    int client_slot;            // Client slot
    uint64_t correlation_id;    // For ack matching (0 = no ack needed)
    int correlation_slot;       // For ack lookup
    int32_t status;             // Status code (0 = success)
    struct timespec user_payload_deadline;
} internal_payload_t;

#define INTERNAL_RCV_BUFFER_SIZE (sizeof(internal_mach_msg_t) + 1024)

/* ============================================================================
 * RESOURCE TRACKING
 * ============================================================================ */

typedef enum {
    RES_TYPE_PORT,
    RES_TYPE_MEMORY,
    RES_TYPE_QUEUE,
    RES_TYPE_THREAD,
    RES_TYPE_MUTEX,
    RES_TYPE_POOL,
    RES_TYPE_CUSTOM
} resource_type_t;

typedef struct resource_tracker resource_tracker_t;

resource_tracker_t* resource_tracker_create(void);
bool resource_tracker_add(resource_tracker_t *tracker, resource_type_t type,
                         void *resource, void (*custom_cleanup)(void*),
                         const char *debug_name);
void resource_tracker_cleanup_all(resource_tracker_t *tracker);
void resource_tracker_destroy(resource_tracker_t *tracker);

/* ============================================================================
 * ACK MANAGEMENT
 * ============================================================================ */

typedef struct {
    uint64_t correlation_id;
    event_t *event;
    internal_payload_t *reply_payload;
    size_t reply_size;
    const void *reply_user_payload;
    size_t reply_user_size;
    atomic_bool received;
    atomic_bool cancelled;  // Prevents use-after-free on timeout race
} ack_waiter_t;

#define USER_PLY_SAFETY_MS 10ULL

/* ============================================================================
 * CLIENT INFO (Server-side)
 * ============================================================================ */

typedef struct {
    uint32_t id;                    // Unique client ID
    mach_port_t port;               // Client's reply port
    dispatch_queue_t queue;         // Sequential message processing
    bool death_notif_setup;         // Death notification registered
    volatile bool active;           // Client is active
    char debug_name[64];            // For logging
} client_info_t;

/* ============================================================================
 * SERVER STRUCTURE (Internal)
 * ============================================================================ */

#define MAX_CLIENTS 100
#define MAX_ACKS 256

struct mach_server {
    // Mach resources
    mach_port_t service_port;
    char service_name[128];
    
    // Client management
    client_info_t *clients[MAX_CLIENTS];
    pthread_mutex_t clients_lock;
    int client_count;
    uint32_t next_client_id;
    
    // Message handling
    pthread_t receiver_thread;
    
    // Acknowledgment tracking
    Pool ack_pool;
    uint64_t next_correlation_id;
    pthread_mutex_t ack_lock;
    
    // Lifecycle
    volatile sig_atomic_t running;
    
    // Callbacks & user data
    server_callbacks_t callbacks;
    void *user_data;
    
    // Resource tracking
    resource_tracker_t *resources;
};

/* ============================================================================
 * CLIENT STRUCTURE (Internal)
 * ============================================================================ */

struct mach_client {
    // Connection state
    mach_port_t server_port;
    mach_port_t local_port;
    char service_name[128];
    uint32_t client_id;             // Server-assigned ID
    int client_slot;                // Server-assigned slot
    
    // Message handling
    pthread_t receiver_thread;
    dispatch_queue_t message_queue;  // Sequential processing queue
    
    // Acknowledgment tracking
    Pool ack_pool;
    uint64_t next_correlation_id;
    pthread_mutex_t ack_lock;
    
    // Lifecycle
    volatile sig_atomic_t connected;
    volatile sig_atomic_t running;
    
    // Callbacks & user data
    client_callbacks_t callbacks;
    void *user_data;
    
    // Resource tracking
    resource_tracker_t *resources;
};

client_info_t* create_client(uint32_t id, mach_port_t port);
void destroy_client(client_info_t *client);
void remove_client(mach_server_t *server, client_info_t *client);

/* ============================================================================
 * LOW-LEVEL PROTOCOL FUNCTIONS
 * ============================================================================ */

/* Send a message (low-level) */
kern_return_t protocol_send_message(
    mach_port_t dest_port,
    mach_port_t local_port,
    mach_msg_id_t msg_id,
    internal_payload_t *payload,
    size_t payload_size,
    const void *user_payload,
    size_t user_payload_size,
    uint64_t user_payload_tio_ms
);

/* Send a message and wait for ack */
kern_return_t protocol_send_with_ack(
    mach_port_t dest_port,
    mach_port_t reply_port,
    Pool *ack_pool,
    pthread_mutex_t *ack_lock,
    uint64_t *next_correlation_id,
    mach_msg_id_t msg_id,
    internal_payload_t *payload,
    size_t payload_size,
    const void *user_payload,
    size_t user_payload_size,
    internal_payload_t **ack_payload,
    size_t *ack_size,
    const void **ack_user_payload,
    size_t *ack_user_size,
    uint64_t timeout_ms
);

/* Send an acknowledgment */
kern_return_t protocol_send_ack(
    mach_port_t dest_port,
    mach_msg_id_t original_msg_id,
    uint64_t correlation_id,
    int correlation_slot,
    internal_payload_t *ack_payload,
    size_t ack_payload_size,
    const void *ack_user_payload,
    size_t ack_user_payload_size
);

/* Receive and dispatch messages (blocking with timeout) */
typedef bool (*message_handler_t)(
    mach_port_t service_port,
    mach_msg_header_t *header,
    internal_payload_t *payload,
    size_t payload_size,
    const void *user_payload,
    size_t user_payload_size,
    void *context
);

void protocol_receive_loop(
    mach_port_t service_port,
    volatile sig_atomic_t *running,
    Pool *ack_pool,
    pthread_mutex_t *ack_lock,
    message_handler_t handler,
    void *context
);

/* ============================================================================
 * HELPER FUNCTIONS
 * ============================================================================ */

// /* Create a payload buffer */
// internal_payload_t* create_payload(size_t data_size);

// /* Free a payload */
// void free_payload(internal_payload_t *payload);

// /* Copy user data into payload */
// void copy_to_payload(internal_payload_t *payload, const void *data, size_t size);

/* Find client by ID (server-side, must hold clients_lock) */
client_info_t* find_client_by_id_locked(mach_server_t *server, uint32_t client_id, int *slot);

/* Find client by port (server-side, must hold clients_lock) */
client_info_t* find_client_by_port_locked(mach_server_t *server, mach_port_t port, int *slot);

struct timespec calc_deadline(uint64_t timeout_ms);

bool is_deadline_expired(struct timespec deadline, uint64_t safety_ms);

bool has_no_deadline(struct timespec deadline);

#endif /* INTERNAL_H */