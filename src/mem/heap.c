#include <stdint.h>
#include <stddef.h>
#include <limine.h>
#include <kprintf.h>
#include <string.h>
#include <stdbool.h>
#include <pmm.h>
#include <vmm.h>


// Simple heap structure:
// 1. Use buddy PMM for large (>2KB) allocations
// 2. Use slab caches for common small sizes (16, 32, 64, 128, 256, 512 bytes)
// 3. Each slab = 1 page with objects of same size