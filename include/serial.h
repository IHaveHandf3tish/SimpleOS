#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <io.h>

#define COM1 0x3f8

int init_serial();
int is_transmit_empty();
void write_serial(char a);
void write_string_serial(const char *str);

#endif // SERIAL_H