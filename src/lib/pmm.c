#include <stdint.h>
#include <stddef.h>
#include <limine.h>
#include <kprintf.h>
#include <string.h>
#include <stdbool.h> 

// bitmap method for PMM, will swap to buddy system later

static uint8_t *bitmap = NULL;
static size_t bitmap_size = 0;
static size_t total_pages = 0;
static size_t last_free_index = 0;
static uintptr_t highest_addr = 0;

// Helper bits
void bitmap_set(size_t bit) { bitmap[bit / 8] |= (1 << (bit % 8)); }
void bitmap_unset(size_t bit) { bitmap[bit / 8] &= ~(1 << (bit % 8)); }
bool bitmap_test(size_t bit) { return bitmap[bit / 8] & (1 << (bit % 8)); }

// MODIFIED: We no longer define requests here. We accept them as args.
void pmm_init(struct limine_memmap_response *memmap, struct limine_hhdm_response *hhdm) {
    
    // Safety check: Did the old code pass us valid data?
    if (!memmap || !hhdm) {
        kprintf("PMM Error: Received NULL responses from kmain.\n");
        return; // Or hcf()
    }

    uintptr_t hhdm_offset = hhdm->offset;

    // 1. Calculate RAM size
    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE) {
            uintptr_t top = e->base + e->length;
            if (top > highest_addr) highest_addr = top;
        }
    }

    total_pages = highest_addr / 4096;
    bitmap_size = total_pages / 8;

    // 2. Find a hole for the bitmap
    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE && e->length >= bitmap_size) {
            bitmap = (uint8_t *)(e->base + hhdm_offset);
            
            // Initial state: Everything used
            memset(bitmap, 0xFF, bitmap_size);
            break;
        }
    }

    // 3. Mark USABLE RAM as free (0)
    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE) {
            for (size_t j = 0; j < e->length; j += 4096) {
                bitmap_unset((e->base + j) / 4096);
            }
        }
    }

    // 4. Mark the bitmap itself as used
    uintptr_t bitmap_phys = (uintptr_t)bitmap - hhdm_offset;
    size_t bitmap_start_page = bitmap_phys / 4096;
    size_t bitmap_num_pages = (bitmap_size + 4095) / 4096;
    
    for (size_t i = 0; i < bitmap_num_pages; i++) {
        bitmap_set(bitmap_start_page + i);
    }

    // 5. Protect the first 1MB
    for(size_t i = 0; i < 256; i++) bitmap_set(i);

    kprintf("PMM Ready. Total RAM: %d MB\n", highest_addr / 1024 / 1024);
}
void *pmm_alloc_page(void) {
    for (size_t i = last_free_index; i < total_pages; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            last_free_index = i;
            return (void *)(i * 4096); 
        }
    }
    // Search from start if needed...
    if (last_free_index > 0) {
        for (size_t i = 0; i < last_free_index; i++) {
             if (!bitmap_test(i)) {
                bitmap_set(i);
                last_free_index = i;
                return (void *)(i * 4096);
            }
        }
    }
    return NULL; 
}

void pmm_free_page(void *page) {
    if (page == 0) return;
    
    uintptr_t phys = (uintptr_t)page;
    size_t page_index = phys / 4096;
    
    // Safety checks
    if (page_index >= total_pages) {
        kprintf("PMM: Tried to free invalid page 0x%lx\n", phys);
        return;
    }
    
    if (page_index < 256) {
        kprintf("PMM: Tried to free protected page 0x%lx\n", phys);
        return;
    }
    
    if (!bitmap_test(page_index)) {
        kprintf("PMM: Double free detected at 0x%lx\n", phys);
        return;
    }
    
    bitmap_unset(page_index);
    
    // Optimization: Update last_free_index if we freed earlier
    if (page_index < last_free_index) {
        last_free_index = page_index;
    }
}

void *pmm_alloc_pages(size_t count) {
    if (count == 0) return NULL;
    
    // Find 'count' consecutive free pages
    for (size_t i = last_free_index; i < total_pages - count; i++) {
        bool found = true;
        
        // Check if 'count' pages starting at 'i' are all free
        for (size_t j = 0; j < count; j++) {
            if (bitmap_test(i + j)) {
                found = false;
                i += j; // Skip ahead
                break;
            }
        }
        
        if (found) {
            // Mark all pages as used
            for (size_t j = 0; j < count; j++) {
                bitmap_set(i + j);
            }
            last_free_index = i + count;
            return (void *)(i * 4096);
        }
    }
    
    // Try from beginning if needed
    for (size_t i = 256; i < last_free_index - count; i++) {
        bool found = true;
        for (size_t j = 0; j < count; j++) {
            if (bitmap_test(i + j)) {
                found = false;
                i += j;
                break;
            }
        }
        if (found) {
            for (size_t j = 0; j < count; j++) {
                bitmap_set(i + j);
            }
            last_free_index = i + count;
            return (void *)(i * 4096);
        }
    }
    
    return NULL;
}

void pmm_free_pages(void *pages, size_t count) {
    if (!pages || count == 0) return;
    
    uintptr_t phys = (uintptr_t)pages;
    for (size_t i = 0; i < count; i++) {
        pmm_free_page((void *)(phys + i * 4096));
    }
}

size_t pmm_get_total_memory(void) {
    return highest_addr;
}

size_t pmm_get_used_memory(void) {
    size_t used = 0;
    for (size_t i = 0; i < total_pages; i++) {
        if (bitmap_test(i)) used++;
    }
    return used * 4096;
}

size_t pmm_get_free_memory(void) {
    return pmm_get_total_memory() - pmm_get_used_memory();
}

void pmm_print_stats(void) {
    size_t total = pmm_get_total_memory() / (1024 * 1024);
    size_t used = pmm_get_used_memory() / (1024 * 1024);
    size_t free = pmm_get_free_memory() / (1024 * 1024);
    
    kprintf("PMM Stats:\n");
    kprintf("  Total: %d MB\n", total);
    kprintf("  Used:  %d MB\n", used);
    kprintf("  Free:  %d MB\n", free);
}

// for VT support
// Allocate physically contiguous, 4KB-aligned memory
void *pmm_alloc_aligned(size_t size, size_t alignment) {
    size_t pages = (size + 4095) / 4096;
    size_t align_pages = alignment / 4096;
    
    // Find aligned block
    for (size_t i = 0; i < total_pages; i += align_pages) {
        bool found = true;
        for (size_t j = 0; j < pages; j++) {
            if (bitmap_test(i + j)) {
                found = false;
                break;
            }
        }
        
        if (found) {
            for (size_t j = 0; j < pages; j++) {
                bitmap_set(i + j);
            }
            return (void *)(i * 4096);
        }
    }
    
    return NULL;
}

// Free physically contiguous, 4KB-aligned memory
void pmm_free_aligned(void *ptr, size_t size) {
    if (!ptr || size == 0) return;
    size_t pages = (size + 4095) / 4096;
    uintptr_t phys = (uintptr_t)ptr;
    for (size_t i = 0; i < pages; i++) {
        pmm_free_page((void *)(phys + i * 4096));
    }
}

void test_pmm(void) {
    kprintf("Testing PMM...\n");
    
    // Test 1: Single allocation
    void *p1 = pmm_alloc_page();
    kprintf("Allocated page at: 0x%lx\n", (uintptr_t)p1);
    
    // Test 2: Multiple allocations
    void *pages[10];
    for (int i = 0; i < 10; i++) {
        pages[i] = pmm_alloc_page();
        kprintf("Page %d: 0x%lx\n", i, (uintptr_t)pages[i]);
    }
    
    // Test 3: Free and re-allocate
    pmm_free_page(pages[5]);
    void *p2 = pmm_alloc_page();
    kprintf("Freed pages[5], reallocated as: 0x%lx\n", (uintptr_t)p2);
    
    // Test 4: Contiguous allocation
    void *contig = pmm_alloc_pages(5);
    kprintf("Allocated 5 contiguous pages at: 0x%lx\n", (uintptr_t)contig);
    
    // Test 5: Stats
    pmm_print_stats();
    
    kprintf("PMM tests complete!\n");
}