#include <include/io.h>
#include <stdint.h>

/** This reads a byte from the specified I/O port. */
uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile (
        "inb %1, %0"
        : "=a"(ret)
        : "Nd"(port)
    );
    return ret;
}


/** This just writes a char to port for now. */
void outb(uint16_t port, uint8_t value) {
    __asm__ volatile (
        "outb %0, %1" 
        : 
        : "a"(value), "Nd"(port) // sends value to al and port to dx
    );
}
