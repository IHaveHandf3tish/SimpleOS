#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stddef.h>

// Page Table Entry Flags
#define PTE_PRESENT   (1ull << 0)
#define PTE_WRITABLE  (1ull << 1)
#define PTE_USER      (1ull << 2)
#define PTE_NX        (1ull << 63) // No Execute

void vmm_init(void);
void vmm_map_page(uint64_t* pml4, uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_switch_pml4(uint64_t* pml4);

extern uint64_t* kernel_pml4;

void vmm_unmap_page(uint64_t* pml4, uint64_t virt);
uint64_t vmm_get_physical_address(uint64_t* pml4, uint64_t virt);
uint64_t* vmm_create_address_space(void);
void vmm_destroy_address_space(uint64_t* pml4);
void vmm_map_range(uint64_t* pml4, uint64_t virt_start, uint64_t phys_start, size_t length, uint64_t flags);
// testing function to verify VMM functionality
void test_vmm(void);


#endif