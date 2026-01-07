#include <vmm.h>
#include <pmm.h>
#include <kprintf.h>
#include <string.h>
#include <limine.h>

extern volatile struct limine_hhdm_request hhdm_request;
extern volatile struct limine_memmap_request memmap_request;
extern volatile struct limine_executable_address_request kernel_address_request;

uint64_t* kernel_pml4 = NULL;
static uint64_t hhdm_offset = 0;

// Helper: Get the index for a specific level
static uint64_t get_index(uint64_t virt, int level) {
    return (virt >> (12 + level * 9)) & 0x1FF;
}

// Helper: Get virtual address from physical
static void* phys_to_virt(uint64_t phys) {
    return (void*)(phys + hhdm_offset);
}

// Helper: Switch CR3 (Physical Address)
void vmm_switch_pml4(uint64_t* pml4) {
    uint64_t phys = (uint64_t)pml4 - hhdm_offset;
    asm volatile("mov %0, %%cr3" :: "r"(phys) : "memory");
}

void vmm_map_page(uint64_t* pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t* table = pml4;

    for (int level = 3; level > 0; level--) {
        int index = get_index(virt, level);
        
        if (!(table[index] & PTE_PRESENT)) {
            uint64_t new_table_phys = (uint64_t)pmm_alloc_page();
            uint64_t* new_table_virt = (uint64_t*)phys_to_virt(new_table_phys);
            memset(new_table_virt, 0, 4096);
            
            // Present | Writable | User
            table[index] = new_table_phys | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
        }

        uint64_t next_phys = table[index] & 0x000FFFFFFFFFF000;
        table = (uint64_t*)phys_to_virt(next_phys);
    }

    int index = get_index(virt, 0);
    table[index] = phys | flags;
    asm volatile("invlpg (%0)" :: "r" (virt) : "memory");
}

void vmm_init(void) {
    // Check responses
    if (!hhdm_request.response || !kernel_address_request.response || !memmap_request.response) {
        kprintf("VMM Critical: Missing Limine responses.\n");
        for (;;) asm("hlt");
    }

    hhdm_offset = hhdm_request.response->offset;

    // 1. Allocate Kernel PML4
    uint64_t phys_pml4 = (uint64_t)pmm_alloc_page();
    kernel_pml4 = (uint64_t*)phys_to_virt(phys_pml4);
    memset(kernel_pml4, 0, 4096);
    kprintf("VMM: Created PML4 at Phys 0x%x\n", phys_pml4);

    struct limine_memmap_response *memmap = memmap_request.response;
    // CORRECTED TYPE:
    struct limine_executable_address_response *kaddr = kernel_address_request.response;

    // 2. Map the Kernel and Modules
    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];

        if (e->type == LIMINE_MEMMAP_EXECUTABLE_AND_MODULES) {
            uint64_t virt_start = e->base + (kaddr->virtual_base - kaddr->physical_base);
            for (uint64_t j = 0; j < e->length; j += 4096) {
                vmm_map_page(kernel_pml4, virt_start + j, e->base + j, PTE_PRESENT | PTE_WRITABLE);
            }
            kprintf("VMM: Mapped Kernel at 0x%x\n", virt_start);
        }
        else if (e->type == LIMINE_MEMMAP_FRAMEBUFFER) {
             for (uint64_t j = 0; j < e->length; j += 4096) {
                vmm_map_page(kernel_pml4, e->base + j, e->base + j, PTE_PRESENT | PTE_WRITABLE);
            }
        }
    }

    // 3. Map HHDM (All Physical RAM)
    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        for (uint64_t j = 0; j < e->length; j += 4096) {
            vmm_map_page(kernel_pml4, e->base + j + hhdm_offset, e->base + j, 
                 PTE_PRESENT | PTE_WRITABLE);
        }
    }

    // 4. Switch
    kprintf("VMM: Switching Page Tables...\n");
    vmm_switch_pml4(kernel_pml4);
}

void vmm_unmap_page(uint64_t* pml4, uint64_t virt) {
    uint64_t* table = pml4;
    
    // Walk to the final page table
    for (int level = 3; level > 0; level--) {
        int index = get_index(virt, level);
        
        if (!(table[index] & PTE_PRESENT)) {
            return; // Already unmapped
        }
        
        uint64_t next_phys = table[index] & 0x000FFFFFFFFFF000;
        table = (uint64_t*)phys_to_virt(next_phys);
    }
    
    // Clear the entry
    int index = get_index(virt, 0);
    table[index] = 0;
    
    // Invalidate TLB
    asm volatile("invlpg (%0)" :: "r"(virt) : "memory");
}

uint64_t vmm_get_physical_address(uint64_t* pml4, uint64_t virt) {
    uint64_t* table = pml4;
    
    // Walk to the final page table
    for (int level = 3; level > 0; level--) {
        int index = get_index(virt, level);
        
        if (!(table[index] & PTE_PRESENT)) {
            return 0; // Not mapped
        }
        
        uint64_t next_phys = table[index] & 0x000FFFFFFFFFF000;
        table = (uint64_t*)phys_to_virt(next_phys);
    }
    
    // Get the physical address from the final entry
    int index = get_index(virt, 0);
    if (!(table[index] & PTE_PRESENT)) {
        return 0; // Not mapped
    }
    
    return (table[index] & 0x000FFFFFFFFFF000) | (virt & 0xFFF);
}

uint64_t* vmm_create_address_space(void) {
    uint64_t phys_pml4 = (uint64_t)pmm_alloc_page();
    uint64_t* pml4 = (uint64_t*)phys_to_virt(phys_pml4);
    memset(pml4, 0, 4096);
    
    // Optionally: Copy kernel mappings from kernel_pml4
    // (Higher half mappings at PML4[256-511])
    for (int i = 256; i < 512; i++) {
        pml4[i] = kernel_pml4[i];
    }
    
    return pml4;
}

void vmm_destroy_address_space(uint64_t* pml4) {
    // Walk and free all page tables
    // (Complex - implement when needed)
    // For now, just free the PML4:
    uint64_t phys = (uint64_t)pml4 - hhdm_offset;
    pmm_free_page((void*)phys);
}

// Additional helper functions can be added as needed.

void vmm_map_range(uint64_t* pml4, uint64_t virt_start, uint64_t phys_start, 
                   size_t size, uint64_t flags) {
    for (size_t offset = 0; offset < size; offset += 4096) {
        vmm_map_page(pml4, virt_start + offset, phys_start + offset, flags);
    }
}

// testing function to verify VMM functionality
void test_vmm(void) {
    kprintf("Testing VMM...\n");
    
    // Test 1: Map a new page
    void *phys = pmm_alloc_page();
    uint64_t virt = 0xDEAD000000;
    vmm_map_page(kernel_pml4, virt, (uint64_t)phys, PTE_PRESENT | PTE_WRITABLE);
    
    // Test 2: Write to it
    volatile uint64_t *ptr = (uint64_t*)virt;
    *ptr = 0x123456789ABCDEF;
    kprintf("Wrote 0x%lx to 0x%lx\n", *ptr, virt);
    
    // Test 3: Verify physical address
    uint64_t phys_check = vmm_get_physical_address(kernel_pml4, virt);
    kprintf("Physical address: 0x%lx (expected 0x%lx)\n", phys_check, (uint64_t)phys);
    
    // Test 4: Unmap
    vmm_unmap_page(kernel_pml4, virt);
    
    // Test 5: Create new address space
    uint64_t *new_pml4 = vmm_create_address_space();
    kprintf("Created new PML4 at: 0x%lx\n", (uint64_t)new_pml4);
    
    kprintf("VMM tests complete!\n");
}