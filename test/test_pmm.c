/*
 * Host-side correctness tests for memory/pmm.c.
 *
 * pmm.c is #included directly so the tests can reach its static state
 * (free_list, hhdm_offset, request) and static helpers (list_add,
 * list_remove) without modifying the source.
 *
 * Physical memory is simulated with a malloc'd arena. A fake Limine HHDM
 * response is installed so that "physical" address P is written through at
 * arena + (P - PHYS_BASE). The arena is filled with a sentinel byte so we
 * can also verify that the PMM only touches the node header at the start
 * of each free block.
 *
 * Tests that are expected to crash on buggy code run in a forked child so
 * one crash does not take down the whole binary.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "limine.h"

/* Mach-O rejects section(".limine_requests"); neutralise it for the host.
 * limine.h is already included above, so the macro can't affect it. */
#define section(x) unused

/* Host stand-ins for kernel services pmm.c uses on its error paths. hcf.h's
 * `hlt` won't assemble on an arm64 host, so claim its include guard first. */
#define HCF_H
static jmp_buf hcf_env;
static int hcf_armed;
static int hcf_calls;
static void hcf(void) {
    hcf_calls++;
    if (hcf_armed) longjmp(hcf_env, 1);   /* let a test observe the halt */
    abort();
}
void console_printf(const char *str, ...) { (void)str; }
void log_error(const char *message) { (void)message; }

#include "../src/memory/pmm.c"
#undef section

static int tests_run;
static int tests_failed;
static int current_failed;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("    %s:%d: CHECK failed: %s\n", __FILE__, __LINE__,    \
                   #cond);                                                 \
            current_failed = 1;                                            \
        }                                                                  \
    } while (0)

#define RUN(test)                                                          \
    do {                                                                   \
        current_failed = 0;                                                \
        test();                                                            \
        tests_run++;                                                       \
        if (current_failed) {                                              \
            tests_failed++;                                                \
            printf("FAIL %s\n", #test);                                    \
        } else {                                                           \
            printf("ok   %s\n", #test);                                    \
        }                                                                  \
    } while (0)

/* ---- simulated physical memory ---------------------------------------- */

#define SENTINEL   0xCC
#define ARENA_SIZE (16u * 1024 * 1024)  /* 16 MiB of fake RAM */
#define NODE_BYTES sizeof(memory_node_t)

static uint8_t *arena;
static paddr_t phys_base;               /* physical address of arena[0] */
static struct limine_hhdm_response fake_hhdm;

static void *phys_to_virt(paddr_t p) { return arena + (p - phys_base); }
static paddr_t virt_to_phys(const void *v) {
    return phys_base + (paddr_t)((const uint8_t *)v - arena);
}

/* Fresh arena mapped at fake physical address `base`; empty free lists. */
static void setup(paddr_t base) {
    if (!arena) {
        arena = malloc(ARENA_SIZE);
        if (!arena) { printf("out of memory\n"); exit(2); }
    }
    for (size_t i = 0; i < ARENA_SIZE; i++) arena[i] = SENTINEL;
    phys_base = base;
    fake_hhdm.revision = 0;
    fake_hhdm.offset = (uint64_t)(uintptr_t)arena - base;
    request.response = &fake_hhdm;
    get_hhdm_offset();                  /* maps phys P to arena + (P - base) */
    for (int i = 0; i <= MAX_ORDER; i++) free_list[i] = NULL;
    for (int i = 0; i <= MAX_ORDER; i++) bitmap_list[i] = (bitmap_t){0};
}

/* Run fn in a child process; returns 1 if it exited cleanly with status 0. */
static int survives(void (*fn)(void)) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        fn();
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* ---- invariant checker ------------------------------------------------- */

#define MAX_BLOCKS 8192
typedef struct { paddr_t start, end; unsigned order; } blk_t;
static blk_t blocks[MAX_BLOCKS];
static size_t nblocks;

static int blk_cmp(const void *a, const void *b) {
    const blk_t *x = a, *y = b;
    return (x->start > y->start) - (x->start < y->start);
}

/* Collects every free block, checking per-list structure along the way.
 * Returns 0 if a list is malformed (bad prev links, out-of-arena node...). */
static int collect_blocks(void) {
    nblocks = 0;
    for (unsigned o = 0; o <= MAX_ORDER; o++) {
        memory_node_t *prev = NULL;
        for (memory_node_t *n = free_list[o]; n; prev = n, n = n->next) {
            if ((uint8_t *)n < arena || (uint8_t *)n >= arena + ARENA_SIZE) return 0;
            if (n->prev != prev) return 0;
            if (nblocks >= MAX_BLOCKS) return 0;
            paddr_t s = virt_to_phys(n);
            blocks[nblocks++] = (blk_t){ s, s + ((paddr_t)PAGE_SIZE << o), o };
        }
    }
    qsort(blocks, nblocks, sizeof blocks[0], blk_cmp);
    return 1;
}

/* After pmm_add_block(base, size) on an empty PMM, the free lists must be a
 * buddy-compatible exact cover of [round_up(base), round_down(base+size)). */
static void check_cover(paddr_t base, size_t size) {
    paddr_t lo = (base + PAGE_SIZE - 1) & ~(paddr_t)(PAGE_SIZE - 1);
    paddr_t hi = (base + size) & ~(paddr_t)(PAGE_SIZE - 1);
    if (hi < lo) hi = lo;

    CHECK(collect_blocks());

    paddr_t cursor = lo;
    for (size_t i = 0; i < nblocks; i++) {
        blk_t *b = &blocks[i];
        paddr_t bytes = (paddr_t)PAGE_SIZE << b->order;
        CHECK(b->order <= MAX_ORDER);
        CHECK(b->start % bytes == 0);       /* naturally aligned          */
        CHECK(b->start == cursor);          /* no gap, no overlap         */
        cursor = b->end;
        CHECK(b->start >= lo && b->end <= hi);

        /* Greedy decomposition must not leave two free buddies unmerged
         * (below the max order, where merging isn't possible). */
        if (b->order < MAX_ORDER && i + 1 < nblocks) {
            blk_t *nx = &blocks[i + 1];
            int are_buddies = nx->order == b->order &&
                              nx->start == b->end &&
                              (b->start % (bytes * 2)) == 0;
            CHECK(!are_buddies);
        }
    }
    CHECK(cursor == hi);                    /* covers the whole region    */
}

/* Only the node header at the start of each free block may be modified. */
static void check_only_headers_touched(void) {
    CHECK(collect_blocks());
    size_t expected_dirty = nblocks * NODE_BYTES;
    size_t dirty = 0;
    for (size_t i = 0; i < ARENA_SIZE; i++) {
        if (arena[i] != SENTINEL) dirty++;
    }
    /* Pointers may legitimately contain 0xCC bytes, so dirty can be lower
     * than expected, but never higher. */
    CHECK(dirty <= expected_dirty);
    for (size_t i = 0; i < nblocks; i++) {
        uint8_t *p = phys_to_virt(blocks[i].start);
        for (size_t j = NODE_BYTES; j < PAGE_SIZE; j++) {
            CHECK(p[j] == SENTINEL);
        }
    }
}

static unsigned count_order(unsigned o) {
    unsigned c = 0;
    for (memory_node_t *n = free_list[o]; n; n = n->next) c++;
    return c;
}

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
    check_cover(0, 3 * PAGE_SIZE);
    CHECK(count_order(1) == 1 && count_order(0) == 1);
    CHECK(free_list[1] == (memory_node_t *)phys_to_virt(0));
    CHECK(free_list[0] == (memory_node_t *)phys_to_virt(2 * PAGE_SIZE));
}

static void test_add_block_exact_max_order(void) {
    setup(0);
    pmm_add_block(0, 2u * 1024 * 1024);
    CHECK(count_order(MAX_ORDER) == 1);
    check_cover(0, 2u * 1024 * 1024);
}

static void test_add_block_larger_than_max_order_splits(void) {
    setup(0);
    pmm_add_block(0, 6u * 1024 * 1024);
    CHECK(count_order(MAX_ORDER) == 3);
    check_cover(0, 6u * 1024 * 1024);
}

static void test_add_block_whole_arena(void) {
    setup(0);
    pmm_add_block(0, ARENA_SIZE);
    CHECK(count_order(MAX_ORDER) == ARENA_SIZE / (2u * 1024 * 1024));
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
    pmm_add_block(0, 4 * PAGE_SIZE);
    pmm_add_block(0x100000, 4 * PAGE_SIZE);
    CHECK(count_order(2) == 2);
    CHECK(collect_blocks());
    CHECK(nblocks == 2);
}

static void test_add_block_page_zero_is_max_order_aligned(void) {
    /* current_page == 0 must not call ctz(0). A 1-page region at 0 must still
     * yield order 0 (bounded by remaining), not order 9. */
    setup(0);
    pmm_add_block(0, PAGE_SIZE);
    CHECK(count_order(0) == 1);
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

#define BM_WORDS 512
static uint64_t bm_storage[MAX_ORDER + 1][BM_WORDS];

static void setup_bitmaps(paddr_t top) {
    size_t pages = top / PAGE_SIZE;
    for (unsigned o = 0; o <= MAX_ORDER; o++) {
        size_t bits = blocks_at_order(pages, o);
        if (bitmap_words(bits) > BM_WORDS) { printf("bm_storage too small\n"); exit(2); }
        bitmap_list[o] = bitmap_init(bm_storage[o], bits);
    }
}

/* Child-side assertion: report the failed expression and exit non-zero. */
#define EXPECT(cond)                                                       \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("    %s:%d: child check failed: %s\n", __FILE__,        \
                   __LINE__, #cond);                                       \
            fflush(stdout);                                                \
            _exit(20);                                                     \
        }                                                                  \
    } while (0)

/* Runs fn in a child. Returns 1 if it exited 0; otherwise prints how it
 * ended (signal or exit status) and returns 0. */
static int run_child(void (*fn)(void), const char *what) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        if (!getenv("PMM_SHOW_CRASH")) {
            int fd = open("/dev/null", O_WRONLY);
            if (fd >= 0) dup2(fd, 2);
        }
        fn();
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 1;
    if (WIFSIGNALED(status)) {
        printf("    [%s] child died on signal %d (%s)\n", what,
               WTERMSIG(status), strsignal(WTERMSIG(status)));
    } else {
        printf("    [%s] child exited with status %d%s\n", what,
               WEXITSTATUS(status),
               WEXITSTATUS(status) == 20 ? "" : " (1 usually means a sanitizer abort)");
    }
    return 0;
}

/* True if every byte of the arena at or above physical address `from` still
 * holds the sentinel, i.e. nothing wrote into the nonexistent memory. */
static int untouched_from(paddr_t from) {
    for (const uint8_t *p = phys_to_virt(from); p < arena + ARENA_SIZE; p++) {
        if (*p != SENTINEL) return 0;
    }
    return 1;
}

/* Exactly one free block, of `order`, at `addr`; all other lists empty. */
static int only_free_block_is(paddr_t addr, unsigned order) {
    for (unsigned o = 0; o <= MAX_ORDER; o++) {
        if (o != order && free_list[o] != NULL) return 0;
    }
    return count_order(order) == 1 &&
           free_list[order] == (memory_node_t *)phys_to_virt(addr);
}

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
    RUN(test_add_block_page_zero_is_max_order_aligned);
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
