/*
 * Host-side integration tests: console_printf -> putchar -> serial_putchar
 * -> outb, using the real kernel/console.c and io/serial.c.
 *
 * Both kernel files are compiled with -Dputchar=fable_putchar to avoid the
 * host libc's putchar. This file stubs the port I/O layer (inb/outb) to
 * simulate a COM1 UART and records every byte written to the data port.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kernel/console.h"

#define COM1      0x3F8
#define COM1_LSR  (COM1 + 5)
#define LSR_THRE  0x20

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
        uart_reset();                                                      \
        test();                                                            \
        tests_run++;                                                       \
        if (current_failed) {                                              \
            tests_failed++;                                                \
            printf("FAIL %s\n", #test);                                    \
        } else {                                                           \
            printf("ok   %s\n", #test);                                    \
        }                                                                  \
    } while (0)

/* ---- simulated UART ----------------------------------------------------- */

static char tx[1024];
static size_t tx_len;
static int other_port_writes;
static int lsr_polls;
/* Number of LSR reads that report "busy" before each byte is accepted. */
static int busy_polls_per_byte;
static int busy_remaining;
/* Set if a byte is written to the data port while the UART is busy. */
static bool wrote_while_busy;

static void uart_reset(void) {
    tx_len = 0;
    other_port_writes = 0;
    lsr_polls = 0;
    busy_polls_per_byte = 0;
    busy_remaining = 0;
    wrote_while_busy = false;
}

uint8_t inb(uint16_t port) {
    if (port == COM1_LSR) {
        lsr_polls++;
        if (busy_remaining > 0) {
            busy_remaining--;
            return 0x00;
        }
        return LSR_THRE;
    }
    return 0;
}

void outb(uint16_t port, uint8_t value) {
    if (port != COM1) {
        other_port_writes++;
        return;
    }
    if (busy_remaining > 0) {
        wrote_while_busy = true;
    }
    if (tx_len < sizeof(tx)) {
        tx[tx_len++] = (char)value;
    }
    busy_remaining = busy_polls_per_byte;
}

static bool tx_equals(const char *expected, size_t n) {
    return tx_len == n && memcmp(tx, expected, n) == 0;
}

#define TX_IS(lit) tx_equals((lit), sizeof(lit) - 1)

/* ---- tests -------------------------------------------------------------- */

static void test_plain_text_reaches_data_port(void) {
    console_printf("abc");
    CHECK(TX_IS("abc"));
    CHECK(other_port_writes == 0);
}

static void test_newline_becomes_crlf(void) {
    console_printf("a\nb\n");
    CHECK(TX_IS("a\r\nb\r\n"));
}

static void test_newline_from_arguments_becomes_crlf(void) {
    console_printf("%c|%s", '\n', "x\ny");
    CHECK(TX_IS("\r\n|x\r\ny"));
}

static void test_existing_cr_not_doubled(void) {
    console_printf("\r\n");
    CHECK(TX_IS("\r\r\n"));
}

static void test_formatted_output_end_to_end(void) {
    console_printf("pid=%d addr=%p ok=%b\n", 7, (void *)0x1000, 1);
    CHECK(TX_IS("pid=7 addr=0x1000 ok=true\r\n"));
}

static void test_polls_lsr_once_per_byte_when_ready(void) {
    console_printf("hi\n");
    /* 'h', 'i', '\r', '\n' */
    CHECK(tx_len == 4);
    CHECK(lsr_polls == 4);
}

static void test_waits_for_transmitter_empty(void) {
    busy_polls_per_byte = 3;
    busy_remaining = 3;
    console_printf("%d\n", 42);
    CHECK(TX_IS("42\r\n"));
    CHECK(!wrote_while_busy);
    /* 4 bytes, each preceded by 3 busy polls and 1 ready poll. */
    CHECK(lsr_polls == 16);
}

static void test_nul_char_is_transmitted(void) {
    console_printf("[%c]", '\0');
    CHECK(TX_IS("[\0]"));
}

static void test_empty_format_sends_nothing(void) {
    console_printf("");
    CHECK(tx_len == 0);
    CHECK(lsr_polls == 0);
}

int main(void) {
    RUN(test_plain_text_reaches_data_port);
    RUN(test_newline_becomes_crlf);
    RUN(test_newline_from_arguments_becomes_crlf);
    RUN(test_existing_cr_not_doubled);
    RUN(test_formatted_output_end_to_end);
    RUN(test_polls_lsr_once_per_byte_when_ready);
    RUN(test_waits_for_transmitter_empty);
    RUN(test_nul_char_is_transmitted);
    RUN(test_empty_format_sends_nothing);

    printf("\n%d/%d tests passed\n", tests_run - tests_failed, tests_run);
    return tests_failed ? 1 : 0;
}
