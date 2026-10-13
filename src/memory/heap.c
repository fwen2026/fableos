#include <memory/heap.h>
#include <memory/pmm.h>
#include <util/hcf.h>
#include <util/math.h>

#define CACHE_COUNT 8 // from 16 bytes to 2048 bytes: half a page.
#define SLAB_SIZE 4096 // 4096 bytes
#define LARGE_HEADER_SIZE 16
#define IDENTIFIER 0xDEADBEEF
#define MAX_ORDER 9

typedef struct slab_node{
    struct slab_node *next;
} slab_node_t;


typedef struct slab{
    struct slab *prev;
    struct slab *next;
    slab_node_t *free_head;
    unsigned cache_idx;
    uint16_t num_occupied;
} slab_t;


typedef struct {
    slab_t *empty_head;
    slab_t *partial_head;
    slab_t *full_head;
} cache_t;

static cache_t cache_list[CACHE_COUNT];


typedef struct {
    uint32_t order;
    uint32_t identifier;
} large_alloc_header_t;


/** Inserts a new node inside the free list for each slab. */
void insert_slab_node(slab_node_t *node, slab_node_t **head){
    node->next = *head;
    *head = node;
}

/** Inserts a new node inside the linked list for a specified cache
 * @param slab The slab to insert
 * @param head The address for the head of the linked list for the specified cache
 */
void insert_cache_node(slab_t *slab, slab_t **head){
    if(*head != NULL && (*head)->prev != NULL){ hcf(); }
    if(*head){
        (*head)->prev = slab;
    }

    slab->next = *head;
    slab->prev = NULL;

    *head = slab;
}


/** Pops a node from the linked list for a specified cache
 * @param head The address for the head of the list.
 * @return The popped slab, or NULL if the list is empty.
 */
slab_t *pop_cache_node(slab_t **head){
    slab_t *slab = *head;
    if(slab == NULL){ return NULL; }

    *head = slab->next;
    if(*head != NULL){
        (*head)->prev = NULL;
    }

    slab->next = NULL;
    slab->prev = NULL; // isolate it.

    return slab;
}


/** Adds a new slab to the free section of a specified cache from physical memory. Initializes metadata for that slab as well. */
void slab_init(unsigned idx){
    paddr_t phys = pmm_alloc(0);
    if(phys == 0){
        log_error("PMM allocation failed");
        return;
    }
    slab_t* new_slab = (slab_t*)(phys + get_hhdm_offset()); // convert to virt memory

    new_slab->cache_idx = idx;
    new_slab->free_head = NULL; //temp for now. fill in later.
    new_slab->num_occupied = 0; // NOT counting metadata

    // calculate the LAST address we can fill up the free list to.
    size_t block_size = 16UL << idx;
    size_t remaining_space = ((SLAB_SIZE - sizeof(slab_t)) / block_size) * block_size; // round this DOWN.
    uintptr_t first_addr = (uintptr_t)new_slab + 
                                SLAB_SIZE - remaining_space;
    uintptr_t last_addr = (uintptr_t)new_slab + SLAB_SIZE;
    
    for(uintptr_t addr = first_addr; addr < last_addr; addr += block_size){
        insert_slab_node((slab_node_t*)addr, &new_slab->free_head);
    }

    insert_cache_node(new_slab, &(cache_list[idx].empty_head));
}


/** Bare-bones init func */
void heap_init(){
    for(unsigned i = 0; i < CACHE_COUNT; i++){
        cache_list[i].empty_head = NULL;
        cache_list[i].partial_head = NULL;
        cache_list[i].full_head = NULL;
    } // we'll init each slab during malloc, if necessary.
}


/** Because the slabs can't support sizes past 2048 bytes, we need a different approach. */
static void *kmalloc_large(size_t size){
    size_t size_and_metadata = size + LARGE_HEADER_SIZE;
    size_t num_pages = div_ceil(size_and_metadata, SLAB_SIZE);

    unsigned ord = (num_pages <= 1)
        ? 0
        : 64 - __builtin_clzll(num_pages - 1);

    if(ord > MAX_ORDER){
        return NULL;
    }

    paddr_t phys = pmm_alloc(ord);
    if(phys == 0) return NULL;

    large_alloc_header_t *header = (large_alloc_header_t*)(phys + get_hhdm_offset());
    header->order = ord;
    header->identifier = IDENTIFIER;

    return (uint8_t*)header + LARGE_HEADER_SIZE;
}


/** Allocates `size` bytes of memory. If init fails, returns NULL.
 * @param size The amount of memory to allocate, in bytes.
 */
void *kmalloc(size_t size){
    if(size <= 2048){
        unsigned idx = (size < 16) 
            ? 0 
            : 64 - (__builtin_clzll(size - 1) + 4);

        slab_t *slab;
        slab_node_t *head;
        if(cache_list[idx].partial_head == NULL){
            if(cache_list[idx].empty_head == NULL){
                slab_init(idx);
                if(cache_list[idx].empty_head == NULL){
                    return NULL;
                }
            } 
            slab = cache_list[idx].empty_head;

            head = slab->free_head;
            slab->free_head = head->next; // remove from the free list. 
            slab->num_occupied ++;

            slab_t *allocated_slab = pop_cache_node(&(cache_list[idx].empty_head));
            insert_cache_node(allocated_slab, &(cache_list[idx].partial_head));
        } else{
            slab = cache_list[idx].partial_head;

            head = slab->free_head;
            slab->free_head = head->next; 
            slab->num_occupied ++;
        }

        if(cache_list[idx].partial_head->free_head == NULL){
            slab_t *allocated_slab = pop_cache_node(&(cache_list[idx].partial_head));
            insert_cache_node(allocated_slab, &(cache_list[idx].full_head));
        } 

        return head;
    } else{
        return kmalloc_large(size);
    }
}