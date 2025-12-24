
#include "pic.h"
#include <io.h>

void pic_send_eoi(uint8_t irq) {
    if(irq >= 8)
        outb(PIC2_COMMAND, PIC_EOI);

    outb(PIC1_COMMAND, PIC_EOI);
}

void init_pic(void) {
    // Save masks just in case
    uint8_t a1 = inb(PIC1_DATA); 
    uint8_t a2 = inb(PIC2_DATA);

    // --- ICW1: Start initialization ---
    outb(PIC1_COMMAND, 0x11);
    io_wait();
    outb(PIC2_COMMAND, 0x11);
    io_wait();

    // --- ICW2: Remap offsets ---
    // Master PIC vector offset (0x20 = 32)
    // IRQ 0..7 -> Int 32..39
    outb(PIC1_DATA, 0x20); 
    io_wait();
    
    // Slave PIC vector offset (0x28 = 40)
    // IRQ 8..15 -> Int 40..47
    outb(PIC2_DATA, 0x28); 
    io_wait();

    // --- ICW3: Cascade Setup ---
    outb(PIC1_DATA, 4); // Tell Master there is a Slave at IRQ2 (0000 0100)
    io_wait();
    outb(PIC2_DATA, 2); // Tell Slave its cascade identity (0000 0010)
    io_wait();

    // --- ICW4: 8086 Mode ---
    outb(PIC1_DATA, 0x01);
    io_wait();
    outb(PIC2_DATA, 0x01);
    io_wait();

    // --- Restore or Set Masks ---
    outb(PIC1_DATA, 0xFF); 
    outb(PIC2_DATA, 0xFF);
}

// Helper to enable a specific IRQ (e.g., keyboard is IRQ1)
void pic_clear_mask(uint8_t irq_line) {
    uint16_t port;
    uint8_t value;

    if(irq_line < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq_line -= 8;
    }
    value = inb(port) & ~(1 << irq_line);
    outb(port, value);
}