// Seed corpus generator for the fuzzers: runs command sequences from the host tests' card suite through the T=0 and
// T=1 reader models (tests/reader_t0.c, reader_t1.c) and records every byte they queue for the card (fake_phy_push()
// is wrapped at link time), in the input format of each harness.  The APDU, ATR, PPS and NVM seeds are written from
// tables and from a real nvm_save().
//
//   build-fuzz/fuzz_gen_corpus tests/fuzz/corpus
#include "fake_nvm.h"
#include "fake_phy.h"
#include "fuzz_common.h"
#include "hal/nvm.h"
#include "reader.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

size_t hex_parse(const char *hex, uint8_t *out, size_t cap);

// ---- recording ---------------------------------------------------------------------------------------------------

static uint8_t s_rec[65536];
static size_t s_rec_len;

void __real_fake_phy_push(const uint8_t *data, size_t len);
void __wrap_fake_phy_push(const uint8_t *data, size_t len) {
    if (s_rec_len + len > sizeof s_rec) {
        fprintf(stderr, "gen_corpus: recording overflow\n");
        exit(1);
    }
    memcpy(&s_rec[s_rec_len], data, len);
    s_rec_len += len;
    __real_fake_phy_push(data, len);
}

static const char *s_out;

static void write_seed(const char *harness, const char *name, const uint8_t *prefix, size_t prefix_len,
                       const uint8_t *data, size_t len) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", s_out, harness);
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        perror(path);
        exit(1);
    }
    snprintf(path, sizeof path, "%s/%s/%s", s_out, harness, name);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(prefix, 1, prefix_len, f) != prefix_len || fwrite(data, 1, len, f) != len || fclose(f) != 0) {
        perror(path);
        exit(1);
    }
}

// ---- command sequences (from tests/test_card_suite.c) ------------------------------------------------------------

typedef struct {
    const char *name;
    const char *apdus[12];
} scenario_t;

// clang-format off
static const scenario_t s_scenarios[] = {
    { "applet", { "00 A4 04 00 05 F0 50 49 43 4F", "80 10 00 00 00", "80 20 00 00 04 DE AD BE EF",
                  "80 30 00 00 04", "80 40 00 00 00", "00 11 00 00 00", "90 10 00 00 00" } },
    { "pin", { "00 D6 00 00 03 41 42 43", "00 20 00 01 04 30 30 30 30", "00 20 00 01 00",
               "00 20 00 01 04 31 32 33 34", "00 D6 00 00 05 50 69 63 6F 21", "00 B0 00 00 10",
               "00 24 00 01 08 31 32 33 34 34 33 32 31", "00 2C 01 01 08 31 32 33 34 35 36 37 38",
               "00 26 00 01 04 34 33 32 31", "00 28 00 01 04 34 33 32 31" } },
    { "binary", { "00 B0 00 00 00", "00 B0 81 05 04", "00 B0 01 00 10", "10 D6 00 F0 04 43 48 41 49",
                  "00 D6 00 F0 04 4E 45 44 21", "00 B0 00 F0 08", "00 0E 00 10 00", "00 D0 00 00 01 FF" } },
    { "select", { "00 A4 00 00 02 3F 00", "00 A4 01 04 02 7F 10", "00 A4 02 0C 02 01 02", "00 A4 03 00 00",
                  "00 A4 08 04 04 7F 10 01 01", "00 A4 02 00 02 01 99", "00 A4 04 02 05 F0 50 49 43 4F",
                  "00 A4 09 04 02 01 03" } },
    { "records", { "00 B2 01 14 10", "00 B2 02 04 10", "00 B2 00 02 10", "00 B2 05 04 10",
                   "00 DC 03 04 10 74 68 69 72 64 20 72 65 63 6F 72 64 20 20 20 20", "00 A2 01 04 03 72 65 63",
                   "00 E2 00 18 08 01 02 03 04 05 06 07 08", "00 B2 01 1C 08", "00 D2 00 1C 08 11 12 13 14 15 16 17 18",
                   "00 0C 01 04 00" } },
    { "varrec", { "00 A4 02 0C 02 01 04", "00 B2 01 04 00", "00 20 00 81 04 30 30 30 30",
                  "00 E2 00 00 03 61 62 63", "00 DC 01 04 03 61 62 63", "00 B2 01 04 00" } },
    { "auth", { "00 CA 01 00 00", "00 CA 03 00 00", "00 84 00 00 08", "00 82 00 00 08 01 02 03 04 05 06 07 08",
                "00 88 00 00 04 01 02 03 04 00", "00 2A 80 86 02 01 02" } },
    { "channels", { "00 70 00 00 01", "01 A4 04 00 05 F0 50 49 43 4F", "01 B0 00 00 05", "01 D6 00 00 01 41",
                    "00 70 80 01 00", "01 B0 00 00 05", "00 70 00 00 01", "00 70 00 00 01", "00 70 00 00 01" } },
};
// clang-format on

#define N_SCENARIOS (sizeof s_scenarios / sizeof s_scenarios[0])

static size_t apdu_bytes(const char *hex, uint8_t *buf, size_t cap) { return hex_parse(hex, buf, cap); }

// The same sequence over T=0: one input per scenario, plus extended length through ENVELOPE.
static void gen_t0(void) {
    static reader_result_t r;
    uint8_t buf[600];
    const uint8_t raw[1] = { 0x00 };
    for (size_t i = 0; i < N_SCENARIOS; i++) {
        fuzz_card_reset(0, false, 32);
        t0r_begin();
        s_rec_len = 0;
        for (const char *const *a = s_scenarios[i].apdus; *a; a++) t0r_apdu(buf, apdu_bytes(*a, buf, sizeof buf), &r);
        write_seed("t0", s_scenarios[i].name, raw, 1, s_rec, s_rec_len);
    }
    fuzz_card_reset(0, false, 32);
    t0r_begin();
    s_rec_len = 0;
    size_t n = apdu_bytes("00 C2 00 00 0B 80 20 00 00 00 00 04 DE AD BE EF", buf, sizeof buf);
    t0r_apdu(buf, n, &r);
    n = apdu_bytes("00 C2 00 00 FF 80 20 00 00 00 01 2C", buf, sizeof buf);
    for (int i = 0; i < 248; i++) buf[n++] = (uint8_t)(i + 1);
    t0r_apdu(buf, n, &r);
    n = apdu_bytes("00 C2 00 00 34", buf, sizeof buf);
    for (int i = 248; i < 300; i++) buf[n++] = (uint8_t)(i + 1);
    t0r_apdu(buf, n, &r);
    write_seed("t0", "envelope", raw, 1, s_rec, s_rec_len);
    // A wrong Le (6C xx and the re-issued command) and an unanswered 61 xx.
    fuzz_card_reset(0, false, 32);
    t0r_begin();
    s_rec_len = 0;
    n = apdu_bytes("00 B0 00 00 20", buf, sizeof buf);
    t0r_tpdu(buf, n, &r);
    n = apdu_bytes("80 30 00 00 08", buf, sizeof buf);
    t0r_apdu(buf, n, &r);
    n = apdu_bytes("00 A4 04 00 05 F0 50 49 43 4F 00", buf, sizeof buf);
    t0r_tpdu(buf, n, &r);
    n = apdu_bytes("00 C0 00 00 04", buf, sizeof buf);
    t0r_tpdu(buf, n, &r);
    write_seed("t0", "getresponse", raw, 1, s_rec, s_rec_len);
}

// Strip the LRC from every recorded block: the framed input format of fuzz_t1 (the harness appends the EDC).
static size_t frame_blocks(const uint8_t *in, size_t len, uint8_t *out) {
    size_t i = 0, o = 0;
    while (i + 3 <= len) {
        size_t n = 3 + in[i + 2];
        if (i + n + 1 > len) break;
        memcpy(&out[o], &in[i], n);
        o += n;
        i += n + 1;
    }
    return o;
}

static void gen_t1_seq(const char *name, const char *const *apdus, const uint8_t *ext, size_t ext_len) {
    static reader_result_t r;
    static uint8_t framed[65536];
    uint8_t buf[600];
    fuzz_card_reset(1, false, 254);
    t1r_begin(254);
    s_rec_len = 0;
    for (const char *const *a = apdus; a && *a; a++) t1r_apdu(buf, apdu_bytes(*a, buf, sizeof buf), &r);
    if (ext) t1r_apdu(ext, ext_len, &r);
    const uint8_t lrc[2] = { 0x00, 0xFE }, crc[2] = { 0x03, 0xFE };
    char crc_name[64];
    write_seed("t1", name, lrc, 2, s_rec, s_rec_len);
    snprintf(crc_name, sizeof crc_name, "%s-crc", name);
    write_seed("t1", crc_name, crc, 2, framed, frame_blocks(s_rec, s_rec_len, framed));
}

static void gen_t1(void) {
    for (size_t i = 0; i < N_SCENARIOS; i++) gen_t1_seq(s_scenarios[i].name, s_scenarios[i].apdus, NULL, 0);
    // Extended length, chained in both directions with the default IFSC / IFSD of 32.
    uint8_t ext[400];
    size_t n = apdu_bytes("80 20 00 00 00 01 2C", ext, sizeof ext);
    for (int i = 0; i < 300; i++) ext[n++] = (uint8_t)(i + 1);
    ext[n++] = 0;
    ext[n++] = 0;
    static reader_result_t r;
    static uint8_t framed[65536];
    fuzz_card_reset(1, false, 32);
    t1r_begin(32);
    s_rec_len = 0;
    uint8_t ifs[] = { 0x00, 0xC1, 0x01, 0x20 };
    t1r_exchange(ifs, sizeof ifs);
    t1r_apdu(ext, n, &r);
    t1r_resynch();
    const uint8_t lrc[2] = { 0x00, 0x20 }, crc[2] = { 0x03, 0x20 };
    write_seed("t1", "chained", lrc, 2, s_rec, s_rec_len);
    write_seed("t1", "chained-crc", crc, 2, framed, frame_blocks(s_rec, s_rec_len, framed));
}

// ---- tables ------------------------------------------------------------------------------------------------------

static void gen_apdu(void) {
    // clang-format off
    static const char *const apdus[] = {
        "00 A4 04 00", "00 B0 00 00 00", "00 D6 00 00 03 41 42 43", "00 A4 04 00 05 F0 50 49 43 4F 00",
        "80 20 00 00 00 00 04 DE AD BE EF 00 00", "80 20 00 00 00 00 04 DE AD BE EF", "00 B0 00 00 00 01 00",
    };
    // clang-format on
    for (size_t i = 0; i < sizeof apdus / sizeof apdus[0]; i++) {
        uint8_t buf[64];
        char name[16];
        snprintf(name, sizeof name, "apdu%zu", i);
        write_seed("apdu", name, NULL, 0, buf, apdu_bytes(apdus[i], buf, sizeof buf));
    }
}

static void gen_atr(void) {
    // ATRs from the ATR and session tests (the demo's T=0 and T=1 ATRs among them) and an inverse-convention one.
    // clang-format off
    static const char *const atrs[] = {
        "3B 08 50 69 63 6F 37 38 31 36",
        "3B 88 81 31 FE 45 50 69 63 6F 37 38 31 36 BE",
        "3B 98 13 80 31 FE 45 50 69 63 6F 37 38 31 36 BC",
        "3B 90 13 10 00",
        "3F 96 11 80 31 FE 45 01 02 03 04 05 06 5F",
    };
    // clang-format on
    for (size_t i = 0; i < sizeof atrs / sizeof atrs[0]; i++) {
        uint8_t buf[64];
        char name[16];
        snprintf(name, sizeof name, "atr%zu", i);
        write_seed("atr", name, NULL, 0, buf, apdu_bytes(atrs[i], buf, sizeof buf));
    }
}

static void gen_pps(void) {
    // offered protocols, current F/D, hook flag, then PPS0 [PPS1..3] PCK (PPSS = FF already consumed)
    // clang-format off
    static const char *const pps[] = {
        "03 11 00 00 FF",          // T=0, no PPS1
        "03 11 00 01 FE",          // T=1, no PPS1
        "03 11 00 11 13 FD",       // T=1, F/D 372/4
        "03 11 01 11 96 78",       // T=1, F/D 512/32 with the hook
        "83 11 01 71 13 00 00 9D", // PPS1..3, the refusing hook
        "01 11 00 01 FE",          // T=1 not offered
    };
    // clang-format on
    for (size_t i = 0; i < sizeof pps / sizeof pps[0]; i++) {
        uint8_t buf[16];
        char name[16];
        snprintf(name, sizeof name, "pps%zu", i);
        write_seed("pps", name, NULL, 0, buf, apdu_bytes(pps[i], buf, sizeof buf));
    }
}

// A flash with two images written by nvm_save(), as chunks of the first bytes of each slot (fuzz_nvm fixes the CRCs).
static void gen_nvm(void) {
    fuzz_card_reset(0, false, 32);
    uint8_t *blob = card_profile->nvm_blob;
    size_t size = card_profile->nvm_size;
    nvm_save(blob, size);
    blob[0] ^= 1;
    nvm_save(blob, size);
    const uint8_t *flash = fake_nvm_raw();
    uint8_t in[2 + 2 * (2 + 64)];
    size_t n = 0;
    in[n++] = 0x09; // fix CRCs, save + load round trip
    in[n++] = 0x00;
    for (int s = 0; s < 2; s++) {
        in[n++] = (uint8_t)s;
        in[n++] = 64;
        memcpy(&in[n], flash + (size_t)s * NVM_SECTOR_BYTES, 64);
        n += 64;
    }
    write_seed("nvm", "two-images", NULL, 0, in, n);
    in[0] = 0x07; // fix CRCs, migration hook that accepts; layout 0 differs from nvm_version 1 below
    in[1] = 0x01;
    write_seed("nvm", "migrate", NULL, 0, in, n);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <corpus directory>\n", argv[0]);
        return 2;
    }
    s_out = argv[1];
    mkdir(s_out, 0755);
    gen_apdu();
    gen_atr();
    gen_pps();
    gen_t0();
    gen_t1();
    gen_nvm();
    return 0;
}
