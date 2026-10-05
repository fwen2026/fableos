#include <memory/pmm.h>
#include <stddef.h>
#include <util/math.h>

#define MAX_ORDER 10 // from min size for a page up to 2 MiB
#define PAGE_SIZE 0x1000 // 4 KiB

typedef struct {
    paddr_t addr;
} pmm_block_t; // represents a block of physical memory

typedef struct memory_node {
    pmm_block_t block;
    struct memory_node *next;
} memory_node_t; // represents a linked list of memory blocks

static memory_node_t *free_list[MAX_ORDER]; // 10 linked lists, one for each order


/** Adds a series of buddy-compatible segments to the free list, given a region of usable memory
 * @param base_addr The base address of the memory region
 * @param size The size of the memory region
 */
void pmm_add_block(paddr_t base_addr, size_t size) {
    // align base and size to 4kib
    paddr_t end_addr = (base_addr + size) & ~(0x1000 - 1); // rounds DOWN
    base_addr = (base_addr + 0x1000 - 1) & ~(0x1000 - 1);

    while (base_addr < end_addr) {
        uint64_t current_page = base_addr / PAGE_SIZE;
        uint64_t remaining_page = (end_addr - base_addr) / PAGE_SIZE;

        unsigned int alignment = (current_page == 0)
                                ? MAX_ORDER
                                : __builtin_ctzll(current_page); // counts zeroes from right

        unsigned int max_remaining = 63 - __builtin_clzll(remaining_page); // counts from left

        unsigned int order = min(min(alignment, max_remaining), MAX_ORDER);
        pmm_add_free_page(base_addr, order); // TODO: implement
        base_addr += (1ULL << order) * PAGE_SIZE; // increments the base address by the size of the allocated block
    }
}


/** Initializes each region in memory in aligned power-of-two blocks. 
 * First gets the total amount of physical memory available from Limine...
 * ... and then adds it to the free list.
*/
void pmm_init(void){
    // Initialize the physical memory manager
}