// Minimal test harness: no dependencies, one process, counts checks.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern int g_checks, g_failures;

#define CHECK(cond)                                                                                   \
    do {                                                                                              \
        g_checks++;                                                                                   \
        if (!(cond)) {                                                                                \
            g_failures++;                                                                             \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                 \
        }                                                                                             \
    } while (0)

#define CHECK_EQ(a, b)                                                                                \
    do {                                                                                              \
        long long va_ = (long long)(a), vb_ = (long long)(b);                                         \
        g_checks++;                                                                                   \
        if (va_ != vb_) {                                                                             \
            g_failures++;                                                                             \
            printf("  FAIL %s:%d: %s == %s (0x%llX vs 0x%llX)\n", __FILE__, __LINE__, #a, #b, va_, vb_); \
        }                                                                                             \
    } while (0)

// Compare a byte array with a hex string ("DE AD BE EF").
#define CHECK_HEX(buf, len, hex)                                                                      \
    do {                                                                                              \
        g_checks++;                                                                                   \
        if (!hex_equal((buf), (len), (hex))) {                                                        \
            g_failures++;                                                                             \
            printf("  FAIL %s:%d: %s != %s\n    got ", __FILE__, __LINE__, #buf, hex);                \
            hex_print((buf), (len));                                                                  \
            printf("\n");                                                                             \
        }                                                                                             \
    } while (0)

void run_test(const char *name, void (*fn)(void));

// Hex helpers ("DE AD BE EF" <-> bytes).
size_t hex_parse(const char *hex, uint8_t *out, size_t cap);
bool hex_equal(const uint8_t *buf, size_t len, const char *hex);
void hex_print(const uint8_t *buf, size_t len);

// Test groups.
void test_atr(void);
void test_apdu(void);
void test_pps(void);
void test_session(void);
void test_t0(void);
void test_t1(void);
void test_dispatch(void);
void test_select(void);
void test_fs(void);
void test_fs_sec(void);
void test_nvm(void);
void test_nvm_pin_durability(void);
void test_nvm_layout(void);
void test_card_suite(void);
