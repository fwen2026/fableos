void main(void){
    console_print("Hello from FableOS! \n");
    
    for(;;){
        __asm__ volatile ("hlt");
    }
}