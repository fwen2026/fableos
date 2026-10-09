#ifndef HCF_H
#define HCF_H

__attribute__((noreturn))
static inline void hcf(void){
    for(;;) {
        __asm__ volatile ("hlt");
    }
}

#endif // HCF_H