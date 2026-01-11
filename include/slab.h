#ifndef SLAB_H
#define SLAB_H

#include <stddef.h>

typedef struct _LIST_ENTRY {
    struct _LIST_ENTRY* flink;
    struct _LIST_ENTRY* blink;
} LIST_ENTRY;

typedef struct {
    volatile int locked;
} SPIN_LOCK;

typedef struct _CACHE CACHE;
typedef struct _SLAB SLAB;
typedef struct _BUFCTRL BUFCTRL;

struct _SLAB {
    CACHE* cache;
    LIST_ENTRY listEntry;
    size_t objectCount;
    size_t usedObjects;
    size_t bufferSize;
    union {
        void* freeList;
        LIST_ENTRY bufferControlFreeListHead;
    } u;
};

struct _BUFCTRL {
    void* buffer;
    SLAB* parent;
    LIST_ENTRY entry;
};

struct _CACHE {
    size_t size;
    int align;
    int flags;
    SPIN_LOCK lock;
    LIST_ENTRY fullSlabListHead;
    LIST_ENTRY partialSlabListHead;
    LIST_ENTRY emptySlabListHead;
    LIST_ENTRY listEntry;
};

#define CACHE_FLAG_BUFCTL 0x01
#define PAGE_SIZE 4096

void slab_init(void);
CACHE* cache_create(size_t size, int align, int flags);
void* cache_alloc(CACHE* cache);
void cache_free(CACHE* cache, void* obj);
void spin_lock(SPIN_LOCK* lock);
void spin_unlock(SPIN_LOCK* lock);
#endif