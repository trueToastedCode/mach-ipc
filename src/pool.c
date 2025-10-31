#include "pool.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void pool_init(Pool *pool, int capacity, int sizeof_data) {
    pool->capacity = capacity;
    pool->sizeof_data = sizeof_data;
    pool->data = malloc(sizeof_data * capacity);
    pool->next = malloc(sizeof(int) * capacity);
    pool->used = calloc(capacity, sizeof(bool));

    // link all indices into a free list
    for (int i = 0; i < capacity - 1; i++) {
        pool->next[i] = i + 1;
    }
    pool->next[capacity - 1] = -1; // end of list
    pool->free_head = 0;
}

int pool_push(Pool *pool, void *value) {
    if (pool->free_head == -1) {
        return -1; // full
    }
    int index = pool->free_head;
    pool->free_head = pool->next[index]; // move head forward

    if (value) {
        memcpy(
            (char*)pool->data + index * pool->sizeof_data,
            value,
            pool->sizeof_data
        );
    }
    pool->used[index] = true;
    return index;
}

void pool_pop(Pool *pool, int index) {
    if (index < 0 || index >= pool->capacity || !pool->used[index]) return;

    pool->used[index] = false;
    pool->next[index] = pool->free_head; // link back into free list
    pool->free_head = index;
}

void* pool_get(Pool *pool, int index) {
    if (index < 0 || index >= pool->capacity || !pool->used[index]) {
        // invalid index or not active
        return NULL;
    }
    return (char*)pool->data + index * pool->sizeof_data;
}

bool pool_is_active(Pool *pool, int index) {
    return index >= 0 && index < pool->capacity && pool->used[index];
}

bool pool_has_capacity(Pool *pool) {
    return pool->free_head != -1;
}

void pool_free(Pool *pool) {
    free(pool->data);
    free(pool->next);
    free(pool->used);
}