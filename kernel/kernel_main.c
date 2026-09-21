#include <kernel/console.h>

void kernel_main(void){
    console_print("Hello from FableOS! \n");

    for(;;){
        __asm__ volatile ("hlt");
    }
}