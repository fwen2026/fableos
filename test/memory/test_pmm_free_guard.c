/*
 * Tests for pmm_free()'s "already free" guard: a block may not be freed if it,
 * or any larger block containing it, is currently on a free list.
 *
 * The bit for a block in bitmap_list[order] is clear exactly while that block
 * sits on free_list[order]. So walking from the freed block's own order up to
 * MAX_ORDER and finding a clear bit at any level means the memory is already
 * free (either the block itself, or an ancestor that would end up overlapping).
 *
 * Groups:
 *   rejected   every kind of already-free block must be refused with that
 *              guard's message, leaving lists, bitmaps and memory untouched
 *   accepted   legitimate frees that sit next to free memory but are not
 *              covered by it must still go through (false-positive guards)
 *
 * Known gap, deliberately not tested: the guard only looks UP the tree. Freeing
 * a block while a block INSIDE it is free (a descendant) is not detected.
 */
#include "pmm_harness.h"

#define BASE      ((paddr_t)0x400000)
#define BLOCK(o)  ((paddr_t)PAGE_SIZE << (o))
#define TWO_MIB   BLOCK(MAX_ORDER)
#define ALREADY_FREE_MSG "Freeing an already-free block"

static void fresh(size_t size) {
    setup(BASE);
    setup_bitmaps(BASE + size);
    pmm_add_block(BASE, size);
}

/* ---- snapshot: lists, every bitmap, and the target's own bytes --------- */

typedef struct {
    memory_node_t *heads[MAX_ORDER + 1];
    uint64_t bm[MAX_ORDER + 1][BM_WORDS];
    uint64_t um[BM_WORDS];
    uint8_t target[2 * sizeof(memory_node_t)];
} snap_t;

static void take(snap_t *s, paddr_t target) {
    memcpy(s->heads, free_list, sizeof free_list);
    memcpy(s->bm, bm_storage, sizeof bm_storage);
    memcpy(s->um, um_storage, sizeof um_storage);
    memcpy(s->target, phys_to_virt(target), sizeof s->target);
}

static int unchanged(const snap_t *s, paddr_t target) {
    return memcmp(s->heads, free_list, sizeof free_list) == 0 &&
           memcmp(s->bm, bm_storage, sizeof bm_storage) == 0 &&
           memcmp(s->um, um_storage, sizeof um_storage) == 0 &&
           memcmp(s->target, phys_to_virt(target), sizeof s->target) == 0;
}

/* The free must be refused by the already-free guard itself (not an earlier
 * check), and must change nothing. */
static void expect_rejected(paddr_t addr, unsigned order) {
    snap_t s;
    take(&s, addr);
    error_calls = 0;
    last_error = NULL;

    pmm_free(addr, order);

    CHECK(error_calls == 1);
    CHECK(last_error && strcmp(last_error, ALREADY_FREE_MSG) == 0);
    CHECK(unchanged(&s, addr));
    EXPECT_STATUS(PASS);
}

static void expect_accepted(paddr_t addr, unsigned order) {
    error_calls = 0;
    pmm_free(addr, order);
    CHECK(error_calls == 0);
    EXPECT_STATUS(PASS);
}

/* ============================== rejected ================================ */

/* A page in the middle of a free 2 MiB block. */
static void test_rejects_page_inside_free_max_order_block(void) {
    fresh(TWO_MIB);
    EXPECT_STATUS(PASS);
    expect_rejected(BASE + 3 * PAGE_SIZE, 0);
}

/* The very last page of that block, where the word-aligned bit index is highest. */
static void test_rejects_last_page_of_free_max_order_block(void) {
    fresh(TWO_MIB);
    expect_rejected(BASE + TWO_MIB - PAGE_SIZE, 0);
}

/* An order-3 block in the middle of a free order-5 block: the free ancestor
 * is two orders above the freed block, with unrelated bits in between. */
static void test_rejects_block_two_orders_below_free_block(void) {
    fresh(BLOCK(5));
    CHECK(count_order(5) == 1);
    EXPECT_STATUS(PASS);
    expect_rejected(BASE + BLOCK(3), 3);
}

/* After pmm_alloc(0) splits a 2 MiB block, the free halves are one block per
 * order. A page inside the free order-1 block (the immediate parent level)... */
static void test_rejects_page_inside_free_order1_block_after_split(void) {
    fresh(TWO_MIB);
    CHECK(pmm_alloc(0) == BASE);
    EXPECT_STATUS(PASS);
    expect_rejected(BASE + 3 * PAGE_SIZE, 0);               /* inside free [2P, 4P) */
}

/* ...and one deep in the free order-8 block, whose bit sits high in the chain. */
static void test_rejects_page_inside_free_order8_block_after_split(void) {
    fresh(TWO_MIB);
    CHECK(pmm_alloc(0) == BASE);
    expect_rejected(BASE + 256 * PAGE_SIZE + 7 * PAGE_SIZE, 0);
}

/* The same block freed twice with nothing in between. */
static void test_rejects_same_block_freed_twice(void) {
    fresh(BLOCK(5));
    paddr_t a = pmm_alloc(2);
    paddr_t b = pmm_alloc(2);
    CHECK(a != 0 && b != 0);
    pmm_free(a, 2);
    EXPECT_STATUS(PASS);
    snap_t s;
    take(&s, a);
    error_calls = 0;
    pmm_free(a, 2);
    CHECK(error_calls == 1);
    CHECK(unchanged(&s, a));
    EXPECT_STATUS(PASS);
}

/* The realistic double free: the first free coalesces the page into a big
 * block, so the page's OWN bit is still "allocated". Only the ancestor walk
 * can see that it is free now. */
static void test_rejects_double_free_after_coalescing(void) {
    fresh(TWO_MIB);
    paddr_t p = pmm_alloc(0);
    CHECK(p == BASE);
    pmm_free(p, 0);
    CHECK(only_free_block_is(BASE, MAX_ORDER));             /* it coalesced */
    CHECK(bitmap_get(&bitmap_list[0], p / PAGE_SIZE));      /* own-order bit says "allocated" */
    expect_rejected(p, 0);
}

/* A block that was never allocated at all. */
static void test_rejects_free_of_never_allocated_free_block(void) {
    fresh(TWO_MIB);
    expect_rejected(BASE, MAX_ORDER);
}

/* ============================== accepted ================================ */

/* The allocated page has free memory all around it, but none of it contains
 * it; freeing it must work and coalesce the whole block back together. */
static void test_accepts_allocated_page_surrounded_by_free_blocks(void) {
    fresh(TWO_MIB);
    paddr_t p = pmm_alloc(0);
    CHECK(p == BASE);
    for (unsigned o = 0; o < MAX_ORDER; o++) CHECK(count_order(o) == 1);
    expect_accepted(p, 0);
    CHECK(only_free_block_is(BASE, MAX_ORDER));
}

/* Two neighbouring allocated pages: the parent order-1 block is allocated,
 * so freeing one page is fine and must not merge with its allocated buddy. */
static void test_accepts_page_whose_buddy_is_allocated(void) {
    fresh(TWO_MIB);
    paddr_t a = pmm_alloc(0), b = pmm_alloc(0);
    CHECK(a == BASE && b == BASE + PAGE_SIZE);
    expect_accepted(b, 0);
    CHECK(count_order(0) == 1);
    CHECK(free_list[0] == (memory_node_t *)phys_to_virt(b));
}

/* Two 2 MiB blocks: freeing the allocated one while its neighbour is free is
 * fine (the neighbour is not an ancestor, and there is no order 10 to merge into). */
static void test_accepts_max_order_block_next_to_free_max_order_block(void) {
    fresh(2 * TWO_MIB);
    paddr_t a = pmm_alloc(MAX_ORDER);
    CHECK(a != 0);
    CHECK(count_order(MAX_ORDER) == 1);
    expect_accepted(a, MAX_ORDER);
    CHECK(count_order(MAX_ORDER) == 2);
}

/* Free in the opposite order to allocation, mixed orders: nothing the PMM
 * handed out may ever be refused. */
static void test_accepts_everything_allocated_in_reverse_order(void) {
    fresh(TWO_MIB);
    paddr_t got[6];
    unsigned orders[6] = { 0, 3, 1, 5, 0, 2 };
    for (int i = 0; i < 6; i++) { got[i] = pmm_alloc(orders[i]); CHECK(got[i] != 0); }
    for (int i = 5; i >= 0; i--) expect_accepted(got[i], orders[i]);
    CHECK(only_free_block_is(BASE, MAX_ORDER));
}

int main(void) {
    printf("-- rejected\n");
    RUN(test_rejects_page_inside_free_max_order_block);
    RUN(test_rejects_last_page_of_free_max_order_block);
    RUN(test_rejects_block_two_orders_below_free_block);
    RUN(test_rejects_page_inside_free_order1_block_after_split);
    RUN(test_rejects_page_inside_free_order8_block_after_split);
    RUN(test_rejects_same_block_freed_twice);
    RUN(test_rejects_double_free_after_coalescing);
    RUN(test_rejects_free_of_never_allocated_free_block);

    printf("-- accepted\n");
    RUN(test_accepts_allocated_page_surrounded_by_free_blocks);
    RUN(test_accepts_page_whose_buddy_is_allocated);
    RUN(test_accepts_max_order_block_next_to_free_max_order_block);
    RUN(test_accepts_everything_allocated_in_reverse_order);

    printf("\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
