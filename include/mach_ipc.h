#ifndef MACH_IPC_H
#define MACH_IPC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

/* ============================================================================
 * MESSAGE ID ENCODING (keep your original design)
 * 
 * 32-bit message ID layout:
 * [31:20] - Magic (0x875) - 12 bits
 * [19:8]  - Features flags - 12 bits  
 * [7:0]   - Message type - 8 bits
 * ============================================================================ */

#define INTERNAL_MSG_MAGIC          (0x875UL << 20)
#define INTERNAL_MSG_TYPE_MASK      (0xFFUL)
#define INTERNAL_MSG_FEATURE_MASK   (0xFFFUL << 8)

/* Feature flags - keep your original design */
#define INTERNAL_FEATURE_ITRN   (1UL << 8)   // For internal usage (seperate internel/external messages types and define same types without collision)
#define INTERNAL_FEATURE_WACK   (1UL << 9)   // Wait for acknowledgment (will be set/unset automatically)
#define INTERNAL_FEATURE_IACK   (1UL << 10)  // Is an acknowledgment (will be set/unset automatically)
#define INTERNAL_FEATURE_LPCY   (1UL << 11)  // copy local copy instead of moving
// #define INTERNAL_FEATURE_UPSH   (1UL << 12)  // User payload share instead of copy

/* Check if message ID belongs to our protocol */
#define IS_THIS_PROTOCOL_MSG(id) \
    (((id) & (0xFFFUL << 20)) == INTERNAL_MSG_MAGIC)

/* Check feature flags */
#define HAS_FEATURE_ITRN(id) \
    (((id) & INTERNAL_FEATURE_ITRN) != 0)

#define HAS_FEATURE_WACK(id) \
    (((id) & INTERNAL_FEATURE_WACK) != 0)

#define HAS_FEATURE_IACK(id) \
    (((id) & INTERNAL_FEATURE_IACK) != 0)

#define HAS_FEATURE_LPCY(id) \
    (((id) & INTERNAL_FEATURE_LPCY) != 0)

// #define HAS_FEATURE_UPSH(id) \
//     (((id) & INTERNAL_FEATURE_UPSH) != 0)

/* Check specific message type (ignoring features except internal/external) */
#define IS_INTERNAL_MSG_TYPE(id, type) \
    (((id) & (0xFFF000FFUL | (INTERNAL_FEATURE_ITRN))) == ((INTERNAL_MSG_MAGIC) | (INTERNAL_FEATURE_ITRN) | (type)))

#define IS_EXTERNAL_MSG_TYPE(id, type) \
    (((id) & (0xFFF000FFUL | (INTERNAL_FEATURE_ITRN))) == ((INTERNAL_MSG_MAGIC) | (type)))

#define IS_INTERNAL_MSG(id) \
    (((id) & (0xFFF00000UL | (INTERNAL_FEATURE_ITRN))) == ((INTERNAL_MSG_MAGIC) | (INTERNAL_FEATURE_ITRN)))

#define IS_EXTERNAL_MSG(id) \
    (((id) & (0xFFF00000UL | (INTERNAL_FEATURE_ITRN))) == (INTERNAL_MSG_MAGIC))

/* Set/unset features */
#define SET_FEATURE(id, feat) ((id) | (feat))
#define UNSET_FEATURE(id, feat) ((id) & (~(feat)))

/* Internal message types (framework control messages) */
typedef enum {
    INTERNAL_MSG_TYPE_CONNECT = 1,
    // INTERNAL_MSG_TYPE_DISCONNECT = 2,
    // INTERNAL_MSG_TYPE_DEATH_NOTIFY = 3,
} internal_msg_type_t;

/* Construct internal message IDs */
#define INTERNAL_MSG_ID(type) (INTERNAL_MSG_MAGIC | INTERNAL_FEATURE_ITRN | (type))

/* Construct external message IDs */
#define EXTERNAL_MSG_ID(type) (INTERNAL_MSG_MAGIC | (type))

/* Common message IDs */
#define MSG_ID_CONNECT      INTERNAL_MSG_ID(INTERNAL_MSG_TYPE_CONNECT)

/* User message ID (pass through user's type, defaults to external unless internal is already set) */
#define MSG_ID_USER(type)   EXTERNAL_MSG_ID(type)

/* ============================================================================
 * MACH IPC FRAMEWORK - Public API
 * 
 * A simple, clean abstraction over Mach IPC for building client-server
 * applications. Handles all the complexity of Mach ports, messages, and
 * resource management internally.
 * ============================================================================ */

/* Forward declarations - implementation details hidden */
typedef struct mach_server mach_server_t;
typedef struct mach_client mach_client_t;
typedef struct mach_message mach_message_t;

/* Client identifier - opaque to users */
typedef struct {
    uint32_t id;
    int slot;
    void *internal;  // Private - don't touch
} client_handle_t;

#define INVALID_CLIENT ((client_handle_t){0, -1, NULL})
#define IS_VALID_CLIENT(h) ((h).id != 0 && (h).internal != NULL)

/* Message types - users can define custom types > 1000 */
typedef enum {
    MSG_TYPE_CONNECT = 1,
    MSG_TYPE_DISCONNECT = 2,
    MSG_TYPE_PING = 3
} message_type_t;

/* Status codes */
typedef enum {
    IPC_SUCCESS = 0,
    IPC_ERROR_INVALID_PARAM = -1,
    IPC_ERROR_NO_MEMORY = -2,
    IPC_ERROR_NOT_CONNECTED = -3,
    IPC_ERROR_TIMEOUT = -4,
    IPC_ERROR_SEND_FAILED = -5,
    IPC_ERROR_INTERNAL = -6,
    IPC_ERROR_CLIENT_FULL = -7,
    IPC_USER_BASE = 1000 // 1000+ space ment for custom user status codes
} ipc_status_t;

/* ============================================================================
 * SERVER API
 * ============================================================================ */

/* Server callbacks - all called sequentially per client (thread-safe) */
typedef struct {
    /* Called when a new client connects (optional) */
    void (*on_client_connected)(mach_server_t *server, client_handle_t client, void *user_data);
    
    /* Called when a client disconnects (optional) */
    void (*on_client_disconnected)(mach_server_t *server, client_handle_t client, void *user_data);
    
    /* Called when a message arrives that doesn't require a response */
    void (*on_message)(mach_server_t *server, client_handle_t client, 
                       mach_port_t *remote_port, uint32_t msg_type, const void *data, size_t size, void *user_data);
    
    /* Called when a message arrives that requires a response 
     * Return your response data and size. Framework will free it after sending.
     * Return NULL to indicate error. */
    void* (*on_message_with_reply)(mach_server_t *server, client_handle_t client,
                                   mach_port_t *remote_port, uint32_t msg_type, const void *data, size_t size,
                                   size_t *reply_size, void *user_data, int *reply_status);
} server_callbacks_t;

/* Create a server bound to a service name */
mach_server_t* mach_server_create(const char *service_name, 
                                   const server_callbacks_t *callbacks,
                                   void *user_data);

/* Start the server (blocks until stopped or error) */
ipc_status_t mach_server_run(mach_server_t *server);

/* Stop the server gracefully (call from signal handler or another thread) */
void mach_server_stop(mach_server_t *server);

/* Send a message to a specific client (non-blocking) */
ipc_status_t mach_server_send(mach_server_t *server, client_handle_t client,
                              uint32_t msg_type, const void *data, size_t size);

/* Send a message to a client and wait for reply (blocking with timeout) */
ipc_status_t mach_server_send_with_reply(mach_server_t *server, client_handle_t client,
                                         uint32_t msg_type, const void *data, size_t size,
                                         const void **reply_data, size_t *reply_size,
                                         uint32_t timeout_ms);

/* Broadcast to all connected clients */
ipc_status_t mach_server_broadcast(mach_server_t *server, uint32_t msg_type,
                                   const void *data, size_t size);

/* Get number of connected clients */
int mach_server_client_count(mach_server_t *server);

/* Disconnect a specific client */
void mach_server_disconnect_client(mach_server_t *server, client_handle_t client);

/* Cleanup and free server */
void mach_server_destroy(mach_server_t *server);

/* ============================================================================
 * CLIENT API
 * ============================================================================ */

/* Client callbacks - all called sequentially (thread-safe) */
typedef struct {
    /* Called when successfully connected to server (optional) */
    void (*on_connected)(mach_client_t *client, void *user_data);
    
    /* Called when disconnected from server (optional) */
    void (*on_disconnected)(mach_client_t *client, void *user_data);
    
    /* Called when a message arrives that doesn't require a response */
    void (*on_message)(mach_client_t *client, mach_port_t *remote_port, uint32_t msg_type,
                       const void *data, size_t size, void *user_data);
    
    /* Called when a message arrives that requires a response */
    void* (*on_message_with_reply)(mach_client_t *client, mach_port_t *remote_port, uint32_t msg_type,
                                   const void *data, size_t size,
                                   size_t *reply_size, void *user_data, int *reply_status);
} client_callbacks_t;

/* Create a client (doesn't connect yet) */
mach_client_t* mach_client_create(const client_callbacks_t *callbacks, void *user_data);

/* Connect to a server (blocking, with timeout) */
ipc_status_t mach_client_connect(mach_client_t *client, const char *service_name,
                                 uint32_t timeout_ms);

/* Check if connected */
bool mach_client_is_connected(mach_client_t *client);

/* Send a message to server with port (non-blocking) */
ipc_status_t mach_client_send_with_port(mach_client_t *client, mach_port_t local_port,
                                        uint32_t msg_type, const void *data, size_t size);

/* Send a message to server (non-blocking) */
ipc_status_t mach_client_send(mach_client_t *client, uint32_t msg_type,
                              const void *data, size_t size);

/* Send a message and wait for reply (blocking with timeout) */
ipc_status_t mach_client_send_with_port_and_reply(mach_client_t *client, mach_port_t local_port,
                                         uint32_t msg_type, const void *data, size_t size,
                                         const void **reply_data, size_t *reply_size,
                                         uint32_t timeout_ms);

/* Send a message and wait for reply (blocking with timeout) */
ipc_status_t mach_client_send_with_reply(mach_client_t *client, uint32_t msg_type,
                                         const void *data, size_t size,
                                         const void **reply_data, size_t *reply_size,
                                         uint32_t timeout_ms);

/* Disconnect from server */
void mach_client_disconnect(mach_client_t *client);

/* Cleanup and free client */
void mach_client_destroy(mach_client_t *client);

/* ============================================================================
 * UTILITY FUNCTIONS
 * ============================================================================ */

/* Get human-readable error string */
const char* ipc_status_string(ipc_status_t status);

void set_user_ipc_status_string(const char *(*_user_ipc_status_string)(ipc_status_t));

// /* Allocate memory for reply data (use in callbacks) */
void* ipc_alloc(size_t size);

/* Free memory allocated by framework */
void ipc_free(void *ptr);

/* Free payload data */
void ply_free(void *ptr, size_t size);

/* ============================================================================
 * SHARED MEMORY
 * ============================================================================ */

 /* Shared memory handle - opaque to users */
typedef struct {
    mach_vm_address_t address;
    mach_vm_size_t size;
    mach_port_t mem_object;
    bool is_owner;  // true if we allocated it, false if we mapped it
} shared_memory_t;

/**
 * Allocate shared memory (sender side)
 * 
 * @param size Size of memory to allocate
 * @param out_shmem Output shared memory handle
 * @return KERN_SUCCESS or error code
 */
kern_return_t shared_memory_create(mach_vm_size_t size, shared_memory_t **out_shmem);

/**
 * Get pointer to shared memory data
 * 
 * @param shmem Shared memory handle
 * @return Pointer to data or NULL
 */
void* shared_memory_get_data(shared_memory_t *shmem);

/**
 * Get size to shared memory data
 * 
 * @param shmem Shared memory handle
 * @return Size of data or zero
 */
size_t shared_memory_get_size(shared_memory_t *shmem);

/**
 * Get memory object port for sending (doesn't transfer ownership)
 * 
 * @param shmem Shared memory handle
 * @return Memory object port
 */
mach_port_t shared_memory_get_port(shared_memory_t *shmem);

/**
 * Map received shared memory (receiver side)
 * 
 * @param mem_object Memory object port received
 * @param size Size of memory
 * @param out_shmem Output shared memory handle
 * @return KERN_SUCCESS or error code
 */
kern_return_t shared_memory_map(mach_port_t mem_object, mach_vm_size_t size, 
                                shared_memory_t **out_shmem);

/**
 * Destroy shared memory
 * 
 * @param shmem Shared memory handle
 */
void shared_memory_destroy(shared_memory_t *shmem);

#endif /* MACH_IPC_H */
