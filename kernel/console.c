#include <kernel/console.h>

/** Prints to the console */
void console_print(const char *current_char) {
    while (*current_char != '\0') {
        putchar(*current_char);
        current_char++;
    }
}


void putchar(char c) {
    if (c == '\n') {
        serial_putchar('\r');
    }
    serial_putchar(c);
}


void serial_putchar(char c) {
    while ((inb(0x3F8 + 5) & 0x20) == 0);
    outb(0x3F8, c);
}
