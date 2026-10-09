#pragma once

#include <stdint.h>

typedef uint64_t psize_t;
typedef uint64_t paddr_t;

typedef struct {
    paddr_t addr;
    psize_t size;
} pmm_region_t;

typedef struct {
    pmm_region_t *regions; // TODO: find a place where this can live
    uint64_t count;
} pmm_regions_t;


void pmm_init(void);
void log_memory_map(void);
pmm_regions_t *pmm_alloc(psize_t size);
void pmm_free(paddr_t addr, psize_t size);
void pmm_get_stats(pmm_regions_t *regions);