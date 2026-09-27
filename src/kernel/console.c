#include <kernel/console.h>
#include <io/serial.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>

/** Integer printing, handles signs and hex/dec */
static void print_int(uint64_t val, bool is_positive, bool hex){
    char buf[20]; // Buffer to hold the string representation
    int max_idx = 0;

    if (!is_positive) {
        putchar('-');
        val = -val;
    }

    for (int j = 0; j < 20; j++) {
        int digit = hex ? (val % 16) : (val % 10);

        if (digit < 10) {
            buf[j] = digit + '0';
        } else {
            buf[j] = 'a' + (digit - 10);
        }

        val /= hex ? 16 : 10;
        if (val == 0) {
            max_idx = j;
            break;
        }
    }

    for (int j = max_idx; j >= 0; j--) {
        putchar(buf[j]);
    }
}


/** Prints a string to the console */
static void print_string(const char *str) {
    while (*str != '\0') {
        putchar(*str);
        str++;
    }
}

/** Prints to the console character by character*/
void console_printf(const char *str, ...) {
    va_list args;
    va_start(args, str);

    while (*str != '\0') {
        if (*str == '%') {
            str++;
            if(*str == '\0'){ break; }

            const char *temp = str;

            switch(*temp){
                case 'c': {
                    char c = (char)va_arg(args, int);
                    putchar(c);
                    str = temp + 1;
                    break;
                }
                case 'p': {
                    void *ptr = va_arg(args, void *);
                    print_string("0x");
                    print_int((uintptr_t)ptr, true, true);
                    str = temp + 1;
                    break;
                }
                case 's': {
                    const char *s = va_arg(args, const char *);
                    if(!s) {
                        s = "(null)";
                    }
                    print_string(s);
                    str = temp + 1;
                    break;
                }
                case 'b': {
                    bool b = (bool)va_arg(args, int);
                    if (b) {
                        print_string("true");
                    } else {
                        print_string("false");
                    }
                    str = temp + 1;
                    break;
                }
                case 'd': {
                    int i = va_arg(args, int);

                    print_int(i, i >= 0, false);
                    str = temp + 1; // remember to add this line. its not for free now.
                    break;
                }
                case 'l': {
                    temp++;

                    switch(*temp) {
                        case 'x': {
                            uint64_t i = va_arg(args, uint64_t);

                            print_int(i, true, true);
                            str = temp + 1;
                            break;
                        }
                        case 'u': {
                            uint64_t i = va_arg(args, uint64_t);

                            print_int(i, true, false);
                            str = temp + 1;
                            break;
                        }
                        case 'd': {
                            int64_t i = va_arg(args, int64_t);

                            print_int(i, i >= 0, false);
                            str = temp + 1;
                            break;
                        }
                        default: {
                            break;
                        }
                    }

                    break;
                }
                default: {
                    putchar(*str);
                    str = temp + 1;
                    break;
                }
            } 
        }
        else {
            putchar(*str);
            str++;
        }
    }
    va_end(args);
}