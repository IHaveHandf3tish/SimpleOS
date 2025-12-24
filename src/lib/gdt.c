#include <gdt.h>
#include <stdint.h>

struct gdt_descriptor {
    uint16_t size;
    uint64_t offset;
} __attribute__((packed));

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_middle;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} __attribute__((packed));

__attribute__((aligned(4096)))
static struct gdt_entry gdt[5];
static struct gdt_descriptor gdtr;

extern void load_gdt(void *gdtr_pointer);

void init_gdt(void) {
    // Null descriptor
    gdt[0] = (struct gdt_entry){0, 0, 0, 0, 0, 0};

    // Code segment descriptor
       gdt[1] = (struct gdt_entry){
       .limit_low = 0x0000,
       .base_low = 0x0000,
       .base_middle = 0x00,
       .access = 0x9A,        // Present, Ring 0, Code, Executable, Readable
       .granularity = 0xA0,   // G=1, L=1 (64-bit), D=0
       .base_high = 0x00
   };
    // Data segment descriptor
    gdt[2] = (struct gdt_entry){
        .limit_low = 0xFFFF,
        .base_low = 0x0000,
        .base_middle = 0x00,
        .access = 0x92, // Present, Ring 0, Data Segment, Writable
        .granularity = 0x00, // 4KB granularity, 32-bit
        .base_high = 0x00
    };

    // User mode code segment descriptor
    gdt[3] = (struct gdt_entry){
        .limit_low = 0xFFFF,
        .base_low = 0x0000,
        .base_middle = 0x00,
        .access = 0xFA, // Present, Ring 3, Code Segment, Executable, Readable
        .granularity = 0xA0, // 4KB granularity, 32-bit
        .base_high = 0x00
    };

    // User mode data segment descriptor
    gdt[4] = (struct gdt_entry){
        .limit_low = 0xFFFF,
        .base_low = 0x0000,
        .base_middle = 0x00,
        .access = 0xF2, // Present, Ring 3, Data Segment, Writable
        .granularity = 0x00, // 4KB granularity, 32-bit
        .base_high = 0x00
    };

    gdtr.size = sizeof(gdt) - 1;
    gdtr.offset = (uint64_t)&gdt;

    load_gdt(&gdtr);
}