/*
 * Host-side unit tests for util/string.c.
 *
 * The Makefile renames the kernel's memcpy/memset/memmove/memcmp to
 * fable_* via -D so they don't collide with the host libc. Because of
 * that, these tests deliberately avoid <string.h> and never rely on the
 * host's mem* functions to check results.
 */
#include <stdint.h>
#include <stdio.h>

#include "util/string.h"

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

#define GUARD 0xA5

static void fill_pattern(uint8_t *buf, size_t n, uint8_t seed) {
    for (size_t i = 0; i < n; i++) {
        buf[i] = (uint8_t)(seed + i * 7);
    }
}

static int all_equal(const uint8_t *buf, size_t n, uint8_t value) {
    for (size_t i = 0; i < n; i++) {
        if (buf[i] != value) {
            return 0;
        }
    }
    return 1;
}

static int bytes_equal(const uint8_t *a, const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

/* ---------------------------------------------------------------- memcpy */

static void test_memcpy_returns_dest(void) {
    uint8_t src[4] = {1, 2, 3, 4};
    uint8_t dst[4];
    CHECK(memcpy(dst, src, sizeof dst) == dst);
    CHECK(memcpy(dst, src, 0) == dst);
}

static void test_memcpy_copies_bytes(void) {
    uint8_t src[64], dst[64] = {0};
    fill_pattern(src, sizeof src, 3);
    memcpy(dst, src, sizeof src);
    CHECK(bytes_equal(dst, src, sizeof src));
}

static void test_memcpy_all_byte_values(void) {
    uint8_t src[256], dst[256] = {0};
    for (size_t i = 0; i < 256; i++) {
        src[i] = (uint8_t)i;
    }
    memcpy(dst, src, sizeof src);
    CHECK(bytes_equal(dst, src, sizeof src));
}

static void test_memcpy_zero_length(void) {
    uint8_t src[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t dst[8];
    memset(dst, GUARD, sizeof dst);
    memcpy(dst, src, 0);
    CHECK(all_equal(dst, sizeof dst, GUARD));
}

static void test_memcpy_respects_bounds(void) {
    uint8_t src[16], buf[32];
    fill_pattern(src, sizeof src, 9);
    memset(buf, GUARD, sizeof buf);
    memcpy(buf + 8, src, 10);
    CHECK(all_equal(buf, 8, GUARD));
    CHECK(bytes_equal(buf + 8, src, 10));
    CHECK(all_equal(buf + 18, sizeof buf - 18, GUARD));
}

/* ---------------------------------------------------------------- memset */

static void test_memset_returns_s(void) {
    uint8_t buf[4];
    CHECK(memset(buf, 0, sizeof buf) == buf);
    CHECK(memset(buf, 0, 0) == buf);
}

static void test_memset_fills(void) {
    uint8_t buf[100];
    memset(buf, 0x3C, sizeof buf);
    CHECK(all_equal(buf, sizeof buf, 0x3C));
    memset(buf, 0, sizeof buf);
    CHECK(all_equal(buf, sizeof buf, 0));
}

static void test_memset_truncates_value(void) {
    uint8_t buf[8];
    memset(buf, 0x1FF, sizeof buf);
    CHECK(all_equal(buf, sizeof buf, 0xFF));
    memset(buf, -1, sizeof buf);
    CHECK(all_equal(buf, sizeof buf, 0xFF));
    memset(buf, 0x1234, sizeof buf);
    CHECK(all_equal(buf, sizeof buf, 0x34));
}

static void test_memset_zero_length(void) {
    uint8_t buf[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t expected[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    memset(buf, 0, 0);
    CHECK(bytes_equal(buf, expected, sizeof buf));
}

static void test_memset_respects_bounds(void) {
    uint8_t buf[32];
    for (size_t i = 0; i < sizeof buf; i++) {
        buf[i] = GUARD;
    }
    memset(buf + 5, 0x11, 20);
    CHECK(all_equal(buf, 5, GUARD));
    CHECK(all_equal(buf + 5, 20, 0x11));
    CHECK(all_equal(buf + 25, sizeof buf - 25, GUARD));
}

/* --------------------------------------------------------------- memmove */

static void test_memmove_returns_dest(void) {
    uint8_t buf[8] = {0};
    CHECK(memmove(buf + 2, buf, 4) == buf + 2);
    CHECK(memmove(buf, buf + 2, 4) == buf);
    CHECK(memmove(buf, buf, 4) == buf);
    CHECK(memmove(buf, buf + 1, 0) == buf);
}

static void test_memmove_non_overlapping(void) {
    uint8_t src[32], dst[32] = {0};
    fill_pattern(src, sizeof src, 17);
    memmove(dst, src, sizeof src);
    CHECK(bytes_equal(dst, src, sizeof src));
}

static void test_memmove_overlap_dest_after_src(void) {
    uint8_t buf[16], expected[16];
    for (size_t i = 0; i < sizeof buf; i++) {
        buf[i] = expected[i] = (uint8_t)i;
    }
    /* Shift [0..10) to [4..14). A naive forward copy would smear. */
    for (size_t i = 0; i < 10; i++) {
        expected[4 + i] = (uint8_t)i;
    }
    memmove(buf + 4, buf, 10);
    CHECK(bytes_equal(buf, expected, sizeof buf));
}

static void test_memmove_overlap_dest_before_src(void) {
    uint8_t buf[16], expected[16];
    for (size_t i = 0; i < sizeof buf; i++) {
        buf[i] = expected[i] = (uint8_t)i;
    }
    /* Shift [4..14) to [0..10). A naive backward copy would smear. */
    for (size_t i = 0; i < 10; i++) {
        expected[i] = (uint8_t)(4 + i);
    }
    memmove(buf, buf + 4, 10);
    CHECK(bytes_equal(buf, expected, sizeof buf));
}

static void test_memmove_overlap_by_one(void) {
    uint8_t buf[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t right[8] = {1, 1, 2, 3, 4, 5, 6, 7};
    memmove(buf + 1, buf, 7);
    CHECK(bytes_equal(buf, right, sizeof buf));

    uint8_t buf2[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t left[8] = {2, 3, 4, 5, 6, 7, 8, 8};
    memmove(buf2, buf2 + 1, 7);
    CHECK(bytes_equal(buf2, left, sizeof buf2));
}

static void test_memmove_same_pointer(void) {
    uint8_t buf[16], expected[16];
    fill_pattern(buf, sizeof buf, 5);
    fill_pattern(expected, sizeof expected, 5);
    memmove(buf, buf, sizeof buf);
    CHECK(bytes_equal(buf, expected, sizeof buf));
}

static void test_memmove_zero_length(void) {
    uint8_t buf[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t expected[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    memmove(buf + 2, buf, 0);
    memmove(buf, buf + 2, 0);
    CHECK(bytes_equal(buf, expected, sizeof buf));
}

/* ---------------------------------------------------------------- memcmp */

static void test_memcmp_equal(void) {
    uint8_t a[32], b[32];
    fill_pattern(a, sizeof a, 1);
    fill_pattern(b, sizeof b, 1);
    CHECK(memcmp(a, b, sizeof a) == 0);
    CHECK(memcmp(a, a, sizeof a) == 0);
}

static void test_memcmp_zero_length(void) {
    uint8_t a[1] = {1}, b[1] = {2};
    CHECK(memcmp(a, b, 0) == 0);
}

static void test_memcmp_sign(void) {
    uint8_t a[4] = {1, 2, 3, 4};
    uint8_t b[4] = {1, 2, 9, 4};
    CHECK(memcmp(a, b, sizeof a) < 0);
    CHECK(memcmp(b, a, sizeof a) > 0);
}

static void test_memcmp_first_difference_wins(void) {
    uint8_t a[4] = {1, 5, 0, 0};
    uint8_t b[4] = {1, 4, 9, 9};
    CHECK(memcmp(a, b, sizeof a) > 0);
    CHECK(memcmp(b, a, sizeof a) < 0);
}

static void test_memcmp_unsigned_bytes(void) {
    /* Bytes compare as unsigned char: 0x80 > 0x7F, 0xFF > 0x00. */
    uint8_t hi[1] = {0x80}, lo[1] = {0x7F};
    CHECK(memcmp(hi, lo, 1) > 0);
    CHECK(memcmp(lo, hi, 1) < 0);

    uint8_t ff[1] = {0xFF}, zero[1] = {0x00};
    CHECK(memcmp(ff, zero, 1) > 0);
    CHECK(memcmp(zero, ff, 1) < 0);
}

static void test_memcmp_stops_at_n(void) {
    uint8_t a[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t b[8] = {1, 2, 3, 4, 0, 0, 0, 0};
    CHECK(memcmp(a, b, 4) == 0);
    CHECK(memcmp(a, b, 5) > 0);
}

static void test_memcmp_last_byte(void) {
    uint8_t a[16], b[16];
    fill_pattern(a, sizeof a, 2);
    fill_pattern(b, sizeof b, 2);
    b[15]++;
    CHECK(memcmp(a, b, sizeof a) < 0);
    CHECK(memcmp(a, b, sizeof a - 1) == 0);
}

int main(void) {
    RUN(test_memcpy_returns_dest);
    RUN(test_memcpy_copies_bytes);
    RUN(test_memcpy_all_byte_values);
    RUN(test_memcpy_zero_length);
    RUN(test_memcpy_respects_bounds);

    RUN(test_memset_returns_s);
    RUN(test_memset_fills);
    RUN(test_memset_truncates_value);
    RUN(test_memset_zero_length);
    RUN(test_memset_respects_bounds);

    RUN(test_memmove_returns_dest);
    RUN(test_memmove_non_overlapping);
    RUN(test_memmove_overlap_dest_after_src);
    RUN(test_memmove_overlap_dest_before_src);
    RUN(test_memmove_overlap_by_one);
    RUN(test_memmove_same_pointer);
    RUN(test_memmove_zero_length);

    RUN(test_memcmp_equal);
    RUN(test_memcmp_zero_length);
    RUN(test_memcmp_sign);
    RUN(test_memcmp_first_difference_wins);
    RUN(test_memcmp_unsigned_bytes);
    RUN(test_memcmp_stops_at_n);
    RUN(test_memcmp_last_byte);

    printf("\n%d/%d tests passed\n", tests_run - tests_failed, tests_run);
    return tests_failed ? 1 : 0;
}
