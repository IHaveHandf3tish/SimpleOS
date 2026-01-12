
#include <stdint.h>
#include <stddef.h>
#include <limine.h>
#include <kprintf.h>
#include <string.h>
#include <stdbool.h>
#include <pmm.h>
#include <slab.h>
#include <mm_constants.h>

// Slab size classes (powers of 2 for efficient allocation)
#define SLAB_16    0
#define SLAB_32    1
#define SLAB_64    2
#define SLAB_128   3
#define SLAB_256   4
#define SLAB_512   5
#define SLAB_1024  6
#define SLAB_2048  7

static const size_t slab_sizes[NUM_SLAB_CLASSES] = {
    16, 32, 64, 128, 256, 512, 1024, 2048
};

typedef struct slab_header {
    struct slab_header *next;
    size_t object_size;
    size_t objects_total;
    size_t objects_used;
    void *free_list;
} slab_header_t;

static slab_header_t *slab_lists[NUM_SLAB_CLASSES] = {NULL};
static uintptr_t hhdm_offset = 0;
static bool heap_initialized = false;
static SPIN_LOCK heap_lock = {0};

// Get slab class index for given size
static int get_slab_class(size_t size) {
    for (int i = 0; i < NUM_SLAB_CLASSES; i++) {
        if (size <= slab_sizes[i]) {
            return i;
        }
    }
    return -1; // Too large for slab
}

// Create a new slab for the given class
static slab_header_t *create_slab(int class) {
    if (class < 0 || class >= NUM_SLAB_CLASSES) {
        kprintf("Heap Error: Invalid slab class %d\n", class);
        return NULL;
    }
    
    size_t obj_size = slab_sizes[class];
    
    // Allocate a page for the slab
    void *page = pmm_alloc_page();
    if (!page) {
        kprintf("Heap Critical: Failed to allocate page for slab class %d (%d bytes)\n",
                class, obj_size);
        return NULL;
    }
    
    // Map to virtual address
    slab_header_t *slab = (slab_header_t *)((uintptr_t)page + hhdm_offset);
    
    // Calculate how many objects fit in a page (minus header)
    size_t usable = PAGE_SIZE - sizeof(slab_header_t);
    size_t objects = usable / obj_size;
    
    if (objects == 0) {
        kprintf("Heap Error: Object size %d too large for slab (no objects fit)\n", obj_size);
        uintptr_t phys = (uintptr_t)slab - hhdm_offset;
        pmm_free_page((void *)phys);
        return NULL;
    }
    
    slab->next = NULL;
    slab->object_size = obj_size;
    slab->objects_total = objects;
    slab->objects_used = 0;
    slab->free_list = NULL;
    
    // Build free list - objects start after header
    uintptr_t obj_start = (uintptr_t)slab + sizeof(slab_header_t);
    void **prev = &slab->free_list;
    
    for (size_t i = 0; i < objects; i++) {
        void *obj = (void *)(obj_start + (i * obj_size));
        *prev = obj;
        prev = (void **)obj;
    }
    *prev = NULL;
    
    return slab;
}

// Find slab that owns this object
static slab_header_t *find_slab_for_object(void *ptr, int class) {
    if (!ptr || class < 0 || class >= NUM_SLAB_CLASSES) {
        return NULL;
    }
    
    for (slab_header_t *slab = slab_lists[class]; slab != NULL; slab = slab->next) {
        uintptr_t slab_start = (uintptr_t)slab;
        uintptr_t slab_end = slab_start + PAGE_SIZE;
        uintptr_t obj = (uintptr_t)ptr;
        
        if (obj >= slab_start && obj < slab_end) {
            return slab;
        }
    }
    return NULL;
}

void heap_init(struct limine_hhdm_response *hhdm) {
    if (!hhdm) {
        kprintf("Heap Error: NULL HHDM response\n");
        return;
    }
    
    hhdm_offset = hhdm->offset;
    
    // Initialize slab lists
    for (int i = 0; i < NUM_SLAB_CLASSES; i++) {
        slab_lists[i] = NULL;
    }
    
    heap_initialized = true;
    kprintf("Heap initialized. Slab classes: ");
    for (int i = 0; i < NUM_SLAB_CLASSES; i++) {
        kprintf("%d%s", slab_sizes[i], i < NUM_SLAB_CLASSES - 1 ? ", " : "");
    }
    kprintf(" bytes\n");
}

void *kmalloc(size_t size) {
    spin_lock(&heap_lock);
    
    if (!heap_initialized) {
        kprintf("Heap Error: kmalloc called before heap_init\n");
        spin_unlock(&heap_lock);
        return NULL;
    }
    
    if (size == 0) {
        kprintf("Heap Warning: kmalloc called with size=0\n");
        spin_unlock(&heap_lock);
        return NULL;
    }
    
    int class = get_slab_class(size);
    
    // Large allocation - use PMM directly
    if (class < 0) {
        if (size > PMM_MAX_CONTIGUOUS_BYTES - sizeof(size_t)) {
            kprintf("Heap Error: Allocation of %d bytes exceeds maximum (%d bytes)\n",
                    size, PMM_MAX_CONTIGUOUS_BYTES - sizeof(size_t));
            spin_unlock(&heap_lock);
            return NULL;
        }
        
        size_t pages = BYTES_TO_PAGES(size + sizeof(size_t));
        void *mem = pmm_alloc_pages(pages);
        if (!mem) {
            kprintf("Heap Critical: Out of memory allocating %d bytes (%d pages)\n", 
                    size, pages);
            spin_unlock(&heap_lock);
            return NULL; 
        }
        
        // Store size in header for kfree
        size_t *header = (size_t *)((uintptr_t)mem + hhdm_offset);
        *header = pages;
        
        spin_unlock(&heap_lock);
        return (void *)(header + 1);
    }
    
    // Small allocation - use slab
    slab_header_t *slab = slab_lists[class];
    
    // Find a slab with free objects
    while (slab && slab->objects_used >= slab->objects_total) {
        slab = slab->next;
    }
    
    // No suitable slab found, create new one
    if (!slab) {
        slab = create_slab(class);
        if (!slab) {
            kprintf("Heap Critical: Failed to create slab for %d byte allocation\n", size);
            spin_unlock(&heap_lock);
            return NULL;
        }
        
        // Add to front of list
        slab->next = slab_lists[class];
        slab_lists[class] = slab;
    }
    
    // Allocate from free list
    if (!slab->free_list) {
        kprintf("Heap Error: Slab corruption - no free objects but objects_used < objects_total\n");
        kprintf("  Class: %d, Used: %d, Total: %d\n", 
                class, slab->objects_used, slab->objects_total);
        spin_unlock(&heap_lock);
        return NULL;
    }
    
    void *obj = slab->free_list;
    slab->free_list = *(void **)obj;
    slab->objects_used++;
    
    // Zero the memory
    memset(obj, 0, slab->object_size);
    spin_unlock(&heap_lock);
    return obj;
}

void kfree(void *ptr) {
    if (!ptr) {
        kprintf("Heap Warning: kfree called with NULL pointer\n");
        return;
    }
    
    spin_lock(&heap_lock);
    
    if (!heap_initialized) {
        kprintf("Heap Error: kfree called before heap_init\n");
        spin_unlock(&heap_lock);
        return;
    }
    
    uintptr_t addr = (uintptr_t)ptr;
    
    // Check if it's a small allocation (slab)
    int class = -1;
    slab_header_t *slab = NULL;
    
    for (int i = 0; i < NUM_SLAB_CLASSES; i++) {
        slab = find_slab_for_object(ptr, i);
        if (slab) {
            class = i;
            break;
        }
    }
    
    if (class >= 0 && slab) {
        // Verify object is within valid range
        uintptr_t obj_start = (uintptr_t)slab + sizeof(slab_header_t);
        uintptr_t obj_end = obj_start + (slab->objects_total * slab->object_size);
        
        if (addr < obj_start || addr >= obj_end) {
            kprintf("Heap Error: Invalid free - pointer 0x%lx not aligned to object boundary\n", addr);
            spin_unlock(&heap_lock);
            return;
        }
        
        // Slab allocation - return to free list
        *(void **)ptr = slab->free_list;
        slab->free_list = ptr;
        slab->objects_used--;
        
        // Optional: free completely empty slabs (except keep one)
        if (slab->objects_used == 0 && slab != slab_lists[class]) {
            // Find and remove from list
            slab_header_t **prev = &slab_lists[class];
            while (*prev && *prev != slab) {
                prev = &(*prev)->next;
            }
            if (*prev) {
                *prev = slab->next;
                
                // Free the page
                uintptr_t phys = (uintptr_t)slab - hhdm_offset;
                pmm_free_page((void *)phys);
            }
        }
    } else {
        // Large allocation - stored size before pointer
        size_t *header = (size_t *)ptr - 1;
        size_t pages = *header;
        
        if (pages == 0 || pages > PMM_MAX_CONTIGUOUS_PAGES) {
            kprintf("Heap Error: Invalid large allocation header (pages=%d)\n", pages);
            spin_unlock(&heap_lock);
            return;
        }
        
        uintptr_t phys = (uintptr_t)header - hhdm_offset;
        pmm_free_pages((void *)phys, pages);
    }
    spin_unlock(&heap_lock);
}

void *krealloc(void *ptr, size_t new_size) {
    if (!ptr) {
        return kmalloc(new_size);
    }
    
    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }
    
    // Allocate new block
    void *new_ptr = kmalloc(new_size);
    if (!new_ptr) {
        kprintf("Heap Error: krealloc failed to allocate %d bytes\n", new_size);
        return NULL;
    }
    
    // Try to determine old size (approximate for slab allocations)
    size_t old_size = 0;
    
    // Check if it's in a slab
    for (int i = 0; i < NUM_SLAB_CLASSES; i++) {
        if (find_slab_for_object(ptr, i)) {
            old_size = slab_sizes[i];
            break;
        }
    }
    
    // If not in slab, it's a large allocation
    if (old_size == 0) {
        size_t *header = (size_t *)ptr - 1;
        size_t pages = *header;
        
        if (pages == 0 || pages > PMM_MAX_CONTIGUOUS_PAGES) {
            kprintf("Heap Error: krealloc detected corrupted header (pages=%d)\n", pages);
            kfree(new_ptr);
            return NULL;
        }
        
        old_size = PAGES_TO_BYTES(pages) - sizeof(size_t);
    }
    
    // Copy old data
    size_t copy_size = old_size < new_size ? old_size : new_size;
    memcpy(new_ptr, ptr, copy_size);
    
    kfree(ptr);
    return new_ptr;
}

void heap_print_stats(void) {
    if (!heap_initialized) {
        kprintf("Heap not initialized\n");
        return;
    }
    
    kprintf("\n=== Heap Statistics ===\n");
    
    size_t total_slabs = 0;
    size_t total_memory = 0;
    size_t used_memory = 0;
    
    for (int i = 0; i < NUM_SLAB_CLASSES; i++) {
        size_t class_slabs = 0;
        size_t class_objects = 0;
        size_t class_used = 0;
        
        for (slab_header_t *slab = slab_lists[i]; slab != NULL; slab = slab->next) {
            class_slabs++;
            class_objects += slab->objects_total;
            class_used += slab->objects_used;
        }
        
        if (class_slabs > 0) {
            total_slabs += class_slabs;
            total_memory += PAGES_TO_BYTES(class_slabs);
            used_memory += class_used * slab_sizes[i];
            
            kprintf("  %4d byte slabs: %d slabs, %d/%d objects (%d%% used)\n",
                    slab_sizes[i], class_slabs, class_used, class_objects,
                    class_objects > 0 ? (class_used * 100 / class_objects) : 0);
        }
    }
    
    kprintf("\nTotal slabs: %d (%d KB allocated, %d KB used)\n",
            total_slabs, total_memory / 1024, used_memory / 1024);
    kprintf("=======================\n\n");
}

void test_heap(void) {
    kprintf("\n=== Testing Heap Allocator ===\n");
    
    if (!heap_initialized) {
        kprintf("Error: Heap not initialized\n");
        return;
    }
    
    // Test small allocations
    void *small1 = kmalloc(16);
    void *small2 = kmalloc(32);
    void *small3 = kmalloc(64);
    
    if (small1 && small2 && small3) {
        kprintf("Small allocs: 0x%lx, 0x%lx, 0x%lx\n", 
                (uintptr_t)small1, (uintptr_t)small2, (uintptr_t)small3);
    } else {
        kprintf("Failed to allocate small objects\n");
    }
    
    // Test large allocation
    void *large = kmalloc(8192);
    if (large) {
        kprintf("Large alloc (8KB): 0x%lx\n", (uintptr_t)large);
    } else {
        kprintf("Failed to allocate large object\n");
    }
    
    heap_print_stats();
    
    // Free and reallocate
    kfree(small2);
    void *small4 = kmalloc(32);
    if (small4) {
        kprintf("After free and realloc: 0x%lx (should reuse)\n", (uintptr_t)small4);
    }
    
    // Test realloc
    void *resized = krealloc(small3, 128);
    if (resized) {
        kprintf("Realloc 64->128: 0x%lx\n", (uintptr_t)resized);
    } else {
        kprintf("Failed to realloc\n");
    }
    
    // Cleanup
    kfree(small1);
    kfree(small4);
    kfree(resized);
    kfree(large);
    
    kprintf("After cleanup:\n");
    heap_print_stats();
    kprintf("==============================\n\n");
}