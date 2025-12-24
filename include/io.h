#ifndef IO_H
#define IO_H

#include <stdint.h>

// Send a byte to a hardware port
void outb(uint16_t port, uint8_t val);

// Read a byte from a hardware port
uint8_t inb(uint16_t port);
void io_wait(void);

#endif // IO_H