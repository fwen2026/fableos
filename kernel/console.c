#include <kernel/console.h>
#include <io/serial.h>

/** Prints to the console character by character*/
void console_print(const char *current_char) {
    while (*current_char != '\0') {
        putchar(*current_char);
        current_char++;
    }
}
