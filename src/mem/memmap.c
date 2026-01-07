#include <limine.h>
#include <kprintf.h>
#include <stdint.h>
#include <stddef.h>
#include <memmap.h>

extern volatile struct limine_memmap_request memmap_request;

// Helper to convert Limine types to readable strings
const char *get_memmap_type(uint64_t type) {
    switch (type) {
        case LIMINE_MEMMAP_USABLE: return "Usable";
        case LIMINE_MEMMAP_RESERVED: return "Reserved";
        case LIMINE_MEMMAP_ACPI_RECLAIMABLE: return "ACPI Reclaimable";
        case LIMINE_MEMMAP_ACPI_NVS: return "ACPI NVS";
        case LIMINE_MEMMAP_BAD_MEMORY: return "Bad Memory";
        case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE: return "Bootloader Reclaimable";
        case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES: return "Kernel/Modules";
        case LIMINE_MEMMAP_FRAMEBUFFER: return "Framebuffer";
        default: return "Unknown";
    }
}

void print_memmap(void) {
    struct limine_memmap_response *response = memmap_request.response;

    if (response == NULL) {
        kprintf("Error: No memory map received from bootloader.\n");
        return;
    }

    kprintf("--- Memory Map ---\n");
    kprintf("Entries: %d\n", response->entry_count);

    uint64_t total_usable_memory = 0;

    for (size_t i = 0; i < response->entry_count; i++) {
        struct limine_memmap_entry *entry = response->entries[i];
        
        // Print every entry
        kprintf("Base: 0x%x, Len: 0x%x, Type: %s\n", 
                entry->base, entry->length, get_memmap_type(entry->type));

        if (entry->type == LIMINE_MEMMAP_USABLE) {
            total_usable_memory += entry->length;
        }
    }

    kprintf("Total Usable RAM: %d MB\n", total_usable_memory / 1024 / 1024);
}