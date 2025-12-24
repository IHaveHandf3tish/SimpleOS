#include <kprintf.h>
#include <stdarg.h>
#include <serial.h>
#include <string.h>
#include <stdbool.h>

void kputchar(char c) {
    if (c == '\n') {
        write_serial('\r'); // Move to start of line
    }
    write_serial(c);
}

void kputstring(const char *str) {
    write_string_serial(str);
}

static void kprint_int(long value, int base, bool is_signed) {
    char buffer[64];
    const char digits[] = "0123456789abcdef";
    bool negative = false;
    size_t i = 0;
    if (value == 0) {
        kputchar('0');
        return;
    }
    unsigned long u_value;

    if (is_signed && value < 0) {
        negative = true;
        u_value = -(unsigned long)value;
    } else {
        u_value = (unsigned long)value;
    }
    while (u_value != 0) {
        buffer[i++] = digits[u_value % base];
        u_value /= base;
    }
    if (negative) {
        buffer[i++] = '-';
    }
    for (size_t j = 0; j < i; j++) {
        kputchar(buffer[i - j - 1]);
    }
}
void kprintf(const char *format, ...) {
    va_list args;
    va_start(args, format);

    for (size_t i = 0; format[i] != '\0'; i++) {
        if (format[i] == '%') {
            i++;
            switch (format[i]) {
                case 'd': {
                    int value = va_arg(args, int);
                    kprint_int(value, 10, true);
                    break;
                }
                case 'u': {
                    unsigned int value = va_arg(args, unsigned int);
                    kprint_int((int)value, 10, false);
                    break;
                }
                case 'x': {
                    unsigned int value = va_arg(args, unsigned int);
                    kprint_int((int)value, 16, false);
                    break;
                }
                case 'c': {
                    char c = (char)va_arg(args, int);
                    kputchar(c);
                    break;
                }
                case 's': {
                    const char *str = va_arg(args, const char *);
                    kputstring(str);
                    break;
                }
                case '%': {
                    kputchar('%');
                    break;
                }
                default:
                    // Unsupported format specifier, print it as is
                    kputchar('%');
                    kputchar(format[i]);
                    break;
            }
        } else {
            kputchar(format[i]);
        }
    }

    va_end(args);
}
