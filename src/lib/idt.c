#include <idt.h>
#include <kprintf.h>
#include <stdint.h>
#include <string.h> 
#include "pic.h"

__attribute__((aligned(0x10))) 
static struct idt_entry idt[256];

static struct idt_ptr idtr;

extern void load_idt(void *ptr);
extern void isr1(void);
extern void isr_page_fault(void);

void idt_set_descriptor(uint8_t vector, void *isr, uint8_t flags) {
    struct idt_entry *entry = &idt[vector];

    
    uint64_t addr = (uintptr_t)isr;

    entry->offset_low  = addr & 0xFFFF; 
    entry->selector    = 0x08; // Kernel Code Segment
    entry->ist         = 0;
    entry->attributes  = flags;
    entry->offset_mid  = (addr >> 16) & 0xFFFF;
    entry->offset_high = (addr >> 32) & 0xFFFFFFFF;
    entry->reserved    = 0;
}

void exception_handler(void) {
    kprintf("INTERRUPT RECEIVED! Kernel is alive.\n");
    __asm__ volatile ("cli; hlt"); 
}

void init_idt(void) {
    memset(&idt, 0, sizeof(struct idt_entry) * 256);

    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (uintptr_t)&idt;

    // Set up Exception Handlers (ISR 0-31)
    idt_set_descriptor(1, isr1, 0x8E);
    idt_set_descriptor(14, isr_page_fault, 0x8E);

    // Initialize and Remap the PIC
    init_pic();
    kprintf("PIC Remapped and Initialized.\n");

    // Load the table
    load_idt(&idtr);
    kprintf("IDT Loaded.\n");
    
    // Enable Interrupts
    __asm__ volatile ("sti"); 
    kprintf("Interrupts Enabled.\n");
}

void page_fault_handler(uint64_t error_code, uint64_t fault_addr) {
    kprintf("\n=== PAGE FAULT ===\n");
    kprintf("Address: 0x%lx\n", fault_addr);
    kprintf("Error Code: 0x%lx\n", error_code);
    kprintf("  Present: %s\n", (error_code & 1) ? "yes" : "no");
    kprintf("  Write: %s\n", (error_code & 2) ? "yes" : "no");
    kprintf("  User: %s\n", (error_code & 4) ? "yes" : "no");
    kprintf("  Reserved: %s\n", (error_code & 8) ? "yes" : "no");
    kprintf("  Instruction: %s\n", (error_code & 16) ? "yes" : "no");
    
    // Halt for now
    for (;;) asm("hlt");
}