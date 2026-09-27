/*
 * Host-side unit tests for console_printf (kernel/console.c).
 *
 * kernel/console.c is compiled separately with -Dputchar=fable_putchar so
 * the kernel's `void putchar(char)` doesn't collide with the host libc's.
 * This file provides fable_putchar, which records every byte into a
 * capture buffer so each call's exact output (including embedded NULs)
 * can be compared against an expected byte string.
 *
 * Expectations for the supported conversions (%c %s %d %b %p %ld %lu %lx
 * %%) follow C printf semantics. There is no standard for malformed or
 * unsupported specifiers, so those tests pin down the current behaviour:
 * the '%' is dropped and the following characters are echoed literally
 * without consuming an argument.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kernel/console.h"

static int tests_run;
static int tests_failed;
static int current_failed;

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

/* ---- putchar capture ---------------------------------------------------- */

#define CAPTURE_SIZE 8192

static char captured[CAPTURE_SIZE];
static size_t captured_len;
static bool captured_overflow;

void fable_putchar(char c) {
    if (captured_len < CAPTURE_SIZE) {
        captured[captured_len++] = c;
    } else {
        captured_overflow = true;
    }
}

static void capture_reset(void) {
    captured_len = 0;
    captured_overflow = false;
}

static void print_escaped(const char *buf, size_t n) {
    putc('"', stdout);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == '\n') {
            fputs("\\n", stdout);
        } else if (c == '\r') {
            fputs("\\r", stdout);
        } else if (c == '"' || c == '\\') {
            printf("\\%c", c);
        } else if (c < 0x20 || c >= 0x7f) {
            printf("\\x%02x", c);
        } else {
            putc(c, stdout);
        }
    }
    putc('"', stdout);
}

static void check_output(const char *expected, size_t expected_len,
                         const char *call, const char *file, int line) {
    if (!captured_overflow && captured_len == expected_len &&
        memcmp(captured, expected, expected_len) == 0) {
        return;
    }
    printf("    %s:%d: console_printf(%s)\n", file, line, call);
    printf("      expected ");
    print_escaped(expected, expected_len);
    printf(" (%zu bytes)\n      got      ", expected_len);
    print_escaped(captured, captured_len);
    printf(" (%zu bytes%s)\n", captured_len,
           captured_overflow ? ", capture overflowed" : "");
    current_failed = 1;
}

/*
 * EXPECT("literal", fmt, args...) — `expected` must be a string literal so
 * its length (including any embedded '\0') comes from sizeof.
 */
#define EXPECT(expected, ...)                                              \
    do {                                                                   \
        capture_reset();                                                   \
        console_printf(__VA_ARGS__);                                       \
        check_output((expected), sizeof(expected) - 1, #__VA_ARGS__,       \
                     __FILE__, __LINE__);                                  \
    } while (0)

/* Same as EXPECT but for an expected string built at runtime. */
#define EXPECT_BUF(expected, expected_len, ...)                            \
    do {                                                                   \
        capture_reset();                                                   \
        console_printf(__VA_ARGS__);                                       \
        check_output((expected), (expected_len), #__VA_ARGS__,             \
                     __FILE__, __LINE__);                                  \
    } while (0)

/* ---- plain text --------------------------------------------------------- */

static void test_empty_format(void) {
    EXPECT("", "");
}

static void test_plain_text(void) {
    EXPECT("hello", "hello");
    EXPECT("a", "a");
    EXPECT("hello, world!", "hello, world!");
}

static void test_control_and_high_bytes_pass_through(void) {
    /* CRLF translation lives in putchar, not console_printf. */
    EXPECT("line1\nline2\n", "line1\nline2\n");
    EXPECT("\t\r\x1b[0m", "\t\r\x1b[0m");
    EXPECT("\x7f\x80\xff", "\x7f\x80\xff");
}

static void test_long_format_string(void) {
    char fmt[4001];
    for (size_t i = 0; i < 4000; i++) {
        fmt[i] = (char)('a' + i % 26);
    }
    fmt[4000] = '\0';
    EXPECT_BUF(fmt, 4000, fmt);
}

/* ---- %d (int) ----------------------------------------------------------- */

static void test_d_zero_and_small(void) {
    EXPECT("0", "%d", 0);
    EXPECT("1", "%d", 1);
    EXPECT("-1", "%d", -1);
    EXPECT("9", "%d", 9);
    EXPECT("-9", "%d", -9);
}

static void test_d_digit_rollover(void) {
    EXPECT("10", "%d", 10);
    EXPECT("-10", "%d", -10);
    EXPECT("99", "%d", 99);
    EXPECT("100", "%d", 100);
    EXPECT("999999999", "%d", 999999999);
    EXPECT("1000000000", "%d", 1000000000);
    EXPECT("-1000000000", "%d", -1000000000);
}

static void test_d_int_limits(void) {
    EXPECT("2147483647", "%d", INT32_MAX);
    EXPECT("-2147483648", "%d", INT32_MIN);
    EXPECT("2147483646", "%d", INT32_MAX - 1);
    EXPECT("-2147483647", "%d", INT32_MIN + 1);
}

static void test_d_embedded_in_text(void) {
    EXPECT("x=42;", "x=%d;", 42);
    EXPECT("-5 apples", "%d apples", -5);
    EXPECT("1 2 3", "%d %d %d", 1, 2, 3);
    EXPECT("12", "%d%d", 1, 2);
}

/* ---- %ld (int64_t) ------------------------------------------------------ */

static void test_ld_basic(void) {
    EXPECT("0", "%ld", (int64_t)0);
    EXPECT("1", "%ld", (int64_t)1);
    EXPECT("-1", "%ld", (int64_t)-1);
}

static void test_ld_beyond_32_bits(void) {
    /* Values just past the int range must not be truncated. */
    EXPECT("2147483648", "%ld", (int64_t)INT32_MAX + 1);
    EXPECT("-2147483649", "%ld", (int64_t)INT32_MIN - 1);
    EXPECT("4294967296", "%ld", (int64_t)1 << 32);
    EXPECT("-4294967296", "%ld", -((int64_t)1 << 32));
}

static void test_ld_int64_limits(void) {
    EXPECT("9223372036854775807", "%ld", INT64_MAX);
    EXPECT("-9223372036854775808", "%ld", INT64_MIN);
    EXPECT("-9223372036854775807", "%ld", INT64_MIN + 1);
}

/* ---- %lu (uint64_t) ----------------------------------------------------- */

static void test_lu_basic(void) {
    EXPECT("0", "%lu", (uint64_t)0);
    EXPECT("1", "%lu", (uint64_t)1);
    EXPECT("4294967295", "%lu", (uint64_t)UINT32_MAX);
    EXPECT("4294967296", "%lu", (uint64_t)UINT32_MAX + 1);
}

static void test_lu_twenty_digit_boundary(void) {
    /* 20 digits exactly fills print_int's 20-byte buffer. */
    EXPECT("18446744073709551615", "%lu", UINT64_MAX);
    EXPECT("10000000000000000000", "%lu", (uint64_t)10000000000000000000ULL);
    EXPECT("9999999999999999999", "%lu", (uint64_t)9999999999999999999ULL);
    EXPECT("9223372036854775808", "%lu", (uint64_t)1 << 63);
}

static void test_lu_negative_bit_patterns_are_unsigned(void) {
    EXPECT("18446744073709551615", "%lu", (uint64_t)-1);
    EXPECT("18446744071562067968", "%lu", (uint64_t)(int64_t)INT32_MIN);
}

/* ---- %lx (uint64_t, lowercase hex, no prefix) --------------------------- */

static void test_lx_digit_boundaries(void) {
    EXPECT("0", "%lx", (uint64_t)0);
    EXPECT("9", "%lx", (uint64_t)9);
    EXPECT("a", "%lx", (uint64_t)10);
    EXPECT("f", "%lx", (uint64_t)15);
    EXPECT("10", "%lx", (uint64_t)16);
    EXPECT("ff", "%lx", (uint64_t)255);
    EXPECT("100", "%lx", (uint64_t)256);
}

static void test_lx_common_values(void) {
    EXPECT("deadbeef", "%lx", (uint64_t)0xdeadbeef);
    EXPECT("123456789abcdef", "%lx", (uint64_t)0x0123456789abcdefULL);
    EXPECT("fedcba9876543210", "%lx", (uint64_t)0xfedcba9876543210ULL);
}

static void test_lx_limits(void) {
    EXPECT("ffffffffffffffff", "%lx", UINT64_MAX);
    EXPECT("8000000000000000", "%lx", (uint64_t)1 << 63);
    EXPECT("7fffffffffffffff", "%lx", (uint64_t)INT64_MAX);
    EXPECT("ffffffff", "%lx", (uint64_t)UINT32_MAX);
    EXPECT("100000000", "%lx", (uint64_t)UINT32_MAX + 1);
}

/* ---- %p ----------------------------------------------------------------- */

static void test_p_null(void) {
    EXPECT("0x0", "%p", (void *)NULL);
}

static void test_p_small_and_typical(void) {
    EXPECT("0x1", "%p", (void *)1);
    EXPECT("0xf", "%p", (void *)0xf);
    EXPECT("0x10", "%p", (void *)0x10);
    EXPECT("0x1000", "%p", (void *)0x1000);
    EXPECT("0xdeadbeef", "%p", (void *)0xdeadbeef);
}

static void test_p_kernel_addresses(void) {
    /* Higher-half addresses: not zero-padded, lowercase, no sign. */
    EXPECT("0xffffffff80000000", "%p", (void *)0xffffffff80000000ULL);
    EXPECT("0xffff800000000000", "%p", (void *)0xffff800000000000ULL);
    EXPECT("0xffffffffffffffff", "%p", (void *)UINTPTR_MAX);
}

static void test_p_matches_real_pointers(void) {
    int local;
    static int global;
    const void *ptrs[] = { &local, &global, (void *)test_p_null, "literal" };

    for (size_t i = 0; i < sizeof(ptrs) / sizeof(ptrs[0]); i++) {
        char want[32];
        int n = snprintf(want, sizeof(want), "0x%llx",
                         (unsigned long long)(uintptr_t)ptrs[i]);
        EXPECT_BUF(want, (size_t)n, "%p", ptrs[i]);
    }
}

static void test_p_function_and_const_pointers(void) {
    const char *s = (const char *)0xabc;
    EXPECT("[0xabc]", "[%p]", (const void *)s);
}

/* ---- %c ----------------------------------------------------------------- */

static void test_c_basic(void) {
    EXPECT("A", "%c", 'A');
    EXPECT("abc", "%c%c%c", 'a', 'b', 'c');
    EXPECT(" ", "%c", ' ');
    EXPECT("\n", "%c", '\n');
}

static void test_c_special_chars(void) {
    /* A '%' argument is printed, not re-interpreted. */
    EXPECT("%", "%c", '%');
    EXPECT("%d", "%c%c", '%', 'd');
    /* NUL is emitted as a byte and does not terminate output. */
    EXPECT("[\0]", "[%c]", '\0');
    EXPECT("\xff", "%c", 0xff);
    EXPECT("\x80", "%c", 0x80);
}

static void test_c_int_truncates_to_char(void) {
    /* The int argument is narrowed to char. */
    EXPECT("A", "%c", 0x141);
    EXPECT("\0", "%c", 0x100);
    EXPECT("\xff", "%c", -1);
}

/* ---- %s ----------------------------------------------------------------- */

static void test_s_basic(void) {
    EXPECT("hello", "%s", "hello");
    EXPECT("<hi>", "<%s>", "hi");
    EXPECT("ab", "%s%s", "a", "b");
}

static void test_s_empty(void) {
    EXPECT("", "%s", "");
    EXPECT("[]", "[%s]", "");
    EXPECT("x", "%s%s%s", "", "x", "");
}

static void test_s_null(void) {
    EXPECT("(null)", "%s", (const char *)NULL);
    EXPECT("s=(null);", "s=%s;", (const char *)NULL);
}

static void test_s_argument_not_reinterpreted(void) {
    EXPECT("%d %s %%", "%s", "%d %s %%");
    EXPECT("100%", "%s", "100%");
}

static void test_s_long(void) {
    char s[3001];
    for (size_t i = 0; i < 3000; i++) {
        s[i] = (char)('A' + i % 26);
    }
    s[3000] = '\0';
    EXPECT_BUF(s, 3000, "%s", s);
}

static void test_s_high_bytes(void) {
    EXPECT("caf\xc3\xa9", "%s", "caf\xc3\xa9");
}

/* ---- %b (bool extension) ------------------------------------------------ */

static void test_b_values(void) {
    EXPECT("true", "%b", true);
    EXPECT("false", "%b", false);
    EXPECT("false", "%b", 0);
    EXPECT("true", "%b", 1);
}

static void test_b_nonzero_ints_are_true(void) {
    EXPECT("true", "%b", 2);
    EXPECT("true", "%b", -1);
    EXPECT("true", "%b", 256);
    EXPECT("true", "%b", INT32_MIN);
}

/* ---- %% ----------------------------------------------------------------- */

static void test_percent_literal(void) {
    EXPECT("%", "%%");
    EXPECT("100%", "100%%");
    EXPECT("%%", "%%%%");
    EXPECT("50% off", "50%% off");
}

static void test_percent_literal_consumes_no_argument(void) {
    EXPECT("%d", "%%d");
    EXPECT("%7", "%%%d", 7);
    EXPECT("% 1", "%% %d", 1);
}

/* ---- malformed / unsupported specifiers --------------------------------- */

static void test_trailing_percent(void) {
    /* A lone '%' at the end is dropped and nothing is read past the NUL. */
    EXPECT("", "%");
    EXPECT("abc", "abc%");
    EXPECT("5", "%d%", 5);
}

static void test_trailing_l(void) {
    EXPECT("l", "%l");
    EXPECT("abcl", "abc%l");
}

static void test_unknown_conversion_echoed(void) {
    EXPECT("x", "%x");
    EXPECT("u", "%u");
    EXPECT("i", "%i");
    EXPECT("X", "%X");
    EXPECT("f", "%f");
    EXPECT("z", "%z");
    EXPECT("?", "%?");
    EXPECT(" ", "% ");
}

static void test_unknown_l_conversion_echoed(void) {
    EXPECT("lz", "%lz");
    EXPECT("lX", "%lX");
    EXPECT("ls", "%ls");
    EXPECT("lc", "%lc");
    EXPECT("lp", "%lp");
}

static void test_unsupported_length_modifiers(void) {
    EXPECT("lld", "%lld");
    EXPECT("llu", "%llu");
    EXPECT("hd", "%hd");
    EXPECT("zu", "%zu");
}

static void test_width_precision_flags_unsupported(void) {
    EXPECT("5d", "%5d");
    EXPECT("05d", "%05d");
    EXPECT("-d", "%-d");
    EXPECT("+d", "%+d");
    EXPECT(".3s", "%.3s");
    EXPECT("#lx", "%#lx");
}

static void test_unknown_conversion_consumes_no_argument(void) {
    /* Unknown specifiers don't consume an argument, so the next valid
     * specifier picks up the argument intended for the unknown one. */
    EXPECT("x|42", "%x|%d", 42);
    EXPECT("lz 7", "%lz %d", 7);
    EXPECT("5d 1", "%5d %d", 1, 2);
    EXPECT("l9", "%l%d", 9);
    EXPECT("l%", "%l%%");
}

static void test_percent_newline_and_nul_char(void) {
    EXPECT("\n", "%\n");
    EXPECT("a\nb", "a%\nb");
}

/* ---- combinations / argument ordering ----------------------------------- */

static void test_every_specifier_once(void) {
    EXPECT("c=Z s=str d=-12 b=true p=0xbeef ld=-9000000000 "
           "lu=18000000000 lx=abc123 %",
           "c=%c s=%s d=%d b=%b p=%p ld=%ld lu=%lu lx=%lx %%",
           'Z', "str", -12, true, (void *)0xbeef,
           (int64_t)-9000000000LL, (uint64_t)18000000000ULL,
           (uint64_t)0xabc123);
}

static void test_many_arguments(void) {
    /* More arguments than fit in registers on either ABI. */
    EXPECT("1 2 3 4 5 6 7 8 9 10 11 12",
           "%d %d %d %d %d %d %d %d %d %d %d %d",
           1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12);
}

static void test_mixed_widths_interleaved(void) {
    /* Alternating 32- and 64-bit args checks va_arg type handling. */
    EXPECT("-1 18446744073709551615 -2 ffffffffffffffff -3 "
           "-9223372036854775808 x",
           "%d %lu %d %lx %d %ld %c",
           -1, UINT64_MAX, -2, UINT64_MAX, -3, INT64_MIN, 'x');
}

static void test_adjacent_specifiers(void) {
    EXPECT("0x10truea-1", "%p%b%c%d", (void *)0x10, 1, 'a', -1);
    EXPECT("ff255-1", "%lx%lu%ld",
           (uint64_t)255, (uint64_t)255, (int64_t)-1);
}

static void test_repeated_calls_independent(void) {
    EXPECT("1", "%d", 1);
    EXPECT("2", "%d", 2);
    EXPECT("(null)", "%s", (const char *)NULL);
    EXPECT("ok", "ok");
}

int main(void) {
    RUN(test_empty_format);
    RUN(test_plain_text);
    RUN(test_control_and_high_bytes_pass_through);
    RUN(test_long_format_string);

    RUN(test_d_zero_and_small);
    RUN(test_d_digit_rollover);
    RUN(test_d_int_limits);
    RUN(test_d_embedded_in_text);

    RUN(test_ld_basic);
    RUN(test_ld_beyond_32_bits);
    RUN(test_ld_int64_limits);

    RUN(test_lu_basic);
    RUN(test_lu_twenty_digit_boundary);
    RUN(test_lu_negative_bit_patterns_are_unsigned);

    RUN(test_lx_digit_boundaries);
    RUN(test_lx_common_values);
    RUN(test_lx_limits);

    RUN(test_p_null);
    RUN(test_p_small_and_typical);
    RUN(test_p_kernel_addresses);
    RUN(test_p_matches_real_pointers);
    RUN(test_p_function_and_const_pointers);

    RUN(test_c_basic);
    RUN(test_c_special_chars);
    RUN(test_c_int_truncates_to_char);

    RUN(test_s_basic);
    RUN(test_s_empty);
    RUN(test_s_null);
    RUN(test_s_argument_not_reinterpreted);
    RUN(test_s_long);
    RUN(test_s_high_bytes);

    RUN(test_b_values);
    RUN(test_b_nonzero_ints_are_true);

    RUN(test_percent_literal);
    RUN(test_percent_literal_consumes_no_argument);

    RUN(test_trailing_percent);
    RUN(test_trailing_l);
    RUN(test_unknown_conversion_echoed);
    RUN(test_unknown_l_conversion_echoed);
    RUN(test_unsupported_length_modifiers);
    RUN(test_width_precision_flags_unsupported);
    RUN(test_unknown_conversion_consumes_no_argument);
    RUN(test_percent_newline_and_nul_char);

    RUN(test_every_specifier_once);
    RUN(test_many_arguments);
    RUN(test_mixed_widths_interleaved);
    RUN(test_adjacent_specifiers);
    RUN(test_repeated_calls_independent);

    printf("\n%d/%d tests passed\n", tests_run - tests_failed, tests_run);
    return tests_failed ? 1 : 0;
}
