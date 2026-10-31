#include "apdu/dispatch.h"
#include "fake_phy.h"
#include "fakes.h"
#include "proto/t0.h"
#include "reader.h"
#include "test.h"

static bool tpdu(const char *hex, reader_result_t *r) {
    uint8_t buf[600];
    size_t n = hex_parse(hex, buf, sizeof buf);
    return t0r_tpdu(buf, n, r);
}

static bool apdu(const char *hex, reader_result_t *r) {
    uint8_t buf[600];
    size_t n = hex_parse(hex, buf, sizeof buf);
    return t0r_apdu(buf, n, r);
}

void test_t0(void) {
    reader_result_t r;
    size_t n;
    const uint8_t *tx;
    g_fake_protocol = 0;
    dispatch_reset();
    t0_session_begin();
    t0r_begin();

    // Case 2 with the right Le: ACK (= INS), data, SW in one go.
    fake_phy_tx_clear();
    CHECK(tpdu("80 30 00 00 04", &r));
    CHECK_EQ(r.sw, 0x9000);
    CHECK_EQ(r.len, 4);
    tx = fake_phy_tx_log(&n);
    CHECK_EQ(n, 7);
    CHECK_EQ(tx[0], 0x30);
    CHECK_EQ(tx[5], 0x90);
    uint32_t counter = ((uint32_t)r.data[0] << 24) | ((uint32_t)r.data[1] << 16) | ((uint32_t)r.data[2] << 8) |
                       r.data[3];

    // Wrong Le: 6C xx, and the re-issued command is served from the kept response (no second execution).
    CHECK(tpdu("80 30 00 00 00", &r));
    CHECK_EQ(r.sw, 0x6C04);
    CHECK_EQ(r.len, 0);
    CHECK(tpdu("80 30 00 00 04", &r));
    CHECK_EQ(r.sw, 0x9000);
    CHECK_EQ(r.data[3], (counter + 1) & 0xFF);
    CHECK(tpdu("80 30 00 00 04", &r));          // a fresh command executes again
    CHECK_EQ(r.data[3], (counter + 2) & 0xFF);

    // Case 4: 61 xx, GET RESPONSE with the right / wrong Le, GET RESPONSE with nothing pending.
    CHECK(tpdu("00 A4 04 00 05 F0 50 49 43 4F", &r));
    CHECK_EQ(r.sw, 0x6109);
    CHECK(tpdu("00 C0 00 00 20", &r));
    CHECK_EQ(r.sw, 0x6C09);
    CHECK(tpdu("00 C0 00 00 09", &r));
    CHECK_EQ(r.sw, 0x9000);
    CHECK_HEX(r.data, r.len, "6F 07 84 05 F0 50 49 43 4F");
    CHECK(tpdu("00 C0 00 00 09", &r));
    CHECK_EQ(r.sw, 0x6985);

    // A long response is fetched in 256-byte chunks: 61 00 means "256 or more".
    uint8_t big[5 + 255];
    memcpy(big, "\x80\x20\x00\x00\xFF", 5);
    for (int i = 0; i < 255; i++) big[5 + i] = (uint8_t)i;
    CHECK(t0r_tpdu(big, sizeof big, &r));
    CHECK_EQ(r.sw, 0x61FF);
    CHECK(tpdu("00 C0 00 00 00", &r));          // Le = 256 > 255 remaining -> 6C FF
    CHECK_EQ(r.sw, 0x6CFF);
    CHECK(tpdu("00 C0 00 00 FF", &r));
    CHECK_EQ(r.sw, 0x9000);
    CHECK_EQ(r.len, 255);

    // Case 3 with P3 = 0 (VERIFY status) sends no ACK and goes straight to the SW.
    fake_phy_tx_clear();
    CHECK(tpdu("00 20 00 01 00", &r));
    CHECK_EQ(r.sw, 0x63C3);
    tx = fake_phy_tx_log(&n);
    CHECK_EQ(n, 2);

    // Unknown INS is treated as case 1: no ACK, 6D00 even with P3 != 0.
    CHECK(tpdu("00 11 00 00 05", &r));
    CHECK_EQ(r.sw, 0x6D00);
    // CLA FF after the first command is not a PPS.
    CHECK(tpdu("FF 00 00 00 00", &r));
    CHECK_EQ(r.sw, 0x6E00);

    // NULL procedure bytes keep the reader waiting; the 6C re-issue does not run the slow handler again.
    uint32_t nulls = t0r_null_bytes();
    CHECK(apdu("80 40 00 00 00", &r));
    CHECK_EQ(r.sw, 0x9000);
    CHECK_HEX(r.data, r.len, "64 6F 6E 65");
    CHECK_EQ(t0r_null_bytes(), nulls + 4);

    // ENVELOPE: malformed content, chain terminated by a short ENVELOPE, empty ENVELOPE as terminator.
    CHECK(tpdu("00 C2 00 00 03 01 02 03", &r));
    CHECK_EQ(r.sw, 0x6A80);
    CHECK(apdu("00 C2 00 00 0B 80 20 00 00 00 00 04 DE AD BE EF", &r));
    CHECK_EQ(r.sw, 0x9000);
    CHECK_HEX(r.data, r.len, "DE AD BE EF");
    uint8_t env[5 + 255];
    memcpy(env, "\x00\xC2\x00\x00\xFF", 5);
    memcpy(env + 5, "\x80\x20\x00\x00\x00\x00\xF8", 7);      // case 3E, Lc = 248 -> exactly 255 bytes
    for (int i = 0; i < 248; i++) env[12 + i] = (uint8_t)i;
    CHECK(t0r_tpdu(env, sizeof env, &r));
    CHECK_EQ(r.sw, 0x9000);
    CHECK(apdu("00 C2 00 00 00", &r));          // terminator
    CHECK_EQ(r.sw, 0x9000);
    CHECK_EQ(r.len, 248);
    CHECK_EQ(r.data[247], 247);

    // The reader asserting RST in the middle of a command aborts it.
    fake_phy_push_hex("80 20 00 00 04 DE AD");
    fake_phy_set_reset(true);
    CHECK(!t0_handle_command(-1));
    fake_phy_set_reset(false);
    fake_phy_clear();
}
