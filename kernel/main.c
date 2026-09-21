#include <include/console.h>

void kernel_main(void){
    console_print("Hello from FableOS! \n");

    hcf();
}

void hcf(void){
    for(;;){
        __asm__ volatile ("hlt");
    }
}