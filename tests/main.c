#include "test.h"

#include "iso7816_pico/fs.h"
#include "iso7816_pico/sec.h"
#include "iso7816_pico/card.h"
#include "fakes.h"

#include <stdlib.h>

int g_checks, g_failures;

void run_test(const char *name, void (*fn)(void)) {
    int before = g_failures;
    printf("%s\n", name);
    fn();
    if (g_failures != before) printf("  -> %d failure(s)\n", g_failures - before);
}

size_t hex_parse(const char *hex, uint8_t *out, size_t cap) {
    size_t n = 0;
    int hi = -1;
    for (const char *p = hex; *p; p++) {
        int v;
        if (*p >= '0' && *p <= '9') v = *p - '0';
        else if (*p >= 'a' && *p <= 'f') v = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') v = *p - 'A' + 10;
        else continue;
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

bool hex_equal(const uint8_t *buf, size_t len, const char *hex) {
    uint8_t tmp[8192];
    size_t n = hex_parse(hex, tmp, sizeof tmp);
    return n == len && memcmp(buf, tmp, len) == 0;
}

void hex_print(const uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) printf(i ? " %02X" : "%02X", buf[i]);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) g_verbose_log = true;
    }
    // What sys/main.c does at boot.
    extern const card_profile_t demo_card_profile;
    card_profile = &demo_card_profile;
    fs_init(card_profile->files, card_profile->n_files);
    sec_init(card_profile->pins, card_profile->n_pins);

    run_test("atr", test_atr);
    run_test("apdu", test_apdu);
    run_test("pps", test_pps);
    run_test("session", test_session);
    run_test("t0", test_t0);
    run_test("t1", test_t1);
    run_test("dispatch", test_dispatch);
    run_test("select", test_select);
    run_test("fs", test_fs);
    run_test("fs / sec edge cases", test_fs_sec);
    run_test("nvm", test_nvm);
    run_test("nvm pin durability", test_nvm_pin_durability);
    run_test("nvm layout", test_nvm_layout);
    run_test("card suite", test_card_suite);
    printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
