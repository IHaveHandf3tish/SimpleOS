#ifndef IDT_H
#define IDT_H
#include <stdint.h>

// The struct defined by the x86_64 architecture
struct idt_entry {
    uint16_t offset_low;       // Lower 16 bits of handler address
    uint16_t selector;         // Kernel Code Segment selector (0x08)
    uint8_t  ist;              // Interrupt Stack Table (usually 0)
    uint8_t  attributes;       // Type and attributes
    uint16_t offset_mid;       // Middle 16 bits of handler address
    uint32_t offset_high;      // Upper 32 bits of handler address
    uint32_t reserved;         // Must be zero
} __attribute__((packed));

// Pointer structure used by 'lidt' instruction
struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

void init_idt(void);

#endif // IDT_H