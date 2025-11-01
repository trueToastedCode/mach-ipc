/* ============================================================================
 * Protocol Implementation - Low-level Mach IPC
 * Migrated and cleaned up from your POC base.c
 * ============================================================================ */

#include "internal.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>

/* ============================================================================
 * PAYLOAD MANAGEMENT
 * ============================================================================ */

// internal_payload_t* create_payload(size_t data_size) {
//     size_t total_size = sizeof(internal_payload_t) + data_size;
//     internal_payload_t *payload = calloc(1, total_size);
//     if (!payload) {
//         LOG_ERROR_MSG("Failed to allocate payload of size %zu", total_size);
//         return NULL;
//     }
//     payload->data_size = data_size;
//     return payload;
// }

// void free_payload(internal_payload_t *payload) {
//     if (payload) {
//         free(payload);
//     }
// }

// void copy_to_payload(internal_payload_t *payload, const void *data, size_t size) {
//     if (payload && data && size > 0 && size <= payload->data_size) {
//         memcpy(payload->data, data, size);
//     }
// }

/* ============================================================================
 * LOW-LEVEL MESSAGE SENDING
 * ============================================================================ */

kern_return_t protocol_send_message(
    mach_port_t dest_port,
    mach_port_t reply_port,
    mach_msg_id_t msg_id,
    internal_payload_t *payload,
    size_t payload_size,
    const void *user_payload,
    size_t user_payload_size
) {
    if (!payload || payload_size < sizeof(internal_payload_t)) {
        LOG_ERROR_MSG("Invalid payload");
        return KERN_INVALID_ARGUMENT;
    }
    
    internal_mach_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    
    // Set up header
    msg.header.msgh_bits = reply_port
        ? (MACH_MSGH_BITS_COMPLEX | 
           MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MOVE_SEND))
        : (MACH_MSGH_BITS_COMPLEX | 
           MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0));
    msg.header.msgh_size = sizeof(msg);
    msg.header.msgh_remote_port = dest_port;
    msg.header.msgh_local_port = reply_port;
    msg.header.msgh_id = msg_id;
    
    // Set up body and OOL descriptor
    msg.body.msgh_descriptor_count = 2;

    msg.payload.address = payload;
    msg.payload.size = payload_size;
    msg.payload.copy = MACH_MSG_VIRTUAL_COPY;
    msg.payload.deallocate = false;
    msg.payload.type = MACH_MSG_OOL_DESCRIPTOR;

    msg.user_payload.address = user_payload;
    msg.user_payload.size = user_payload_size;
    msg.user_payload.copy = MACH_MSG_VIRTUAL_COPY;
    msg.user_payload.deallocate = false;
    msg.user_payload.type = MACH_MSG_OOL_DESCRIPTOR;
    
    LOG_DEBUG_MSG("Sending message: id=0x%x, size=%zu", msg_id, payload_size);
    
    kern_return_t kr = mach_msg(
        &msg.header,
        MACH_SEND_MSG,
        msg.header.msgh_size,
        0,
        MACH_PORT_NULL,
        100,  // 100ms timeout
        MACH_PORT_NULL
    );
    
    if (kr != KERN_SUCCESS) {
        LOG_ERROR_MSG("mach_msg send failed: 0x%x (%s)", kr, mach_error_string(kr));
    } else {
        LOG_DEBUG_MSG("Message sent successfully");
    }
    
    return kr;
}

/* ============================================================================
 * ACKNOWLEDGMENT HANDLING
 * ============================================================================ */

static bool register_ack_waiter(
    Pool *ack_pool,
    pthread_mutex_t *ack_lock,
    uint64_t correlation_id,
    ack_waiter_t **waiter_out
) {
    if (correlation_id == 0) {
        LOG_ERROR_MSG("Cannot register ack with correlation_id=0");
        return false;
    }
    
    pthread_mutex_lock(ack_lock);
    
    int slot = pool_push(ack_pool, NULL);
    if (slot == -1) {
        pthread_mutex_unlock(ack_lock);
        LOG_ERROR_MSG("Ack pool is full");
        return false;
    }
    
    ack_waiter_t *waiter = (ack_waiter_t*)pool_get(ack_pool, slot);
    if (!waiter) {
        pool_pop(ack_pool, slot);
        pthread_mutex_unlock(ack_lock);
        LOG_ERROR_MSG("Failed to get ack waiter from pool");
        return false;
    }
    
    event_t *evt = event_create();
    if (!evt) {
        pool_pop(ack_pool, slot);
        pthread_mutex_unlock(ack_lock);
        LOG_ERROR_MSG("Failed to create ack event");
        return false;
    }
    
    *waiter = (ack_waiter_t){
        .correlation_id = correlation_id,
        .event = evt,
        .reply_payload = NULL,
        .reply_size = 0,
        .reply_user_payload = NULL,
        .reply_user_size = 0,
        .received = false,
        .cancelled = false
    };
    
    *waiter_out = waiter;
    
    pthread_mutex_unlock(ack_lock);
    return true;
}

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
    uint32_t timeout_ms
) {
    // Assign correlation ID
    pthread_mutex_lock(ack_lock);
    uint64_t correlation_id = (*next_correlation_id)++;
    pthread_mutex_unlock(ack_lock);
    
    payload->correlation_id = correlation_id;
    
    // Register ack waiter
    ack_waiter_t *waiter = NULL;
    if (!register_ack_waiter(ack_pool, ack_lock, correlation_id, &waiter)) {
        return KERN_FAILURE;
    }
    
    // Send message with WACK feature
    mach_msg_id_t ack_msg_id = SET_FEATURE(msg_id, INTERNAL_FEATURE_WACK);
    kern_return_t kr = protocol_send_message(dest_port, reply_port, 
                                             ack_msg_id, payload, payload_size,
                                             user_payload, user_payload_size);
    
    if (kr != KERN_SUCCESS) {
        // Cleanup waiter
        pthread_mutex_lock(ack_lock);
        event_destroy(waiter->event);
        int slot = ((char*)waiter - (char*)ack_pool->data) / ack_pool->sizeof_data;
        pool_pop(ack_pool, slot);
        pthread_mutex_unlock(ack_lock);
        return kr;
    }
    
    LOG_DEBUG_MSG("Waiting for ack (correlation_id=%llu, timeout=%ums)", 
                  correlation_id, timeout_ms);
    
    // Wait for reply (released lock during wait)
    bool got_reply = event_wait_timeout(waiter->event, timeout_ms);
    
    // CRITICAL SECTION: Mark as cancelled BEFORE checking if we got reply
    // This prevents the race where ack arrives after timeout but before cleanup
    pthread_mutex_lock(ack_lock);
    
    kern_return_t result;
    
    if (got_reply && waiter->received && !waiter->cancelled) {
        // Success: got reply before timeout
        LOG_INFO_MSG("Ack received (correlation_id=%llu)", correlation_id);
        *ack_payload = waiter->reply_payload;
        *ack_size = waiter->reply_size;
        *ack_user_payload = waiter->reply_user_payload;
        *ack_user_size = waiter->reply_user_size;
        result = KERN_SUCCESS;
    } else {
        // Timeout or cancelled
        LOG_ERROR_MSG("Ack timeout (correlation_id=%llu)", correlation_id);
        
        // Mark as cancelled so late arrivals are ignored
        waiter->cancelled = true;
        
        // If reply arrived during our lock acquisition, we need to clean it up
        if (waiter->received && waiter->reply_payload) {
            LOG_WARN_MSG("Ack arrived during timeout handling, cleaning up (correlation_id=%llu)", 
                        correlation_id);
            vm_deallocate(mach_task_self(), 
                          (vm_address_t)waiter->reply_payload, 
                          waiter->reply_size);
            if (waiter->reply_user_payload && waiter->reply_user_size) {
                vm_deallocate(mach_task_self(), 
                              (vm_address_t)waiter->reply_user_payload, 
                              waiter->reply_user_size);
            }
        }
        
        *ack_payload = NULL;
        *ack_size = 0;
        *ack_user_payload = NULL;
        *ack_user_size = 0;
        result = KERN_OPERATION_TIMED_OUT;
    }
    
    // Cleanup waiter
    event_destroy(waiter->event);
    int slot = ((char*)waiter - (char*)ack_pool->data) / ack_pool->sizeof_data;
    pool_pop(ack_pool, slot);
    
    pthread_mutex_unlock(ack_lock);
    
    return result;
}

kern_return_t protocol_send_ack(
    mach_port_t dest_port,
    mach_msg_id_t original_msg_id,
    uint64_t correlation_id,
    internal_payload_t *ack_payload,
    size_t ack_payload_size,
    const void *ack_user_payload,
    size_t ack_user_payload_size
) {
    if (correlation_id == 0) {
        LOG_ERROR_MSG("Cannot send ack with correlation_id=0");
        return KERN_INVALID_ARGUMENT;
    }
    
    ack_payload->correlation_id = correlation_id;
    
    // Set IACK feature, remove WACK if present
    mach_msg_id_t ack_msg_id = original_msg_id;
    ack_msg_id = UNSET_FEATURE(ack_msg_id, INTERNAL_FEATURE_WACK);
    ack_msg_id = SET_FEATURE(ack_msg_id, INTERNAL_FEATURE_IACK);
    
    return protocol_send_message(dest_port, MACH_PORT_NULL, 
                                 ack_msg_id, ack_payload, ack_payload_size,
                                 ack_user_payload, ack_user_payload_size);
}

/* ============================================================================
 * MESSAGE RECEIVING
 * ============================================================================ */

static bool handle_ack_message(
    Pool *ack_pool,
    pthread_mutex_t *ack_lock,
    internal_payload_t *payload,
    size_t payload_size,
    const void *user_payload,
    size_t user_payload_size
) {
    if (payload->correlation_id == 0) {
        LOG_ERROR_MSG("Received ack with correlation_id=0");
        return false;
    }
    
    pthread_mutex_lock(ack_lock);
    
    // Find matching waiter
    ack_waiter_t *waiter = NULL;
    int slot = -1;
    
    for (int i = 0; i < ack_pool->capacity; i++) {
        if (!pool_is_active(ack_pool, i)) continue;
        
        ack_waiter_t *w = (ack_waiter_t*)pool_get(ack_pool, i);
        if (w && w->correlation_id == payload->correlation_id) {
            waiter = w;
            slot = i;
            break;
        }
    }
    
    if (!waiter) {
        pthread_mutex_unlock(ack_lock);
        LOG_WARN_MSG("Ack for unknown correlation_id=%llu (already cleaned up?)", 
                     payload->correlation_id);
        return false;
    }
    
    // CRITICAL: Check if waiter was cancelled (timed out)
    if (waiter->cancelled) {
        pthread_mutex_unlock(ack_lock);
        LOG_WARN_MSG("Ack arrived after timeout (correlation_id=%llu), discarding", 
                     payload->correlation_id);
        // Caller will deallocate the payload
        return false;
    }
    
    // Store reply - waiter is still valid
    waiter->reply_payload = payload;
    waiter->reply_size = payload_size;
    waiter->reply_user_payload = user_payload;
    waiter->reply_user_size = user_payload_size;
    waiter->received = true;
    
    // Signal waiter thread
    event_signal(waiter->event);
    
    pthread_mutex_unlock(ack_lock);
    
    LOG_DEBUG_MSG("Matched ack to waiter (correlation_id=%llu)", 
                  payload->correlation_id);
    
    // Return true to indicate we took ownership of the payload
    return true;
}

void protocol_receive_loop(
    mach_port_t service_port,
    volatile sig_atomic_t *running,
    Pool *ack_pool,
    pthread_mutex_t *ack_lock,
    message_handler_t handler,
    void *context
) {
    char rcv_buffer[INTERNAL_RCV_BUFFER_SIZE];
    mach_msg_header_t *header = (mach_msg_header_t*)rcv_buffer;
    internal_mach_msg_t *intrl_mach_msg = (internal_mach_msg_t*)rcv_buffer;
    
    LOG_INFO_MSG("Starting receive loop on port %u", service_port);
    
    while (*running) {
        kern_return_t kr = mach_msg(
            header,
            MACH_RCV_MSG | MACH_RCV_TIMEOUT,
            0,
            INTERNAL_RCV_BUFFER_SIZE,
            service_port,
            1000,  // 1 second timeout for responsiveness
            MACH_PORT_NULL
        );
        
        if (kr == MACH_RCV_TIMED_OUT) {
            continue;
        }
        
        if (kr != KERN_SUCCESS) {
            LOG_ERROR_MSG("mach_msg receive failed: 0x%x (%s)", 
                         kr, mach_error_string(kr));
            continue;
        }
        
        // Check if it's our protocol message
        if (!IS_THIS_PROTOCOL_MSG(header->msgh_id)) {
            // Pass to handler (might be death notification, etc.)
            handler(service_port, header, NULL, 0, NULL, 0, context);
            continue;
        }
        
        // Validate message structure
        if (intrl_mach_msg->body.msgh_descriptor_count < 2) {
            LOG_ERROR_MSG("Invalid descriptor count");
            continue;
        }
        
        if (intrl_mach_msg->payload.type != MACH_MSG_OOL_DESCRIPTOR) {
            LOG_ERROR_MSG("Invalid payload type");
            continue;
        }
        
        // Extract payload
        internal_payload_t *payload = (internal_payload_t*)intrl_mach_msg->payload.address;
        size_t payload_size = intrl_mach_msg->payload.size;
        const void *user_payload = (internal_payload_t*)intrl_mach_msg->user_payload.address;
        size_t user_payload_size = intrl_mach_msg->user_payload.size;
        
        if (!payload || payload_size < sizeof(internal_payload_t)) {
            LOG_ERROR_MSG("Invalid payload data");
            continue;
        }
        
        LOG_DEBUG_MSG("Received message: id=0x%x, size=%zu, user_size=%zu, correlation=%llu",
                      header->msgh_id, payload_size, user_payload_size, payload->correlation_id);
        
        // Handle acknowledgments
        if (HAS_FEATURE_IACK(header->msgh_id)) {
            if (handle_ack_message(ack_pool, ack_lock, payload, payload_size, user_payload, user_payload_size)) {
                // Ack was matched and accepted, don't deallocate
                // The waiter owns it now
                continue;
            }
            // Ack was rejected (timeout/unknown), fall through to deallocate
        } else {
            // Regular message - pass to handler
            if (!handler(service_port, header, payload, payload_size, user_payload, user_payload_size, context)) {
                // handler signaled it will handle the payload cleanup
                continue;
            }
        }
        
        // Cleanup OOL memory
        vm_deallocate(mach_task_self(), (vm_address_t)payload, payload_size);
        if (user_payload && user_payload_size) {
            vm_deallocate(mach_task_self(), (vm_address_t)user_payload, user_payload_size);
        }
    }
    
    LOG_INFO_MSG("Receive loop stopped");
}

/* ============================================================================
 * HELPER FUNCTIONS
 * ============================================================================ */

client_info_t* find_client_by_id_locked(mach_server_t *server, uint32_t client_id) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_info_t *client = server->clients[i];
        if (client && client->id == client_id && client->active) {
            return client;
        }
    }
    return NULL;
}

client_info_t* find_client_by_port_locked(mach_server_t *server, mach_port_t port) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_info_t *client = server->clients[i];
        if (client && client->port == port && client->active) {
            return client;
        }
    }
    return NULL;
}