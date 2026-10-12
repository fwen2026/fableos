/*
 * Host-side correctness tests for memory/pmm.c (allocator, free lists,
 * add_block, free/merge). See pmm_harness.h for the simulated-memory setup.
 *
 * Tests that are expected to crash on buggy code run in a forked child so
 * one crash does not take down the whole binary.
 */
#include "pmm_harness.h"

/* ======================= list_add / list_remove ======================== */

static void test_list_add_empty(void) {
    memory_node_t a;
    memory_node_t *head = NULL;
    list_add(&head, &a);
    CHECK(head == &a);
    CHECK(a.next == NULL);
    CHECK(a.prev == NULL);
}

static void test_list_add_pushes_front_and_links(void) {
    memory_node_t a, b, c;
    memory_node_t *head = NULL;
    list_add(&head, &a);
    list_add(&head, &b);
    list_add(&head, &c);
    CHECK(head == &c);
    CHECK(c.prev == NULL && c.next == &b);
    CHECK(b.prev == &c    && b.next == &a);
    CHECK(a.prev == &b    && a.next == NULL);
}

static void test_list_add_null_args(void) {
    memory_node_t a;
    memory_node_t *head = NULL;
    list_add(NULL, &a);       /* must not crash */
    list_add(&head, NULL);
    CHECK(head == NULL);
}

static void test_list_add_overwrites_stale_links(void) {
    memory_node_t a, junk;
    memory_node_t *head = NULL;
    a.next = &junk;           /* stale pointers from earlier use */
    a.prev = &junk;
    list_add(&head, &a);
    CHECK(a.next == NULL);
    CHECK(a.prev == NULL);
}

static void test_list_remove_returns_head_lifo(void) {
    memory_node_t a, b, c;
    memory_node_t *head = NULL;
    list_add(&head, &a);
    list_add(&head, &b);
    list_add(&head, &c);

    CHECK(list_remove(&head) == &c);
    CHECK(head == &b && b.prev == NULL);
    CHECK(list_remove(&head) == &b);
    CHECK(head == &a && a.prev == NULL);
    CHECK(list_remove(&head) == &a);
    CHECK(head == NULL);
}

static void test_list_remove_single_element(void) {
    memory_node_t a;
    memory_node_t *head = NULL;
    list_add(&head, &a);
    CHECK(list_remove(&head) == &a);
    CHECK(head == NULL);
}

static void test_list_remove_null_head_ptr(void) {
    CHECK(list_remove(NULL) == NULL);
}

/* Documented contract: "@returns ... NULL if the list was empty". */
static void remove_from_empty(void) {
    memory_node_t *head = NULL;
    memory_node_t *r = list_remove(&head);
    if (r != NULL || head != NULL) _exit(1);
}
static void test_list_remove_empty_list_returns_null(void) {
    CHECK(survives(remove_from_empty));
}

/* A removed node should not keep dangling links into the list. */
static void test_list_remove_detaches_node(void) {
    memory_node_t a, b;
    memory_node_t *head = NULL;
    list_add(&head, &a);
    list_add(&head, &b);
    memory_node_t *r = list_remove(&head);
    CHECK(r == &b);
    CHECK(r->next == NULL);   /* currently still points at `a` */
    CHECK(r->prev == NULL);
}

/* Re-adding the same node twice makes it point to itself (cycle). */
static void test_list_add_same_node_twice_is_not_a_cycle(void) {
    memory_node_t a;
    memory_node_t *head = NULL;
    list_add(&head, &a);
    list_add(&head, &a);
    CHECK(a.next != &a);
}

/* ======================== get_hhdm_offset ============================== */

static void test_get_hhdm_offset_reads_response(void) {
    setup(0);
    fake_hhdm.offset = 0xFFFF800000000000ULL;
    get_hhdm_offset();
    CHECK(hhdm_offset == (uintptr_t)0xFFFF800000000000ULL);
}

static void test_get_hhdm_offset_null_response_halts_keeps_value(void) {
    setup(0);
    hhdm_offset = 0x1234000;
    request.response = NULL;
    hcf_calls = 0;
    hcf_armed = 1;
    if (setjmp(hcf_env) == 0) get_hhdm_offset();
    hcf_armed = 0;
    CHECK(hcf_calls == 1);
    CHECK(hhdm_offset == 0x1234000);
    request.response = &fake_hhdm;
}

/* ======================== pmm_add_free_page ============================ */

static void test_add_free_page_places_node_at_hhdm_address(void) {
    setup(0x100000);
    pmm_add_free_page(0x103000, 0);
    CHECK(free_list[0] == (memory_node_t *)phys_to_virt(0x103000));
    CHECK(free_list[0]->next == NULL && free_list[0]->prev == NULL);
}

static void test_add_free_page_each_order_lands_in_its_list(void) {
    setup(0);
    for (unsigned o = 0; o <= MAX_ORDER; o++) {
        paddr_t addr = ((paddr_t)PAGE_SIZE << o);   /* aligned for order o */
        pmm_add_free_page(addr, o);
        CHECK(free_list[o] == (memory_node_t *)phys_to_virt(addr));
        CHECK(count_order(o) == 1);
    }
}

static void test_add_free_page_order_too_large_is_ignored(void) {
    setup(0);
    pmm_add_free_page(0x200000, MAX_ORDER + 1);
    pmm_add_free_page(0x200000, 1000);
    for (unsigned o = 0; o <= MAX_ORDER; o++) CHECK(free_list[o] == NULL);
    for (size_t i = 0; i < 64; i++) CHECK(arena[0x200000 + i] == SENTINEL);
}

static void test_add_free_page_chains_in_same_order(void) {
    setup(0);
    pmm_add_free_page(0x1000, 0);
    pmm_add_free_page(0x2000, 0);
    pmm_add_free_page(0x3000, 0);
    CHECK(count_order(0) == 3);
    CHECK(free_list[0] == (memory_node_t *)phys_to_virt(0x3000));
    CHECK(free_list[0]->prev == NULL);
    CHECK(free_list[0]->next->prev == free_list[0]);
}

/* Misaligned address for the requested order breaks the buddy invariant. */
static void test_add_free_page_rejects_misaligned_address(void) {
    setup(0);
    pmm_add_free_page(0x1000, 3);   /* order 3 needs 32 KiB alignment */
    CHECK(free_list[3] == NULL);
}

/* ============================ pmm_add_block ============================ */

static void test_add_block_single_page(void) {
    setup(0);
    pmm_add_block(0x5000, 0x1000);
    CHECK(count_order(0) == 1);
    CHECK(free_list[0] == (memory_node_t *)phys_to_virt(0x5000));
    check_cover(0x5000, 0x1000);
}

static void test_add_block_zero_size(void) {
    setup(0);
    pmm_add_block(0x5000, 0);
    check_cover(0x5000, 0);
    CHECK(nblocks == 0);
}

static void test_add_block_smaller_than_page(void) {
    setup(0);
    pmm_add_block(0x5000, 0xFFF);
    CHECK(nblocks == 0 || collect_blocks());
    for (unsigned o = 0; o <= MAX_ORDER; o++) CHECK(free_list[o] == NULL);
}

static void test_add_block_subpage_straddling_page_boundary(void) {
    /* 0x1800..0x2800 contains no whole page. */
    setup(0);
    pmm_add_block(0x1800, 0x1000);
    for (unsigned o = 0; o <= MAX_ORDER; o++) CHECK(free_list[o] == NULL);
}

static void test_add_block_unaligned_base_rounds_up(void) {
    setup(0);
    pmm_add_block(0x1001, 0x3000);   /* usable: 0x2000..0x4000 */
    check_cover(0x1001, 0x3000);
    CHECK(nblocks == 1 && blocks[0].start == 0x2000 && blocks[0].order == 1);
}

static void test_add_block_unaligned_end_rounds_down(void) {
    setup(0);
    pmm_add_block(0x2000, 0x2FFF);   /* usable: 0x2000..0x4000 */
    check_cover(0x2000, 0x2FFF);
    CHECK(nblocks == 1 && blocks[0].start == 0x2000 && blocks[0].order == 1);
}

static void test_add_block_at_zero_three_pages(void) {
    setup(0);
    pmm_add_block(0, 3 * PAGE_SIZE);
    /* page 0 is skipped: pages 1 and 2 remain, as two order-0 blocks
     * (they are not buddies; 1's buddy is 0, 2's is 3). */
    check_cover(0, 3 * PAGE_SIZE);
    CHECK(count_order(0) == 2 && count_order(1) == 0);
    CHECK(free_list[0] == (memory_node_t *)phys_to_virt(2 * PAGE_SIZE));
    CHECK(free_list[0]->next == (memory_node_t *)phys_to_virt(PAGE_SIZE));
}

static void test_add_block_exact_max_order(void) {
    /* A 2 MiB-aligned 2 MiB region is exactly one max-order block. */
    setup(0);
    pmm_add_block(2u * 1024 * 1024, 2u * 1024 * 1024);
    CHECK(count_order(MAX_ORDER) == 1);
    check_cover(2u * 1024 * 1024, 2u * 1024 * 1024);

    /* Starting at 0 loses page 0, so the same span can no longer form a
     * max-order block: pages 1..511 ramp up as orders 0..8. */
    setup(0);
    pmm_add_block(0, 2u * 1024 * 1024);
    CHECK(count_order(MAX_ORDER) == 0);
    for (unsigned o = 0; o <= 8; o++) CHECK(count_order(o) == 1);
    check_cover(0, 2u * 1024 * 1024);
}

static void test_add_block_larger_than_max_order_splits(void) {
    setup(0);
    pmm_add_block(2u * 1024 * 1024, 6u * 1024 * 1024);
    CHECK(count_order(MAX_ORDER) == 3);
    check_cover(2u * 1024 * 1024, 6u * 1024 * 1024);

    /* From 0: ramp over pages 1..511, then two full max-order blocks. */
    setup(0);
    pmm_add_block(0, 6u * 1024 * 1024);
    CHECK(count_order(MAX_ORDER) == 2);
    for (unsigned o = 0; o <= 8; o++) CHECK(count_order(o) == 1);
    check_cover(0, 6u * 1024 * 1024);
}

static void test_add_block_whole_arena(void) {
    setup(0);
    pmm_add_block(0, ARENA_SIZE);
    /* one 2 MiB block's worth of address space is lost to the page-0 ramp */
    CHECK(count_order(MAX_ORDER) == ARENA_SIZE / (2u * 1024 * 1024) - 1);
    check_cover(0, ARENA_SIZE);
    check_only_headers_touched();
}

static void test_add_block_ramp_up_and_down(void) {
    /* pages 1..511: expect one block per order 0..8, ascending then none. */
    setup(0);
    pmm_add_block(PAGE_SIZE, 511u * PAGE_SIZE);
    check_cover(PAGE_SIZE, 511u * PAGE_SIZE);
    for (unsigned o = 0; o <= 8; o++) CHECK(count_order(o) == 1);
    CHECK(count_order(MAX_ORDER) == 0);
}

static void test_add_block_straddles_2mib_boundary(void) {
    setup(0);
    paddr_t base = 2u * 1024 * 1024 - 3 * PAGE_SIZE;
    pmm_add_block(base, 6 * PAGE_SIZE);
    check_cover(base, 6 * PAGE_SIZE);
}

static void test_add_block_nonzero_fake_phys_base(void) {
    /* Address 0 is special-cased in pmm_add_block; try a 2 MiB-aligned
     * non-zero base, and a base that isn't aligned to anything in particular. */
    setup(4u * 1024 * 1024);
    pmm_add_block(4u * 1024 * 1024, 4u * 1024 * 1024);
    CHECK(count_order(MAX_ORDER) == 2);
    check_cover(4u * 1024 * 1024, 4u * 1024 * 1024);

    setup(0x1000);
    pmm_add_block(0x1000 + 0x7000, 0x13000);
    check_cover(0x1000 + 0x7000, 0x13000);
}

static void test_add_block_two_regions_accumulate(void) {
    setup(0);
    pmm_add_block(0x4000, 4 * PAGE_SIZE);
    pmm_add_block(0x100000, 4 * PAGE_SIZE);
    CHECK(count_order(2) == 2);
    CHECK(collect_blocks());
    CHECK(nblocks == 2);
}

static void test_add_block_never_frees_page_zero(void) {
    /* current_page == 0 must not reach ctz(0) (undefined). Page 0 is skipped,
     * so a 1-page region at 0 is empty... */
    setup(0);
    pmm_add_block(0, PAGE_SIZE);
    for (unsigned o = 0; o <= MAX_ORDER; o++) CHECK(free_list[o] == NULL);

    /* ...and a 2-page region at 0 yields just page 1. */
    setup(0);
    pmm_add_block(0, 2 * PAGE_SIZE);
    CHECK(count_order(0) == 1);
    CHECK(free_list[0] == (memory_node_t *)phys_to_virt(PAGE_SIZE));
    for (unsigned o = 1; o <= MAX_ORDER; o++) CHECK(free_list[o] == NULL);
}

/* Randomised property test over many (base, size) pairs. */
static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;
static uint64_t rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static void test_add_block_random_regions(void) {
    for (int iter = 0; iter < 300; iter++) {
        setup(0);
        paddr_t base = rng() % (ARENA_SIZE / 2);
        size_t size = rng() % (ARENA_SIZE / 2);
        pmm_add_block(base, size);
        check_cover(base, size);
        check_only_headers_touched();
        if (current_failed) {
            printf("    failing case: base=%#llx size=%#zx\n",
                   (unsigned long long)base, size);
            return;
        }
    }
}

/* ===================== pmm_free: merge bounds ========================== *
 *
 * pmm_free() merges a freed block with its buddy while the buddy's bit in
 * bitmap_list[order] is clear ("free"). These tests probe what happens when
 * the buddy lies outside the memory the PMM manages.
 *
 * The earlier tests never initialise bitmap_list (every bitmap has size 0),
 * which makes bitmap_get() answer "free" for everything. Here the bitmaps
 * are built the way pmm_init() builds them: sized from physical address 0
 * up to the end of RAM, every bit starting out "allocated".
 *
 * "Top of RAM" is a point inside the arena; the sentinel bytes above it
 * stand for memory that does not exist, so any write there is out of bounds.
 *
 * Every scenario runs in a forked child, because the expected failure mode
 * is a wild pointer dereference. Set PMM_SHOW_CRASH=1 to see the sanitizer
 * report from a crashing child instead of discarding it.
 */

/* The topmost block of RAM is freed and its buddy would sit entirely past
 * the end of RAM: the buddy's index is >= the bitmap's size. bitmap_get()
 * answers "free" (false) for out-of-range indices, so pmm_free() believes a
 * nonexistent buddy is free and tries to unlink a list node that was never
 * there. Correct behaviour: the block stays unmerged, nothing past the top
 * is touched. */
static unsigned scen_order;

static void scen_buddy_past_top(void) {
    const paddr_t base = 0x400000;      /* 2 MiB-aligned; also keeps buddy != 0 */
    const paddr_t bytes = (paddr_t)PAGE_SIZE << scen_order;
    const paddr_t top = base + bytes;   /* RAM ends right after this block */

    setup(base);
    setup_bitmaps(top);
    pmm_add_block(base, bytes);
    EXPECT(only_free_block_is(base, scen_order));       /* precondition */
    EXPECT(pmm_alloc(scen_order) == base);              /* precondition */
    EXPECT(bitmap_get(&bitmap_list[scen_order], base / bytes));
    EXPECT(base / bytes + 1 >= bitmap_list[scen_order].size); /* buddy is OOB */

    pmm_free(base, scen_order);                         /* <-- the probe */

    EXPECT(only_free_block_is(base, scen_order));
    EXPECT(!bitmap_get(&bitmap_list[scen_order], base / bytes));
    EXPECT(untouched_from(top));
}

static void test_free_top_block_does_not_merge_past_end_of_ram(void) {
    for (unsigned o = 0; o < MAX_ORDER; o++) {  /* order 9 never merges */
        char what[32];
        snprintf(what, sizeof what, "order %u", o);
        scen_order = o;
        CHECK(run_child(scen_buddy_past_top, what));
    }
}

/* Control: the buddy is past the last *free* memory but still inside the
 * bitmap, so its bit is still the initial "allocated". This must never
 * merge, today or after a fix; if only the test above fails, the problem is
 * specifically the out-of-range bitmap lookup. */
static void scen_buddy_in_hole(void) {
    const paddr_t base = 0x400000;
    setup(base);
    setup_bitmaps(base + 2 * PAGE_SIZE);    /* bitmap covers the buddy page */
    pmm_add_block(base, PAGE_SIZE);         /* ...but it was never freed */
    EXPECT(pmm_alloc(0) == base);
    EXPECT(base / PAGE_SIZE + 1 < bitmap_list[0].size);

    pmm_free(base, 0);

    EXPECT(only_free_block_is(base, 0));
    EXPECT(untouched_from(base + PAGE_SIZE));
}

static void test_free_next_to_never_freed_buddy_does_not_merge(void) {
    CHECK(run_child(scen_buddy_in_hole, "buddy inside bitmap, never freed"));
}

/* Freeing an address outside managed memory. pmm_add_free_page() writes the
 * list node unconditionally while bitmap_clear() silently ignores the
 * out-of-range index, so the bogus block lands on a free list with no bitmap
 * record and a later pmm_alloc() would hand out memory that doesn't exist.
 * Correct behaviour: reject it, touch nothing. */
static void scen_free_beyond_top(void) {
    const paddr_t base = 0x400000;
    const paddr_t top = base + 4 * PAGE_SIZE;
    setup(base);
    setup_bitmaps(top);
    pmm_add_block(base, 4 * PAGE_SIZE);
    EXPECT(count_order(2) == 1);            /* precondition */

    pmm_free(top + 0x10000, 0);             /* well past end of RAM */

    EXPECT(untouched_from(top));
    EXPECT(count_order(0) == 0);
    EXPECT(count_order(2) == 1);
}

static void test_free_address_beyond_end_of_ram_is_rejected(void) {
    CHECK(run_child(scen_free_beyond_top, "free 0x10000 past top"));
}

/* Merging must stop at MAX_ORDER: two free order-9 buddies stay separate
 * (there is no free_list[10]; get_buddy() returns 0 for such orders). */
static void scen_no_merge_past_max_order(void) {
    const paddr_t base = 0x400000;
    const paddr_t bytes = (paddr_t)PAGE_SIZE << MAX_ORDER;
    setup(base);
    setup_bitmaps(base + 2 * bytes);
    pmm_add_block(base, 2 * bytes);
    EXPECT(count_order(MAX_ORDER) == 2);
    paddr_t a = pmm_alloc(MAX_ORDER);
    paddr_t b = pmm_alloc(MAX_ORDER);
    EXPECT(a && b && (a ^ b) == bytes);     /* they are each other's buddy */

    pmm_free(a, MAX_ORDER);
    pmm_free(b, MAX_ORDER);

    EXPECT(count_order(MAX_ORDER) == 2);
    for (unsigned o = 0; o < MAX_ORDER; o++) EXPECT(free_list[o] == NULL);
    EXPECT(untouched_from(base + 2 * bytes));
}

static void test_free_never_merges_beyond_max_order(void) {
    CHECK(run_child(scen_no_merge_past_max_order, "two order-9 buddies"));
}

/* A merge cascade that has to find the buddy of an already-merged block.
 * Freeing page B+0x1000 (after B) merges with B into an order-1 block at B;
 * the next level must look up the buddy of *that* block (B+0x2000), but
 * pmm_free() keeps passing the original `addr` to get_buddy(), yielding
 * B+0x3000, which is not a block boundary at all. Same symptom as the
 * out-of-range case (an unlink of a node that doesn't exist) via a
 * different path. Correct behaviour: everything coalesces to one order-9
 * block at B. */
static void scen_cascade_uses_merged_block_for_buddy(void) {
    const paddr_t base = 0x400000;
    const paddr_t bytes = (paddr_t)PAGE_SIZE << MAX_ORDER;
    setup(base);
    setup_bitmaps(base + bytes);
    pmm_add_block(base, bytes);
    EXPECT(only_free_block_is(base, MAX_ORDER));
    paddr_t p0 = pmm_alloc(0);
    paddr_t p1 = pmm_alloc(0);
    EXPECT(p0 == base && p1 == base + PAGE_SIZE);

    pmm_free(p0, 0);                        /* buddy still allocated */
    pmm_free(p1, 0);                        /* merges, then must keep going */

    EXPECT(only_free_block_is(base, MAX_ORDER));
}

static void test_free_cascade_coalesces_to_max_order(void) {
    CHECK(run_child(scen_cascade_uses_merged_block_for_buddy,
                    "free B, then B+4K, expect one order-9 block"));
}

/* ---- documented-but-untestable: wraparound ------------------------------
 * pmm_add_block(0xFFFFFFFFFFFFF001, 0xFFE) computes end = ...F000 and rounds
 * base up to 2^64 == 0, so the loop would "free" physical memory starting at
 * 0. It can't be exercised on the host without writing through wild
 * pointers, so it is argued from the code in the report instead. */

int main(void) {
    printf("-- list_add / list_remove\n");
    RUN(test_list_add_empty);
    RUN(test_list_add_pushes_front_and_links);
    RUN(test_list_add_null_args);
    RUN(test_list_add_overwrites_stale_links);
    RUN(test_list_remove_returns_head_lifo);
    RUN(test_list_remove_single_element);
    RUN(test_list_remove_null_head_ptr);
    RUN(test_list_remove_empty_list_returns_null);
    RUN(test_list_remove_detaches_node);
    RUN(test_list_add_same_node_twice_is_not_a_cycle);

    printf("-- get_hhdm_offset\n");
    RUN(test_get_hhdm_offset_reads_response);
    RUN(test_get_hhdm_offset_null_response_halts_keeps_value);

    printf("-- pmm_add_free_page\n");
    RUN(test_add_free_page_places_node_at_hhdm_address);
    RUN(test_add_free_page_each_order_lands_in_its_list);
    RUN(test_add_free_page_order_too_large_is_ignored);
    RUN(test_add_free_page_chains_in_same_order);
    RUN(test_add_free_page_rejects_misaligned_address);

    printf("-- pmm_add_block\n");
    RUN(test_add_block_single_page);
    RUN(test_add_block_zero_size);
    RUN(test_add_block_smaller_than_page);
    RUN(test_add_block_subpage_straddling_page_boundary);
    RUN(test_add_block_unaligned_base_rounds_up);
    RUN(test_add_block_unaligned_end_rounds_down);
    RUN(test_add_block_at_zero_three_pages);
    RUN(test_add_block_exact_max_order);
    RUN(test_add_block_larger_than_max_order_splits);
    RUN(test_add_block_whole_arena);
    RUN(test_add_block_ramp_up_and_down);
    RUN(test_add_block_straddles_2mib_boundary);
    RUN(test_add_block_nonzero_fake_phys_base);
    RUN(test_add_block_two_regions_accumulate);
    RUN(test_add_block_never_frees_page_zero);
    RUN(test_add_block_random_regions);

    printf("-- pmm_free merge bounds\n");
    RUN(test_free_top_block_does_not_merge_past_end_of_ram);
    RUN(test_free_next_to_never_freed_buddy_does_not_merge);
    RUN(test_free_address_beyond_end_of_ram_is_rejected);
    RUN(test_free_never_merges_beyond_max_order);
    RUN(test_free_cascade_coalesces_to_max_order);

    printf("\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
