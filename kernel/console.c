#include <kernel/console.h>
#include <io/serial.h>
#include <stdarg.h>

/** Prints to the console character by character*/
void console_printf(const char *str, ...) {
    va_list args;
    va_start(args, str);

    while (*str != '\0') {
        if (*str == '%') {
            str ++;
            switch(*str){
                // TODO: add cases here
            }
        } 
        else {
            putchar(*str);
        }
        str++;
    }
    va_end(args);
}