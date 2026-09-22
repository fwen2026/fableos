#include <io/serial.h>
#include <io/io.h>

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