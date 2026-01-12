#ifndef MM_CONSTANTS_H
#define MM_CONSTANTS_H

// Page sizes
#define PAGE_SIZE           4096
#define PAGE_SHIFT          12
#define LARGE_PAGE_SIZE     (2 * 1024 * 1024)  // 2MB
#define LARGE_PAGE_SHIFT    21

// Page alignment macros
#define PAGE_ALIGN_DOWN(addr) ((addr) & ~(PAGE_SIZE - 1))
#define PAGE_ALIGN_UP(addr)   (((addr) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define IS_PAGE_ALIGNED(addr) (((addr) & (PAGE_SIZE - 1)) == 0)

#define LARGE_PAGE_ALIGN_DOWN(addr) ((addr) & ~(LARGE_PAGE_SIZE - 1))
#define LARGE_PAGE_ALIGN_UP(addr)   (((addr) + LARGE_PAGE_SIZE - 1) & ~(LARGE_PAGE_SIZE - 1))
#define IS_LARGE_PAGE_ALIGNED(addr) (((addr) & (LARGE_PAGE_SIZE - 1)) == 0)

// Pages <-> bytes conversion
#define BYTES_TO_PAGES(bytes) (((bytes) + PAGE_SIZE - 1) / PAGE_SIZE)
#define PAGES_TO_BYTES(pages) ((pages) * PAGE_SIZE)

// PMM specific
#define PMM_MAX_ORDER           11
#define PMM_MIN_ORDER           0
#define PMM_MAX_CONTIGUOUS_PAGES (1UL << PMM_MAX_ORDER)  // 2048 pages
#define PMM_MAX_CONTIGUOUS_BYTES (PMM_MAX_CONTIGUOUS_PAGES * PAGE_SIZE)  // 8MB

// Protected regions
#define FIRST_MB_BYTES          (1024 * 1024)
#define FIRST_MB_PAGES          (FIRST_MB_BYTES / PAGE_SIZE)  // 256

// VMM page table flags
#define PTE_PRESENT     (1UL << 0)
#define PTE_WRITABLE    (1UL << 1)
#define PTE_USER        (1UL << 2)
#define PTE_WRITETHROUGH (1UL << 3)
#define PTE_CACHE_DISABLE (1UL << 4)
#define PTE_ACCESSED    (1UL << 5)
#define PTE_DIRTY       (1UL << 6)
#define PTE_HUGE        (1UL << 7)  // PS bit for 2MB pages
#define PTE_GLOBAL      (1UL << 8)
#define PTE_NO_EXECUTE  (1UL << 63)

// Common flag combinations
#define PTE_KERNEL_DATA  (PTE_PRESENT | PTE_WRITABLE)
#define PTE_KERNEL_CODE  (PTE_PRESENT)
#define PTE_USER_DATA    (PTE_PRESENT | PTE_WRITABLE | PTE_USER)
#define PTE_USER_CODE    (PTE_PRESENT | PTE_USER)

// Page table address mask
#define PTE_ADDR_MASK    0x000FFFFFFFFFF000UL

// Page table indices
#define PT_ENTRIES       512
#define PML4_SHIFT       39
#define PDPT_SHIFT       30
#define PD_SHIFT         21
#define PT_SHIFT         12
#define PT_INDEX_MASK    0x1FF

// Heap allocator
#define SLAB_MIN_SIZE    16
#define SLAB_MAX_SIZE    2048
#define NUM_SLAB_CLASSES 8

#endif // MM_CONSTANTS_H