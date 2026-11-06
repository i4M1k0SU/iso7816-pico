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

    // ENVELOPE with an inner extended case 2 (Lc = 0, Le = 4 = the response length): the ENVELOPE TPDU is case 3, so
    // the data comes through 61 xx / GET RESPONSE, not as a second ACK + data (the reader model flags that as an
    // error and t0r_tpdu fails).
    fake_phy_tx_clear();
    CHECK(tpdu("00 C2 00 00 07 80 30 00 00 00 00 04", &r));
    CHECK_EQ(r.sw, 0x6104);
    CHECK_EQ(r.len, 0);
    tx = fake_phy_tx_log(&n);
    CHECK_EQ(n, 3);                              // ACK for the ENVELOPE data, then 61 04 and nothing else
    CHECK_EQ(tx[0], 0xC2);
    CHECK(tpdu("00 C0 00 00 04", &r));
    CHECK_EQ(r.sw, 0x9000);
    CHECK_EQ(r.len, 4);
    counter = ((uint32_t)r.data[0] << 24) | ((uint32_t)r.data[1] << 16) | ((uint32_t)r.data[2] << 8) | r.data[3];
    // Inner Le (16) differs from the response length: still 61 04, never 6C xx to the ENVELOPE.
    CHECK(tpdu("00 C2 00 00 07 80 30 00 00 00 00 10", &r));
    CHECK_EQ(r.sw, 0x6104);
    CHECK(apdu("00 C2 00 00 07 80 30 00 00 00 00 00", &r));   // Le = 65536, through the PC/SC-like model
    CHECK_EQ(r.sw, 0x9000);
    CHECK_EQ(r.len, 4);
    CHECK_EQ(r.data[3], (counter + 2) & 0xFF);   // one execution per ENVELOPE (the previous one was discarded)

    // ENVELOPE with an inner extended case 4 (ECHO, Lc = 3, Le = 65536).
    CHECK(tpdu("00 C2 00 00 0C 80 20 00 00 00 00 03 AA BB CC 00 00", &r));
    CHECK_EQ(r.sw, 0x6103);
    CHECK(tpdu("00 C0 00 00 03", &r));
    CHECK_EQ(r.sw, 0x9000);
    CHECK_HEX(r.data, r.len, "AA BB CC");

    // A partial header (3 bytes, then silence) is dropped without an answer; the next full header works.
    uint32_t timeouts = fake_phy_rx_timeouts();
    fake_phy_tx_clear();
    fake_phy_push_hex("80 30 00");
    CHECK(t0_handle_command(-1));
    tx = fake_phy_tx_log(&n);
    CHECK_EQ(n, 0);
    CHECK_EQ(fake_phy_rx_timeouts(), timeouts + 1);
    CHECK(tpdu("80 30 00 00 04", &r));
    CHECK_EQ(r.sw, 0x9000);
    CHECK_EQ(r.len, 4);
    // The deadline is WWT = 960 x D x WI ETU (fake PHY: ETU 93 us, rounded up to 94; D as an earlier PPS left it).
    uint32_t d = phy_params()->d;
    CHECK_EQ(fake_phy_last_timeout_us(), 960u * 10 * d * 94);
    t0_set_wi(2);
    CHECK(tpdu("80 30 00 00 04", &r));
    CHECK_EQ(fake_phy_last_timeout_us(), 960u * 2 * d * 94);
    t0_set_wi(0);                                // reserved value: back to the default
    CHECK(tpdu("80 30 00 00 04", &r));
    CHECK_EQ(fake_phy_last_timeout_us(), 960u * 10 * d * 94);
    // A stray character followed by silence does not shift the next header.
    fake_phy_tx_clear();
    fake_phy_push_hex("3F");
    fake_phy_push_gap();
    fake_phy_push_hex("80 30 00 00 04");
    CHECK(t0_handle_command(-1));
    tx = fake_phy_tx_log(&n);
    CHECK_EQ(n, 0);
    CHECK_EQ(fake_phy_pending(), 5);
    fake_phy_clear();
    CHECK(tpdu("80 30 00 00 04", &r));
    CHECK_EQ(r.sw, 0x9000);
    // The same with the session layer passing the first byte (as session_run() does).
    fake_phy_tx_clear();
    CHECK(t0_handle_command(0x3F));
    tx = fake_phy_tx_log(&n);
    CHECK_EQ(n, 0);

    // A data field that stops short is dropped: the card sent only the ACK, and the next command works.
    fake_phy_tx_clear();
    fake_phy_push_hex("80 20 00 00 04 DE AD");
    CHECK(t0_handle_command(-1));
    tx = fake_phy_tx_log(&n);
    CHECK_EQ(n, 1);
    CHECK_EQ(tx[0], 0x20);
    CHECK_EQ(fake_phy_pending(), 0);
    CHECK(apdu("80 20 00 00 02 01 02", &r));
    CHECK_EQ(r.sw, 0x9000);
    CHECK_HEX(r.data, r.len, "01 02");
    // Same for an ENVELOPE piece: the chain is discarded, so a following short ENVELOPE stands alone.
    fake_phy_tx_clear();
    fake_phy_push_hex("00 C2 00 00 FF 80 20 00 00");
    CHECK(t0_handle_command(-1));
    tx = fake_phy_tx_log(&n);
    CHECK_EQ(n, 1);
    CHECK(apdu("00 C2 00 00 09 80 20 00 00 00 00 02 01 02", &r));
    CHECK_EQ(r.sw, 0x9000);
    CHECK_HEX(r.data, r.len, "01 02");

    // Case 2 whose handler returns no data with 90 00 (MANAGE CHANNEL open with P2 = 01 answers no channel number):
    // SW only, no ACK and no 6C xx (6C 00 would mean La = 256).
    fake_phy_tx_clear();
    CHECK(tpdu("00 70 00 01 01", &r));
    CHECK_EQ(r.sw, 0x9000);
    CHECK_EQ(r.len, 0);
    tx = fake_phy_tx_log(&n);
    CHECK_EQ(n, 2);
    CHECK(tpdu("00 70 80 01 00", &r));           // close it again
    CHECK_EQ(r.sw, 0x9000);

    // The reader asserting RST in the middle of a command aborts it.
    fake_phy_push_hex("80 20 00 00 04 DE AD");
    fake_phy_set_reset(true);
    CHECK(!t0_handle_command(-1));
    fake_phy_set_reset(false);
    fake_phy_clear();
}
