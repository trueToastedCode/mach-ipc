#ifndef MACH_IPC_H
#define MACH_IPC_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

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
    void *internal;  // Private - don't touch
} client_handle_t;

#define INVALID_CLIENT ((client_handle_t){0, NULL})
#define IS_VALID_CLIENT(h) ((h).id != 0 && (h).internal != NULL)

/* Message types - users can define custom types > 1000 */
typedef enum {
    MSG_TYPE_CONNECT = 1,
    MSG_TYPE_DISCONNECT = 2,
    MSG_TYPE_PING = 3,
    MSG_TYPE_USER_DEFINED = 1000
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
    IPC_ERROR_CLIENT_FULL = -7
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
                       uint32_t msg_type, const void *data, size_t size, void *user_data);
    
    /* Called when a message arrives that requires a response 
     * Return your response data and size. Framework will free it after sending.
     * Return NULL to indicate error. */
    void* (*on_message_with_reply)(mach_server_t *server, client_handle_t client,
                                   uint32_t msg_type, const void *data, size_t size,
                                   size_t *reply_size, void *user_data);
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
                                         void **reply_data, size_t *reply_size,
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
    void (*on_message)(mach_client_t *client, uint32_t msg_type,
                       const void *data, size_t size, void *user_data);
    
    /* Called when a message arrives that requires a response */
    void* (*on_message_with_reply)(mach_client_t *client, uint32_t msg_type,
                                   const void *data, size_t size,
                                   size_t *reply_size, void *user_data);
} client_callbacks_t;

/* Create a client (doesn't connect yet) */
mach_client_t* mach_client_create(const client_callbacks_t *callbacks, void *user_data);

/* Connect to a server (blocking, with timeout) */
ipc_status_t mach_client_connect(mach_client_t *client, const char *service_name,
                                 uint32_t timeout_ms);

/* Check if connected */
bool mach_client_is_connected(mach_client_t *client);

/* Send a message to server (non-blocking) */
ipc_status_t mach_client_send(mach_client_t *client, uint32_t msg_type,
                              const void *data, size_t size);

/* Send a message and wait for reply (blocking with timeout) */
ipc_status_t mach_client_send_with_reply(mach_client_t *client, uint32_t msg_type,
                                         const void *data, size_t size,
                                         void **reply_data, size_t *reply_size,
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

/* Allocate memory for reply data (use in callbacks) */
void* ipc_alloc(size_t size);

/* Free memory allocated by framework */
void ipc_free(void *ptr);

#endif /* MACH_IPC_H */