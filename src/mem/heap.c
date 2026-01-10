#include <stdint.h>
#include <stddef.h>
#include <limine.h>
#include <kprintf.h>
#include <string.h>
#include <stdbool.h>
#include <pmm.h>
#include <slab.h>

// Slab size classes (powers of 2 for efficient allocation)
#define SLAB_16    0
#define SLAB_32    1
#define SLAB_64    2
#define SLAB_128   3
#define SLAB_256   4
#define SLAB_512   5
#define SLAB_1024  6
#define SLAB_2048  7
#define NUM_SLABS  8

static const size_t slab_sizes[NUM_SLABS] = {16, 32, 64, 128, 256, 512, 1024, 2048};

typedef struct slab_header {
    struct slab_header *next;
    size_t object_size;
    size_t objects_total;
    size_t objects_used;
    void *free_list;
} slab_header_t;

static slab_header_t *slab_lists[NUM_SLABS] = {NULL};
static uintptr_t hhdm_offset = 0;
static bool heap_initialized = false;

// Get slab class index for given size
static int get_slab_class(size_t size) {
    for (int i = 0; i < NUM_SLABS; i++) {
        if (size <= slab_sizes[i]) {
            return i;
        }
    }
    return -1; // Too large for slab
}

// Create a new slab for the given class
static slab_header_t *create_slab(int class) {
    size_t obj_size = slab_sizes[class];
    
    // Allocate a page for the slab
    void *page = pmm_alloc_page();
    if (!page) {
        return NULL;
    }
    
    // Map to virtual address
    slab_header_t *slab = (slab_header_t *)((uintptr_t)page + hhdm_offset);
    
    // Calculate how many objects fit in a page (minus header)
    size_t usable = 4096 - sizeof(slab_header_t);
    size_t objects = usable / obj_size;
    
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
    for (slab_header_t *slab = slab_lists[class]; slab != NULL; slab = slab->next) {
        uintptr_t slab_start = (uintptr_t)slab;
        uintptr_t slab_end = slab_start + 4096;
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
    for (int i = 0; i < NUM_SLABS; i++) {
        slab_lists[i] = NULL;
    }
    
    heap_initialized = true;
    kprintf("Heap initialized. Slab classes: ");
    for (int i = 0; i < NUM_SLABS; i++) {
        kprintf("%d%s", slab_sizes[i], i < NUM_SLABS - 1 ? ", " : "");
    }
    kprintf(" bytes\n");
}

void *kmalloc(size_t size) {
    if (!heap_initialized || size == 0) {
        return NULL;
    }
    
    int class = get_slab_class(size);
    
    // Large allocation - use PMM directly
    if (class < 0) {
        size_t pages = (size + sizeof(size_t) + 4095) / 4096;
        void *mem = pmm_alloc_pages(pages);
        if (!mem) {
            return NULL;
        }
        
        // Store size in header for kfree
        size_t *header = (size_t *)((uintptr_t)mem + hhdm_offset);
        *header = pages;
        
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
            return NULL;
        }
        
        // Add to front of list
        slab->next = slab_lists[class];
        slab_lists[class] = slab;
    }
    
    // Allocate from free list
    if (!slab->free_list) {
        kprintf("Heap Error: Slab has no free objects but objects_used < objects_total\n");
        return NULL;
    }
    
    void *obj = slab->free_list;
    slab->free_list = *(void **)obj;
    slab->objects_used++;
    
    // Zero the memory
    memset(obj, 0, slab->object_size);
    
    return obj;
}

void kfree(void *ptr) {
    if (!ptr || !heap_initialized) {
        return;
    }
    
    uintptr_t addr = (uintptr_t)ptr;
    
    // Check if it's a small allocation (slab)
    int class = -1;
    slab_header_t *slab = NULL;
    
    for (int i = 0; i < NUM_SLABS; i++) {
        slab = find_slab_for_object(ptr, i);
        if (slab) {
            class = i;
            break;
        }
    }
    
    if (class >= 0 && slab) {
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
        
        uintptr_t phys = (uintptr_t)header - hhdm_offset;
        pmm_free_pages((void *)phys, pages);
    }
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
        return NULL;
    }
    
    // Try to determine old size (approximate for slab allocations)
    size_t old_size = 0;
    
    // Check if it's in a slab
    for (int i = 0; i < NUM_SLABS; i++) {
        if (find_slab_for_object(ptr, i)) {
            old_size = slab_sizes[i];
            break;
        }
    }
    
    // If not in slab, it's a large allocation
    if (old_size == 0) {
        size_t *header = (size_t *)ptr - 1;
        old_size = (*header * 4096) - sizeof(size_t);
    }
    
    // Copy old data
    size_t copy_size = old_size < new_size ? old_size : new_size;
    memcpy(new_ptr, ptr, copy_size);
    
    kfree(ptr);
    return new_ptr;
}

void heap_print_stats(void) {
    kprintf("\n=== Heap Statistics ===\n");
    
    for (int i = 0; i < NUM_SLABS; i++) {
        size_t total_slabs = 0;
        size_t total_objects = 0;
        size_t used_objects = 0;
        
        for (slab_header_t *slab = slab_lists[i]; slab != NULL; slab = slab->next) {
            total_slabs++;
            total_objects += slab->objects_total;
            used_objects += slab->objects_used;
        }
        
        if (total_slabs > 0) {
            kprintf("  %4d byte slabs: %d slabs, %d/%d objects (%d%% used)\n",
                    slab_sizes[i], total_slabs, used_objects, total_objects,
                    total_objects > 0 ? (used_objects * 100 / total_objects) : 0);
        }
    }
    
    kprintf("=======================\n\n");
}

void test_heap(void) {
    kprintf("\n=== Testing Heap Allocator ===\n");
    
    // Test small allocations
    void *small1 = kmalloc(16);
    void *small2 = kmalloc(32);
    void *small3 = kmalloc(64);
    kprintf("Small allocs: 0x%lx, 0x%lx, 0x%lx\n", 
            (uintptr_t)small1, (uintptr_t)small2, (uintptr_t)small3);
    
    // Test large allocation
    void *large = kmalloc(8192);
    kprintf("Large alloc (8KB): 0x%lx\n", (uintptr_t)large);
    
    heap_print_stats();
    
    // Free and reallocate
    kfree(small2);
    void *small4 = kmalloc(32);
    kprintf("After free and realloc: 0x%lx (should reuse)\n", (uintptr_t)small4);
    
    // Test realloc
    void *resized = krealloc(small3, 128);
    kprintf("Realloc 64->128: 0x%lx\n", (uintptr_t)resized);
    
    // Cleanup
    kfree(small1);
    kfree(small4);
    kfree(resized);
    kfree(large);
    
    kprintf("After cleanup:\n");
    heap_print_stats();
    kprintf("==============================\n\n");
}