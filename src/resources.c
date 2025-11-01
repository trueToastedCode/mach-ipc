/* ============================================================================
 * Resource Tracker - Automatic cleanup system
 * 
 * Ensures all resources are properly freed even on error paths.
 * Resources are freed in reverse order of registration (stack-like).
 * ============================================================================ */

#include "internal.h"
#include "log.h"
#include <stdlib.h>
#include <pthread.h>

#define MAX_RESOURCES 256

typedef struct {
    void *resource;
    resource_type_t type;
    void (*custom_cleanup)(void*);
    const char *debug_name;
    bool active;
} tracked_resource_t;

struct resource_tracker {
    tracked_resource_t resources[MAX_RESOURCES];
    int count;
    pthread_mutex_t lock;
};

/* ============================================================================
 * Cleanup Functions
 * ============================================================================ */

static void cleanup_port(void *res) {
    mach_port_t *port = (mach_port_t*)res;
    if (*port != MACH_PORT_NULL) {
        // Check what rights we have on this port
        mach_port_type_t type;
        kern_return_t kr = mach_port_type(mach_task_self(), *port, &type);
        
        if (kr == KERN_SUCCESS && (type & MACH_PORT_TYPE_RECEIVE)) {
            // We have receive right - use mach_port_destruct
            mach_port_delta_t srd = 0; // sync value (not used for ports without guard)
            kr = mach_port_destruct(mach_task_self(), *port, srd, 0);
            if (kr != KERN_SUCCESS && kr != KERN_INVALID_RIGHT) {
                LOG_ERROR_MSG("Port destruct failed: %s", mach_error_string(kr));
            } else if (kr == KERN_SUCCESS) {
                LOG_DEBUG_MSG("Destructed port %u", *port);
            }
        } else {
            // Only have send right (or already cleaned up) - deallocate
            kr = mach_port_deallocate(mach_task_self(), *port);
            if (kr != KERN_SUCCESS && kr != KERN_INVALID_RIGHT) {
                LOG_ERROR_MSG("Port deallocation failed: %s", mach_error_string(kr));
            } else if (kr == KERN_SUCCESS) {
                LOG_DEBUG_MSG("Deallocated port %u", *port);
            }
        }
        // KERN_INVALID_RIGHT means already cleaned up - this is fine during shutdown
        *port = MACH_PORT_NULL;
    }
}

static void cleanup_memory(void *res) {
    void **ptr = (void**)res;
    if (*ptr) {
        LOG_DEBUG_MSG("Freeing memory at %p", *ptr);
        free(*ptr);
        *ptr = NULL;
    }
}

static void cleanup_queue(void *res) {
    dispatch_queue_t *queue = (dispatch_queue_t*)res;
    if (*queue) {
        LOG_DEBUG_MSG("Releasing dispatch queue");
        // Drain queue before releasing
        dispatch_sync(*queue, ^{});
        dispatch_release(*queue);
        *queue = NULL;
    }
}

static void cleanup_thread(void *res) {
    pthread_t *thread = (pthread_t*)res;
    if (*thread) {
        LOG_DEBUG_MSG("Joining thread");
        pthread_join(*thread, NULL);
        *thread = 0;
    }
}

static void cleanup_mutex(void *res) {
    pthread_mutex_t *mutex = (pthread_mutex_t*)res;
    LOG_DEBUG_MSG("Destroying mutex");
    pthread_mutex_destroy(mutex);
}

static void cleanup_pool(void *res) {
    Pool *pool = (Pool*)res;
    LOG_DEBUG_MSG("Freeing pool");
    pool_free(pool);
}

/* ============================================================================
 * Public API
 * ============================================================================ */

resource_tracker_t* resource_tracker_create(void) {
    resource_tracker_t *tracker = calloc(1, sizeof(resource_tracker_t));
    if (!tracker) {
        LOG_ERROR_MSG("Failed to allocate resource tracker");
        return NULL;
    }
    
    pthread_mutex_init(&tracker->lock, NULL);
    tracker->count = 0;
    
    return tracker;
}

bool resource_tracker_add(resource_tracker_t *tracker, 
                         resource_type_t type,
                         void *resource,
                         void (*custom_cleanup)(void*),
                         const char *debug_name) {
    if (!tracker || !resource) {
        LOG_ERROR_MSG("Invalid parameters to resource_tracker_add");
        return false;
    }
    
    pthread_mutex_lock(&tracker->lock);
    
    if (tracker->count >= MAX_RESOURCES) {
        pthread_mutex_unlock(&tracker->lock);
        LOG_ERROR_MSG("Resource tracker is full");
        return false;
    }
    
    int idx = tracker->count++;
    tracker->resources[idx] = (tracked_resource_t){
        .resource = resource,
        .type = type,
        .custom_cleanup = custom_cleanup,
        .debug_name = debug_name,
        .active = true
    };
    
    pthread_mutex_unlock(&tracker->lock);
    
    LOG_DEBUG_MSG("Tracked resource %d: type=%d, name=%s", 
                  idx, type, debug_name ? debug_name : "unnamed");
    
    return true;
}

bool resource_tracker_remove(resource_tracker_t *tracker, void *resource) {
    if (!tracker || !resource) return false;
    
    pthread_mutex_lock(&tracker->lock);
    
    // Find and mark as inactive (don't shift array)
    for (int i = 0; i < tracker->count; i++) {
        if (tracker->resources[i].active && 
            tracker->resources[i].resource == resource) {
            tracker->resources[i].active = false;
            LOG_DEBUG_MSG("Untracked resource %d", i);
            pthread_mutex_unlock(&tracker->lock);
            return true;
        }
    }
    
    pthread_mutex_unlock(&tracker->lock);
    LOG_WARN_MSG("Resource %p not found in tracker", resource);
    return false;
}

void resource_tracker_cleanup_all(resource_tracker_t *tracker) {
    if (!tracker) return;
    
    LOG_INFO_MSG("Cleaning up %d tracked resources", tracker->count);
    
    // Cleanup in reverse order (like stack unwinding)
    for (int i = tracker->count - 1; i >= 0; i--) {
        tracked_resource_t *res = &tracker->resources[i];
        
        if (!res->active) continue;
        
        LOG_DEBUG_MSG("Cleaning up resource %d: type=%d, name=%s",
                     i, res->type, res->debug_name ? res->debug_name : "unnamed");
        
        switch (res->type) {
            case RES_TYPE_PORT:
                cleanup_port(res->resource);
                break;
            case RES_TYPE_MEMORY:
                cleanup_memory(res->resource);
                break;
            case RES_TYPE_QUEUE:
                cleanup_queue(res->resource);
                break;
            case RES_TYPE_THREAD:
                cleanup_thread(res->resource);
                break;
            case RES_TYPE_MUTEX:
                cleanup_mutex(res->resource);
                break;
            case RES_TYPE_POOL:
                cleanup_pool(res->resource);
                break;
            case RES_TYPE_CUSTOM:
                if (res->custom_cleanup) {
                    res->custom_cleanup(res->resource);
                }
                break;
        }
        
        res->active = false;
    }
    
    tracker->count = 0;
    LOG_INFO_MSG("Resource cleanup complete");
}

void resource_tracker_destroy(resource_tracker_t *tracker) {
    if (!tracker) return;
    
    // Cleanup any remaining resources
    resource_tracker_cleanup_all(tracker);
    
    pthread_mutex_destroy(&tracker->lock);
    free(tracker);
}

/* ============================================================================
 * Convenience Macros for Common Patterns
 * ============================================================================ */

#define TRACK_PORT(tracker, port) \
    resource_tracker_add(tracker, RES_TYPE_PORT, &(port), NULL, #port)

#define TRACK_MEMORY(tracker, ptr) \
    resource_tracker_add(tracker, RES_TYPE_MEMORY, &(ptr), NULL, #ptr)

#define TRACK_QUEUE(tracker, queue) \
    resource_tracker_add(tracker, RES_TYPE_QUEUE, &(queue), NULL, #queue)

#define TRACK_THREAD(tracker, thread) \
    resource_tracker_add(tracker, RES_TYPE_THREAD, &(thread), NULL, #thread)

#define TRACK_MUTEX(tracker, mutex) \
    resource_tracker_add(tracker, RES_TYPE_MUTEX, &(mutex), NULL, #mutex)

#define TRACK_POOL(tracker, pool) \
    resource_tracker_add(tracker, RES_TYPE_POOL, &(pool), NULL, #pool)

/* ============================================================================
 * Usage Example
 * ============================================================================ */

#ifdef EXAMPLE_USAGE
void example_function(void) {
    resource_tracker_t *tracker = resource_tracker_create();
    
    // Allocate resources
    mach_port_t port = MACH_PORT_NULL;
    mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
    TRACK_PORT(tracker, port);
    
    void *buffer = malloc(1024);
    TRACK_MEMORY(tracker, buffer);
    
    dispatch_queue_t queue = dispatch_queue_create("test", DISPATCH_QUEUE_SERIAL);
    TRACK_QUEUE(tracker, queue);
    
    pthread_mutex_t lock;
    pthread_mutex_init(&lock, NULL);
    TRACK_MUTEX(tracker, lock);
    
    Pool pool;
    pool_init(&pool, 10, sizeof(int));
    TRACK_POOL(tracker, pool);
    
    // Do work...
    // If error occurs anywhere, just call cleanup and return
    if (some_error) {
        resource_tracker_cleanup_all(tracker);
        resource_tracker_destroy(tracker);
        return;
    }
    
    // Normal cleanup
    resource_tracker_cleanup_all(tracker);
    resource_tracker_destroy(tracker);
}
#endif

/* ============================================================================
 * Debug Helpers
 * ============================================================================ */

#ifdef DEBUG
void resource_tracker_dump(resource_tracker_t *tracker) {
    if (!tracker) return;
    
    pthread_mutex_lock(&tracker->lock);
    
    printf("=== Resource Tracker Dump ===\n");
    printf("Total resources: %d\n", tracker->count);
    
    for (int i = 0; i < tracker->count; i++) {
        tracked_resource_t *res = &tracker->resources[i];
        if (res->active) {
            printf("  [%d] type=%d, ptr=%p, name=%s\n",
                   i, res->type, res->resource,
                   res->debug_name ? res->debug_name : "unnamed");
        }
    }
    
    pthread_mutex_unlock(&tracker->lock);
    printf("============================\n");
}
#endif