#include <memory/pmm.h>
#include "limine.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <util/math.h>
#include <util/hcf.h>

#define MAX_ORDER 9 // from min size for a page up to 2 MiB
#define PAGE_SIZE 0x1000 // 4 KiB


__attribute__((used, section(".limine_requests"))) 
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0
};

static uintptr_t hhdm_offset;

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0
};

void get_hhdm_offset(void) {
    if(request.response == NULL){
        log_error("Failed to get HHDM offset");
        hcf();
    };
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

typedef struct {
    uint64_t *value;
    size_t size;
} bitmap_t;

static bitmap_t bitmap_list[MAX_ORDER + 1];


/** Constructs a bitmap
 * @param addr The address of the bitmap
 * @param size The number of bits represented in the bitmap
 */
bitmap_t bitmap_init(void *addr, size_t size) {
    bitmap_t bitmap;
    bitmap.size = size;
    bitmap.value = (uint64_t *)addr;
    int num_ints = size / 64 + (size % 64 != 0); // calculate the num of ints we need
    for (int i = 0; i < num_ints; i++) {
        bitmap.value[i] = UINT64_MAX; // set all bits to 1. clear when pmm_init
    }
    return bitmap;
}


/** Sets a bit to the specified value.
 * @param bitmap The bitmap to modify
 * @param index The index of the bit to set
 * @param value The value to set the bit to. True is allocated. False is free.
 */
 void bitmap_set_value(bitmap_t *bitmap, size_t index, bool value) {
    if (!bitmap || index >= bitmap->size) return; // return if out of bounds or doesn't exist

    if (value) {
        bitmap->value[index / 64] |= (1ULL << (index % 64));
    } else {
        bitmap->value[index / 64] &= ~(1ULL << (index % 64));
    }
}


void bitmap_clear(bitmap_t *bitmap, size_t index) {bitmap_set_value(bitmap, index, false);}
void bitmap_set(bitmap_t *bitmap, size_t index) {bitmap_set_value(bitmap, index, true);}


bool bitmap_get(bitmap_t *bitmap, size_t index) {
    if (!bitmap || index >= bitmap->size) return false; 

    return (bitmap->value[index / 64] & (1ULL << (index % 64))) != 0;
}


/** Gets the size of the bitmap, in bytes. */
size_t bitmap_get_size(bitmap_t *bitmap) {
    if (!bitmap) return 0;
    uint64_t num_ints = bitmap->size / 64 + (bitmap->size % 64 != 0);
    return num_ints * sizeof(uint64_t);
}

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


/** Adds a free page to the free list, at the specified order and at the specified address*/
void pmm_add_free_page(paddr_t addr, unsigned int order) {
    if (order > MAX_ORDER) return;
    if (addr & (((paddr_t)PAGE_SIZE << order) - 1)) return; // must be aligned to its block size

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
    base_addr = (base_addr + 0x1000 - 1) & ~(0x1000 - 1); // rounds UP

    while (base_addr < end_addr) {
        uint64_t current_page = base_addr / PAGE_SIZE;
        uint64_t remaining_page = (end_addr - base_addr) / PAGE_SIZE;

        unsigned int alignment = (current_page == 0)
                                ? MAX_ORDER
                                : __builtin_ctzll(current_page); // counts zeroes from right

        unsigned int max_remaining = 63 - __builtin_clzll(remaining_page); // counts from left

        unsigned int order = min(min(alignment, max_remaining), MAX_ORDER);
        pmm_add_free_page(base_addr, order);
        bitmap_clear(&bitmap_list[order], base_addr / (PAGE_SIZE * (1ULL << order)));
        base_addr += (1ULL << order) * PAGE_SIZE; // increments the base address by the size of the allocated block
    }
}


/** Initializes each region in memory in aligned power-of-two blocks. First gets the total amount 
 * of physical memory available from Limine and then adds it to the free list.
*/
void pmm_init(void){
    if (memmap_request.response == NULL) {
        log_error("Memory map request failed.\n");
        hcf();
    }

    console_printf("Memory map request received.\n");
    get_hhdm_offset(); // ensures hhdm_offset is set

    size_t zero_orders = 0;
    int highest_usable_index = -1;
    struct limine_memmap_response *response = memmap_request.response;

    for(uint64_t i = 0; i < response->entry_count; i++) {
        struct limine_memmap_entry *entry = response->entries[i];
        if(entry->type == LIMINE_MEMMAP_USABLE) {
            highest_usable_index = i;
        }
    }

    if (highest_usable_index == -1) {
        log_error("No usable memory found.\n");
        hcf();
    }

    struct limine_memmap_entry *entry = response->entries[highest_usable_index];
    zero_orders = (entry->base + entry->length) / PAGE_SIZE;

    size_t net_bytes = 0;

    for(uint64_t i = 0; i < 10; i++) {
        uint64_t num_blocks = zero_orders / (1ULL << i) + (zero_orders % (1ULL << i) != 0); // ceil
        uint64_t words = num_blocks / 64 + (num_blocks % 64 != 0);
        net_bytes += words * sizeof(uint64_t);
    }

    uint64_t idx = -1;
    for(uint64_t i = 0; i < response->entry_count; i++) {
        struct limine_memmap_entry *entry = response->entries[i];
        if(entry->type == LIMINE_MEMMAP_USABLE && entry->length >= net_bytes) {
            idx = i;
            break;
        }
    }

    if(idx == -1) {
        log_error("No suitable memory region found.\n");
        hcf();
    }

    struct limine_memmap_entry *bitmap_entry = response->entries[idx];
    void *cursor = (void *) (bitmap_entry->base + hhdm_offset);
    for(uint64_t i = 0; i < 10; i++) {
        uint64_t num_blocks = zero_orders / (1ULL << i) + (zero_orders % (1ULL << i) != 0); // ceil
        bitmap_list[i] = bitmap_init(cursor, num_blocks);
        cursor += bitmap_get_size(&bitmap_list[i]);
    }

    for(uint64_t i = 0; i < response->entry_count; i++) {
        struct limine_memmap_entry *entry = response->entries[i];
        if(entry->type == LIMINE_MEMMAP_USABLE) {
            if(i == idx) {
                // Skip the region used for bitmaps
                paddr_t bitmap_end = bitmap_entry->base + net_bytes;
                if (entry->base < bitmap_end) {
                    pmm_add_block(bitmap_end, entry->length - (bitmap_end - entry->base));
                    continue;
                }
            }
            pmm_add_block(entry->base, entry->length);
        }
    }
}
