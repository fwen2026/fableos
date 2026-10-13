/*
 * Tests for the "unmanaged page" protection: unmanaged_list, mark_managed(),
 * block_is_managed(), the pmm_init() that builds it, and the pmm_free() guard
 * that refuses to touch memory the PMM never owned.
 *
 * unmanaged_list has one bit per page. Set = not ours (page 0, the PMM's own
 * metadata, holes, non-usable regions, rounding slack). It starts all-set and
 * pmm_add_block() clears the pages it takes in.
 *
 * Groups:
 *   block_is_managed   the range check on its own, every order, word edges
 *   pmm_add_block      which pages it marks (and doesn't) as managed
 *   pmm_init           the metadata layout and the resulting managed set, for
 *                      several fake Limine memory maps
 *   pmm_free           rejects every kind of unmanaged block without touching
 *                      anything, and still accepts legitimate frees
 */
#include "pmm_harness.h"

#define BASE        ((paddr_t)0x400000)
#define BLOCK(o)    ((paddr_t)PAGE_SIZE << (o))
#define UNMANAGED_MSG "Freeing a block that touches unmanaged memory"

static int page_managed(paddr_t addr) {
    return !bitmap_get(&unmanaged_list, addr / PAGE_SIZE);
}

/* ===================== block_is_managed (range check) =================== */

/* For every order: exactly this block managed -> true; the block next to it
 * (unmanaged) -> false; the parent (covers the unmanaged sibling) -> false;
 * and a single unmanaged page anywhere inside it (first, middle, last, the
 * word edges) flips the answer to false. */
static void test_block_is_managed_every_order(void) {
    for (unsigned o = 0; o <= MAX_ORDER; o++) {
        setup(BASE);
        setup_bitmaps(BASE + 4 * BLOCK(MAX_ORDER));
        mark_managed(BASE, o);

        CHECK(block_is_managed(BASE, o));
        CHECK(!block_is_managed(BASE + BLOCK(o), o));       /* neighbour */
        if (o < MAX_ORDER) CHECK(!block_is_managed(BASE, o + 1));

        size_t first = BASE / PAGE_SIZE, count = (size_t)1 << o;
        size_t probes[] = { 0, count / 2, count - 1, 31, 32, 63, 64 };
        for (size_t k = 0; k < sizeof probes / sizeof probes[0]; k++) {
            if (probes[k] >= count) continue;
            bitmap_set_allocate(&unmanaged_list, first + probes[k]);
            CHECK(!block_is_managed(BASE, o));
            bitmap_set_clear(&unmanaged_list, first + probes[k]);
            CHECK(block_is_managed(BASE, o));
        }
    }
}

/* A block that sits inside one word must only look at its own bits: unmanaged
 * pages on either side in the same word are irrelevant. */
static void test_block_is_managed_ignores_neighbours_in_same_word(void) {
    setup(BASE);
    setup_bitmaps(BASE + BLOCK(MAX_ORDER));
    paddr_t blk = BASE + 8 * PAGE_SIZE;                     /* order 3: pages 8..15 */
    mark_managed(blk, 3);
    CHECK(block_is_managed(blk, 3));
    CHECK(!page_managed(blk - PAGE_SIZE));
    CHECK(!page_managed(blk + BLOCK(3)));
}

/* A block past the end of the bitmap is unmanaged; one ending exactly at the
 * end is fine. */
static void test_block_is_managed_bitmap_end(void) {
    setup(BASE);
    setup_bitmaps(BASE + BLOCK(MAX_ORDER));
    size_t pages = unmanaged_list.size;
    paddr_t last = (paddr_t)(pages - 1) * PAGE_SIZE;
    mark_managed(last, 0);
    CHECK(block_is_managed(last, 0));
    CHECK(!block_is_managed(last + PAGE_SIZE, 0));          /* one past the end */
    CHECK(!block_is_managed((paddr_t)pages * PAGE_SIZE * 4, 0));
}

static void test_block_is_managed_empty_bitmap_rejects_everything(void) {
    setup(BASE);                                            /* bitmaps size 0 */
    CHECK(!block_is_managed(BASE, 0));
    CHECK(!block_is_managed(0, MAX_ORDER));
}

/* ========================= pmm_add_block marking ======================== */

static void test_add_block_marks_pages_managed(void) {
    setup(BASE);
    setup_bitmaps(BASE + BLOCK(MAX_ORDER));
    pmm_add_block(BASE, 5 * PAGE_SIZE);
    for (int i = 0; i < 5; i++) CHECK(page_managed(BASE + i * PAGE_SIZE));
    CHECK(!page_managed(BASE + 5 * PAGE_SIZE));
    CHECK(!page_managed(BASE - PAGE_SIZE));
}

/* Page 0 is never handed out by pmm_add_block, so it must stay unmanaged. */
static void test_add_block_leaves_page_zero_unmanaged(void) {
    setup(0);
    setup_bitmaps(BLOCK(4));
    pmm_add_block(0, 4 * PAGE_SIZE);
    CHECK(!page_managed(0));
    CHECK(page_managed(PAGE_SIZE) && page_managed(2 * PAGE_SIZE) && page_managed(3 * PAGE_SIZE));
}

/* Unaligned edges round inward: partly covered pages are not claimed. */
static void test_add_block_unaligned_edges_do_not_claim_partial_pages(void) {
    setup(BASE);
    setup_bitmaps(BASE + BLOCK(MAX_ORDER));
    pmm_add_block(BASE + 0x800, 3 * PAGE_SIZE);             /* [BASE+0x800, BASE+0x3800) */
    CHECK(!page_managed(BASE));
    CHECK(page_managed(BASE + PAGE_SIZE));
    CHECK(page_managed(BASE + 2 * PAGE_SIZE));
    CHECK(!page_managed(BASE + 3 * PAGE_SIZE));
}

static void test_add_block_zero_size_marks_nothing(void) {
    setup(BASE);
    setup_bitmaps(BASE + BLOCK(MAX_ORDER));
    pmm_add_block(BASE, 0);
    for (size_t i = 0; i < unmanaged_list.size; i++) CHECK(bitmap_get(&unmanaged_list, i));
}

/* A region that decomposes into several orders is marked page for page. */
static void test_add_block_mixed_orders_marks_exact_cover(void) {
    setup(BASE);
    setup_bitmaps(BASE + BLOCK(MAX_ORDER));
    size_t pages = 1 + 2 + 4 + 8 + 3;                       /* odd-sized region */
    pmm_add_block(BASE + PAGE_SIZE, pages * PAGE_SIZE);
    for (size_t i = 0; i < unmanaged_list.size; i++) {
        paddr_t a = (paddr_t)i * PAGE_SIZE;
        int want = a >= BASE + PAGE_SIZE && a < BASE + (1 + pages) * PAGE_SIZE;
        CHECK(page_managed(a) == want);
    }
}

/* ============================ pmm_init ================================== */

static struct limine_memmap_entry *map_ptrs[8];
static struct limine_memmap_response map_resp;

/* Fake Limine memory map. All usable entries are page-aligned (as the
 * protocol guarantees); the reserved one deliberately is not. */
static struct limine_memmap_entry E_LOW      = { 0x001000, 0x09E000, LIMINE_MEMMAP_USABLE   };
static struct limine_memmap_entry E_RESERVED = { 0x09FC00, 0x002000, LIMINE_MEMMAP_RESERVED }; /* ends mid page 0xA1 */
static struct limine_memmap_entry E_HIGH     = { 0x100000, 0x300000, LIMINE_MEMMAP_USABLE   };
static struct limine_memmap_entry E_ZERO     = { 0x000000, 0x09F000, LIMINE_MEMMAP_USABLE   };

static size_t map_n;

/* The simulated arena covers phys [arena_base, arena_base + 16 MiB); every
 * usable entry must lie inside it because pmm_add_block writes node headers. */
static void init_with_map_at(paddr_t arena_base, struct limine_memmap_entry **entries, size_t n) {
    setup(arena_base);
    for (size_t i = 0; i < n; i++) map_ptrs[i] = entries[i];
    map_n = n;
    map_resp.revision = 0;
    map_resp.entry_count = n;
    map_resp.entries = map_ptrs;
    memmap_request.response = &map_resp;
    pmm_init();
}

static void init_with_map(struct limine_memmap_entry **entries, size_t n) {
    init_with_map_at(0, entries, n);
}

static void init_standard(void) {
    struct limine_memmap_entry *m[] = { &E_LOW, &E_RESERVED, &E_HIGH };
    init_with_map(m, 3);
}

static paddr_t meta_start(void) { return virt_to_phys(bitmap_list[0].value); }
static paddr_t meta_end(void) {
    return virt_to_phys(unmanaged_list.value) + bitmap_get_size(&unmanaged_list);
}

/* Independent oracle: a page should be managed iff it lies fully inside a
 * usable entry, is not page 0, and does not overlap the metadata area. */
static int expected_managed(size_t page) {
    paddr_t lo = (paddr_t)page * PAGE_SIZE, hi = lo + PAGE_SIZE;
    if (page == 0) return 0;
    if (lo < meta_end() && hi > meta_start()) return 0;
    for (size_t i = 0; i < map_n; i++) {
        struct limine_memmap_entry *e = map_ptrs[i];
        if (e->type == LIMINE_MEMMAP_USABLE && lo >= e->base && hi <= e->base + e->length) return 1;
    }
    return 0;
}

static void check_managed_set_matches_map(void) {
    size_t bad = 0;
    for (size_t p = 0; p < unmanaged_list.size; p++) {
        if (page_managed((paddr_t)p * PAGE_SIZE) != expected_managed(p)) {
            if (bad++ < 3) printf("    page 0x%zx: managed=%d, expected %d\n", p,
                                  page_managed((paddr_t)p * PAGE_SIZE), expected_managed(p));
        }
    }
    CHECK(bad == 0);
}

static void test_init_sizes_and_back_to_back_layout(void) {
    init_standard();
    size_t pages = 0x400000 / PAGE_SIZE;
    CHECK(unmanaged_list.size == pages);
    for (unsigned o = 0; o <= MAX_ORDER; o++) CHECK(bitmap_list[o].size == (pages + (1u << o) - 1) >> o);

    CHECK(meta_start() == E_LOW.base);                      /* first usable entry that fits */
    for (unsigned o = 0; o < MAX_ORDER; o++)
        CHECK(bitmap_list[o + 1].value == bitmap_list[o].value + bitmap_words(bitmap_list[o].size));
    CHECK(unmanaged_list.value == bitmap_list[MAX_ORDER].value + bitmap_words(bitmap_list[MAX_ORDER].size));
    CHECK(meta_end() <= E_LOW.base + E_LOW.length);
}

static void test_init_managed_set_standard_map(void) {
    init_standard();
    check_managed_set_matches_map();
    EXPECT_STATUS(PASS);
}

static void test_init_metadata_pages_are_unmanaged_and_next_page_is_managed(void) {
    init_standard();
    for (paddr_t a = meta_start() & ~(paddr_t)(PAGE_SIZE - 1); a < meta_end(); a += PAGE_SIZE)
        CHECK(!page_managed(a));
    paddr_t next = (meta_end() + PAGE_SIZE - 1) & ~(paddr_t)(PAGE_SIZE - 1);
    CHECK(page_managed(next));
}

/* Every page the unaligned reserved entry touches is unmanaged, including the
 * last one it only partly covers (0xA1). */
static void test_init_unaligned_reserved_entry_pages_unmanaged(void) {
    init_standard();
    CHECK(!page_managed(0x9F000));
    CHECK(!page_managed(0xA0000));
    CHECK(!page_managed(0xA1000));
}

static void test_init_holes_and_page_zero_unmanaged(void) {
    init_standard();
    CHECK(!page_managed(0));
    for (paddr_t a = 0xA2000; a < 0x100000; a += PAGE_SIZE) CHECK(!page_managed(a));
}

/* Limine happens to sort entries, but pmm_init shouldn't depend on it: the
 * bitmaps must still be sized to the highest usable address. */
static void test_init_unsorted_map_sizes_to_highest_usable(void) {
    struct limine_memmap_entry *m[] = { &E_HIGH, &E_RESERVED, &E_LOW };
    init_with_map(m, 3);
    CHECK(unmanaged_list.size == 0x400000 / PAGE_SIZE);
    CHECK(meta_start() == E_HIGH.base);                     /* first entry in list order that fits */
    check_managed_set_matches_map();
    EXPECT_STATUS(PASS);
}

/* A usable entry that starts at address 0: page 0 stays unmanaged even though
 * it is "usable". (Metadata goes into the other entry so it isn't what hides it.) */
static void test_init_usable_page_zero_stays_unmanaged(void) {
    struct limine_memmap_entry *m[] = { &E_HIGH, &E_ZERO, &E_RESERVED };
    init_with_map(m, 3);
    CHECK(meta_start() == E_HIGH.base);
    CHECK(!page_managed(0));
    CHECK(page_managed(PAGE_SIZE));
    check_managed_set_matches_map();
}

/* A 64 MiB address space: the metadata is ~6 KiB, so it spans two pages and
 * its end falls mid-page. A small map hides sizing mistakes behind page
 * rounding; this one doesn't. Everything below 48 MiB is unmanaged, which
 * also exercises a bitmap much larger than the memory actually usable. */
static void test_init_large_map_metadata_spans_pages(void) {
    static struct limine_memmap_entry big = { 0x3001000, 0x4000000 - 0x3001000, LIMINE_MEMMAP_USABLE };
    struct limine_memmap_entry *m[] = { &big };
    init_with_map_at(0x3000000, m, 1);

    CHECK(unmanaged_list.size == 0x4000000 / PAGE_SIZE);
    CHECK(meta_end() - meta_start() > PAGE_SIZE);           /* really spans pages */
    CHECK((meta_end() & (PAGE_SIZE - 1)) != 0);             /* ...and ends mid-page */
    CHECK(meta_start() == big.base);

    paddr_t last_meta_page = (meta_end() - 1) & ~(paddr_t)(PAGE_SIZE - 1);
    CHECK(!page_managed(last_meta_page));
    CHECK(page_managed(last_meta_page + PAGE_SIZE));
    check_managed_set_matches_map();
    EXPECT_STATUS(PASS);
}

/* ============================ pmm_free guard ============================ */

typedef struct {
    memory_node_t *heads[MAX_ORDER + 1];
    uint8_t meta[2048];
    size_t meta_len;
    uint8_t target[2 * sizeof(memory_node_t)];
} snapshot_t;

static void take_snapshot(snapshot_t *s, paddr_t target) {
    memcpy(s->heads, free_list, sizeof free_list);
    s->meta_len = meta_end() - meta_start();
    if (s->meta_len > sizeof s->meta) { printf("metadata larger than snapshot\n"); exit(2); }
    memcpy(s->meta, phys_to_virt(meta_start()), s->meta_len);
    memcpy(s->target, phys_to_virt(target), sizeof s->target);
}

static int snapshot_unchanged(const snapshot_t *s, paddr_t target) {
    return memcmp(s->heads, free_list, sizeof free_list) == 0 &&
           memcmp(s->meta, phys_to_virt(meta_start()), s->meta_len) == 0 &&
           memcmp(s->target, phys_to_virt(target), sizeof s->target) == 0;
}

/* The free must be refused by the unmanaged-memory guard specifically (not
 * by an earlier alignment/bounds check or by the double-free check), and
 * nothing may change: free lists, bitmaps, and the target's own bytes. */
static void expect_rejected_as_unmanaged(paddr_t addr, unsigned order) {
    snapshot_t s;
    take_snapshot(&s, addr);
    error_calls = 0;
    last_error = NULL;

    pmm_free(addr, order);

    CHECK(error_calls == 1);
    CHECK(last_error && strcmp(last_error, UNMANAGED_MSG) == 0);
    CHECK(snapshot_unchanged(&s, addr));
    EXPECT_STATUS(PASS);
}

static void test_free_rejects_page_zero(void) {
    init_standard();
    EXPECT_STATUS(PASS);
    expect_rejected_as_unmanaged(0, 0);
}

static void test_free_rejects_metadata_page(void) {
    init_standard();
    expect_rejected_as_unmanaged(meta_start() & ~(paddr_t)(PAGE_SIZE - 1), 0);
}

static void test_free_rejects_unaligned_reserved_entry_tail_page(void) {
    init_standard();
    expect_rejected_as_unmanaged(0xA1000, 0);
}

static void test_free_rejects_hole_between_regions(void) {
    init_standard();
    expect_rejected_as_unmanaged(0xB0000, 0);
    expect_rejected_as_unmanaged(0xC0000, 3);               /* order-3 block, wholly in the hole */
}

/* Half managed, half not: pages 0x9E (usable) and 0x9F (reserved). */
static void test_free_rejects_block_straddling_region_edge(void) {
    init_standard();
    CHECK(page_managed(0x9E000) && !page_managed(0x9F000));
    expect_rejected_as_unmanaged(0x9E000, 1);
}

/* Multi-word blocks take the word-scan path of block_is_managed. */
static void test_free_rejects_large_blocks_containing_unmanaged_pages(void) {
    init_standard();
    expect_rejected_as_unmanaged(0x80000, 6);               /* 64 pages: covers reserved + hole */
    expect_rejected_as_unmanaged(0, MAX_ORDER);             /* 2 MiB: page 0, metadata, hole, ... */
}

static void count_orders(unsigned out[MAX_ORDER + 1]) {
    for (unsigned o = 0; o <= MAX_ORDER; o++) out[o] = count_order(o);
}

/* False-positive guard: blocks the PMM really handed out can be returned, at
 * the orders that exercise both the single-word and the word-scan paths. */
static void test_free_accepts_allocated_blocks(void) {
    unsigned orders[] = { 0, 1, 3, 5, 6, 8, MAX_ORDER };
    for (size_t k = 0; k < sizeof orders / sizeof orders[0]; k++) {
        unsigned o = orders[k];
        init_standard();
        unsigned before[MAX_ORDER + 1], after[MAX_ORDER + 1];
        count_orders(before);

        paddr_t p = pmm_alloc(o);
        CHECK(p != 0);
        CHECK(page_managed(p));

        error_calls = 0;
        pmm_free(p, o);
        CHECK(error_calls == 0);

        count_orders(after);
        CHECK(memcmp(before, after, sizeof before) == 0);   /* free lists back to their original shape */
        EXPECT_STATUS(PASS);
    }
}

/* Draining every page and giving them all back must never trip the guard. */
static void test_free_accepts_every_page_of_a_drained_region(void) {
    init_standard();
    enum { MAXP = 1024 };
    static paddr_t got[MAXP];
    size_t n = 0;
    while (n < MAXP && (got[n] = pmm_alloc(0)) != 0) n++;
    CHECK(n > 0 && n < MAXP);

    error_calls = 0;
    for (size_t i = 0; i < n; i++) pmm_free(got[i], 0);
    CHECK(error_calls == 0);
    EXPECT_STATUS(PASS);
    CHECK(count_order(MAX_ORDER) >= 1);                     /* it coalesced back */
}

int main(void) {
    printf("-- block_is_managed\n");
    RUN(test_block_is_managed_every_order);
    RUN(test_block_is_managed_ignores_neighbours_in_same_word);
    RUN(test_block_is_managed_bitmap_end);
    RUN(test_block_is_managed_empty_bitmap_rejects_everything);

    printf("-- pmm_add_block marking\n");
    RUN(test_add_block_marks_pages_managed);
    RUN(test_add_block_leaves_page_zero_unmanaged);
    RUN(test_add_block_unaligned_edges_do_not_claim_partial_pages);
    RUN(test_add_block_zero_size_marks_nothing);
    RUN(test_add_block_mixed_orders_marks_exact_cover);

    printf("-- pmm_init\n");
    RUN(test_init_sizes_and_back_to_back_layout);
    RUN(test_init_managed_set_standard_map);
    RUN(test_init_metadata_pages_are_unmanaged_and_next_page_is_managed);
    RUN(test_init_unaligned_reserved_entry_pages_unmanaged);
    RUN(test_init_holes_and_page_zero_unmanaged);
    RUN(test_init_unsorted_map_sizes_to_highest_usable);
    RUN(test_init_usable_page_zero_stays_unmanaged);
    RUN(test_init_large_map_metadata_spans_pages);

    printf("-- pmm_free guard\n");
    RUN(test_free_rejects_page_zero);
    RUN(test_free_rejects_metadata_page);
    RUN(test_free_rejects_unaligned_reserved_entry_tail_page);
    RUN(test_free_rejects_hole_between_regions);
    RUN(test_free_rejects_block_straddling_region_edge);
    RUN(test_free_rejects_large_blocks_containing_unmanaged_pages);
    RUN(test_free_accepts_allocated_blocks);
    RUN(test_free_accepts_every_page_of_a_drained_region);

    printf("\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
