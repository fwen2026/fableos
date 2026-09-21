void main(void){
    for(;;){
        console_print("Hello from FableOS! \n");
        __asm__ volatile ("hlt");
    }
}