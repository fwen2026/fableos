#include <kernel/console.h>
#include <limine.h>

void hcf(void){
    for(;;){
        __asm__ volatile ("hlt");
    }
}

void kmain(void){
    console_print("Hello from FableOS! \n");

    hcf();
}