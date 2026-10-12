#include <memory/pmm.h>
#include "limine.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <util/math.h>
#include <util/hcf.h>
#include <kernel/console.h>

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
    }
    hhdm_offset = request.response->offset;
}

static const char *memory_map_entry_types[] = {
    [0] = "Available",
    [1] = "Reserved",
    [2] = "ACPI Reclaimable",
    [3] = "ACPI NVS",
    [4] = "Bad RAM",
    [5] = "Bootloader Reclaimable",
    [6] = "Executable and Modules",
    [7] = "Framebuffer",
    [8] = "Reserved Mapped"
};


/** Memory map logging */
void log_memory_map(void) {
    if (memmap_request.response != NULL) {
        console_printf("Memory map request received.\n");

        struct limine_memmap_response *response = memmap_request.response;

        for(uint64_t i = 0; i < response->entry_count; i++) {
            struct limine_memmap_entry *entry = response->entries[i];
            console_printf("Memory map entry %d: base = 0x%lx, length = 0x%lx, type = %s\n",
                           i, entry->base, entry->length, memory_map_entry_types[entry->type]);
        }
    }
}


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
static bitmap_t unmanaged_list;


/** Number of 64-bit words needed to hold the given number of bits */
static size_t bitmap_words(size_t bits) {
    return div_ceil(bits, 64);
}


/** Number of blocks of the given order needed to cover total_pages pages */
static size_t blocks_at_order(size_t total_pages, unsigned int order) {
    return div_ceil(total_pages, 1ULL << order);
}


// --- stuff for data structures ---


/** Constructs a bitmap
 * @param addr The address of the bitmap
 * @param size The number of bits represented in the bitmap
 */
bitmap_t bitmap_init(void *addr, size_t size) {
    bitmap_t bitmap;
    bitmap.size = size;
    bitmap.value = (uint64_t *)addr;
    size_t num_ints = bitmap_words(size); // calculate the num of ints we need
    for (size_t i = 0; i < num_ints; i++) {
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


void bitmap_set_clear(bitmap_t *bitmap, size_t index) {bitmap_set_value(bitmap, index, false);}
void bitmap_set_allocate(bitmap_t *bitmap, size_t index) {bitmap_set_value(bitmap, index, true);}


/** Gets a bit. Out-of-bounds (or missing bitmap) reads as true, i.e. allocated,
 * so memory the PMM doesn't manage is never mistaken for a free buddy.
 */
bool bitmap_get(bitmap_t *bitmap, size_t index) {
    if (!bitmap || index >= bitmap->size) return true;

    return (bitmap->value[index / 64] & (1ULL << (index % 64))) != 0;
}


/** Gets the size of the bitmap, in bytes. */
size_t bitmap_get_size(bitmap_t *bitmap) {
    if (!bitmap) return 0;
    return bitmap_words(bitmap->size) * sizeof(uint64_t);
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

 /** Removes a node at a specified address.
 * @param addr The address of the node to remove
 * @returns The removed node, or NULL if not found
 */
static memory_node_t *list_unlink(paddr_t addr, unsigned order) {
    memory_node_t *current = (memory_node_t *)(addr + hhdm_offset);
    if (!current) return NULL;

    // Unlink the node from the list
    if (current->prev) {
        current->prev->next = current->next;
    } else{
        free_list[order] = current -> next;
    }
    if (current->next) {
        current->next->prev = current->prev;
    }

    current->next = NULL;
    current->prev = NULL;

    return current;
}


// ---helpers for adding or removing a free page---


/** Adds a free page to the free list, at the specified order and at the specified address*/
void pmm_add_free_page(paddr_t addr, unsigned int order) {
    if (order > MAX_ORDER) return;
    if (addr & (((paddr_t)PAGE_SIZE << order) - 1)) return; // must be aligned to its block size

    memory_node_t *new_node = (memory_node_t *)(addr + hhdm_offset); // cast the address to a memory_node_t pointer
    list_add(&free_list[order], new_node); // adds the new node to the free list
    bitmap_set_clear(&bitmap_list[order], addr / (PAGE_SIZE * (1ULL << order)));
}


/** Pops the first free page from the free list at the specified order.*/
paddr_t pmm_pop_free_page(unsigned int order) {
    if (order > MAX_ORDER) return 0;

    memory_node_t *node = list_remove(&free_list[order]);
    if (!node) return 0;
    paddr_t addr = (uintptr_t)node - hhdm_offset;
    bitmap_set_allocate(&bitmap_list[order], addr / (PAGE_SIZE * (1ULL << order)));
    return addr;
}


/** Removes the node of a free list of a specified order at a specified memory address */
paddr_t pmm_unlink_free_page(unsigned int order, paddr_t addr) {
    if (order > MAX_ORDER) return 0;

    memory_node_t *node = list_unlink(addr, order);
    if (!node) return 0;

    paddr_t freed_addr = (uintptr_t)node - hhdm_offset;
    bitmap_set_allocate(&bitmap_list[order], freed_addr / (PAGE_SIZE * (1ULL << order)));
    return freed_addr;
}


/** Marks every page of a block of an order at address usable. */
static void mark_managed(paddr_t addr, unsigned order) {
    size_t first = addr / PAGE_SIZE;
    for(paddr_t addr = first; addr < first + (1ULL << order); addr++ ){
        bitmap_set_clear(&unmanaged_list, addr);
    }
}


/** True if every page of the block of a certain order at a certain address is managed by the PMM. A block that
 * touches even one unmanaged page, or goes out of bounds is rejected.
 */
static bool block_is_managed(paddr_t addr, unsigned order) {
    size_t first = addr / PAGE_SIZE;
    size_t count = 1ULL << order;
    if (first + count > unmanaged_list.size) return false;

    if (count < 64) {
        uint64_t mask = ((1ULL << count) - 1) << (first % 64);
        return (unmanaged_list.value[first / 64] & mask) == 0;
    }
    for (size_t w = first / 64; w < (first + count) / 64; w++) {
        if (unmanaged_list.value[w] != 0) return false;
    }
    return true;
}


/** Adds a series of buddy-compatible segments to the free list, given a region of usable memory. Also updates the bitmap accordingly.
 * @param base_addr The base address of the memory region
 * @param size The size of the memory region
 */
void pmm_add_block(paddr_t base_addr, size_t size) {
    // alignment
    paddr_t end_addr = (base_addr + size) & ~(PAGE_SIZE - 1); // rounds DOWN
    if(base_addr == 0) {base_addr = PAGE_SIZE;}
    base_addr = (base_addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1); // rounds UP

    while (base_addr < end_addr) {
        uint64_t current_page = base_addr / PAGE_SIZE;
        uint64_t remaining_page = (end_addr - base_addr) / PAGE_SIZE;

        unsigned int alignment = __builtin_ctzll(current_page); // counts zeroes from right

        unsigned int max_remaining = 63 - __builtin_clzll(remaining_page); // counts from left

        unsigned int order = min(min(alignment, max_remaining), MAX_ORDER);
        pmm_add_free_page(base_addr, order);
        mark_managed(base_addr, order);
        base_addr += (1ULL << order) * PAGE_SIZE; // increments the base address by the size of the allocated block
    }
}


/** Determines the buddy of a block at address with order n */
paddr_t get_buddy(paddr_t addr, unsigned order) {
    if (order > MAX_ORDER) return 0;

    // Calculate the size of the block
    size_t block_size = PAGE_SIZE << order;
    return addr ^ block_size;
}

 // --- exposed methods ---


/** Initializes each region in memory in aligned power-of-two blocks. First gets the total amount 
 * of physical memory available from Limine and then adds it to the free list.
*/
void pmm_init(void){
    if (memmap_request.response == NULL) {
        log_error("Memory map request failed.");
        hcf();
    }

    get_hhdm_offset(); // ensures hhdm_offset is set

    size_t zero_orders = 0;
    uint64_t max_addr = 0; // end of the highest usable region. doesn't rely on limine's sort order
    bool found_usable = false;
    struct limine_memmap_response *response = memmap_request.response;

    for(uint64_t i = 0; i < response->entry_count; i++) {
        struct limine_memmap_entry *entry = response->entries[i];
        if(entry->type != LIMINE_MEMMAP_USABLE) continue;

        found_usable = true;
        uint64_t end = entry->base + entry->length;
        if (end > max_addr) max_addr = end;
    }

    if (!found_usable) {
        log_error("No usable memory found.");
        hcf();
    }

    zero_orders = max_addr / PAGE_SIZE;

    size_t net_bytes = 0;

    for(unsigned int i = 0; i <= MAX_ORDER; i++) {
        net_bytes += bitmap_words(blocks_at_order(zero_orders, i)) * sizeof(uint64_t);
    }
    net_bytes += bitmap_words(zero_orders) * sizeof(uint64_t); // same size as zero

    size_t idx = SIZE_MAX;
    for(uint64_t i = 0; i < response->entry_count; i++) {
        struct limine_memmap_entry *entry = response->entries[i];
        if(entry->type == LIMINE_MEMMAP_USABLE && entry->length >= net_bytes) {
            idx = i;
            break;
        }
    }

    if(idx == SIZE_MAX) {
        log_error("No suitable memory region found.");
        hcf();
    }

    struct limine_memmap_entry *bitmap_entry = response->entries[idx];
    void *cursor = (void *) (bitmap_entry->base + hhdm_offset);
    for(unsigned int i = 0; i <= MAX_ORDER; i++) {
        bitmap_list[i] = bitmap_init(cursor, blocks_at_order(zero_orders, i));
        cursor += bitmap_get_size(&bitmap_list[i]);
    }
    unmanaged_list = bitmap_init(cursor, zero_orders);

    // Only usable regions are added. pmm_add_block also updates the allowed list.
    for(uint64_t i = 0; i < response->entry_count; i++) {
        struct limine_memmap_entry *entry = response->entries[i];
        if(entry->type == LIMINE_MEMMAP_USABLE) {
            if(i == idx) {
                // Skip the region used for bitmaps
                pmm_add_block(entry->base + net_bytes, entry->length - net_bytes);
                continue;
            }
            pmm_add_block(entry->base, entry->length);
        }
    }
}


/** Allocates a chunk of memory, and updates both representations accordingly. 
 * @param order The order of the memory block to allocate.
 * @return The physical address of the allocated memory, or 0 on failure.
 */
paddr_t pmm_alloc(unsigned order) {
    if (order > MAX_ORDER) {
        log_error("Allocation order exceeds max order");
        return 0;
    }

    unsigned available_order = order;
    while(available_order < MAX_ORDER + 1 && (free_list[available_order] == NULL)) { available_order++; }

    if (available_order == MAX_ORDER + 1) {
        log_error("No available memory");
        return 0;
    }

    // iteratively split larger blocks until we reach the designated order
    for(; available_order > order; available_order--) {
        paddr_t addr = pmm_pop_free_page(available_order);

        if(addr == 0) {
            log_error("Failed to pop free page");
            return 0;
        }

        // add the second half FIRST so LIFO takes care of the rest each iteration.
        pmm_add_free_page(addr + (1ULL << (available_order - 1)) * PAGE_SIZE, available_order - 1);
        pmm_add_free_page(addr, available_order - 1);
    }

    return pmm_pop_free_page(order);
}


/** Frees a block of memory.
 * @param addr The physical address of the memory block to free.
 * @param order The order of the memory block to free.
 */
void pmm_free(paddr_t addr, unsigned order) {
    if (order > MAX_ORDER) {
        log_error("Freeing order exceeds max order");
        return;
    }
    if ((addr & (((paddr_t)PAGE_SIZE << order) - 1)) != 0) {
        log_error("Address is not aligned to specified order");
        return;
    }
    // the whole block must lie within managed memory (order 0's bitmap has one bit per page)
    if (addr / PAGE_SIZE + (1ULL << order) > bitmap_list[0].size) {
        log_error("Freeing a block outside managed memory");
        return;
    }
    // only free allowed blocks
    if (!block_is_managed(addr, order)) {
        log_error("Freeing a block that touches unmanaged memory");
        return;
    }

    // don't free already free blocks
    for(unsigned j = order; j <= MAX_ORDER; j++){
        if(bitmap_get(&bitmap_list[j], addr / (PAGE_SIZE * (1ULL << j))) == 0){ 
            log_error("Freeing an already-free block");
            return;
        }
    }

    // and finally, no double freeing.
    if(!bitmap_get(&bitmap_list[order], addr / (PAGE_SIZE << order))) {
        log_error("Double free detected");
        return;
    }

    pmm_add_free_page(addr, order);

    unsigned buddy_order = order;
    paddr_t buddy_addr = addr;

    while (buddy_order < MAX_ORDER){
        paddr_t buddy = get_buddy(buddy_addr, buddy_order);
        if (buddy == 0) break;

        // Check if the buddy is free
        if (bitmap_get(&bitmap_list[buddy_order], buddy / (PAGE_SIZE << buddy_order)) != 0) break;

        // If it's free, merge them
        pmm_unlink_free_page(buddy_order, buddy);
        pmm_unlink_free_page(buddy_order, buddy_addr);

        buddy_addr = min(buddy_addr, buddy);
        pmm_add_free_page(buddy_addr, buddy_order + 1);
        buddy_order++;
    }
}

// --- diagnostics ---

/** Returns the number of free entries in a bitmap */
unsigned num_free(bitmap_t *bitmap){
    unsigned n = 0;
    for(unsigned i = 0; i < bitmap_words(bitmap->size); i++){
        n += __builtin_popcountll(~(bitmap->value[i]));
    }
    return n;
}


/** Checks whether the bitmap and free list are in sync, along with a couple other invariants*/
Status pmm_check(void) {
    for (unsigned int i = 0; i <= MAX_ORDER; i++) {
        unsigned node_count = 0;
        memory_node_t *current = free_list[i];
        memory_node_t *prev = NULL;
        while (current) {
            paddr_t physical = (uintptr_t)current - hhdm_offset;
            if (physical & (((paddr_t)PAGE_SIZE << i) - 1)) { return MISALIGNMENT; } // this needs to be first
            if (current -> prev != prev){ return LL_INIT; }
            if (bitmap_get(&bitmap_list[i], physical / (PAGE_SIZE * (1ULL << i))) == 1) { return DESYNC; }
            if (i != MAX_ORDER && bitmap_get(&bitmap_list[i], 
                get_buddy(physical, i) / (PAGE_SIZE * (1ULL << i))) == 0){ return FREE_BUDDY; }

            for(unsigned j = i + 1; j <= MAX_ORDER; j++){
                if(bitmap_get(&bitmap_list[j], physical / (PAGE_SIZE * (1ULL << j))) == 0){ return OVERLAP; }
            }
            node_count++;
            prev = current;
            current = current->next;
        }
        if (node_count != num_free(&bitmap_list[i])){
            return DESYNC;
        }
    }
    return PASS;
}
