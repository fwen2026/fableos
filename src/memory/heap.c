#include <memory/heap.h>
#include <memory/pmm.h>
#include <util/hcf.h>
#include <util/math.h>

#define CACHE_COUNT 8 // from 16 bytes to 2048 bytes: half a page.

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


/** Bare-bones init func */
void heap_init(){
    for(unsigned i = 0; i < CACHE_COUNT; i++){
        cache_list[i].empty_head = NULL;
        cache_list[i].partial_head = NULL;
        cache_list[i].full_head = NULL;
    } // we'll init each slab during malloc, if necessary.
}