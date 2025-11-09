#include "linear_ts_pool.h"
#include <stdlib.h>
#include <string.h>

void linear_ts_pool_init(LinearTSPool *pool, int capacity, int sizeof_data) {
    pool->capacity = capacity;
    pool->sizeof_data = sizeof_data;
    pool->data = malloc(sizeof_data * capacity);
    pool->used = calloc(capacity, sizeof(bool));
    pool->locks = malloc(sizeof(pthread_mutex_t) * capacity);
    
    pthread_mutex_init(&pool->pool_lock, NULL);
    
    for (int i = 0; i < capacity; i++) {
        pthread_mutex_init(&pool->locks[i], NULL);
    }
}

bool linear_ts_pool_set(LinearTSPool *pool, int index, void *value) {
    if (index < 0 || index >= pool->capacity) {
        return false;
    }
    
    pthread_mutex_lock(&pool->pool_lock);
    
    if (value) {
        memcpy(
            (char*)pool->data + index * pool->sizeof_data,
            value,
            pool->sizeof_data
        );
    }
    pool->used[index] = true;
    
    pthread_mutex_unlock(&pool->pool_lock);
    return true;
}

void linear_ts_pool_remove(LinearTSPool *pool, int index) {
    if (index < 0 || index >= pool->capacity) return;
    
    pthread_mutex_lock(&pool->pool_lock);
    pool->used[index] = false;
    pthread_mutex_unlock(&pool->pool_lock);
}

void* linear_ts_pool_get(LinearTSPool *pool, int index) {
    if (index < 0 || index >= pool->capacity) {
        return NULL;
    }
    
    pthread_mutex_lock(&pool->pool_lock);
    bool active = pool->used[index];
    pthread_mutex_unlock(&pool->pool_lock);
    
    if (!active) {
        return NULL;
    }
    
    return (char*)pool->data + index * pool->sizeof_data;
}

bool linear_ts_pool_is_active(LinearTSPool *pool, int index) {
    if (index < 0 || index >= pool->capacity) {
        return false;
    }
    
    pthread_mutex_lock(&pool->pool_lock);
    bool active = pool->used[index];
    pthread_mutex_unlock(&pool->pool_lock);
    
    return active;
}

int linear_ts_pool_find_free(LinearTSPool *pool) {
    pthread_mutex_lock(&pool->pool_lock);
    
    int result = -1;
    for (int i = 0; i < pool->capacity; i++) {
        if (!pool->used[i]) {
            result = i;
            break;
        }
    }
    
    pthread_mutex_unlock(&pool->pool_lock);
    return result;
}

bool linear_ts_pool_lock_entry(LinearTSPool *pool, int index) {
    if (index < 0 || index >= pool->capacity) {
        return false;
    }
    
    // Check if entry is active before locking
    pthread_mutex_lock(&pool->pool_lock);
    bool active = pool->used[index];
    pthread_mutex_unlock(&pool->pool_lock);
    
    if (!active) {
        return false;
    }
    
    pthread_mutex_lock(&pool->locks[index]);
    
    // Double-check after acquiring lock
    pthread_mutex_lock(&pool->pool_lock);
    active = pool->used[index];
    pthread_mutex_unlock(&pool->pool_lock);
    
    if (!active) {
        pthread_mutex_unlock(&pool->locks[index]);
        return false;
    }
    
    return true;
}

void linear_ts_pool_unlock_entry(LinearTSPool *pool, int index) {
    if (index < 0 || index >= pool->capacity) {
        return;
    }
    pthread_mutex_unlock(&pool->locks[index]);
}

bool linear_ts_pool_trylock_entry(LinearTSPool *pool, int index) {
    if (index < 0 || index >= pool->capacity) {
        return false;
    }
    
    pthread_mutex_lock(&pool->pool_lock);
    bool active = pool->used[index];
    pthread_mutex_unlock(&pool->pool_lock);
    
    if (!active) {
        return false;
    }
    
    if (pthread_mutex_trylock(&pool->locks[index]) != 0) {
        return false;
    }
    
    // Double-check after acquiring lock
    pthread_mutex_lock(&pool->pool_lock);
    active = pool->used[index];
    pthread_mutex_unlock(&pool->pool_lock);
    
    if (!active) {
        pthread_mutex_unlock(&pool->locks[index]);
        return false;
    }
    
    return true;
}

void linear_ts_pool_free(LinearTSPool *pool) {
    for (int i = 0; i < pool->capacity; i++) {
        pthread_mutex_destroy(&pool->locks[i]);
    }
    free(pool->locks);
    free(pool->data);
    free(pool->used);
    pthread_mutex_destroy(&pool->pool_lock);
}