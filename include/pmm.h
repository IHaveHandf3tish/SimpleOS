#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>
#include <limine.h> 

// Initializes the Physical Memory Manager.
// Requires the memory map and HHDM response passed from kmain.
void pmm_init(struct limine_memmap_response *memmap, struct limine_hhdm_response *hhdm);

// Allocates a single 4KiB page.
// Returns the PHYSICAL address of the page, or NULL if out of memory.
void *pmm_alloc_page(void);

// Frees a single 4KiB page.
// Expects a PHYSICAL address.
void pmm_free_page(void *ptr);

size_t pmm_get_total_memory(void);
size_t pmm_get_used_memory(void);
size_t pmm_get_free_memory(void);
void pmm_print_stats(void);
// for VT support
// Allocate physically contiguous, 4KB-aligned memory
void *pmm_alloc_aligned(size_t size, size_t alignment);
// Free physically contiguous, 4KB-aligned memory
void pmm_free_aligned(void *ptr, size_t size);
//testing function to verify PMM functionality
void test_pmm(void);

#endif