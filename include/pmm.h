#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>
#include <limine.h> 


void pmm_init(struct limine_memmap_response *memmap, struct limine_hhdm_response *hhdm);

// Allocate a single page (4KB)
void *pmm_alloc_page(void);

// Free a single page
void pmm_free_page(void *page);

// Allocate multiple contiguous pages
void *pmm_alloc_pages(size_t count);

// Free multiple contiguous pages
void pmm_free_pages(void *pages, size_t count);

// Allocate aligned memory
void *pmm_alloc_aligned(size_t size, size_t alignment);

// Free aligned memory
void pmm_free_aligned(void *ptr, size_t size);

// Get total system memory in bytes
size_t pmm_get_total_memory(void);

// Get used memory in bytes
size_t pmm_get_used_memory(void);

// Get free memory in bytes
size_t pmm_get_free_memory(void);

// Print memory statistics
void pmm_print_stats(void);

// Test PMM functionality
void test_pmm(void);

#endif // PMM_H