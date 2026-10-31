// ERASE RECORD (0C) and ERASE BINARY (0E) over T=0 against the demo card profile.
#include "apdu/dispatch.h"
#include "apdu/fs.h"
#include "fake_phy.h"
#include "fakes.h"
#include "proto/t0.h"
#include "reader.h"
#include "test.h"

static reader_result_t r;

static uint16_t sw(const char *hex) {
    uint8_t buf[600];
    size_t n = hex_parse(hex, buf, sizeof buf);
    if (!t0r_apdu(buf, n, &r)) return 0xFFFF;
    return r.sw;
}

// CLA INS P1 P2 (hex) + Lc + data.
static uint16_t sw_data(const char *hdr_hex, const uint8_t *data, size_t n) {
    uint8_t buf[5 + 255];
    if (hex_parse(hdr_hex, buf, 4) != 4 || n > 255) return 0xFFFF;
    buf[4] = (uint8_t)n;
    memcpy(buf + 5, data, n);
    if (!t0r_apdu(buf, 5 + n, &r)) return 0xFFFF;
    return r.sw;
}

static bool all_zero(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (p[i]) return false;
    }
    return true;
}

static const fs_file_t *ef_in_7f10(uint16_t fid) { return fs_child(fs_child(fs_mf(), 0x7F10), fid); }

// Read record n of the current EF and check it is 16 zero bytes.
static void check_record_zero(int n) {
    char cmd[32];
    sprintf(cmd, "00 B2 %02X 04 10", n);
    CHECK_EQ(sw(cmd), 0x9000);
    CHECK(r.len == 16 && all_zero(r.data, 16));
}

void test_fs(void) {
    // The EFs changed here are saved as they are now and put back at the end, so later groups see the same card.
    static const uint16_t fids[] = { 0x0101, 0x0102, 0x0104 };
    static uint8_t saved[3][256];
    const fs_file_t *efs[3];
    for (int i = 0; i < 3; i++) {
        efs[i] = ef_in_7f10(fids[i]);
        CHECK(efs[i] && efs[i]->size <= sizeof saved[i]);
        if (!efs[i] || efs[i]->size > sizeof saved[i]) return;
        memcpy(saved[i], efs[i]->data, efs[i]->size);
    }
    const uint8_t *hello = saved[0];
    const uint8_t *rec1 = saved[1] + FS_REC_HDR_SIZE;

    uint8_t pat[48];
    for (int i = 0; i < 48; i++) pat[i] = (uint8_t)(0xA0 + i);

    g_fake_protocol = 0;
    dispatch_reset();
    t0_session_begin();
    t0r_begin();

    // Both EFs need PIN 01 for updates: without it nothing is erased.
    CHECK_EQ(sw("00 A4 02 0C 02 01 02"), 0x9000);
    CHECK_EQ(sw("00 0C 01 04 00"), 0x6982);
    CHECK_EQ(sw("00 0C 01 05 00"), 0x6982);
    CHECK_EQ(sw("00 B2 01 04 10"), 0x9000);
    CHECK(r.len == 16 && memcmp(r.data, rec1, 16) == 0);
    CHECK_EQ(sw("00 A4 02 0C 02 01 01"), 0x9000);
    CHECK_EQ(sw("00 0E 00 00 00"), 0x6982);
    CHECK_EQ(sw("00 0E 00 10 02 00 20"), 0x6982);
    CHECK_EQ(sw("00 B0 00 00 00"), 0x9000);
    CHECK(r.len == 256 && memcmp(r.data, hello, 256) == 0);

    CHECK_EQ(sw("00 20 00 01 04 31 32 33 34"), 0x9000);

    // ERASE RECORD by number (P2 = 04) in the current EF (linear fixed 4 x 16).
    CHECK_EQ(sw("00 A4 02 0C 02 01 02"), 0x9000);
    CHECK_EQ(sw_data("00 DC 03 04", pat, 16), 0x9000);
    CHECK_EQ(sw("00 B2 03 04 10"), 0x9000);
    CHECK(r.len == 16 && memcmp(r.data, pat, 16) == 0);
    CHECK_EQ(sw("00 0C 03 04 00"), 0x9000);
    check_record_zero(3);

    // P1 = 00: the current record (UPDATE RECORD 3 made record 3 current).
    CHECK_EQ(sw_data("00 DC 03 04", pat, 16), 0x9000);
    CHECK_EQ(sw("00 0C 00 04 00"), 0x9000);
    check_record_zero(3);

    // P2 = 05: from record P1 up to the last one; record 1 stays.
    CHECK_EQ(sw_data("00 DC 03 04", pat, 16), 0x9000);
    CHECK_EQ(sw_data("00 DC 04 04", pat + 16, 16), 0x9000);
    CHECK_EQ(sw("00 0C 02 05 00"), 0x9000);
    for (int n = 2; n <= 4; n++) check_record_zero(n);
    CHECK_EQ(sw("00 B2 01 04 10"), 0x9000);
    CHECK(r.len == 16 && memcmp(r.data, rec1, 16) == 0);

    // No such record / unsupported mode.
    CHECK_EQ(sw("00 0C 05 04 00"), 0x6A83);
    CHECK_EQ(sw("00 0C 01 00 00"), 0x6A86);

    // Linear variable EF 0104 (PIN 81): the erased record is empty and its slot is cleared.
    CHECK_EQ(sw("00 A4 02 0C 02 01 04"), 0x9000);
    CHECK_EQ(sw("00 20 00 81 04 30 30 30 30"), 0x9000);
    CHECK_EQ(sw("00 DC 01 04 03 61 62 63"), 0x9000);
    CHECK_EQ(sw("00 0C 01 04 00"), 0x9000);
    CHECK_EQ(sw("00 B2 01 04 00"), 0x9000);
    CHECK_EQ(r.len, 0);
    CHECK(all_zero(efs[2]->data + FS_REC_HDR_SIZE, 2 + efs[2]->rec_size));

    // ERASE BINARY from P1P2 up to the end offset in the data field: 0x10..0x1F only.
    CHECK_EQ(sw("00 A4 02 0C 02 01 01"), 0x9000);
    CHECK_EQ(sw_data("00 D6 00 00", pat, 48), 0x9000);
    CHECK_EQ(sw("00 0E 00 10 02 00 20"), 0x9000);
    CHECK_EQ(sw("00 B0 00 00 30"), 0x9000);
    CHECK_EQ(r.len, 48);
    CHECK(memcmp(r.data, pat, 16) == 0);
    CHECK(all_zero(r.data + 16, 16));
    CHECK(memcmp(r.data + 32, pat + 32, 16) == 0);

    // ERASE BINARY to the end of the file.
    CHECK_EQ(sw_data("00 D6 00 F0", pat, 16), 0x9000);
    CHECK_EQ(sw("00 0E 00 F0 00"), 0x9000);
    CHECK_EQ(sw("00 B0 00 F0 10"), 0x9000);
    CHECK(r.len == 16 && all_zero(r.data, 16));
    CHECK_EQ(sw("00 B0 00 E0 10"), 0x9000);
    CHECK(r.len == 16 && memcmp(r.data, hello + 0xE0, 16) == 0);

    // End before start, bad end offset length.
    CHECK_EQ(sw("00 0E 00 20 02 00 10"), 0x6B00);
    CHECK_EQ(sw("00 0E 00 00 01 10"), 0x6700);

    for (int i = 0; i < 3; i++) memcpy(efs[i]->data, saved[i], efs[i]->size);
    dispatch_reset();
    fake_phy_clear();
}
