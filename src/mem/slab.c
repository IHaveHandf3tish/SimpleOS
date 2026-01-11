// since we're building an Hypervisor with a Desktop interface, we need slab allocator for small objects

#include <stddef.h>
#include <stdint.h>
#include <limine.h>
#include <kprintf.h>
#include <string.h>
//data structures and function prototypes
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

extern void* pmm_alloc_pages(size_t pages);
extern void pmm_free_pages(void* phys, size_t pages);

static LIST_ENTRY cacheListHead = {&cacheListHead, &cacheListHead};
static SPIN_LOCK globalLock = {0};
//helper functions for list manipulation and locking
static void init_list_head(LIST_ENTRY* head) {
    head->flink = head;
    head->blink = head;
}

static void insert_tail_list(LIST_ENTRY* head, LIST_ENTRY* entry) {
    entry->flink = head;
    entry->blink = head->blink;
    head->blink->flink = entry;
    head->blink = entry;
}

static void remove_entry_list(LIST_ENTRY* entry) {
    entry->blink->flink = entry->flink;
    entry->flink->blink = entry->blink;
    entry->flink = entry;
    entry->blink = entry;
}

static int is_list_empty(LIST_ENTRY* head) {
    return head->flink == head;
}

void spin_lock(SPIN_LOCK* lock) {
    while (__sync_lock_test_and_set(&lock->locked, 1)) {
        while (lock->locked) __asm__ volatile("pause");
    }
}

void spin_unlock(SPIN_LOCK* lock) {
    __sync_lock_release(&lock->locked);
}

void slab_init(void) {
    init_list_head(&cacheListHead);
}

static SLAB* create_slab(CACHE* cache) {
    size_t slab_size = PAGE_SIZE;
    size_t total_size = sizeof(SLAB);
    size_t objects_per_slab;
    
    if (cache->flags & CACHE_FLAG_BUFCTL) {
        objects_per_slab = (slab_size - sizeof(SLAB)) / (cache->size + sizeof(BUFCTRL));
        total_size = slab_size;
    } else {
        objects_per_slab = (slab_size - sizeof(SLAB)) / (cache->size + sizeof(void*));
        total_size = slab_size;
    }
    
    if (objects_per_slab == 0) objects_per_slab = 1;
    
    SLAB* slab = (SLAB*)pmm_alloc_pages(1);
    if (!slab) return NULL;
    
    slab->cache = cache;
    init_list_head(&slab->listEntry);
    slab->objectCount = objects_per_slab;
    slab->usedObjects = 0;
    slab->bufferSize = cache->size;
    
    uintptr_t buffer_start = (uintptr_t)slab + sizeof(SLAB);
    
    if (cache->flags & CACHE_FLAG_BUFCTL) {
        init_list_head(&slab->u.bufferControlFreeListHead);
        BUFCTRL* bufctl_array = (BUFCTRL*)(buffer_start + (cache->size * objects_per_slab));
        
        for (size_t i = 0; i < objects_per_slab; i++) {
            BUFCTRL* bufctl = &bufctl_array[i];
            bufctl->buffer = (void*)(buffer_start + (i * cache->size));
            bufctl->parent = slab;
            insert_tail_list(&slab->u.bufferControlFreeListHead, &bufctl->entry);
        }
    } else {
        slab->u.freeList = NULL;
        void** prev = &slab->u.freeList;
        
        for (size_t i = 0; i < objects_per_slab; i++) {
            void* obj = (void*)(buffer_start + (i * cache->size));
            *prev = obj;
            prev = (void**)obj;
        }
        *prev = NULL;
    }
    
    return slab;
}

CACHE* cache_create(size_t size, int align, int flags) {
    CACHE* cache = (CACHE*)pmm_alloc_pages(1);
    if (!cache) return NULL;
    
    cache->size = size;
    cache->align = align;
    cache->flags = flags;
    cache->lock.locked = 0;
    
    init_list_head(&cache->fullSlabListHead);
    init_list_head(&cache->partialSlabListHead);
    init_list_head(&cache->emptySlabListHead);
    init_list_head(&cache->listEntry);
    
    spin_lock(&globalLock);
    insert_tail_list(&cacheListHead, &cache->listEntry);
    spin_unlock(&globalLock);
    
    return cache;
}

void* cache_alloc(CACHE* cache) {
    spin_lock(&cache->lock);
    
    SLAB* slab = NULL;
    
    if (!is_list_empty(&cache->partialSlabListHead)) {
        slab = (SLAB*)cache->partialSlabListHead.flink;
    } else if (!is_list_empty(&cache->emptySlabListHead)) {
        slab = (SLAB*)cache->emptySlabListHead.flink;
        remove_entry_list(&slab->listEntry);
        insert_tail_list(&cache->partialSlabListHead, &slab->listEntry);
    } else {
        slab = create_slab(cache);
        if (!slab) {
            spin_unlock(&cache->lock);
            return NULL;
        }
        insert_tail_list(&cache->partialSlabListHead, &slab->listEntry);
    }
    
    void* obj = NULL;
    
    if (cache->flags & CACHE_FLAG_BUFCTL) {
        if (!is_list_empty(&slab->u.bufferControlFreeListHead)) {
            BUFCTRL* bufctl = (BUFCTRL*)slab->u.bufferControlFreeListHead.flink;
            remove_entry_list(&bufctl->entry);
            obj = bufctl->buffer;
        }
    } else {
        if (slab->u.freeList) {
            obj = slab->u.freeList;
            slab->u.freeList = *(void**)obj;
        }
    }
    
    if (obj) {
        slab->usedObjects++;
        
        if (slab->usedObjects == slab->objectCount) {
            remove_entry_list(&slab->listEntry);
            insert_tail_list(&cache->fullSlabListHead, &slab->listEntry);
        }
    }
    
    spin_unlock(&cache->lock);
    return obj;
}

void cache_free(CACHE* cache, void* obj) {
    if (!obj) return;
    
    spin_lock(&cache->lock);
    
    uintptr_t obj_addr = (uintptr_t)obj;
    SLAB* slab = (SLAB*)(obj_addr & ~(PAGE_SIZE - 1));

    if (slab->cache != cache) {
         kprintf("PANIC: Slab corruption or wrong cache!\n");
        while(1); // Halt
    }
    
    int was_full = (slab->usedObjects == slab->objectCount);
    
    if (cache->flags & CACHE_FLAG_BUFCTL) {
        uintptr_t buffer_start = (uintptr_t)slab + sizeof(SLAB);
        size_t index = (obj_addr - buffer_start) / cache->size;
        BUFCTRL* bufctl_array = (BUFCTRL*)(buffer_start + (cache->size * slab->objectCount));
        BUFCTRL* bufctl = &bufctl_array[index];
        insert_tail_list(&slab->u.bufferControlFreeListHead, &bufctl->entry);
    } else {
        *(void**)obj = slab->u.freeList;
        slab->u.freeList = obj;
    }
    
    slab->usedObjects--;
    
    if (was_full) {
        remove_entry_list(&slab->listEntry);
        insert_tail_list(&cache->partialSlabListHead, &slab->listEntry);
    } else if (slab->usedObjects == 0) {
        remove_entry_list(&slab->listEntry);
        insert_tail_list(&cache->emptySlabListHead, &slab->listEntry);
    }
    
    spin_unlock(&cache->lock);
}