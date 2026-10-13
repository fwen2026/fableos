#ifndef MATH_H
#define MATH_H

#include <stdint.h>

static inline unsigned int max(unsigned int a, unsigned int b) {
    return (a > b) ? a : b;
}

static inline unsigned int min(unsigned int a, unsigned int b) {
    return (a < b) ? a : b;
}

static inline uint64_t div_ceil(uint64_t a, uint64_t b) {
    return a / b + (a % b != 0);
}

#endif // MATH_H
