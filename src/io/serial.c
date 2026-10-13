#include <io/serial.h>
#include <io/io.h>

/** Resets COM1 to default */
void serial_init() {
    outb(0x3F8 + 1, 0x00); // disable interrupts
    outb(0x3F8 + 3, 0x80); // configures divisor
    outb(0x3F8 + 0, 0x01); // Set divisor to 1 (lo byte) 115200 baud
    outb(0x3F8 + 1, 0x00); //                  (hi byte)
    outb(0x3F8 + 3, 0x03); // 8n1
    outb(0x3F8 + 2, 0xC7); // enable FIFO
    outb(0x3F8 + 4, 0x0B); // config
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