#ifndef LINEAR_POOL_H
#define LINEAR_POOL_H

#include <stdbool.h>
#include <pthread.h>

typedef struct {
    void *data;              // actual values
    pthread_mutex_t *locks;  // per-entry locks
    bool *used;              // whether a slot is in use
    int capacity;            // total capacity
    int sizeof_data;         // size of each element
    pthread_mutex_t pool_lock; // for structural changes (set/remove)
} LinearTSPool;

// Initialize pool with given capacity and element size
void linear_ts_pool_init(LinearTSPool *pool, int capacity, int sizeof_data);

// Set a value at a specific index, return true if successful
bool linear_ts_pool_set(LinearTSPool *pool, int index, void *value);

// Remove (free) a value at a specific index
void linear_ts_pool_remove(LinearTSPool *pool, int index);

// Get a pointer to value by index (returns NULL if invalid)
// Note: You must call linear_pool_lock_entry BEFORE using the returned pointer
void* linear_ts_pool_get(LinearTSPool *pool, int index);

// Check whether an index is in use
bool linear_ts_pool_is_active(LinearTSPool *pool, int index);

// Find first free slot, return index or -1 if full
int linear_ts_pool_find_free(LinearTSPool *pool);

// Lock an entry for exclusive access (blocks other threads from this entry only)
// Returns true if locked successfully, false if entry is not active
bool linear_ts_pool_lock_entry(LinearTSPool *pool, int index);

// Unlock an entry
void linear_ts_pool_unlock_entry(LinearTSPool *pool, int index);

// Try to lock an entry (non-blocking)
bool linear_ts_pool_trylock_entry(LinearTSPool *pool, int index);

// Cleanup pool memory
void linear_ts_pool_free(LinearTSPool *pool);

#endif // LINEAR_POOL_H