#include <kernel/console.h>
#include <io/io.h>

/** Prints to the console character by character*/
void console_print(const char *current_char) {
    while (*current_char != '\0') {
        putchar(*current_char);
        current_char++;
    }
}

/** Sends a character to the serial port */
void serial_putchar(char c) {
    while ((inb(0x3F8 + 5) & 0x20) == 0);
    outb(0x3F8, c);
}


/** Prints a single char in the console */
void putchar(char c) {
    if (c == '\n') {
        serial_putchar('\r');
    }
    serial_putchar(c);
}
