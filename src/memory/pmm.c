#include <memory/pmm.h>
#include "limine.h"
#include <stddef.h>
#include <util/math.h>

#define MAX_ORDER 9 // from min size for a page up to 2 MiB
#define PAGE_SIZE 0x1000 // 4 KiB

static uintptr_t hhdm_offset;

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0
};

void get_hhdm_offset(void) {
    if(request.response == NULL) return;
    hhdm_offset = request.response->offset;
}

typedef struct {
    paddr_t addr;
} pmm_block_t; // represents a block of physical memory

typedef struct memory_node {
    struct memory_node *next;
    struct memory_node *prev;
} memory_node_t; // represents a doubly linked list of memory blocks. each node is stored at its representative block's address

static memory_node_t *free_list[MAX_ORDER + 1]; // 10 linked lists, one for each order


/** Adds a new node to the front of the list */
static void list_add(memory_node_t **head, memory_node_t *new_node) {
    if (!head || !new_node || *head == new_node) return; // adding the head again would make it point to itself

    new_node->next = *head; // takes (*new_node).next. assigns it to (*head). c is weird sometimes
    new_node->prev = NULL;

    if (*head) {
        (*head)->prev = new_node;
    }

    *head = new_node;
}


/** Removes a node from the front of the list
 * @returns The removed node, or NULL if the list was empty
 */
__attribute__((unused)) // not called until pmm_alloc exists; drop this then
static memory_node_t *list_remove(memory_node_t **head) {
    if(!head || !*head) return NULL;

    memory_node_t *node = *head; // temp
    *head = node->next; // mutates head

    if (*head) {
        (*head)->prev = NULL;
    }

    // removes the node from the list
    node->next = NULL;
    node->prev = NULL;

    return node;
}


/** Adds a free page to the free list, at the specified order*/
void pmm_add_free_page(paddr_t addr, unsigned int order) {
    if (order > MAX_ORDER) return;
    if (addr & (((paddr_t)PAGE_SIZE << order) - 1)) return; // must be aligned to its block size

    get_hhdm_offset(); // ensures hhdm_offset is set

    memory_node_t *new_node = (memory_node_t *)(addr + hhdm_offset); // cast the address to a memory_node_t pointer
    list_add(&free_list[order], new_node); // adds the new node to the free list
}


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