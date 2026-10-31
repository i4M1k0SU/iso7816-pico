// Host replacements for the modules that touch Pico hardware: logging, card services, RNG.  (The NVM is the real
// hal/nvm.c over the RAM port in fake_nvm.c.)
#include "card/card.h"
#include "hal/nvm.h"
#include "proto/t0.h"
#include "proto/t1.h"
#include "sys/log.h"

#include "fakes.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

bool g_verbose_log;
uint8_t g_fake_protocol;
static bool s_bootsel;
static uint32_t s_rand = 0x12345678u;

void log_printf(const char *fmt, ...) {
    if (!g_verbose_log) return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

void log_apdu(const char *name, uint8_t cla, uint8_t ins, uint8_t p1, uint8_t p2, const uint8_t *in, size_t in_len,
              const uint8_t *out, size_t out_len, uint16_t sw) {
    (void)in;
    (void)out;
    if (!g_verbose_log) return;
    printf("APDU %-22s %02X %02X %02X %02X | %u -> %u SW %04X\n", name, cla, ins, p1, p2, (unsigned)in_len,
           (unsigned)out_len, sw);
}

void log_drain(void) {}

// card/card.h services (sys/services.c is not compiled on the host).
void card_keepalive(void) {
    if (g_fake_protocol == 1) t1_keepalive();
    else t0_keepalive();
}

uint8_t card_protocol(void) { return g_fake_protocol; }
void card_nvm_changed(void) { nvm_mark_dirty(); }
void card_request_bootsel(void) { s_bootsel = true; }
bool card_bootsel_requested(void) { return s_bootsel; }
void fake_clear_bootsel(void) { s_bootsel = false; }

// pico/rand.h
uint32_t get_rand_32(void) {
    s_rand = s_rand * 1664525u + 1013904223u;
    return s_rand;
}
