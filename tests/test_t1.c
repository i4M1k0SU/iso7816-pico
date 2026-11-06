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

// R(N(R)) from the reader.
static bool r_block(uint8_t nr) {
    uint8_t raw[3] = { 0x00, (uint8_t)(0x80 | (nr << 4)), 0x00 };
    return t1r_exchange(raw, 3);
}

static void copy_last(uint8_t *dst, size_t *len) {
    const uint8_t *b = t1r_last_block(len);
    memcpy(dst, b, *len);
}

// Error recovery (ISO/IEC 7816-3 11.6.3), each scenario from a fresh T=1 session.
static void test_t1_errors(void) {
    reader_result_t r;
    uint8_t saved[300];
    size_t saved_len, n;
    const uint8_t *b;

    // Card sends I(0); the reader missed it and asks again with R(0), but that R-block is corrupted.  The card's
    // R-block "EDC error" must not replace the I-block it keeps: the reader's next R(0) gets the I-block again.
    t1r_begin(254);
    CHECK(xchg("00 00 05 00 11 00 00 00"));
    check_last("00 00 02 6D 00 6F");
    fake_phy_push_hex("00 80 00 00"); // R(0) with a bad LRC
    CHECK(t1_handle_block(-1));
    check_last("00 91 00 91");
    CHECK(r_block(0));
    check_last("00 00 02 6D 00 6F");
    CHECK(r_block(0)); // as often as asked
    check_last("00 00 02 6D 00 6F");
    fake_phy_push_hex("00 80 FF"); // invalid LEN, then a timeout: "other error"
    CHECK(t1_handle_block(-1));
    check_last("00 92 00 92");
    CHECK(r_block(0));
    check_last("00 00 02 6D 00 6F");
    CHECK(xchg("00 40 05 00 11 00 00 00")); // the next command acknowledges I(0)
    check_last("00 40 02 6D 00 2F");

    // Retransmission requests during a chained response: each chunk is repeated on R(N(S) of that chunk), the next one
    // follows on R(N(S) + 1).
    t1r_begin(254);
    t1r_set_ifsd(0x20);
    check_last("00 E1 01 20 C0");
    CHECK(xchg("00 00 05 00 B0 81 00 00")); // READ BINARY 256 bytes -> 258 bytes in 9 I-blocks
    uint32_t chunks = 1, payload = 0;
    for (int hop = 0; hop < 20; hop++) {
        copy_last(saved, &saved_len);
        if (saved_len < 4 || (saved[1] & 0x80)) break;
        uint8_t ns = (saved[1] >> 6) & 1;
        CHECK_EQ(ns, (chunks - 1) & 1);
        payload += saved[2];
        fake_phy_push_hex("00 90 00 00"); // a corrupted block in between changes nothing
        CHECK(t1_handle_block(-1));
        b = t1r_last_block(&n);
        CHECK(n == 4 && (b[1] & 0xC0) == 0x80);
        CHECK(r_block(ns));
        b = t1r_last_block(&n);
        CHECK(n == saved_len && memcmp(b, saved, n) == 0);
        if (!(saved[1] & 0x20)) break;
        CHECK(r_block(ns ^ 1));
        chunks++;
    }
    CHECK_EQ(chunks, 9);
    CHECK_EQ(payload, 258);

    // Retransmission request when nothing was sent yet (first block after the ATR, or just after RESYNCH): an R-block
    // naming the expected I-block, "other error", rather than silence.  NAD 0 before the first valid block, even after
    // a session that used another NAD.
    t1r_begin(254);
    CHECK(xchg("21 00 05 00 11 00 00 00"));
    check_last("12 00 02 6D 00 7D");
    t1r_begin(254);
    fake_phy_push_hex("21 00 05 00 11 00 00 00 00"); // bad LRC
    CHECK(t1_handle_block(-1));
    check_last("00 81 00 81");
    CHECK(r_block(1));
    check_last("00 82 00 82");
    CHECK(xchg("00 40 05 00 11 00 00 00")); // I(1) where I(0) is expected
    check_last("00 82 00 82");
    CHECK(xchg("00 00 05 00 11 00 00 00"));
    check_last("00 00 02 6D 00 6F");
    t1r_resynch();
    check_last("00 E0 00 E0");
    CHECK(r_block(1));
    check_last("00 82 00 82");

    // S(IFS): any IFSD 01..FE is valid (00 and FF are reserved) and is used for the response chunks.
    t1r_begin(254);
    CHECK(xchg("00 C1 01 00"));
    check_last("00 82 00 82");
    CHECK(xchg("00 C1 01 FF"));
    check_last("00 82 00 82");
    t1r_set_ifsd(0x08);
    check_last("00 E1 01 08 E8");
    uint32_t blocks = t1r_block_count();
    CHECK(apdu("00 B0 81 00 10", &r)); // 16 bytes + SW in 8 + 8 + 2
    CHECK_EQ(r.sw, 0x9000);
    CHECK_EQ(r.len, 16);
    CHECK_EQ(t1r_block_count() - blocks, 3);
    t1r_set_ifsd(0x01);
    check_last("00 E1 01 01 E1");

    // S(RESYNCH) restores IFSD = 32.
    t1r_set_ifsd(0x40);
    check_last("00 E1 01 40 A0");
    t1r_resynch();
    check_last("00 E0 00 E0");
    blocks = t1r_block_count();
    CHECK(apdu("00 B0 81 00 00", &r));
    CHECK_EQ(r.sw, 0x9000);
    CHECK_EQ(r.len, 256);
    CHECK_EQ(t1r_block_count() - blocks, 9); // 258 bytes / 32

    // The reader's chain: a repeated I-block (it missed our R-block) and an R-block (it got a corrupted one) both get
    // the acknowledgement again, and the data is not appended twice.
    t1r_begin(254);
    CHECK(xchg("00 20 02 00 B0")); // I(0, M): first half of READ BINARY 4 bytes
    check_last("00 90 00 90");
    CHECK(xchg("00 20 02 00 B0"));
    check_last("00 90 00 90");
    CHECK(r_block(0));
    check_last("00 90 00 90");
    fake_phy_push_hex("00 40 03 81 00 04 00"); // bad LRC
    CHECK(t1_handle_block(-1));
    check_last("00 91 00 91");
    CHECK(xchg("00 40 03 81 00 04"));
    b = t1r_last_block(&n);
    CHECK(n == 4 + 6 && b[1] == 0x00 && b[2] == 6 && b[7] == 0x90 && b[8] == 0x00);

    // LEN = FF (reserved): the card waits until the reader has been silent for CWT before its single R-block, so the
    // rest of the block (up to 256 characters) is neither collided with nor parsed as further blocks.
    t1r_begin(254);
    fake_phy_push_hex("00 00 FF");
    for (int i = 0; i < 64; i++) fake_phy_push_hex("00 00 00 00"); // would each be a valid I-block
    blocks = t1r_block_count();
    CHECK(t1_handle_block(-1));
    CHECK_EQ(t1r_block_count() - blocks, 1);
    check_last("00 82 00 82");
    CHECK_EQ(fake_phy_pending(), 0);
    // The same for a bad EDC caused by a corrupted LEN that is shorter than the block.
    fake_phy_push_hex("00 00 01 AA BB CC DD EE 00");
    blocks = t1r_block_count();
    CHECK(t1_handle_block(-1));
    CHECK_EQ(t1r_block_count() - blocks, 1);
    check_last("00 81 00 81");
    CHECK_EQ(fake_phy_pending(), 0);
    CHECK(xchg("00 00 05 00 11 00 00 00"));
    check_last("00 00 02 6D 00 6F");

    // An S(... response) that the card did not ask for is an invalid block: R-block instead of silence.
    CHECK(xchg("00 E2 00"));
    check_last("00 92 00 92");
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
    CHECK_EQ(t1r_block_count() - blocks, 9); // 258 bytes / 32
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

    // A command chain larger than the APDU buffer: every block is still acknowledged with an R-block (no card-initiated
    // S(ABORT), which many TPDU-level readers do not handle), the end of the chain is answered with 6700, and the next
    // command works normally.
    static uint8_t huge[7 + APDU_BUF_SIZE + 100];
    memcpy(huge, "\x80\x20\x00\x00\x00\x10\x64", 7);
    blocks = t1r_block_count();
    CHECK(t1r_apdu(huge, sizeof huge, &r)); // the model checks each R(N(R)) acknowledgement
    CHECK_EQ(r.sw, 0x6700);
    CHECK_EQ(r.len, 0);
    CHECK_EQ(t1r_block_count() - blocks, (sizeof huge + 253) / 254);
    CHECK(apdu("00 B0 81 00 04", &r));
    CHECK_EQ(r.sw, 0x9000);
    CHECK_EQ(r.len, 4);
    CHECK(xchg("00 C0 00"));
    check_last("00 E0 00 E0");
    CHECK(xchg("00 00 05 00 11 00 00 00"));
    check_last("00 00 02 6D 00 6F");

    test_t1_errors();
}
