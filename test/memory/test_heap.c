/*
 * Host-side tests for memory/heap.c: kmalloc and kfree on top of the real PMM
 * (pmm.c is compiled into the same translation unit, running on the simulated
 * arena from pmm_harness.h). heap.c is #included so the tests can inspect the
 * cache lists and slab metadata directly.
 *
 * check_heap() is the oracle: after operations it verifies every slab list
 * structurally (links, identifiers, free-list contents, which list each slab
 * belongs on). Tests also compare the PMM's free pages before/after and run
 * pmm_check(), so a leak or a corrupted free list shows up even if the heap's
 * own lists look fine.
 *
 * Groups:
 *   slab        small objects: list transitions, reuse, alignment, spare policy
 *   large       allocations above 2048 bytes, straight from the PMM
 *   rejected    kfree() of anything that isn't a live allocation must be
 *               logged and change nothing
 */
#include "pmm_harness.h"
#include "../../src/memory/heap.c"

#define BASE          ((paddr_t)0x400000)
#define HEAP_PMM_SIZE ((size_t)8 * 1024 * 1024)
#define MAX_PTRS      4096

static void fresh_heap(void) {
    setup(BASE);
    setup_bitmaps(BASE + HEAP_PMM_SIZE);
    pmm_add_block(BASE, HEAP_PMM_SIZE);
    heap_init();
}

static size_t pmm_free_pages(void) {
    size_t n = 0;
    for (unsigned o = 0; o <= MAX_ORDER; o++) n += (size_t)count_order(o) << o;
    return n;
}

static slab_t *slab_of(void *p) { return (slab_t *)((uintptr_t)p & ~((uintptr_t)SLAB_SIZE - 1)); }

static size_t slab_cap(slab_t *s) {
    return ((uintptr_t)s + SLAB_SIZE - slab_first_object(s)) / (16UL << s->cache_idx);
}

/* ---- the oracle -------------------------------------------------------- */

#define MAX_SEEN_SLABS 512
static slab_t *seen_slabs[MAX_SEEN_SLABS];
static size_t n_seen_slabs;

enum { KIND_EMPTY, KIND_PARTIAL, KIND_FULL };

static size_t check_slab_list(slab_t *head, unsigned idx, int kind) {
    size_t count = 0;
    slab_t *prev = NULL;
    for (slab_t *s = head; s != NULL; prev = s, s = s->next) {
        if (++count > MAX_SEEN_SLABS) { CHECK(!"slab list too long or cyclic"); break; }

        CHECK(s->prev == prev);
        CHECK(s->identifier == IDENTIFIER_CACHE);
        CHECK(s->cache_idx == idx);
        CHECK(((uintptr_t)s & (SLAB_SIZE - 1)) == 0);
        CHECK((uint8_t *)s >= arena && (uint8_t *)s < arena + ARENA_SIZE);

        for (size_t i = 0; i < n_seen_slabs; i++) CHECK(seen_slabs[i] != s);   /* on exactly one list */
        if (n_seen_slabs < MAX_SEEN_SLABS) seen_slabs[n_seen_slabs++] = s;

        size_t bs = 16UL << idx, cap = slab_cap(s);
        uintptr_t first = slab_first_object(s);
        bool seen[256] = { 0 };
        size_t nfree = 0;
        for (slab_node_t *n = s->free_head; n != NULL && nfree <= cap; n = n->next, nfree++) {
            uintptr_t a = (uintptr_t)n;
            if (!(a >= first && a < (uintptr_t)s + SLAB_SIZE && (a - first) % bs == 0)) {
                CHECK(!"free object outside the slab or off its grid");
                break;
            }
            size_t k = (a - first) / bs;
            CHECK(!seen[k]);                    /* no object twice on the free list */
            seen[k] = true;
        }
        CHECK(nfree <= cap);
        CHECK(nfree + s->num_occupied == cap);

        if (kind == KIND_EMPTY)   CHECK(s->num_occupied == 0);
        if (kind == KIND_PARTIAL) CHECK(s->num_occupied > 0 && s->free_head != NULL);
        if (kind == KIND_FULL)    CHECK(s->free_head == NULL && s->num_occupied == cap);
    }
    return count;
}

/* Returns the total number of slabs (= PMM pages the small-object heap holds). */
static size_t check_heap(void) {
    n_seen_slabs = 0;
    size_t total = 0;
    for (unsigned i = 0; i < CACHE_COUNT; i++) {
        size_t e = check_slab_list(cache_list[i].empty_head,   i, KIND_EMPTY);
        size_t p = check_slab_list(cache_list[i].partial_head, i, KIND_PARTIAL);
        size_t f = check_slab_list(cache_list[i].full_head,    i, KIND_FULL);
        CHECK(e <= 1);                          /* at most one spare per cache */
        total += e + p + f;
    }
    return total;
}

/* ================================ slab =================================== */

static void test_slab_alloc_free_keeps_one_spare_slab(void) {
    fresh_heap();
    size_t pages = pmm_free_pages();
    void *p = kmalloc(16);
    CHECK(p != NULL);
    CHECK(check_heap() == 1);
    CHECK(pages - pmm_free_pages() == 1);

    kfree(p);
    CHECK(check_heap() == 1);                   /* kept as the spare, not given back */
    CHECK(cache_list[0].empty_head == slab_of(p));
    CHECK(pages - pmm_free_pages() == 1);
    EXPECT_STATUS(PASS);
}

static void test_slab_freed_object_is_reused_first(void) {
    fresh_heap();
    void *a = kmalloc(32), *b = kmalloc(32);
    CHECK(a && b && a != b);
    kfree(a);
    CHECK(kmalloc(32) == a);
    kfree(b);
    check_heap();
}

/* Alloc exactly one slab's worth, then free it back, in every size class:
 * partial -> full -> partial -> empty, including the 2048-byte class whose
 * slab holds a single object (full -> empty directly). */
static void test_slab_lists_follow_occupancy_in_every_class(void) {
    static void *ptrs[MAX_PTRS];
    for (unsigned idx = 0; idx < CACHE_COUNT; idx++) {
        fresh_heap();
        size_t size = 16UL << idx;
        ptrs[0] = kmalloc(size);
        CHECK(ptrs[0] != NULL);
        slab_t *slab = slab_of(ptrs[0]);
        size_t cap = slab_cap(slab);

        if (cap > 1) CHECK(cache_list[idx].partial_head == slab);
        for (size_t i = 1; i < cap; i++) ptrs[i] = kmalloc(size);
        CHECK(cache_list[idx].full_head == slab);
        CHECK(cache_list[idx].partial_head == NULL && cache_list[idx].empty_head == NULL);
        CHECK(check_heap() == 1);

        kfree(ptrs[cap - 1]);
        if (cap > 1) {
            CHECK(cache_list[idx].partial_head == slab && cache_list[idx].full_head == NULL);
        } else {
            CHECK(cache_list[idx].empty_head == slab && cache_list[idx].full_head == NULL);
        }
        check_heap();

        for (size_t i = 0; i + 1 < cap; i++) kfree(ptrs[i]);
        CHECK(cache_list[idx].empty_head == slab);
        CHECK(cache_list[idx].partial_head == NULL && cache_list[idx].full_head == NULL);
        CHECK(check_heap() == 1);
        EXPECT_STATUS(PASS);
    }
}

/* Two full slabs, everything freed: one stays as the spare, the other's page
 * must go back to the PMM. */
static void test_slab_second_empty_slab_is_released_to_pmm(void) {
    static void *ptrs[MAX_PTRS];
    fresh_heap();
    size_t pages = pmm_free_pages();
    void *probe = kmalloc(16);
    size_t cap = slab_cap(slab_of(probe));
    kfree(probe);

    size_t n = 2 * cap;
    for (size_t i = 0; i < n; i++) ptrs[i] = kmalloc(16);
    CHECK(check_heap() == 2);
    CHECK(pages - pmm_free_pages() == 2);

    for (size_t i = 0; i < n; i++) kfree(ptrs[i]);
    CHECK(check_heap() == 1);
    CHECK(pages - pmm_free_pages() == 1);
    EXPECT_STATUS(PASS);
}

/* Free in a scrambled order and check every list invariant after each free. */
static void test_slab_random_free_order_keeps_invariants(void) {
    static void *ptrs[MAX_PTRS];
    fresh_heap();
    size_t pages = pmm_free_pages();
    void *probe = kmalloc(64);
    size_t cap = slab_cap(slab_of(probe));
    kfree(probe);

    size_t n = 3 * cap;
    for (size_t i = 0; i < n; i++) ptrs[i] = kmalloc(64);
    CHECK(check_heap() == 3);

    uint64_t rng = 0x9E3779B97F4A7C15ULL;
    for (size_t i = n - 1; i > 0; i--) {                    /* Fisher-Yates */
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        size_t j = rng % (i + 1);
        void *t = ptrs[i]; ptrs[i] = ptrs[j]; ptrs[j] = t;
    }
    for (size_t i = 0; i < n; i++) {
        kfree(ptrs[i]);
        check_heap();
    }
    CHECK(check_heap() == 1);
    CHECK(pages - pmm_free_pages() == 1);
    EXPECT_STATUS(PASS);
}

/* Every class: objects are aligned to their size, never overlap, and keep
 * their contents while neighbours are freed. */
static void test_slab_objects_aligned_disjoint_and_intact(void) {
    static void *ptrs[MAX_PTRS];
    for (unsigned idx = 0; idx < CACHE_COUNT; idx++) {
        fresh_heap();
        size_t bs = 16UL << idx;
        void *probe = kmalloc(bs);
        size_t cap = slab_cap(slab_of(probe));
        kfree(probe);

        size_t n = 2 * cap;
        for (size_t i = 0; i < n; i++) {
            ptrs[i] = kmalloc(bs);
            CHECK(ptrs[i] != NULL);
            CHECK(((uintptr_t)ptrs[i] % bs) == 0);
            memset(ptrs[i], (int)(i * 7 + 1) & 0xFF, bs);
        }
        for (size_t i = 0; i < n; i++)
            for (size_t j = i + 1; j < n; j++) {
                uintptr_t a = (uintptr_t)ptrs[i], b = (uintptr_t)ptrs[j];
                CHECK(a + bs <= b || b + bs <= a);
            }

        for (size_t i = 0; i < n; i += 2) kfree(ptrs[i]);   /* free every other one */
        for (size_t i = 1; i < n; i += 2) {
            uint8_t want = (uint8_t)((i * 7 + 1) & 0xFF);
            const uint8_t *p = ptrs[i];
            for (size_t k = 0; k < bs; k++) if (p[k] != want) { CHECK(!"live object was overwritten"); break; }
        }
        for (size_t i = 1; i < n; i += 2) kfree(ptrs[i]);
        check_heap();
        EXPECT_STATUS(PASS);
    }
}

static void test_slab_size_routing(void) {
    fresh_heap();
    size_t sizes[] = { 0, 1, 15, 16, 17, 32, 33, 64, 65, 128, 129, 256, 257, 512, 513, 1024, 1025, 2047, 2048 };
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        size_t want = sizes[i] ? sizes[i] : 1;
        unsigned idx = 0;
        while ((16UL << idx) < want) idx++;                 /* smallest class that fits */
        void *p = kmalloc(sizes[i]);
        CHECK(p != NULL);
        if (p) CHECK(slab_of(p)->cache_idx == idx);
        kfree(p);
    }
    check_heap();
}

/* ================================ large ================================== */

static unsigned expected_large_order(size_t size) {
    unsigned o = 0;
    while (((size_t)PAGE_SIZE << o) < size + LARGE_HEADER_SIZE) o++;
    return o;
}

static void test_large_alloc_free_restores_pmm_exactly(void) {
    size_t sizes[] = { 2049, 4080, 4081, 8176, 8177, 100000, MAX_LARGE_SIZE };
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        fresh_heap();
        unsigned before[MAX_ORDER + 1], after[MAX_ORDER + 1];
        for (unsigned o = 0; o <= MAX_ORDER; o++) before[o] = count_order(o);

        void *p = kmalloc(sizes[i]);
        CHECK(p != NULL);
        if (!p) continue;
        CHECK((uintptr_t)p % PAGE_SIZE == LARGE_HEADER_SIZE);
        large_alloc_header_t *h = (large_alloc_header_t *)((uint8_t *)p - LARGE_HEADER_SIZE);
        CHECK(h->identifier == IDENTIFIER_HEADER);
        CHECK(h->order == expected_large_order(sizes[i]));

        memset(p, 0xA5, sizes[i]);                          /* the whole usable size is really ours */
        for (size_t k = 0; k < sizes[i]; k += 997) CHECK(((uint8_t *)p)[k] == 0xA5);

        kfree(p);
        for (unsigned o = 0; o <= MAX_ORDER; o++) after[o] = count_order(o);
        CHECK(memcmp(before, after, sizeof before) == 0);
        EXPECT_STATUS(PASS);
    }
}

static void test_large_too_big_is_refused_without_touching_pmm(void) {
    fresh_heap();
    size_t pages = pmm_free_pages();
    CHECK(kmalloc(MAX_LARGE_SIZE + 1) == NULL);
    CHECK(kmalloc(SIZE_MAX) == NULL);
    CHECK(kmalloc(SIZE_MAX - 15) == NULL);                  /* would wrap if the header were added first */
    CHECK(pmm_free_pages() == pages);
}

static void test_large_exhaustion_returns_null_and_recovers(void) {
    fresh_heap();
    void *blocks[8];
    size_t n = 0;
    while (n < 8 && (blocks[n] = kmalloc(MAX_LARGE_SIZE)) != NULL) n++;
    CHECK(n == 4);                                          /* 8 MiB = four 2 MiB blocks */
    CHECK(kmalloc(MAX_LARGE_SIZE) == NULL);
    for (size_t i = 0; i < n; i++) kfree(blocks[i]);
    CHECK(count_order(MAX_ORDER) == 4);                     /* four blocks; order 9 is the top, nothing merges */
    EXPECT_STATUS(PASS);
}

/* ============================== rejected ================================= */

typedef struct {
    cache_t caches[CACHE_COUNT];
    memory_node_t *heads[MAX_ORDER + 1];
    uint64_t bm[MAX_ORDER + 1][BM_WORDS];
    uint64_t um[BM_WORDS];
    uint8_t page[SLAB_SIZE];
    int has_page;
} hsnap_t;

static int in_arena(const void *p) { return (const uint8_t *)p >= arena && (const uint8_t *)p < arena + ARENA_SIZE; }

static void take(hsnap_t *s, void *ptr) {
    memcpy(s->caches, cache_list, sizeof cache_list);
    memcpy(s->heads, free_list, sizeof free_list);
    memcpy(s->bm, bm_storage, sizeof bm_storage);
    memcpy(s->um, um_storage, sizeof um_storage);
    s->has_page = in_arena(ptr);
    if (s->has_page) memcpy(s->page, slab_of(ptr), SLAB_SIZE);
}

static int unchanged(const hsnap_t *s, void *ptr) {
    return memcmp(s->caches, cache_list, sizeof cache_list) == 0 &&
           memcmp(s->heads, free_list, sizeof free_list) == 0 &&
           memcmp(s->bm, bm_storage, sizeof bm_storage) == 0 &&
           memcmp(s->um, um_storage, sizeof um_storage) == 0 &&
           (!s->has_page || memcmp(s->page, slab_of(ptr), SLAB_SIZE) == 0);
}

/* kfree(ptr) must log exactly one error and change nothing at all. */
static void expect_kfree_rejected(void *ptr) {
    hsnap_t *s = malloc(sizeof *s);
    take(s, ptr);
    error_calls = 0;

    kfree(ptr);

    CHECK(error_calls == 1);
    CHECK(unchanged(s, ptr));
    free(s);
    check_heap();
    EXPECT_STATUS(PASS);
}

static void test_reject_null_is_a_silent_noop(void) {
    fresh_heap();
    void *p = kmalloc(64);
    hsnap_t *s = malloc(sizeof *s);
    take(s, p);
    error_calls = 0;
    kfree(NULL);
    CHECK(error_calls == 0);
    CHECK(unchanged(s, p));
    free(s);
}

static void test_reject_double_free_of_slab_object(void) {
    fresh_heap();
    void *a = kmalloc(64), *b = kmalloc(64);
    kfree(a);
    expect_kfree_rejected(a);                               /* a is on the slab's free list */
    kfree(b);                                               /* and the heap still works */
    check_heap();
}

/* The slab is a spare with nothing allocated: any free is a double free. */
static void test_reject_free_into_spare_slab(void) {
    fresh_heap();
    void *a = kmalloc(64);
    kfree(a);
    CHECK(cache_list[2].empty_head == slab_of(a));
    expect_kfree_rejected(a);
}

/* Its page already went back to the PMM, so this isn't even a heap page now. */
static void test_reject_free_after_slab_page_was_released(void) {
    static void *ptrs[MAX_PTRS];
    fresh_heap();
    void *probe = kmalloc(16);
    size_t cap = slab_cap(slab_of(probe));
    kfree(probe);
    size_t n = 2 * cap;
    for (size_t i = 0; i < n; i++) ptrs[i] = kmalloc(16);
    slab_t *second = slab_of(ptrs[n - 1]);
    for (size_t i = 0; i < n; i++) kfree(ptrs[i]);
    CHECK(second != cache_list[0].empty_head);              /* it was the one released */
    expect_kfree_rejected(ptrs[n - 1]);
}

static void test_reject_double_free_of_large_block(void) {
    fresh_heap();
    void *p = kmalloc(5000);
    kfree(p);
    expect_kfree_rejected(p);
}

static void test_reject_pointers_not_on_an_object_boundary(void) {
    fresh_heap();
    void *p = kmalloc(64);
    CHECK(p != NULL);
    expect_kfree_rejected((uint8_t *)p + 1);
    expect_kfree_rejected((uint8_t *)p + 32);               /* middle of the object */
    expect_kfree_rejected((uint8_t *)slab_of(p) + 8);       /* inside the slab's own metadata */
    expect_kfree_rejected((uint8_t *)slab_of(p) + SLAB_SIZE - 1);
    kfree(p);
    check_heap();
}

/* Slab metadata occupies the start of the page, but its size isn't a multiple
 * of the block size, so some pointers there still sit on the object grid
 * (e.g. the page start itself). Only the lower-bound check stops those. */
static void test_reject_grid_aligned_pointers_inside_slab_metadata(void) {
    fresh_heap();
    void *p = kmalloc(16);                                  /* 16-byte class: objects start at +48 */
    uint8_t *page = (uint8_t *)slab_of(p);
    CHECK(slab_first_object(slab_of(p)) - (uintptr_t)page == 48);
    expect_kfree_rejected(page);
    expect_kfree_rejected(page + 16);
    expect_kfree_rejected(page + 32);
    kfree(p);
    check_heap();

    void *q = kmalloc(64);                                  /* 64-byte class: first object at +64 */
    CHECK(slab_first_object(slab_of(q)) - (uintptr_t)slab_of(q) == 64);
    expect_kfree_rejected(slab_of(q));
    kfree(q);
}

/* A large block whose header was damaged must not be handed to the PMM. */
static void test_reject_large_block_with_corrupt_order(void) {
    fresh_heap();
    void *p = kmalloc(5000);
    large_alloc_header_t *h = (large_alloc_header_t *)((uint8_t *)p - LARGE_HEADER_SIZE);
    unsigned good = h->order;
    h->order = MAX_ORDER + 1;
    expect_kfree_rejected(p);
    h->order = 0xFFFFFFFFu;
    expect_kfree_rejected(p);
    h->order = good;                                        /* repaired: now it frees normally */
    error_calls = 0;
    kfree(p);
    CHECK(error_calls == 0);
    EXPECT_STATUS(PASS);
}

static void test_reject_bad_pointers_into_large_blocks(void) {
    fresh_heap();
    void *p = kmalloc(10000);                               /* three pages -> an order-2 block */
    CHECK(p != NULL);
    expect_kfree_rejected((uint8_t *)p + 1);                /* not the start */
    expect_kfree_rejected((uint8_t *)p - LARGE_HEADER_SIZE);/* the header itself */
    expect_kfree_rejected((uint8_t *)p + PAGE_SIZE);        /* an interior page */
    kfree(p);
    EXPECT_STATUS(PASS);
}

static void test_reject_pointer_into_pmm_free_memory(void) {
    fresh_heap();
    expect_kfree_rejected(phys_to_virt(BASE + 3 * 1024 * 1024 + 0x100));
    expect_kfree_rejected(phys_to_virt(BASE + 5 * 1024 * 1024));
}

static void test_reject_pointer_below_the_hhdm(void) {
    fresh_heap();
    expect_kfree_rejected((void *)(uintptr_t)1);
    expect_kfree_rejected((void *)(uintptr_t)0x1000);
}

/* A burst of invalid frees in the middle of real use must not damage
 * anything: every live allocation is still intact and freeable afterwards. */
static void test_reject_does_not_disturb_live_allocations(void) {
    fresh_heap();
    void *live[6];
    size_t sizes[6] = { 16, 100, 700, 2048, 3000, 70000 };
    for (int i = 0; i < 6; i++) { live[i] = kmalloc(sizes[i]); CHECK(live[i] != NULL); memset(live[i], 0x30 + i, sizes[i]); }

    for (int i = 0; i < 6; i++) {
        error_calls = 0;
        kfree((uint8_t *)live[i] + 1);
        CHECK(error_calls == 1);
        kfree((void *)(uintptr_t)8);
    }
    for (int i = 0; i < 6; i++)
        for (size_t k = 0; k < sizes[i]; k += 13) CHECK(((uint8_t *)live[i])[k] == (uint8_t)(0x30 + i));

    size_t pages_held = HEAP_PMM_SIZE / PAGE_SIZE - pmm_free_pages();
    CHECK(pages_held > 0);
    for (int i = 0; i < 6; i++) { error_calls = 0; kfree(live[i]); CHECK(error_calls == 0); }
    check_heap();
    EXPECT_STATUS(PASS);
}

int main(void) {
    printf("-- slab\n");
    RUN(test_slab_alloc_free_keeps_one_spare_slab);
    RUN(test_slab_freed_object_is_reused_first);
    RUN(test_slab_lists_follow_occupancy_in_every_class);
    RUN(test_slab_second_empty_slab_is_released_to_pmm);
    RUN(test_slab_random_free_order_keeps_invariants);
    RUN(test_slab_objects_aligned_disjoint_and_intact);
    RUN(test_slab_size_routing);

    printf("-- large\n");
    RUN(test_large_alloc_free_restores_pmm_exactly);
    RUN(test_large_too_big_is_refused_without_touching_pmm);
    RUN(test_large_exhaustion_returns_null_and_recovers);

    printf("-- rejected frees\n");
    RUN(test_reject_null_is_a_silent_noop);
    RUN(test_reject_double_free_of_slab_object);
    RUN(test_reject_free_into_spare_slab);
    RUN(test_reject_free_after_slab_page_was_released);
    RUN(test_reject_double_free_of_large_block);
    RUN(test_reject_pointers_not_on_an_object_boundary);
    RUN(test_reject_grid_aligned_pointers_inside_slab_metadata);
    RUN(test_reject_bad_pointers_into_large_blocks);
    RUN(test_reject_large_block_with_corrupt_order);
    RUN(test_reject_pointer_into_pmm_free_memory);
    RUN(test_reject_pointer_below_the_hhdm);
    RUN(test_reject_does_not_disturb_live_allocations);

    printf("\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
