/*
 * Shared host-side harness for the memory/pmm.c tests.
 *
 * pmm.c is #included directly so tests can reach its static state
 * (free_list, bitmap_list, hhdm_offset, request) and static helpers without
 * modifying the source. Each test file includes this header exactly once and
 * is built as a single translation unit.
 *
 * Physical memory is simulated with a malloc'd arena. A fake Limine HHDM
 * response is installed so that "physical" address P is written through at
 * arena + (P - phys_base). The arena is filled with a sentinel byte so tests
 * can also verify that the PMM only touches the node header at the start of
 * each free block.
 *
 * Functions are `static inline` so a test file that doesn't use every helper
 * doesn't trip -Wunused-function.
 */
#pragma once
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
static inline void hcf(void) {
    hcf_calls++;
    if (hcf_armed) longjmp(hcf_env, 1);   /* let a test observe the halt */
    abort();
}
void console_printf(const char *str, ...) { (void)str; }
static const char *last_error;     /* most recent log_error() message, for asserting why a call was rejected */
static int error_calls;
void log_error(const char *message) { last_error = message; error_calls++; }

#include "../../src/memory/pmm.c"
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

static inline void *phys_to_virt(paddr_t p) { return arena + (p - phys_base); }
static inline paddr_t virt_to_phys(const void *v) {
    return phys_base + (paddr_t)((const uint8_t *)v - arena);
}

/* Fresh arena mapped at fake physical address `base`; empty free lists. */
static inline void setup(paddr_t base) {
    if (!arena) {
        arena = aligned_alloc(4096, ARENA_SIZE);   /* the real HHDM is page-aligned; heap code masks pointers to pages */
        if (!arena) { printf("out of memory\n"); exit(2); }
    }
    for (size_t i = 0; i < ARENA_SIZE; i++) arena[i] = SENTINEL;
    phys_base = base;
    fake_hhdm.revision = 0;
    fake_hhdm.offset = (uint64_t)(uintptr_t)arena - base;
    request.response = &fake_hhdm;
    init_hhdm_offset();                 /* maps phys P to arena + (P - base) */
    for (int i = 0; i <= MAX_ORDER; i++) free_list[i] = NULL;
    for (int i = 0; i <= MAX_ORDER; i++) bitmap_list[i] = (bitmap_t){0};
    unmanaged_list = (bitmap_t){0};
}

/* Run fn in a child process; returns 1 if it exited cleanly with status 0. */
static inline int survives(void (*fn)(void)) {
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

static inline int blk_cmp(const void *a, const void *b) {
    const blk_t *x = a, *y = b;
    return (x->start > y->start) - (x->start < y->start);
}

/* Collects every free block, checking per-list structure along the way.
 * Returns 0 if a list is malformed (bad prev links, out-of-arena node...). */
static inline int collect_blocks(void) {
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
 * buddy-compatible exact cover of [round_up(base), round_down(base+size)),
 * except that page 0 is never handed out. */
static inline void check_cover(paddr_t base, size_t size) {
    paddr_t lo = (base + PAGE_SIZE - 1) & ~(paddr_t)(PAGE_SIZE - 1);
    if (lo == 0) lo = PAGE_SIZE;
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
static inline void check_only_headers_touched(void) {
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

static inline unsigned count_order(unsigned o) {
    unsigned c = 0;
    for (memory_node_t *n = free_list[o]; n; n = n->next) c++;
    return c;
}


#define BM_WORDS 512
static uint64_t bm_storage[MAX_ORDER + 1][BM_WORDS];
static uint64_t um_storage[BM_WORDS];

static inline void setup_bitmaps(paddr_t top) {
    size_t pages = top / PAGE_SIZE;
    for (unsigned o = 0; o <= MAX_ORDER; o++) {
        size_t bits = blocks_at_order(pages, o);
        if (bitmap_words(bits) > BM_WORDS) { printf("bm_storage too small\n"); exit(2); }
        bitmap_list[o] = bitmap_init(bm_storage[o], bits);
    }
    /* like pmm_init: one "unmanaged" bit per page, all set until add_block */
    if (bitmap_words(pages) > BM_WORDS) { printf("um_storage too small\n"); exit(2); }
    unmanaged_list = bitmap_init(um_storage, pages);
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
static inline int run_child(void (*fn)(void), const char *what) {
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
static inline int untouched_from(paddr_t from) {
    for (const uint8_t *p = phys_to_virt(from); p < arena + ARENA_SIZE; p++) {
        if (*p != SENTINEL) return 0;
    }
    return 1;
}

/* Exactly one free block, of `order`, at `addr`; all other lists empty. */
static inline int only_free_block_is(paddr_t addr, unsigned order) {
    for (unsigned o = 0; o <= MAX_ORDER; o++) {
        if (o != order && free_list[o] != NULL) return 0;
    }
    return count_order(order) == 1 &&
           free_list[order] == (memory_node_t *)phys_to_virt(addr);
}

/* ---- pmm_check() status helpers ---------------------------------------- */

static inline const char *status_name(Status s) {
    switch (s) {
        case PASS:         return "PASS";
        case DESYNC:       return "DESYNC";
        case LL_INIT:      return "LL_INIT";
        case FREE_BUDDY:   return "FREE_BUDDY";
        case OVERLAP:      return "OVERLAP";
        case MISALIGNMENT: return "MISALIGNMENT";
    }
    return "?";
}

#define EXPECT_STATUS(want)                                                \
    do {                                                                   \
        Status got_ = pmm_check();                                         \
        if (got_ != (want)) {                                              \
            printf("    %s:%d: pmm_check() returned %s, expected %s\n",    \
                   __FILE__, __LINE__, status_name(got_),                  \
                   status_name(want));                                     \
            current_failed = 1;                                            \
        }                                                                  \
    } while (0)
