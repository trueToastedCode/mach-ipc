#ifndef POOL_H
#define POOL_H

#include <stdbool.h>

typedef struct {
    void *data;       // actual values
    int *next;        // next free index chain
    int capacity;     // total capacity
    int sizeof_data;  // size of each element
    int free_head;    // head of free list
    bool *used;       // whether a slot is in use
} Pool;

// Initialize pool with given capacity and element size
void pool_init(Pool *pool, int capacity, int sizeof_data);

// Push a value, return its index or -1 if full
int pool_push(Pool *pool, void *value);

// Pop (free) a value by index
void pool_pop(Pool *pool, int index);

// Get a pointer to value by index (returns NULL if invalid)
void* pool_get(Pool *pool, int index);

// Check whether an index is in use
bool pool_is_active(Pool *pool, int index);

// Check whether a pool has capacity left
bool pool_has_capacity(Pool *pool);

// Cleanup pool memory
void pool_free(Pool *pool);

#endif // POOL_H