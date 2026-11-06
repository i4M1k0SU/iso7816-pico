#include "fuzz_common.h"

#include "apdu/dispatch.h"
#include "fake_nvm.h"
#include "fake_phy.h"
#include "hal/nvm.h"
#include "iso7816_pico/fs.h"
#include "iso7816_pico/log.h"
#include "iso7816_pico/sec.h"
#include "proto/t0.h"
#include "proto/t1.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const card_profile_t demo_card_profile;

card_profile_t g_fuzz_profile;

static uint8_t *s_pristine_blob; // the demo's NVM blob as compiled in
static uint8_t s_protocol;
static uint32_t s_rand;
static bool s_bootsel;

void fuzz_card_reset(uint8_t protocol, bool t1_crc, uint8_t t1_ifsc) {
    if (!s_pristine_blob) {
        s_pristine_blob = malloc(demo_card_profile.nvm_size);
        if (!s_pristine_blob) abort();
        memcpy(s_pristine_blob, demo_card_profile.nvm_blob, demo_card_profile.nvm_size);
    }
    g_fuzz_profile = demo_card_profile;
    card_profile = &g_fuzz_profile;
    memcpy(g_fuzz_profile.nvm_blob, s_pristine_blob, g_fuzz_profile.nvm_size);

    fake_nvm_reset();
    nvm_take_dirty();
    fake_phy_clear();
    fake_phy_set_tx_hook(NULL);
    s_protocol = protocol;
    s_rand = 0x12345678u;
    s_bootsel = false;

    fs_init(card_profile->files, card_profile->n_files);
    sec_init(card_profile->pins, card_profile->n_pins);
    dispatch_reset();
    t0_session_begin();
    t1_config_t cfg = { .ifsc = t1_ifsc, .crc = t1_crc, .cwi = 13 };
    t1_session_begin(&cfg);
}

void fuzz_phy_push(const uint8_t *data, size_t len) {
    if (len) fake_phy_push(data, len);
}

// ---- host replacements for the Pico-side services (as tests/fakes.c, but with a resettable RNG) --------------------

void log_printf(const char *fmt, ...) {
    // Format into a buffer anyway, so that a bad argument for a format is still caught by the sanitizers.
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
}

void log_apdu(const char *name, uint8_t cla, uint8_t ins, uint8_t p1, uint8_t p2, const uint8_t *in, size_t in_len,
              const uint8_t *out, size_t out_len, uint16_t sw) {
    // Touch every byte the transports claim to hand over, so that a wrong pointer or length shows up here.
    volatile uint8_t sink = (uint8_t)(cla ^ ins ^ p1 ^ p2 ^ sw ^ (name ? (uint8_t)name[0] : 0));
    for (size_t i = 0; in && i < in_len; i++) sink ^= in[i];
    for (size_t i = 0; out && i < out_len; i++) sink ^= out[i];
    (void)sink;
}

void log_drain(void) {}

void card_keepalive(void) {
    if (s_protocol == 1)
        t1_keepalive();
    else
        t0_keepalive();
}

uint8_t card_protocol(void) { return s_protocol; }
void card_nvm_changed(void) { nvm_mark_dirty(); }
void card_request_bootsel(void) { s_bootsel = true; }
bool card_bootsel_requested(void) { return s_bootsel; }

uint32_t get_rand_32(void) {
    s_rand = s_rand * 1664525u + 1013904223u;
    return s_rand;
}

// tests/fake_phy.c uses the hex parser of the test runner (tests/main.c), which is not linked here.
size_t hex_parse(const char *hex, uint8_t *out, size_t cap) {
    size_t n = 0;
    int hi = -1;
    for (const char *p = hex; *p; p++) {
        int v;
        if (*p >= '0' && *p <= '9')
            v = *p - '0';
        else if (*p >= 'a' && *p <= 'f')
            v = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F')
            v = *p - 'A' + 10;
        else
            continue;
        if (hi < 0) {
            hi = v;
        } else {
            if (n < cap) out[n] = (uint8_t)((hi << 4) | v);
            n++;
            hi = -1;
        }
    }
    return n;
}
