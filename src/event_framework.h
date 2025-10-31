#ifndef EVENT_FRAMEWORK_H
#define EVENT_FRAMEWORK_H

#include <dispatch/dispatch.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    dispatch_semaphore_t sem;
    bool triggered;
} event_t;

// Initialize an event
static inline event_t* event_create(void) {
    event_t* evt = malloc(sizeof(event_t));
    if (!evt) return NULL;
    
    evt->sem = dispatch_semaphore_create(0);
    evt->triggered = false;
    
    return evt;
}

// Wait for an event indefinitely
static inline bool event_wait(event_t* evt) {
    if (!evt) return false;
    dispatch_semaphore_wait(evt->sem, DISPATCH_TIME_FOREVER);
    return true;
}

// Wait for an event with timeout (in milliseconds)
// Returns true if signaled, false if timeout
static inline bool event_wait_timeout(event_t* evt, uint64_t timeout_ms) {
    if (!evt) return false;
    
    dispatch_time_t timeout = dispatch_time(DISPATCH_TIME_NOW, 
                                           timeout_ms * NSEC_PER_MSEC);
    long result = dispatch_semaphore_wait(evt->sem, timeout);
    
    return result == 0; // 0 = signaled, non-zero = timeout
}

// Trigger/signal the event
static inline void event_signal(event_t* evt) {
    if (!evt) return;
    
    if (!evt->triggered) {
        evt->triggered = true;
        dispatch_semaphore_signal(evt->sem);
    }
}

// Reset event to unsignaled state
static inline void event_reset(event_t* evt) {
    if (!evt) return;
    evt->triggered = false;
}

// Destroy event and free resources
static inline void event_destroy(event_t* evt) {
    if (!evt) return;
    
    // Release the semaphore
    dispatch_release(evt->sem);
    free(evt);
}

#endif // EVENT_FRAMEWORK_H

/* 
 * USAGE EXAMPLE:
 * 
 * #include "event_framework.h"
 * 
 * void worker_thread(void* arg) {
 *     event_t* evt = (event_t*)arg;
 *     
 *     // Do some work...
 *     sleep(2);
 *     
 *     // Signal completion
 *     event_signal(evt);
 * }
 * 
 * int main() {
 *     event_t* evt = event_create();
 *     
 *     // Start worker thread
 *     dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
 *         worker_thread(evt);
 *     });
 *     
 *     // Wait with timeout
 *     if (event_wait_timeout(evt, 3000)) {
 *         printf("Event completed!\n");
 *     } else {
 *         printf("Timeout!\n");
 *     }
 *     
 *     event_destroy(evt);
 *     return 0;
 * }
 */