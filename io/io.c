/** This reads a byte from the specified I/O port. */
unsigned char inb(unsigned int port) {
    unsigned char ret;
    __asm__ volatile (
        "inb %1, %0"
        : "=a"(ret)
        : "Nd"(port)
    );
    return ret;
}

/** This just writes a char to port for now. */
void outb(unsigned int port, unsigned char value) {
    __asm__ volatile (
        "outb %0, %1" 
        : 
        : "a"(value), "Nd"(port) // sends value to al and port to dx
    );
}
