

#include <vmm.h>
#include <pmm.h>
#include <kprintf.h>
#include <string.h>
#include <limine.h>
#include <slab.h>
#include <mm_constants.h>

extern volatile struct limine_hhdm_request hhdm_request;
extern volatile struct limine_memmap_request memmap_request;
extern volatile struct limine_executable_address_request kernel_address_request;

uint64_t* kernel_pml4 = NULL;
static uint64_t hhdm_offset = 0;

// Get index for a page table level (0=PT, 1=PD, 2=PDPT, 3=PML4)
static uint64_t get_index(uint64_t virt, int level) {
    return (virt >> (PT_SHIFT + level * 9)) & PT_INDEX_MASK;
}

// Convert physical address to virtual using HHDM
static void* phys_to_virt(uint64_t phys) {
    return (void*)(phys + hhdm_offset);
}

// Switch to a different page table
void vmm_switch_pml4(uint64_t* pml4) {
    if (!pml4) {
        kprintf("VMM Error: vmm_switch_pml4 called with NULL pml4\n");
        return;
    }
    
    uint64_t phys = (uint64_t)pml4 - hhdm_offset;
    asm volatile("mov %0, %%cr3" :: "r"(phys) : "memory");
}

// Map a single 4KB page
void vmm_map_page(uint64_t* pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
    if (!pml4) {
        kprintf("VMM Error: vmm_map_page called with NULL pml4\n");
        return;
    }
    
    if (!IS_PAGE_ALIGNED(virt)) {
        kprintf("VMM Warning: Virtual address 0x%lx not page-aligned\n", virt);
        virt = PAGE_ALIGN_DOWN(virt);
    }
    
    if (!IS_PAGE_ALIGNED(phys)) {
        kprintf("VMM Warning: Physical address 0x%lx not page-aligned\n", phys);
        phys = PAGE_ALIGN_DOWN(phys);
    }
    
    uint64_t* table = pml4;

    // Walk down to PT level, creating tables as needed
    for (int level = 3; level > 0; level--) {
        int index = get_index(virt, level);
        
        if (!(table[index] & PTE_PRESENT)) {
            uint64_t new_table_phys = (uint64_t)pmm_alloc_page();
            if (!new_table_phys) {
                kprintf("VMM Critical: Failed to allocate page table at level %d for virt 0x%lx\n",
                        level, virt);
                return;
            }
            
            uint64_t* new_table_virt = (uint64_t*)phys_to_virt(new_table_phys);
            memset(new_table_virt, 0, PAGE_SIZE);
            table[index] = new_table_phys | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
        }

        uint64_t next_phys = table[index] & PTE_ADDR_MASK;
        table = (uint64_t*)phys_to_virt(next_phys);
    }

    // Set the final PT entry
    int index = get_index(virt, 0);
    
    if (table[index] & PTE_PRESENT) {
        kprintf("VMM Warning: Remapping already mapped page at virt 0x%lx\n", virt);
    }
    
    table[index] = phys | flags;
    asm volatile("invlpg (%0)" :: "r" (virt) : "memory");
}

// Map a 2MB huge page (more efficient for large regions)
void vmm_map_huge_page(uint64_t* pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
    if (!pml4) {
        kprintf("VMM Error: vmm_map_huge_page called with NULL pml4\n");
        return;
    }
    
    if (!IS_LARGE_PAGE_ALIGNED(virt)) {
        kprintf("VMM Error: Virtual address 0x%lx not 2MB-aligned for huge page\n", virt);
        return;
    }
    
    if (!IS_LARGE_PAGE_ALIGNED(phys)) {
        kprintf("VMM Error: Physical address 0x%lx not 2MB-aligned for huge page\n", phys);
        return;
    }
    
    uint64_t* table = pml4;

    // Walk to PD level (stop before PT)
    for (int level = 3; level > 1; level--) {
        int index = get_index(virt, level);
        
        if (!(table[index] & PTE_PRESENT)) {
            uint64_t new_table_phys = (uint64_t)pmm_alloc_page();
            if (!new_table_phys) {
                kprintf("VMM Critical: Failed to allocate page table at level %d for huge page\n",
                        level);
                return;
            }
            
            uint64_t* new_table_virt = (uint64_t*)phys_to_virt(new_table_phys);
            memset(new_table_virt, 0, PAGE_SIZE);
            table[index] = new_table_phys | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
        }

        uint64_t next_phys = table[index] & PTE_ADDR_MASK;
        table = (uint64_t*)phys_to_virt(next_phys);
    }

    // Map as 2MB page with PS bit set
    int index = get_index(virt, 1);
    
    if (table[index] & PTE_PRESENT) {
        kprintf("VMM Warning: Remapping already mapped huge page at virt 0x%lx\n", virt);
    }
    
    table[index] = phys | flags | PTE_HUGE;
    asm volatile("invlpg (%0)" :: "r" (virt) : "memory");
}

void vmm_init(void) {
    if (!hhdm_request.response || !kernel_address_request.response || !memmap_request.response) {
        kprintf("VMM Critical: Missing Limine responses.\n");
        for (;;) asm("hlt");
    }

    hhdm_offset = hhdm_request.response->offset;

    // Allocate and zero kernel PML4
    uint64_t phys_pml4 = (uint64_t)pmm_alloc_page();
    if (!phys_pml4) {
        kprintf("VMM Critical: Failed to allocate PML4\n");
        for (;;) asm("hlt");
    }
    
    kernel_pml4 = (uint64_t*)phys_to_virt(phys_pml4);
    memset(kernel_pml4, 0, PAGE_SIZE);
    kprintf("VMM: Created PML4 at Phys 0x%x\n", phys_pml4);

    struct limine_memmap_response *memmap = memmap_request.response;
    struct limine_executable_address_response *kaddr = kernel_address_request.response;

    // Map kernel and modules
    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];

        if (e->type == LIMINE_MEMMAP_EXECUTABLE_AND_MODULES) {
            uint64_t virt_start = e->base + (kaddr->virtual_base - kaddr->physical_base);
            size_t pages = BYTES_TO_PAGES(e->length);
            
            for (size_t j = 0; j < pages; j++) {
                vmm_map_page(kernel_pml4, 
                            virt_start + PAGES_TO_BYTES(j), 
                            e->base + PAGES_TO_BYTES(j), 
                            PTE_KERNEL_DATA);
            }
            kprintf("VMM: Mapped Kernel at 0x%x (%d pages)\n", virt_start, pages);
        }
        else if (e->type == LIMINE_MEMMAP_FRAMEBUFFER) {
            size_t pages = BYTES_TO_PAGES(e->length);
            
            for (size_t j = 0; j < pages; j++) {
                vmm_map_page(kernel_pml4, 
                            e->base + PAGES_TO_BYTES(j), 
                            e->base + PAGES_TO_BYTES(j), 
                            PTE_KERNEL_DATA);
            }
            kprintf("VMM: Mapped Framebuffer at 0x%x (%d pages)\n", e->base, pages);
        }
    }

    // Map all physical memory via HHDM
    kprintf("VMM: Mapping HHDM...\n");
    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        size_t pages = BYTES_TO_PAGES(e->length);
        
        for (size_t j = 0; j < pages; j++) {
            vmm_map_page(kernel_pml4, 
                        e->base + PAGES_TO_BYTES(j) + hhdm_offset, 
                        e->base + PAGES_TO_BYTES(j), 
                        PTE_KERNEL_DATA);
        }
    }

    kprintf("VMM: Switching Page Tables...\n");
    vmm_switch_pml4(kernel_pml4);
    kprintf("VMM: Initialization complete\n");
}

// Unmap a single page and invalidate TLB
void vmm_unmap_page(uint64_t* pml4, uint64_t virt) {
    if (!pml4) {
        kprintf("VMM Error: vmm_unmap_page called with NULL pml4\n");
        return;
    }
    
    uint64_t* table = pml4;
    
    // Walk to PT level
    for (int level = 3; level > 0; level--) {
        int index = get_index(virt, level);
        
        if (!(table[index] & PTE_PRESENT)) {
            kprintf("VMM Warning: Attempted to unmap non-mapped page at 0x%lx\n", virt);
            return; // Already unmapped
        }
        
        uint64_t next_phys = table[index] & PTE_ADDR_MASK;
        table = (uint64_t*)phys_to_virt(next_phys);
    }
    
    // Clear PT entry
    int index = get_index(virt, 0);
    
    if (!(table[index] & PTE_PRESENT)) {
        kprintf("VMM Warning: Page at 0x%lx already unmapped\n", virt);
    }
    
    table[index] = 0;
    asm volatile("invlpg (%0)" :: "r"(virt) : "memory");
}

// Unmap a range of pages
void vmm_unmap_range(uint64_t* pml4, uint64_t virt_start, size_t size) {
    if (!pml4) {
        kprintf("VMM Error: vmm_unmap_range called with NULL pml4\n");
        return;
    }
    
    if (size == 0) {
        kprintf("VMM Warning: vmm_unmap_range called with size=0\n");
        return;
    }
    
    size_t pages = BYTES_TO_PAGES(size);
    
    for (size_t i = 0; i < pages; i++) {
        vmm_unmap_page(pml4, virt_start + PAGES_TO_BYTES(i));
    }
    
    // Full TLB flush for large ranges (more than 32 pages)
    if (pages > 32) {
        asm volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
    }
}

// Get physical address for a virtual address
uint64_t vmm_get_physical_address(uint64_t* pml4, uint64_t virt) {
    if (!pml4) {
        kprintf("VMM Error: vmm_get_physical_address called with NULL pml4\n");
        return 0;
    }
    
    uint64_t* table = pml4;
    
    // Walk to PT level
    for (int level = 3; level > 0; level--) {
        int index = get_index(virt, level);
        
        if (!(table[index] & PTE_PRESENT)) {
            return 0; // Not mapped
        }
        
        // Check for huge page at PD level
        if (level == 1 && (table[index] & PTE_HUGE)) {
            uint64_t phys_base = table[index] & ~(LARGE_PAGE_SIZE - 1);
            uint64_t offset = virt & (LARGE_PAGE_SIZE - 1);
            return phys_base | offset;
        }
        
        uint64_t next_phys = table[index] & PTE_ADDR_MASK;
        table = (uint64_t*)phys_to_virt(next_phys);
    }
    
    // Extract physical address from PT entry
    int index = get_index(virt, 0);
    if (!(table[index] & PTE_PRESENT)) {
        return 0;
    }
    
    return (table[index] & PTE_ADDR_MASK) | (virt & (PAGE_SIZE - 1));
}

// Create new address space with kernel mappings
uint64_t* vmm_create_address_space(void) {
    uint64_t phys_pml4 = (uint64_t)pmm_alloc_page();
    if (!phys_pml4) {
        kprintf("VMM Error: Failed to allocate PML4 for new address space\n");
        return NULL;
    }
    
    uint64_t* pml4 = (uint64_t*)phys_to_virt(phys_pml4);
    memset(pml4, 0, PAGE_SIZE);
    
    if (!kernel_pml4) {
        kprintf("VMM Error: kernel_pml4 not initialized\n");
        pmm_free_page((void*)phys_pml4);
        return NULL;
    }
    
    // Copy kernel mappings (higher half)
    for (int i = 256; i < PT_ENTRIES; i++) {
        pml4[i] = kernel_pml4[i];
    }
    
    return pml4;
}

// Destroy address space and free all page tables
void vmm_destroy_address_space(uint64_t* pml4) {
    if (!pml4) {
        kprintf("VMM Warning: vmm_destroy_address_space called with NULL pml4\n");
        return;
    }
    
    if (pml4 == kernel_pml4) {
        kprintf("VMM Error: Attempted to destroy kernel address space\n");
        return;
    }
    
    // Only free user-space mappings (PML4[0-255])
    for (int i = 0; i < 256; i++) {
        if (!(pml4[i] & PTE_PRESENT)) continue;
        
        uint64_t* pdpt = (uint64_t*)phys_to_virt(pml4[i] & PTE_ADDR_MASK);
        
        // Walk PDPT entries
        for (int j = 0; j < PT_ENTRIES; j++) {
            if (!(pdpt[j] & PTE_PRESENT)) continue;
            
            uint64_t* pd = (uint64_t*)phys_to_virt(pdpt[j] & PTE_ADDR_MASK);
            
            // Walk PD entries
            for (int k = 0; k < PT_ENTRIES; k++) {
                if (!(pd[k] & PTE_PRESENT)) continue;
                
                // Check if it's a huge page
                if (pd[k] & PTE_HUGE) continue;
                
                // Free PT
                pmm_free_page((void*)(pd[k] & PTE_ADDR_MASK));
            }
            
            // Free PD
            pmm_free_page((void*)(pdpt[j] & PTE_ADDR_MASK));
        }
        
        // Free PDPT
        pmm_free_page((void*)(pml4[i] & PTE_ADDR_MASK));
    }
    
    // Free PML4
    uint64_t phys = (uint64_t)pml4 - hhdm_offset;
    pmm_free_page((void*)phys);
}

// Map a range of pages
void vmm_map_range(uint64_t* pml4, uint64_t virt_start, uint64_t phys_start, 
                   size_t size, uint64_t flags) {
    if (!pml4) {
        kprintf("VMM Error: vmm_map_range called with NULL pml4\n");
        return;
    }
    
    if (size == 0) {
        kprintf("VMM Warning: vmm_map_range called with size=0\n");
        return;
    }
    
    size_t pages = BYTES_TO_PAGES(size);
    
    for (size_t i = 0; i < pages; i++) {
        vmm_map_page(pml4, 
                    virt_start + PAGES_TO_BYTES(i), 
                    phys_start + PAGES_TO_BYTES(i), 
                    flags);
    }
}

// Pre-allocate page tables for a range (prevents allocation during page faults)
void vmm_preallocate_range(uint64_t* pml4, uint64_t virt_start, size_t size) {
    if (!pml4) {
        kprintf("VMM Error: vmm_preallocate_range called with NULL pml4\n");
        return;
    }
    
    if (size == 0) {
        return;
    }
    
    for (uint64_t virt = virt_start; virt < virt_start + size; virt += LARGE_PAGE_SIZE) {
        uint64_t* table = pml4;
        
        // Walk down to PD level, creating tables as needed
        for (int level = 3; level > 1; level--) {
            int index = get_index(virt, level);
            
            if (!(table[index] & PTE_PRESENT)) {
                uint64_t new_table_phys = (uint64_t)pmm_alloc_page();
                if (!new_table_phys) {
                    kprintf("VMM Error: Failed to preallocate page table at level %d\n", level);
                    return;
                }
                
                uint64_t* new_table_virt = (uint64_t*)phys_to_virt(new_table_phys);
                memset(new_table_virt, 0, PAGE_SIZE);
                table[index] = new_table_phys | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
            }

            uint64_t next_phys = table[index] & PTE_ADDR_MASK;
            table = (uint64_t*)phys_to_virt(next_phys);
        }
    }
}

void test_vmm(void) {
    kprintf("\n=== Testing VMM ===\n");
    
    // Test 1: Single page mapping
    void *phys = pmm_alloc_page();
    if (!phys) {
        kprintf("Failed to allocate test page\n");
        return;
    }
    
    kprintf("Allocated physical page: 0x%lx\n", (uint64_t)phys);
    
    uint64_t virt = 0xDEAD000000;
    kprintf("Mapping virtual 0x%lx to physical 0x%lx\n", virt, (uint64_t)phys);
    vmm_map_page(kernel_pml4, virt, (uint64_t)phys, PTE_KERNEL_DATA);
    
    kprintf("Writing to virtual address...\n");
    volatile uint64_t *ptr = (uint64_t*)virt;
    *ptr = 0x123456789ABCDEF;
    kprintf("Single page: wrote 0x123456789ABCDEF, read back 0x%lx\n", *ptr);
    
    uint64_t phys_check = vmm_get_physical_address(kernel_pml4, virt);
    kprintf("Physical addr: 0x%lx (expected 0x%lx) %s\n", 
            phys_check, (uint64_t)phys, phys_check == (uint64_t)phys ? "y" : "n");
    
    // Test 2: Range mapping
    kprintf("\nAllocating 4 pages for range test...\n");
    void *phys_range = pmm_alloc_pages(4);
    if (!phys_range) {
        kprintf("Failed to allocate range\n");
        goto cleanup1;
    }
    
    kprintf("Allocated physical range: 0x%lx\n", (uint64_t)phys_range);
    
    uint64_t virt_range = 0xBEEF000000;
    kprintf("Mapping range: virt 0x%lx -> phys 0x%lx (4 pages)\n", virt_range, (uint64_t)phys_range);
    vmm_map_range(kernel_pml4, virt_range, (uint64_t)phys_range, PAGES_TO_BYTES(4), 
                  PTE_KERNEL_DATA);
    
    kprintf("Writing to range...\n");
    volatile uint64_t *range_ptr = (uint64_t*)virt_range;
    *range_ptr = 0xDEADBEEF;
    kprintf("Wrote 0xDEADBEEF, read back: 0x%lx %s\n", *range_ptr, 
            *range_ptr == 0xDEADBEEF ? "y" : "n");
    
    // Test 3: New address space
    kprintf("\nCreating new address space...\n");
    uint64_t *new_pml4 = vmm_create_address_space();
    if (!new_pml4) {
        kprintf("Failed to create new address space\n");
        goto cleanup2;
    }
    kprintf("Created new address space at 0x%lx \n", (uint64_t)new_pml4);
    
    // Cleanup
    kprintf("\nCleaning up...\n");
    vmm_destroy_address_space(new_pml4);
    
cleanup2:
    vmm_unmap_range(kernel_pml4, virt_range, PAGES_TO_BYTES(4));
    pmm_free_pages(phys_range, 4);
    
cleanup1:
    vmm_unmap_page(kernel_pml4, virt);
    pmm_free_page(phys);
    
    kprintf("VMM tests complete!\n\n");
}