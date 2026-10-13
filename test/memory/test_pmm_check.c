/*
 * Validation of pmm_check(), the PMM's built-in consistency checker.
 *
 * A checker is only useful if it is both quiet on healthy state and loud on
 * broken state, so every Status value gets its own group of tests:
 *
 *   PASS          states that must NOT be flagged, including near-misses that
 *                 look suspicious but are legal (the false-positive guards).
 *   everything    hand-corrupted states, each with exactly one fault, that
 *   else          must be reported as that specific Status.
 *
 * pmm_check() returns the first problem it finds, scanning orders upward and
 * each list head-to-tail, so a corrupted state must contain only the fault
 * under test. Where a state is built from a healthy one, the test first
 * asserts PASS so that a failure can only come from the injected fault.
 *
 * Not tested: a cyclic free list. pmm_check() walks lists with no cycle
 * guard, so that input would hang rather than return a Status.
 */
#include "pmm_harness.h"

#define BASE        ((paddr_t)0x400000)     /* 2 MiB-aligned, so also order-9 aligned */
#define BLOCK(o)    ((paddr_t)PAGE_SIZE << (o))
#define TWO_MIB     BLOCK(MAX_ORDER)

/* ---- state builders ---------------------------------------------------- */

/* PMM managing [0, BASE + size) with nothing free: every bit "allocated",
 * every list empty. This is what pmm_init() builds before adding regions. */
static void fresh_empty(size_t size) {
    setup(BASE);
    setup_bitmaps(BASE + size);
}

/* As fresh_empty(), then [BASE, BASE + size) freed via pmm_add_block(). */
static void fresh(size_t size) {
    fresh_empty(size);
    pmm_add_block(BASE, size);
}

/* Three isolated order-0 free blocks at BASE, BASE+2P, BASE+4P. None are
 * buddies of each other, so this is a healthy state with a 3-node list:
 * free_list[0] = BASE+4P -> BASE+2P -> BASE. */
static void three_singletons(void) {
    fresh_empty(TWO_MIB);
    pmm_add_block(BASE,                    PAGE_SIZE);
    pmm_add_block(BASE + 2 * PAGE_SIZE,    PAGE_SIZE);
    pmm_add_block(BASE + 4 * PAGE_SIZE,    PAGE_SIZE);
}

static memory_node_t *node_at(paddr_t addr) {
    return (memory_node_t *)phys_to_virt(addr);
}

static size_t bit_index(paddr_t addr, unsigned order) {
    return addr / BLOCK(order);
}

/* Pushes a node onto free_list[order] WITHOUT touching the bitmap or
 * checking alignment (pmm_add_free_page would refuse a misaligned address). */
static void list_push_raw(paddr_t addr, unsigned order) {
    list_add(&free_list[order], node_at(addr));
}

/* ======================== PASS: healthy states ========================== */

static void test_pass_untouched_pmm(void) {
    fresh_empty(TWO_MIB);
    CHECK(free_list[0] == NULL && free_list[MAX_ORDER] == NULL);
    EXPECT_STATUS(PASS);
}

static void test_pass_single_max_order_block(void) {
    fresh(TWO_MIB);
    CHECK(count_order(MAX_ORDER) == 1);
    EXPECT_STATUS(PASS);
}

/* Two free order-9 blocks that are each other's buddy. There is no order 10
 * to merge into, so this is legal and must not be flagged FREE_BUDDY. */
static void test_pass_two_max_order_buddies(void) {
    fresh(2 * TWO_MIB);
    CHECK(count_order(MAX_ORDER) == 2);
    EXPECT_STATUS(PASS);
}

/* A block at the very top of RAM has a buddy index past the end of the
 * bitmap, which bitmap_get() reports as "allocated"; not a free buddy. */
static void test_pass_top_of_ram_block_with_oob_buddy(void) {
    fresh(BLOCK(3));
    CHECK(count_order(3) == 1);
    CHECK(bit_index(BASE, 3) + 1 >= bitmap_list[3].size);   /* buddy is OOB */
    EXPECT_STATUS(PASS);
}

/* Adjacent free blocks that are not buddies: BASE+P and BASE+2P touch, but
 * their buddies are BASE and BASE+3P, which are allocated. */
static void test_pass_adjacent_non_buddies(void) {
    fresh_empty(TWO_MIB);
    pmm_add_free_page(BASE + PAGE_SIZE,     0);
    pmm_add_free_page(BASE + 2 * PAGE_SIZE, 0);
    CHECK(count_order(0) == 2);
    EXPECT_STATUS(PASS);
}

/* A fully split block: one free block at every order 0..8. */
static void test_pass_after_split_by_alloc(void) {
    fresh(TWO_MIB);
    CHECK(pmm_alloc(0) == BASE);
    for (unsigned o = 0; o < MAX_ORDER; o++) CHECK(count_order(o) == 1);
    CHECK(count_order(MAX_ORDER) == 0);
    EXPECT_STATUS(PASS);
}

static void test_pass_after_alloc_free_round_trip(void) {
    fresh(TWO_MIB);
    paddr_t p = pmm_alloc(0);
    CHECK(p == BASE);
    pmm_free(p, 0);
    CHECK(only_free_block_is(BASE, MAX_ORDER));
    EXPECT_STATUS(PASS);
}

static void test_pass_after_exhaustion(void) {
    fresh(TWO_MIB);
    CHECK(pmm_alloc(MAX_ORDER) == BASE);
    CHECK(pmm_alloc(MAX_ORDER) == 0);
    CHECK(free_list[MAX_ORDER] == NULL);
    EXPECT_STATUS(PASS);
}

/* pmm_check() is a diagnostic: running it must not alter any state. */
static void test_pass_check_is_read_only(void) {
    fresh(TWO_MIB);
    CHECK(pmm_alloc(0) == BASE);

    uint64_t bm_before[MAX_ORDER + 1][BM_WORDS];
    memory_node_t *heads_before[MAX_ORDER + 1];
    memcpy(bm_before, bm_storage, sizeof bm_storage);
    memcpy(heads_before, free_list, sizeof free_list);

    EXPECT_STATUS(PASS);
    EXPECT_STATUS(PASS);

    CHECK(memcmp(bm_before, bm_storage, sizeof bm_storage) == 0);
    CHECK(memcmp(heads_before, free_list, sizeof free_list) == 0);
}

/* ============================ MISALIGNMENT ============================== */

/* An order-1 block must sit on an 8 KiB boundary; this one is on 4 KiB. */
static void test_misalignment_order1_on_page_boundary(void) {
    fresh_empty(TWO_MIB);
    list_push_raw(BASE + PAGE_SIZE, 1);
    EXPECT_STATUS(MISALIGNMENT);
}

/* A 2 MiB block placed on a 1 MiB boundary: aligned to half its size. */
static void test_misalignment_max_order_on_half_boundary(void) {
    fresh_empty(2 * TWO_MIB);
    list_push_raw(BASE + TWO_MIB / 2, MAX_ORDER);
    EXPECT_STATUS(MISALIGNMENT);
}

/* The source comment says this check must come first: a misaligned node's
 * other fields can't be trusted, so it wins over a broken prev link. */
static void test_misalignment_takes_precedence_over_bad_prev(void) {
    fresh_empty(TWO_MIB);
    list_push_raw(BASE + 2 * PAGE_SIZE, 2);                 /* 8K-aligned, needs 16K */
    free_list[2]->prev = node_at(BASE + 8 * PAGE_SIZE);     /* head with a prev */
    EXPECT_STATUS(MISALIGNMENT);
}

/* ============================== LL_INIT ================================= */

static void test_ll_init_head_has_a_prev(void) {
    three_singletons();
    EXPECT_STATUS(PASS);
    free_list[0]->prev = node_at(BASE + 2 * PAGE_SIZE);
    EXPECT_STATUS(LL_INIT);
}

static void test_ll_init_middle_node_prev_is_null(void) {
    three_singletons();
    EXPECT_STATUS(PASS);
    free_list[0]->next->prev = NULL;
    EXPECT_STATUS(LL_INIT);
}

/* Tail points back at the head, skipping the middle node. */
static void test_ll_init_tail_prev_skips_a_node(void) {
    three_singletons();
    EXPECT_STATUS(PASS);
    memory_node_t *head = free_list[0];
    head->next->next->prev = head;
    EXPECT_STATUS(LL_INIT);
}

/* ============================== DESYNC ================================== */

/* A block is in the free list but its bit says "allocated". */
static void test_desync_listed_block_marked_allocated(void) {
    three_singletons();
    EXPECT_STATUS(PASS);
    bitmap_set_allocate(&bitmap_list[0], bit_index(BASE + 2 * PAGE_SIZE, 0));
    EXPECT_STATUS(DESYNC);
}

/* Same fault, but masked from the per-order count: one listed block is
 * marked allocated while an unlisted block is marked free, so the number of
 * free bits still equals the number of nodes. Only the per-node bit check
 * can catch this. */
static void test_desync_swapped_bits_keep_counts_equal(void) {
    three_singletons();
    EXPECT_STATUS(PASS);
    bitmap_set_allocate(&bitmap_list[0],   bit_index(BASE + 2 * PAGE_SIZE, 0));  /* listed -> allocated */
    bitmap_set_clear(&bitmap_list[0], bit_index(BASE + 8 * PAGE_SIZE, 0));  /* unlisted -> free    */
    EXPECT_STATUS(DESYNC);
}

/* A node vanishes from the list (e.g. popped without updating the bitmap),
 * leaving its bit "free" with nothing to back it. */
static void test_desync_node_lost_from_list(void) {
    three_singletons();
    EXPECT_STATUS(PASS);
    CHECK(list_remove(&free_list[0]) != NULL);              /* bitmap untouched */
    EXPECT_STATUS(DESYNC);
}

/* A stray "free" bit at a high order with every list empty: only the
 * per-order count comparison can see it. */
static void test_desync_stray_free_bit_at_high_order(void) {
    fresh_empty(TWO_MIB);
    EXPECT_STATUS(PASS);
    bitmap_set_clear(&bitmap_list[5], bit_index(BASE, 5));
    EXPECT_STATUS(DESYNC);
}

/* ============================= FREE_BUDDY =============================== */

/* Two free order-0 buddies that were never merged. */
static void test_free_buddy_order0_pair(void) {
    fresh_empty(TWO_MIB);
    pmm_add_free_page(BASE,             0);
    pmm_add_free_page(BASE + PAGE_SIZE, 0);
    EXPECT_STATUS(FREE_BUDDY);
}

static void test_free_buddy_mid_order_pair(void) {
    fresh_empty(TWO_MIB);
    pmm_add_free_page(BASE,            5);
    pmm_add_free_page(BASE + BLOCK(5), 5);
    EXPECT_STATUS(FREE_BUDDY);
}

/* Order 8 is the highest order that can still merge (into order 9), so an
 * unmerged pair here is a fault, unlike the order-9 pair in the PASS tests. */
static void test_free_buddy_pair_just_below_max_order(void) {
    fresh_empty(TWO_MIB);
    pmm_add_free_page(BASE,            MAX_ORDER - 1);
    pmm_add_free_page(BASE + BLOCK(MAX_ORDER - 1), MAX_ORDER - 1);
    EXPECT_STATUS(FREE_BUDDY);
}

/* ============================== OVERLAP ================================= */

/* The same memory is listed twice: as an order-0 block and as the order-1
 * block that contains it. */
static void test_overlap_block_and_its_parent_both_free(void) {
    fresh_empty(TWO_MIB);
    pmm_add_free_page(BASE, 0);
    pmm_add_free_page(BASE, 1);
    EXPECT_STATUS(OVERLAP);
}

/* A small block inside a free order-9 block, several orders up. */
static void test_overlap_page_inside_free_max_order_block(void) {
    fresh(TWO_MIB);
    EXPECT_STATUS(PASS);
    pmm_add_free_page(BASE + 3 * PAGE_SIZE, 0);
    EXPECT_STATUS(OVERLAP);
}

/* An order-3 block in the middle (not the start) of a free order-5 block,
 * two orders up from the node. */
static void test_overlap_block_inside_ancestor_two_orders_up(void) {
    fresh_empty(TWO_MIB);
    pmm_add_free_page(BASE,            5);
    pmm_add_free_page(BASE + BLOCK(3), 3);
    EXPECT_STATUS(OVERLAP);
}

int main(void) {
    printf("-- PASS (healthy states)\n");
    RUN(test_pass_untouched_pmm);
    RUN(test_pass_single_max_order_block);
    RUN(test_pass_two_max_order_buddies);
    RUN(test_pass_top_of_ram_block_with_oob_buddy);
    RUN(test_pass_adjacent_non_buddies);
    RUN(test_pass_after_split_by_alloc);
    RUN(test_pass_after_alloc_free_round_trip);
    RUN(test_pass_after_exhaustion);
    RUN(test_pass_check_is_read_only);

    printf("-- MISALIGNMENT\n");
    RUN(test_misalignment_order1_on_page_boundary);
    RUN(test_misalignment_max_order_on_half_boundary);
    RUN(test_misalignment_takes_precedence_over_bad_prev);

    printf("-- LL_INIT\n");
    RUN(test_ll_init_head_has_a_prev);
    RUN(test_ll_init_middle_node_prev_is_null);
    RUN(test_ll_init_tail_prev_skips_a_node);

    printf("-- DESYNC\n");
    RUN(test_desync_listed_block_marked_allocated);
    RUN(test_desync_swapped_bits_keep_counts_equal);
    RUN(test_desync_node_lost_from_list);
    RUN(test_desync_stray_free_bit_at_high_order);

    printf("-- FREE_BUDDY\n");
    RUN(test_free_buddy_order0_pair);
    RUN(test_free_buddy_mid_order_pair);
    RUN(test_free_buddy_pair_just_below_max_order);

    printf("-- OVERLAP\n");
    RUN(test_overlap_block_and_its_parent_both_free);
    RUN(test_overlap_page_inside_free_max_order_block);
    RUN(test_overlap_block_inside_ancestor_two_orders_up);

    printf("\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
