#include "apdu/dispatch.h"
#include "fake_phy.h"
#include "fakes.h"
#include "proto/t1.h"
#include "reader.h"
#include "test.h"

static bool xchg(const char *hex) {
    uint8_t buf[300];
    size_t n = hex_parse(hex, buf, sizeof buf);
    return t1r_exchange(buf, n);
}

static void check_last(const char *hex) {
    size_t n;
    const uint8_t *b = t1r_last_block(&n);
    CHECK_HEX(b, n, hex);
}

static bool apdu(const char *hex, reader_result_t *r) {
    uint8_t buf[600];
    size_t n = hex_parse(hex, buf, sizeof buf);
    return t1r_apdu(buf, n, r);
}

void test_t1(void) {
    reader_result_t r;
    g_fake_protocol = 1;
    dispatch_reset();
    t1r_begin(254);

    // I(0): unknown INS -> I(0) with 6D 00.
    CHECK(xchg("00 00 05 00 11 00 00 00"));
    check_last("00 00 02 6D 00 6F");

    // Bad LRC -> R(1) with the EDC error bit.
    fake_phy_push_hex("00 40 05 00 11 00 00 00 00");
    CHECK(t1_handle_block(-1));
    check_last("00 91 00 91");

    // The correct I(1) is then processed; the reply carries the card's N(S) = 1.
    CHECK(xchg("00 40 05 00 11 00 00 00"));
    check_last("00 40 02 6D 00 2F");

    // Reader repeats I(1) (it missed our answer): the same block is sent again, nothing is executed twice.
    CHECK(xchg("00 40 05 00 11 00 00 00"));
    check_last("00 40 02 6D 00 2F");

    // R(1) asks for a retransmission of our block 1; R(0) is an ack we answer with R(0).
    CHECK(xchg("00 90 00"));
    check_last("00 40 02 6D 00 2F");
    CHECK(xchg("00 80 00"));
    check_last("00 80 00 80");

    // S(RESYNCH): sequence numbers restart at 0.
    CHECK(xchg("00 C0 00"));
    check_last("00 E0 00 E0");
    CHECK(xchg("00 00 05 00 11 00 00 00"));
    check_last("00 00 02 6D 00 6F");

    // S(ABORT) and an S(WTX request) coming from the reader (invalid: WTX is the card's to request).
    CHECK(xchg("00 C2 00"));
    check_last("00 E2 00 E2");
    CHECK(xchg("00 C3 01 01"));
    check_last("00 92 00 92");

    // Truncated block (LEN says 5, two INF bytes arrive): receive timeout -> R(1) "other error".
    uint32_t timeouts = fake_phy_rx_timeouts();
    fake_phy_push_hex("00 40 05 01 02");
    CHECK(t1_handle_block(-1));
    check_last("00 92 00 92");
    CHECK(fake_phy_rx_timeouts() > timeouts);
    fake_phy_clear();

    // NAD: source / destination nibbles are swapped in the answer.
    CHECK(xchg("21 40 05 00 11 00 00 00"));
    check_last("12 40 02 6D 00 3D");

    // S(IFS): a 256-byte READ BINARY now comes back in 32-byte I-blocks, acknowledged one by one.
    t1r_set_ifsd(0x20);
    check_last("00 E1 01 20 C0");
    uint32_t blocks = t1r_block_count();
    CHECK(apdu("00 B0 81 00 00", &r));
    CHECK_EQ(r.sw, 0x9000);
    CHECK_EQ(r.len, 256);
    CHECK_EQ(t1r_block_count() - blocks, 9);     // 258 bytes / 32
    t1r_set_ifsd(0xFE);

    // S(WTX request) from the slow handler, answered by the reader model.
    uint32_t wtx = t1r_wtx_requests();
    CHECK(apdu("80 40 00 00 00", &r));
    CHECK_EQ(r.sw, 0x9000);
    CHECK_HEX(r.data, r.len, "64 6F 6E 65");
    CHECK_EQ(t1r_wtx_requests(), wtx + 4);

    // Extended length in both directions, chained in 254-byte blocks.
    uint8_t big[9 + 1000];
    memcpy(big, "\x80\x20\x00\x00\x00\x03\xE8", 7);
    for (int i = 0; i < 1000; i++) big[7 + i] = (uint8_t)(i + 1);
    big[1007] = 0;
    big[1008] = 0;
    CHECK(t1r_apdu(big, sizeof big, &r));
    CHECK_EQ(r.sw, 0x9000);
    CHECK_EQ(r.len, 1000);
    CHECK(memcmp(r.data, big + 7, 1000) == 0);

    // A command larger than the APDU buffer is refused with S(ABORT request).
    static uint8_t huge[7 + APDU_BUF_SIZE + 100];
    memcpy(huge, "\x80\x20\x00\x00\x00\x10\x64", 7);
    CHECK(!t1r_apdu(huge, sizeof huge, &r));
    size_t n;
    const uint8_t *b = t1r_last_block(&n);
    CHECK_EQ(n, 4);
    CHECK_EQ(b[1], 0xC2);
    CHECK(xchg("00 E2 00"));                    // our S(ABORT response) is accepted silently
    fake_phy_clear();

    // After the abort the protocol continues normally (resync to be safe, as a reader would).
    CHECK(xchg("00 C0 00"));
    check_last("00 E0 00 E0");
    CHECK(xchg("00 00 05 00 11 00 00 00"));
    check_last("00 00 02 6D 00 6F");
}
