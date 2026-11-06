// The same checks as tools/sc_test.ps1 / sc_test.py, run over both transports against the demo card profile.
#include "apdu/dispatch.h"
#include "fake_phy.h"
#include "fakes.h"
#include "proto/t0.h"
#include "reader.h"
#include "test.h"

typedef bool (*xmit_fn)(const uint8_t *apdu, size_t len, reader_result_t *out);

static xmit_fn s_xmit;
static reader_result_t s_r;

// One step: send the APDU, check the SW and optionally the data (hex) or its length.
static void step(const char *label, const char *apdu_hex, uint16_t sw, const char *data_hex, int len) {
    uint8_t buf[READER_MAX_DATA];
    size_t n = hex_parse(apdu_hex, buf, sizeof buf);
    bool ok = s_xmit(buf, n, &s_r);
    g_checks++;
    bool good = ok && s_r.sw == sw;
    if (good && data_hex) good = hex_equal(s_r.data, s_r.len, data_hex);
    if (good && len >= 0) good = (int)s_r.len == len;
    if (!good) {
        g_failures++;
        printf("  FAIL %-30s %.46s -> %s SW %04X len %u\n", label, apdu_hex, ok ? "" : "(transport error)", s_r.sw,
               (unsigned)s_r.len);
    }
}

static void hexcat(char *dst, const uint8_t *src, size_t n) {
    for (size_t i = 0; i < n; i++) sprintf(dst + strlen(dst), i ? " %02X" : "%02X", src[i]);
}

static void suite(bool t1) {
    char seq[3 * 1000 + 1] = "";
    uint8_t tmp[1000];
    for (int i = 0; i < 200; i++) tmp[i] = (uint8_t)(i + 1);
    hexcat(seq, tmp, 200);
    char apdu[3 * 1000 + 64];
    const char *hello16 = "50 69 63 6F 21 20 66 72 6F 6D 20 69 73 6F 37 38";
    const char *rec1 = "72 65 63 6F 72 64 20 31 20 20 20 20 20 20 20 20";
    const char *rec2 = "72 65 63 6F 72 64 20 32 20 20 20 20 20 20 20 20";
    const char *rec3 = "74 68 69 72 64 20 72 65 63 6F 72 64 20 20 20 20";

    // demo applet (CLA 80)
    step("SELECT by AID", "00 A4 04 00 05 F0 50 49 43 4F", 0x9000, "6F 07 84 05 F0 50 49 43 4F", -1);
    step("SELECT unknown AID", "00 A4 04 00 02 11 22", 0x6A82, NULL, -1);
    step("GET VERSION", "80 10 00 00 00", 0x9000, NULL, -1);
    step("ECHO", "80 20 00 00 04 DE AD BE EF", 0x9000, "DE AD BE EF", -1);
    step("GET COUNTER", "80 30 00 00 04", 0x9000, NULL, 4);
    uint32_t n1 = s_r.data[3];
    step("GET COUNTER (Le=00)", "80 30 00 00 00", 0x9000, NULL, 4);
    CHECK_EQ(s_r.data[3], (n1 + 1) & 0xFF);
    sprintf(apdu, "80 20 00 00 C8 %s", seq);
    step("ECHO 200 bytes", apdu, 0x9000, seq, -1);
    step("SLOW (keep-alive)", "80 40 00 00 00", 0x9000, "64 6F 6E 65", -1);
    step("unknown INS", "00 11 00 00 00", 0x6D00, NULL, -1);
    step("unknown CLA", "90 10 00 00 00", 0x6E00, NULL, -1);

    // transparent EF 0101 + PIN 01
    step("UPDATE BINARY (no PIN)", "00 D6 00 00 03 41 42 43", 0x6982, NULL, -1);
    step("VERIFY wrong PIN", "00 20 00 01 04 30 30 30 30", 0x63C2, NULL, -1);
    step("VERIFY status", "00 20 00 01 00", 0x63C2, NULL, -1);
    step("VERIFY 1234", "00 20 00 01 04 31 32 33 34", 0x9000, NULL, -1);
    step("VERIFY status", "00 20 00 01 00", 0x9000, NULL, -1);
    step("UPDATE BINARY", "00 D6 00 00 05 50 69 63 6F 21", 0x9000, NULL, -1);
    step("READ BINARY 16", "00 B0 00 00 10", 0x9000, hello16, -1);
    step("READ BINARY 256", "00 B0 00 00 00", 0x9000, NULL, 256);
    step("READ BINARY via SFI 1", "00 B0 81 05 04", 0x9000, "20 66 72 6F", -1);
    step("READ BINARY bad offset", "00 B0 01 00 10", 0x6B00, NULL, -1);
    step("UPDATE chained (1/2)", "10 D6 00 F0 04 43 48 41 49", 0x9000, NULL, -1);
    step("UPDATE chained (2/2)", "00 D6 00 F0 04 4E 45 44 21", 0x9000, NULL, -1);
    step("READ BINARY @F0", "00 B0 00 F0 08", 0x9000, "43 48 41 49 4E 45 44 21", -1);
    step("CHANGE REF DATA 1234->4321", "00 24 00 01 08 31 32 33 34 34 33 32 31", 0x9000, NULL, -1);
    step("VERIFY 4321", "00 20 00 01 04 34 33 32 31", 0x9000, NULL, -1);
    step("CHANGE REF DATA back", "00 24 00 01 08 34 33 32 31 31 32 33 34", 0x9000, NULL, -1);
    step("RESET RETRY (PUK)", "00 2C 01 01 08 31 32 33 34 35 36 37 38", 0x9000, NULL, -1);

    // SELECT by FID / path, FCP
    step("SELECT MF", "00 A4 00 00 02 3F 00", 0x9000, "6F 0A 82 01 38 83 02 3F 00 8A 01 05", -1);
    step("SELECT DF 7F10 (FCP)", "00 A4 01 04 02 7F 10", 0x9000,
         "62 11 82 01 38 83 02 7F 10 84 05 F0 50 49 43 4F 8A 01 05", -1);
    step("SELECT EF 0102, no data", "00 A4 02 0C 02 01 02", 0x9000, NULL, 0);
    step("SELECT parent", "00 A4 03 00 00", 0x9000, "6F 0A 82 01 38 83 02 3F 00 8A 01 05", -1);
    step("SELECT path from MF", "00 A4 08 04 04 7F 10 01 01", 0x9000, NULL, 23);
    step("SELECT by DF name", "00 A4 04 00 05 F0 50 49 43 4F", 0x9000, "6F 07 84 05 F0 50 49 43 4F", -1);
    step("SELECT missing FID", "00 A4 02 00 02 01 99", 0x6A82, NULL, -1);

    // records
    step("READ RECORD 1 (SFI 2)", "00 B2 01 14 10", 0x9000, rec1, -1);
    step("READ RECORD 2", "00 B2 02 04 10", 0x9000, rec2, -1);
    step("READ RECORD next (3)", "00 B2 00 02 10", 0x9000, NULL, 16);
    step("READ RECORD 5", "00 B2 05 04 10", 0x6A83, NULL, -1);
    sprintf(apdu, "00 DC 03 04 10 %s", rec3);
    step("UPDATE RECORD 3", apdu, 0x9000, NULL, -1);
    step("READ RECORD 3", "00 B2 03 04 10", 0x9000, rec3, -1);
    step("SEARCH RECORD \"rec\"", "00 A2 01 04 03 72 65 63", 0x9000, "01 02", -1);
    step("APPEND RECORD cyclic", "00 E2 00 18 08 01 02 03 04 05 06 07 08", 0x9000, NULL, -1);
    step("APPEND RECORD cyclic", "00 E2 00 18 08 11 12 13 14 15 16 17 18", 0x9000, NULL, -1);
    step("READ RECORD 1 cyclic", "00 B2 01 1C 08", 0x9000, "11 12 13 14 15 16 17 18", -1);
    step("READ RECORD 2 cyclic", "00 B2 02 1C 08", 0x9000, "01 02 03 04 05 06 07 08", -1);
    step("SELECT EF 0104", "00 A4 02 0C 02 01 04", 0x9000, NULL, 0);
    step("READ RECORD (PIN 81)", "00 B2 01 04 00", 0x6982, NULL, -1);
    step("VERIFY PIN 81", "00 20 00 81 04 30 30 30 30", 0x9000, NULL, -1);
    // UPDATE RECORD does not create records: the first pass over a fresh card appends record 1 (as sc_test does).
    uint8_t rd[] = { 0x00, 0xB2, 0x01, 0x04, 0x00 };
    if (s_xmit(rd, sizeof rd, &s_r) && s_r.sw == 0x6A83) {
        step("APPEND RECORD (var, empty)", "00 E2 00 00 03 61 62 63", 0x9000, NULL, -1);
    }
    step("UPDATE RECORD 1 (var)", "00 DC 01 04 03 61 62 63", 0x9000, NULL, -1);
    step("READ RECORD 1 (var)", "00 B2 01 04 00", 0x9000, "61 62 63", -1);

    // GET DATA, GET CHALLENGE, authentication hooks
    step("GET DATA 0100", "00 CA 01 00 00", 0x9000, "00 11 22 33 44 55 66 77", -1);
    step("GET DATA unknown", "00 CA 03 00 00", 0x6A88, NULL, -1);
    step("GET CHALLENGE", "00 84 00 00 08", 0x9000, NULL, 8);
    for (int i = 0; i < 8; i++) tmp[i] = s_r.data[i] ^ 0x5A;
    strcpy(apdu, "00 82 00 00 08 ");
    hexcat(apdu, tmp, 8);
    step("EXTERNAL AUTH (good)", apdu, 0x9000, NULL, -1);
    step("EXTERNAL AUTH (no challenge)", apdu, 0x6985, NULL, -1);
    step("INTERNAL AUTH", "00 88 00 00 04 01 02 03 04 00", 0x9000, "5B 58 59 5E", -1);
    step("PSO (no hook)", "00 2A 80 86 02 01 02", 0x6A81, NULL, -1);

    // logical channels
    step("MANAGE CHANNEL open", "00 70 00 00 01", 0x9000, "01", -1);
    step("SELECT AID on ch 1", "01 A4 04 00 05 F0 50 49 43 4F", 0x9000, "6F 07 84 05 F0 50 49 43 4F", -1);
    step("READ BINARY on ch 1", "01 B0 00 00 05", 0x9000, "50 69 63 6F 21", -1);
    step("UPDATE on ch 1 (no PIN there)", "01 D6 00 00 01 41", 0x6982, NULL, -1);
    step("MANAGE CHANNEL close", "00 70 80 01 00", 0x9000, NULL, -1);
    step("READ BINARY on closed ch", "01 B0 00 00 05", 0x6881, NULL, -1);

    // extended length
    if (!t1) {
        step("ENVELOPE (ext. ECHO)", "00 C2 00 00 0B 80 20 00 00 00 00 04 DE AD BE EF", 0x9000, "DE AD BE EF", -1);
        for (int i = 0; i < 300; i++) tmp[i] = (uint8_t)(i + 1);
        char big[3 * 300 + 1] = "";
        hexcat(big, tmp, 300);
        strcpy(apdu, "00 C2 00 00 FF 80 20 00 00 00 01 2C ");
        hexcat(apdu, tmp, 248);
        step("ENVELOPE 300 B (1/2)", apdu, 0x9000, NULL, 0);
        strcpy(apdu, "00 C2 00 00 34 ");
        hexcat(apdu, tmp + 248, 52);
        step("ENVELOPE 300 B (2/2)", apdu, 0x9000, big, -1);
    } else {
        step("ECHO 4E (ext. Lc/Le)", "80 20 00 00 00 00 04 DE AD BE EF 00 00", 0x9000, "DE AD BE EF", -1);
        for (int i = 0; i < 1000; i++) tmp[i] = (uint8_t)(i + 1);
        char big[3 * 1000 + 1] = "";
        hexcat(big, tmp, 1000);
        sprintf(apdu, "80 20 00 00 00 03 E8 %s 00 00", big);
        step("ECHO 1000 B extended", apdu, 0x9000, big, -1);
        step("READ BINARY 256 (SFI 1)", "00 B0 81 00 00", 0x9000, NULL, 256);
    }
}

void test_card_suite(void) {
    printf("  T=0\n");
    g_fake_protocol = 0;
    dispatch_reset();
    t0_session_begin();
    t0r_begin();
    s_xmit = t0r_apdu;
    suite(false);

    printf("  T=1\n");
    g_fake_protocol = 1;
    dispatch_reset();
    t1r_begin(254);
    s_xmit = t1r_apdu;
    suite(true);
    fake_phy_clear();
}
