#include <include/console.h>

void kernel_main(void){
    console_print("Hello from FableOS! \n");

    halt();
}

void halt(void){
    for(;;){
        __asm__ volatile ("hlt");
    }
}