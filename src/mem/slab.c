
// Slab allocator for small objects with constants and error handling

#include <stddef.h>
#include <stdint.h>
#include <limine.h>
#include <kprintf.h>
#include <string.h>
#include <mm_constants.h>

// Data structures and function prototypes
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

extern void* pmm_alloc_pages(size_t pages);
extern void pmm_free_pages(void* phys, size_t pages);

static LIST_ENTRY cacheListHead = {&cacheListHead, &cacheListHead};
static SPIN_LOCK globalLock = {0};

// Helper functions for list manipulation and locking
static void init_list_head(LIST_ENTRY* head) {
    if (!head) return;
    head->flink = head;
    head->blink = head;
}

static void insert_tail_list(LIST_ENTRY* head, LIST_ENTRY* entry) {
    if (!head || !entry) return;
    entry->flink = head;
    entry->blink = head->blink;
    head->blink->flink = entry;
    head->blink = entry;
}

static void remove_entry_list(LIST_ENTRY* entry) {
    if (!entry) return;
    entry->blink->flink = entry->flink;
    entry->flink->blink = entry->blink;
    entry->flink = entry;
    entry->blink = entry;
}

static int is_list_empty(LIST_ENTRY* head) {
    if (!head) return 1;
    return head->flink == head;
}

void spin_lock(SPIN_LOCK* lock) {
    if (!lock) return;
    while (__sync_lock_test_and_set(&lock->locked, 1)) {
        while (lock->locked) __asm__ volatile("pause");
    }
}

void spin_unlock(SPIN_LOCK* lock) {
    if (!lock) return;
    __sync_lock_release(&lock->locked);
}

void slab_init(void) {
    init_list_head(&cacheListHead);
    kprintf("Slab allocator initialized\n");
}

static SLAB* create_slab(CACHE* cache) {
    if (!cache) {
        kprintf("Slab Error: create_slab called with NULL cache\n");
        return NULL;
    }
    
    if (cache->size == 0) {
        kprintf("Slab Error: cache has zero size\n");
        return NULL;
    }
    
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
    
    if (objects_per_slab == 0) {
        kprintf("Slab Warning: Object size %d too large, forcing 1 object per slab\n", 
                cache->size);
        objects_per_slab = 1;
    }
    
    SLAB* slab = (SLAB*)pmm_alloc_pages(1);
    if (!slab) {
        kprintf("Slab Critical: Failed to allocate page for slab (object size %d)\n", 
                cache->size);
        return NULL;
    }
    
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
    if (size == 0) {
        kprintf("Slab Error: cache_create called with size=0\n");
        return NULL;
    }
    
    if (size > PAGE_SIZE - sizeof(SLAB)) {
        kprintf("Slab Error: Object size %d exceeds maximum (%d)\n", 
                size, PAGE_SIZE - sizeof(SLAB));
        return NULL;
    }
    
    CACHE* cache = (CACHE*)pmm_alloc_pages(1);
    if (!cache) {
        kprintf("Slab Critical: Failed to allocate page for cache (size %d)\n", size);
        return NULL;
    }
    
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
    if (!cache) {
        kprintf("Slab Error: cache_alloc called with NULL cache\n");
        return NULL;
    }
    
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
            kprintf("Slab Critical: Failed to create slab for allocation (size %d)\n", 
                    cache->size);
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
        } else {
            kprintf("Slab Error: BUFCTL free list empty but slab not full\n");
        }
    } else {
        if (slab->u.freeList) {
            obj = slab->u.freeList;
            slab->u.freeList = *(void**)obj;
        } else {
            kprintf("Slab Error: Free list empty but slab not full\n");
        }
    }
    
    if (obj) {
        slab->usedObjects++;
        
        if (slab->usedObjects == slab->objectCount) {
            remove_entry_list(&slab->listEntry);
            insert_tail_list(&cache->fullSlabListHead, &slab->listEntry);
        }
    } else {
        kprintf("Slab Critical: Failed to allocate object from slab\n");
    }
    
    spin_unlock(&cache->lock);
    return obj;
}

void cache_free(CACHE* cache, void* obj) {
    if (!obj) {
        kprintf("Slab Warning: cache_free called with NULL object\n");
        return;
    }
    
    if (!cache) {
        kprintf("Slab Error: cache_free called with NULL cache\n");
        return;
    }
    
    spin_lock(&cache->lock);
    
    uintptr_t obj_addr = (uintptr_t)obj;
    SLAB* slab = (SLAB*)PAGE_ALIGN_DOWN(obj_addr);

    if (slab->cache != cache) {
        kprintf("Slab PANIC: Slab corruption or wrong cache!\n");
        kprintf("  Object: 0x%lx, Slab: 0x%lx\n", obj_addr, (uintptr_t)slab);
        kprintf("  Slab cache: 0x%lx, Provided cache: 0x%lx\n", 
                (uintptr_t)slab->cache, (uintptr_t)cache);
        spin_unlock(&cache->lock);
        while(1) __asm__ volatile("hlt"); // Halt
    }
    
    if (slab->usedObjects == 0) {
        kprintf("Slab Error: Double free detected at 0x%lx\n", obj_addr);
        spin_unlock(&cache->lock);
        return;
    }
    
    int was_full = (slab->usedObjects == slab->objectCount);
    
    if (cache->flags & CACHE_FLAG_BUFCTL) {
        uintptr_t buffer_start = (uintptr_t)slab + sizeof(SLAB);
        size_t offset = obj_addr - buffer_start;
        
        if (offset % cache->size != 0) {
            kprintf("Slab Error: Unaligned free at 0x%lx (not on object boundary)\n", obj_addr);
            spin_unlock(&cache->lock);
            return;
        }
        
        size_t index = offset / cache->size;
        
        if (index >= slab->objectCount) {
            kprintf("Slab Error: Object index %d exceeds slab capacity %d\n", 
                    index, slab->objectCount);
            spin_unlock(&cache->lock);
            return;
        }
        
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

void cache_destroy(CACHE* cache) {
    if (!cache) {
        kprintf("Slab Warning: cache_destroy called with NULL cache\n");
        return;
    }
    
    spin_lock(&globalLock);
    spin_lock(&cache->lock);
    
    // Free all slabs
    while (!is_list_empty(&cache->fullSlabListHead)) {
        SLAB* slab = (SLAB*)cache->fullSlabListHead.flink;
        remove_entry_list(&slab->listEntry);
        pmm_free_pages(slab, 1);
    }
    
    while (!is_list_empty(&cache->partialSlabListHead)) {
        SLAB* slab = (SLAB*)cache->partialSlabListHead.flink;
        remove_entry_list(&slab->listEntry);
        pmm_free_pages(slab, 1);
    }
    
    while (!is_list_empty(&cache->emptySlabListHead)) {
        SLAB* slab = (SLAB*)cache->emptySlabListHead.flink;
        remove_entry_list(&slab->listEntry);
        pmm_free_pages(slab, 1);
    }
    
    // Remove from global cache list
    remove_entry_list(&cache->listEntry);
    
    spin_unlock(&cache->lock);
    spin_unlock(&globalLock);
    
    pmm_free_pages(cache, 1);
}

void slab_print_stats(void) {
    kprintf("\n=== Slab Allocator Statistics ===\n");
    
    spin_lock(&globalLock);
    
    int cache_count = 0;
    for (LIST_ENTRY* entry = cacheListHead.flink; 
         entry != &cacheListHead; 
         entry = entry->flink) {
        cache_count++;
        CACHE* cache = (CACHE*)entry;
        
        spin_lock(&cache->lock);
        
        int full = 0, partial = 0, empty = 0;
        size_t total_objs = 0, used_objs = 0;
        
        for (LIST_ENTRY* e = cache->fullSlabListHead.flink; 
             e != &cache->fullSlabListHead; e = e->flink) {
            full++;
            SLAB* s = (SLAB*)e;
            total_objs += s->objectCount;
            used_objs += s->usedObjects;
        }
        
        for (LIST_ENTRY* e = cache->partialSlabListHead.flink; 
             e != &cache->partialSlabListHead; e = e->flink) {
            partial++;
            SLAB* s = (SLAB*)e;
            total_objs += s->objectCount;
            used_objs += s->usedObjects;
        }
        
        for (LIST_ENTRY* e = cache->emptySlabListHead.flink; 
             e != &cache->emptySlabListHead; e = e->flink) {
            empty++;
            SLAB* s = (SLAB*)e;
            total_objs += s->objectCount;
        }
        
        kprintf("Cache (size=%4d): %d slabs (full=%d, partial=%d, empty=%d), "
                "%d/%d objects (%d%% used)\n",
                cache->size, full + partial + empty, full, partial, empty,
                used_objs, total_objs, 
                total_objs > 0 ? (used_objs * 100 / total_objs) : 0);
        
        spin_unlock(&cache->lock);
    }
    
    kprintf("Total caches: %d\n", cache_count);
    kprintf("=================================\n\n");
    
    spin_unlock(&globalLock);
}
