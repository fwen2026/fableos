#ifndef HCF_H
#define HCF_H

static inline void hcf(){
    for(;;) {
        __asm__ volatile ("hlt");
    }
}

#endif // HCF_H