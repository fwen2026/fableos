#include <include/console.h>

void hcf(void){
    for(;;){
        __asm__ volatile ("hlt");
    }
}

void kmain(void){
    console_print("Hello from FableOS! \n");

    hcf();
}