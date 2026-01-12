
#include <stdint.h>
#include <stddef.h>
#include <limine.h>
#include <kprintf.h>
#include <string.h>
#include <stdbool.h>
#include <pmm.h>
#include <slab.h>
#include <mm_constants.h>

static uint8_t *bitmap = NULL;
static size_t bitmap_size = 0;
static size_t total_pages = 0;
static uintptr_t highest_addr = 0;
static uintptr_t hhdm_offset = 0;
static SPIN_LOCK pmm_lock = {0};

typedef struct free_block {
    struct free_block *next;
    struct free_block *prev;
} free_block_t;

static free_block_t *free_lists[PMM_MAX_ORDER + 1];

// Helper functions for bitmap manipulation
void bitmap_set(size_t bit) { 
    bitmap[bit / 8] |= (1 << (bit % 8)); 
}

void bitmap_unset(size_t bit) { 
    bitmap[bit / 8] &= ~(1 << (bit % 8)); 
}

bool bitmap_test(size_t bit) { 
    return bitmap[bit / 8] & (1 << (bit % 8)); 
}

// Get buddy index for a page at given order
static inline size_t get_buddy_index(size_t page_index, size_t order) {
    return page_index ^ (1 << order);
}

// Check if a block is free (all pages in block are unset in bitmap)
static bool is_block_free(size_t page_index, size_t order) {
    size_t pages = 1 << order;
    for (size_t i = 0; i < pages; i++) {
        if (bitmap_test(page_index + i)) {
            return false;
        }
    }
    return true;
}

// Mark a block as used in bitmap
static void mark_block_used(size_t page_index, size_t order) {
    size_t pages = 1 << order;
    for (size_t i = 0; i < pages; i++) {
        bitmap_set(page_index + i);
    }
}

// Mark a block as free in bitmap
static void mark_block_free(size_t page_index, size_t order) {
    size_t pages = 1 << order;
    for (size_t i = 0; i < pages; i++) {
        bitmap_unset(page_index + i);
    }
}

// Add block to free list
static void add_to_free_list(size_t page_index, size_t order) {
    void *block_virt = (void *)(page_index * PAGE_SIZE + hhdm_offset);
    free_block_t *block = (free_block_t *)block_virt;
    
    block->next = free_lists[order];
    block->prev = NULL;
    
    if (free_lists[order]) {
        free_lists[order]->prev = block;
    }
    
    free_lists[order] = block;
}

// Remove block from free list
static void remove_from_free_list(free_block_t *block, size_t order) {
    if (block->prev) {
        block->prev->next = block->next;
    } else {
        free_lists[order] = block->next;
    }
    
    if (block->next) {
        block->next->prev = block->prev;
    }
}

// Find and remove a block from free list by page index
static bool find_and_remove_from_free_list(size_t page_index, size_t order) {
    void *block_virt = (void *)(page_index * PAGE_SIZE + hhdm_offset);
    
    for (free_block_t *curr = free_lists[order]; curr != NULL; curr = curr->next) {
        if (curr == block_virt) {
            remove_from_free_list(curr, order);
            return true;
        }
    }
    return false;
}

void pmm_init(struct limine_memmap_response *memmap, struct limine_hhdm_response *hhdm) {
    if (!memmap || !hhdm) {
        kprintf("PMM Error: Received NULL responses from kmain.\n");
        return;
    }

    hhdm_offset = hhdm->offset;

    // Initialize free lists
    for (size_t i = PMM_MIN_ORDER; i <= PMM_MAX_ORDER; i++) {
        free_lists[i] = NULL;
    }

    // 1. Calculate RAM size
    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE) {
            uintptr_t top = e->base + e->length;
            if (top > highest_addr) highest_addr = top;
        }
    }

    total_pages = highest_addr / PAGE_SIZE;
    bitmap_size = total_pages / 8;

    // 2. Find a hole for the bitmap
    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE && e->length >= bitmap_size) {
            bitmap = (uint8_t *)(e->base + hhdm_offset);
            memset(bitmap, 0xFF, bitmap_size);
            break;
        }
    }

    if (!bitmap) {
        kprintf("PMM Critical: Failed to allocate bitmap!\n");
        return;
    }

    // 3. Mark usable as free in bitmap
    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE) {
            for (size_t j = 0; j < e->length; j += PAGE_SIZE) {
                bitmap_unset((e->base + j) / PAGE_SIZE);
            }
        }
    }

    // 4. Mark the bitmap itself as used
    uintptr_t bitmap_phys = (uintptr_t)bitmap - hhdm_offset;
    size_t bitmap_start_page = bitmap_phys / PAGE_SIZE;
    size_t bitmap_num_pages = BYTES_TO_PAGES(bitmap_size);
    
    for (size_t i = 0; i < bitmap_num_pages; i++) {
        bitmap_set(bitmap_start_page + i);
    }

    // 5. Protect the first 1MB
    for(size_t i = 0; i < FIRST_MB_PAGES; i++) {
        bitmap_set(i);
    }

    // 6. Build buddy system free lists from usable regions
    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE) {
            size_t start_page = e->base / PAGE_SIZE;
            size_t num_pages = e->length / PAGE_SIZE;
            
            // Add blocks of largest possible order
            size_t page = start_page;
            while (page < start_page + num_pages) {
                // Skip if already marked as used (bitmap/protected)
                if (bitmap_test(page)) {
                    page++;
                    continue;
                }
                
                // Find largest order that fits
                size_t order;
                for (order = PMM_MAX_ORDER; order >= PMM_MIN_ORDER; order--) {
                    size_t block_pages = 1 << order;
                    
                    // Check alignment and availability
                    if ((page & (block_pages - 1)) == 0 && 
                        page + block_pages <= start_page + num_pages &&
                        is_block_free(page, order)) {
                        add_to_free_list(page, order);
                        page += block_pages;
                        break;
                    }
                    
                    if (order == PMM_MIN_ORDER) {
                        page++; // Single page that couldn't be added
                        break;
                    }
                }
            }
        }
    }

    kprintf("PMM Ready (Buddy System). Total RAM: %d MB\n", highest_addr / (1024 * 1024));
}

// Allocate a block of given order
static void *pmm_alloc_order(size_t order) {
    if (order > PMM_MAX_ORDER) {
        kprintf("PMM Error: Requested order %d exceeds max order %d\n", 
                order, PMM_MAX_ORDER);
        return NULL;
    }

    // Check if we have a free block at this order
    if (free_lists[order]) {
        free_block_t *block = free_lists[order];
        size_t page_index = ((uintptr_t)block - hhdm_offset) / PAGE_SIZE;
        
        remove_from_free_list(block, order);
        mark_block_used(page_index, order);
        
        return (void *)(page_index * PAGE_SIZE);
    }

    // No block available, split a larger block
    if (order < PMM_MAX_ORDER) {
        void *larger_block = pmm_alloc_order(order + 1);
        if (!larger_block) {
            kprintf("PMM Warning: Failed to allocate block of order %d (out of memory)\n", order);
            return NULL;
        }

        size_t page_index = (uintptr_t)larger_block / PAGE_SIZE;
        
        // Split: mark first half as used, add second half to free list
        size_t buddy_index = page_index + (1 << order);
        mark_block_free(buddy_index, order);
        add_to_free_list(buddy_index, order);
        
        return larger_block;
    }

    kprintf("PMM Error: Out of memory at max order %d\n", PMM_MAX_ORDER);
    return NULL;
}

// Free a block and try to coalesce with buddy
static void pmm_free_order(void *page, size_t order) {
    if (!page) {
        kprintf("PMM Warning: Attempted to free NULL pointer\n");
        return;
    }
    
    if (order > PMM_MAX_ORDER) {
        kprintf("PMM Error: Invalid order %d in free (max %d)\n", order, PMM_MAX_ORDER);
        return;
    }

    uintptr_t phys = (uintptr_t)page;
    size_t page_index = phys / PAGE_SIZE;
    
    if (page_index >= total_pages) {
        kprintf("PMM Error: Invalid page index %d (max %d)\n", page_index, total_pages);
        return;
    }
    
    if (page_index < FIRST_MB_PAGES) {
        kprintf("PMM Error: Attempted to free protected memory at 0x%lx\n", phys);
        return;
    }
    
    // Mark as free in bitmap
    mark_block_free(page_index, order);

    // Try to coalesce with buddy
    while (order < PMM_MAX_ORDER) {
        size_t buddy_index = get_buddy_index(page_index, order);
        
        // Check if buddy is free and exists
        if (buddy_index >= total_pages || !is_block_free(buddy_index, order)) {
            break;
        }
        
        // Check if buddy is in the free list
        if (!find_and_remove_from_free_list(buddy_index, order)) {
            break;
        }
        
        // Coalesce: use lower address as parent
        if (buddy_index < page_index) {
            page_index = buddy_index;
        }
        
        order++;
    }
    
    // Add coalesced block to free list
    add_to_free_list(page_index, order);
}

// Calculate order needed for given page count
static size_t pages_to_order(size_t pages) {
    if (pages == 0) {
        return 0;
    }
    
    size_t order = 0;
    size_t size = 1;
    
    while (size < pages && order < PMM_MAX_ORDER) {
        size <<= 1;
        order++;
    }
    
    return order;
}

// Public API
void *pmm_alloc_page(void) {
    spin_lock(&pmm_lock);
    void *page = pmm_alloc_order(PMM_MIN_ORDER);
    spin_unlock(&pmm_lock);
    
    if (!page) {
        kprintf("PMM Critical: Failed to allocate single page (system out of memory)\n");
    }
    
    return page;
}

void pmm_free_page(void *page) {
    if (!page) {
        kprintf("PMM Warning: pmm_free_page called with NULL pointer\n");
        return;
    }
    
    spin_lock(&pmm_lock);
    
    uintptr_t phys = (uintptr_t)page;
    size_t page_index = phys / PAGE_SIZE;
    
    if (page_index >= total_pages || page_index < FIRST_MB_PAGES) {
        kprintf("PMM Error: Invalid free attempt at 0x%lx (index %d)\n", phys, page_index);
        spin_unlock(&pmm_lock);
        return;
    }
    
    pmm_free_order(page, PMM_MIN_ORDER);
    spin_unlock(&pmm_lock);
}

void *pmm_alloc_pages(size_t count) {
    if (count == 0) {
        kprintf("PMM Warning: pmm_alloc_pages called with count=0\n");
        return NULL;
    }
    
    if (count > PMM_MAX_CONTIGUOUS_PAGES) {
        kprintf("PMM Error: Requested %d pages exceeds max contiguous allocation (%d pages)\n",
                count, PMM_MAX_CONTIGUOUS_PAGES);
        return NULL;
    }
    
    spin_lock(&pmm_lock);
    size_t order = pages_to_order(count);
    void *page = pmm_alloc_order(order);
    spin_unlock(&pmm_lock);
    
    if (!page) {
        kprintf("PMM Critical: Failed to allocate %d pages (order %d)\n", count, order);
    }
    
    return page;
}

void pmm_free_pages(void *pages, size_t count) {
    if (!pages) {
        kprintf("PMM Warning: pmm_free_pages called with NULL pointer\n");
        return;
    }
    
    if (count == 0) {
        kprintf("PMM Warning: pmm_free_pages called with count=0\n");
        return;
    }
    
    spin_lock(&pmm_lock);
    size_t order = pages_to_order(count);
    pmm_free_order(pages, order);
    spin_unlock(&pmm_lock);
}

void *pmm_alloc_aligned(size_t size, size_t alignment) {
    if (size == 0) {
        kprintf("PMM Warning: pmm_alloc_aligned called with size=0\n");
        return NULL;
    }
    
    if (alignment == 0) {
        kprintf("PMM Warning: pmm_alloc_aligned called with alignment=0\n");
        return NULL;
    }
    
    spin_lock(&pmm_lock);
    size_t pages = BYTES_TO_PAGES(size);
    size_t order = pages_to_order(pages);
    
    if (alignment <= (PAGE_SIZE << order)) {
        void *block = pmm_alloc_order(order);
        spin_unlock(&pmm_lock);
        
        if (!block) {
            kprintf("PMM Error: Failed to allocate %d bytes with alignment %d\n", 
                    size, alignment);
        }
        
        return block;
    }
    
    void *block = pmm_alloc_order(pages_to_order(alignment / PAGE_SIZE));
    spin_unlock(&pmm_lock);
    
    if (!block) {
        kprintf("PMM Error: Failed to allocate aligned block (size=%d, align=%d)\n",
                size, alignment);
    }
    
    return block;
}

void pmm_free_aligned(void *ptr, size_t size) {
    if (!ptr) {
        kprintf("PMM Warning: pmm_free_aligned called with NULL pointer\n");
        return;
    }
    
    if (size == 0) {
        kprintf("PMM Warning: pmm_free_aligned called with size=0\n");
        return;
    }
    
    spin_lock(&pmm_lock);
    size_t pages = BYTES_TO_PAGES(size);
    size_t order = pages_to_order(pages);
    pmm_free_order(ptr, order);
    spin_unlock(&pmm_lock);
}

// Stats functions
size_t pmm_get_total_memory(void) {
    return highest_addr;
}

size_t pmm_get_used_memory(void) {
    size_t used = 0;
    for (size_t i = 0; i < total_pages; i++) {
        if (bitmap_test(i)) used++;
    }
    return PAGES_TO_BYTES(used);
}

size_t pmm_get_free_memory(void) {
    return pmm_get_total_memory() - pmm_get_used_memory();
}

void pmm_print_stats(void) {
    size_t total = pmm_get_total_memory() / (1024 * 1024);
    size_t used = pmm_get_used_memory() / (1024 * 1024);
    size_t free = pmm_get_free_memory() / (1024 * 1024);
    
    kprintf("PMM Stats (Buddy System):\n");
    kprintf("  Total: %d MB\n", total);
    kprintf("  Used:  %d MB\n", used);
    kprintf("  Free:  %d MB\n", free);
    kprintf("\nFree list distribution:\n");
    for (size_t i = PMM_MIN_ORDER; i <= PMM_MAX_ORDER; i++) {
        int count = 0;
        for (free_block_t *b = free_lists[i]; b != NULL; b = b->next) {
            count++;
        }
        if (count > 0) {
            kprintf("  Order %d (%d pages): %d blocks\n", i, 1 << i, count);
        }
    }
}

void test_pmm(void) {
    kprintf("Testing Buddy PMM...\n");
    pmm_print_stats();
    
    void *p1 = pmm_alloc_page();
    if (p1) {
        kprintf("Single page: 0x%lx\n", (uintptr_t)p1);
    }
    
    void *p2 = pmm_alloc_pages(8);
    if (p2) {
        kprintf("8 pages: 0x%lx\n", (uintptr_t)p2);
    }
    
    pmm_free_page(p1);
    pmm_free_pages(p2, 8);
    kprintf("After freeing:\n");
    pmm_print_stats();
}