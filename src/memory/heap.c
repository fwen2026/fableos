#include <memory/heap.h>
#include <memory/pmm.h>
#include <util/hcf.h>
#include <util/math.h>
#include <kernel/console.h>

#define CACHE_COUNT 8 // from 16 bytes to 2048 bytes: half a page.
#define SLAB_SIZE 4096 // 4096 bytes
#define LARGE_HEADER_SIZE 16
#define IDENTIFIER_HEADER 0xDEADBEEF
#define IDENTIFIER_CACHE 0xCAFEBABE

#define MAX_ORDER 9
#define MAX_LARGE_SIZE (((size_t)SLAB_SIZE << MAX_ORDER) - LARGE_HEADER_SIZE) // biggest block the PMM can give us, minus metadata in bytes

typedef struct slab_node{
    struct slab_node *next;
} slab_node_t;


typedef struct slab{
    uint32_t identifier;
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
    uint32_t identifier;
    uint32_t order;
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


/** Unlink a generalized node from the free list
 * @param node the address of the node to remove
 * @param head the head of the list. necessary for updating if it's the first element.
*/
void unlink_cache_node(slab_t *node, slab_t **head) {
    if(node->prev){
        node->prev->next = node->next;
    } else{
        if(*head != node) { hcf() ;} // this shouldn't be possible
        *head = node->next;
    }

    if(node->next){
        node->next->prev = node->prev;
    }

    node->next = NULL;
    node->prev = NULL;
}


/** Address of the first object in a slab. Objects are packed against the END of the page so each one is
 * naturally aligned to its block size, and the slab_t metadata sits at the start. The slab's cache_idx must be set.
 */
static uintptr_t slab_first_object(slab_t *slab){
    size_t block_size = 16UL << slab->cache_idx;
    size_t usable_space = ((SLAB_SIZE - sizeof(slab_t)) / block_size) * block_size; // round this DOWN.
    return (uintptr_t)slab + SLAB_SIZE - usable_space;
}


/** Adds a new slab to the free section of a specified cache from physical memory. Initializes metadata for that slab as well. */
void slab_init(unsigned idx){
    paddr_t phys = pmm_alloc(0);
    if(phys == 0){
        log_error("PMM allocation failed");
        return;
    }
    slab_t* new_slab = (slab_t*)(phys + get_hhdm_offset()); // convert to virt memory

    new_slab->identifier = IDENTIFIER_CACHE;
    new_slab->cache_idx = idx;
    new_slab->free_head = NULL; //temp for now. fill in later.
    new_slab->num_occupied = 0; // NOT counting metadata

    // calculate the LAST address we can fill up the free list to.
    size_t block_size = 16UL << idx;
    uintptr_t first_addr = slab_first_object(new_slab);
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
    // wraparound check
    if(size > MAX_LARGE_SIZE){
        return NULL;
    }

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
    header->identifier = IDENTIFIER_HEADER;

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


static paddr_t hhdm_to_phys(void *virt){
    return (paddr_t)((uintptr_t)virt - get_hhdm_offset());
}


/** Free a small block from the slab allocator. */
static void kfree_slab(slab_t *slab, void *ptr){
    if(slab->cache_idx >= CACHE_COUNT){
        log_error("Index exceeds bounds");
        return;
    }

    size_t block_size = 16UL << slab->cache_idx;
    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t first_addr = slab_first_object(slab);
    uintptr_t end_addr = (uintptr_t)slab + SLAB_SIZE; // addr is on this page, so it can't be past the end

    if(addr < first_addr || (addr - first_addr) % block_size != 0){
        log_error("Pointer is not an object of its slab");
        return;
    }
    if(slab->num_occupied == 0){
        log_error("Double free detected");
        return;
    }

    size_t capacity = (end_addr - first_addr) / block_size;
    size_t steps = 0;
    for(slab_node_t *node = slab->free_head; node != NULL && steps < capacity; node = node->next, steps++){
        if((uintptr_t)node == addr){
            log_error("Double free detected");
            return;
        }
    }

    cache_t *cache = &cache_list[slab->cache_idx];
    bool was_full = (slab->free_head == NULL); // check if its full.

    insert_slab_node((slab_node_t *)ptr, &slab->free_head);
    slab->num_occupied--;

    if(slab->num_occupied == 0){
        unlink_cache_node(slab, was_full ? &cache->full_head : &cache->partial_head);

        if(cache->empty_head == NULL){
            insert_cache_node(slab, &cache->empty_head); // keep one spare so alloc/free in a loop doesn't thrash the PMM
        } else{
            slab->identifier = 0; 
            pmm_free(hhdm_to_phys(slab), 0);
        }
    } else if(was_full){
        unlink_cache_node(slab, &cache->full_head);
        insert_cache_node(slab, &cache->partial_head);
    }
}


/** Free a large block. */
static void kfree_large(large_alloc_header_t *header, void *ptr){
    if((uintptr_t)ptr != (uintptr_t)header + LARGE_HEADER_SIZE){
        log_error("Not a large allocation");
        return;
    }
    if(header->order > MAX_ORDER){
        log_error("Order exceeds maximum order");
        return;
    }

    unsigned order = header->order;
    header->identifier = 0; // de-identify. also double free check.
    pmm_free(hhdm_to_phys(header), order);
}


/** Frees memory from kmalloc. Differentiates between large/small allocations.
 * @param addr The pointer returned by kmalloc. NULL is allowed and does nothing.
 */
void kfree(void *addr){
    if(addr == NULL) return;

    uintptr_t virt = (uintptr_t)addr;
    if(virt < get_hhdm_offset()){
        log_error("Pointer is not in HHDM");
        return;
    }

    void *page = (void *)(virt & ~((uintptr_t)SLAB_SIZE - 1));
    uint32_t identifier = *(uint32_t *)page;

    if(identifier == IDENTIFIER_CACHE){
        kfree_slab((slab_t *)page, addr);
    } else if(identifier == IDENTIFIER_HEADER){
        kfree_large((large_alloc_header_t *)page, addr);
    } else{
        log_error("Pointer was not allocated");
    }
}
