#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

void *kmalloc(size_t size);
void kfree(void *addr);