#include "iso7816_pico/card.h"
#include "fake_phy.h"
#include "fakes.h"
#include "proto/session.h"
#include "test.h"

void test_session(void) {
    size_t n;
    const uint8_t *tx;
    fake_phy_set_tx_hook(NULL);
    CHECK(session_init());
    const uint8_t *atr = session_atr(&n);
    CHECK_HEX(atr, n, "3B 08 50 69 63 6F 37 38 31 36");

    // ATR, then a PPS to F/D = 372/4, then one command (unknown INS -> 6D00), then the reader goes away.
    fake_phy_clear();
    uint32_t applied = fake_phy_apply_count();
    fake_phy_push_hex("FF 10 13 FC  00 11 00 00 00");
    session_run();
    tx = fake_phy_tx_log(&n);
    CHECK_HEX(tx, n, "3B 08 50 69 63 6F 37 38 31 36  FF 10 13 FC  6D 00");
    CHECK_EQ(fake_phy_apply_count(), applied + 1);
    CHECK_EQ(phy_params()->d, 4);
    CHECK_EQ(session_protocol(), 0);

    // PPS asking for T=1, which this ATR does not offer: the card stays mute until the reset.
    fake_phy_clear();
    applied = fake_phy_apply_count();
    fake_phy_push_hex("FF 11 13 FD  00 11 00 00 00");
    session_run();
    tx = fake_phy_tx_log(&n);
    CHECK_HEX(tx, n, "3B 08 50 69 63 6F 37 38 31 36");
    CHECK_EQ(fake_phy_apply_count(), applied);

    // A PPS after the first command is a protocol error (CLA FF -> 6E00), not a renegotiation.
    fake_phy_clear();
    fake_phy_push_hex("00 11 00 00 00  FF 10 13 FC 00");
    session_run();
    tx = fake_phy_tx_log(&n);
    CHECK_HEX(tx, n, "3B 08 50 69 63 6F 37 38 31 36  6D 00  6E 00");

    // Only one PPS: a second request after a successful one is a command header as well (CLA FF -> 6E00).
    fake_phy_clear();
    applied = fake_phy_apply_count();
    fake_phy_push_hex("FF 10 13 FC  FF 10 13 FC 00");
    session_run();
    tx = fake_phy_tx_log(&n);
    CHECK_HEX(tx, n, "3B 08 50 69 63 6F 37 38 31 36  FF 10 13 FC  6E 00");
    CHECK_EQ(fake_phy_apply_count(), applied + 1);

    // The REBOOT BOOTSEL command ends the session with the flag set, after the 90 00 went out.
    fake_phy_clear();
    fake_phy_push_hex("80 E0 00 00 00  00 11 00 00 00");
    session_run();
    tx = fake_phy_tx_log(&n);
    CHECK_HEX(tx, n, "3B 08 50 69 63 6F 37 38 31 36  90 00");
    CHECK(card_bootsel_requested());
    CHECK_EQ(fake_phy_pending(), 5);     // the second command was never read
    fake_clear_bootsel();
    fake_phy_clear();

    // Specific mode (TA2 = 00: T=0, F/D from TA1 = 13 -> 372/4): the ATR goes out at 372/1, then the card switches
    // to D=4 by itself, without any PPS.
    static const uint8_t specific[] = { 0x3B, 0x90, 0x13, 0x10, 0x00 };
    CHECK(session_init_raw(specific, sizeof specific));
    fake_phy_clear();
    applied = fake_phy_apply_count();
    fake_phy_push_hex("00 11 00 00 00");
    session_run();
    tx = fake_phy_tx_log(&n);
    CHECK_HEX(tx, n, "3B 90 13 10 00  6D 00");
    CHECK_EQ(fake_phy_apply_count(), applied + 1);
    CHECK_EQ(phy_params()->f, 372);
    CHECK_EQ(phy_params()->d, 4);
    CHECK_EQ(session_protocol(), 0);

    // ... and a PPS request is not negotiated in specific mode: FF 10 13 FC 00 is a command header (CLA FF -> 6E00).
    fake_phy_clear();
    applied = fake_phy_apply_count();
    fake_phy_push_hex("FF 10 13 FC 00");
    session_run();
    tx = fake_phy_tx_log(&n);
    CHECK_HEX(tx, n, "3B 90 13 10 00  6E 00");
    CHECK_EQ(fake_phy_apply_count(), applied + 1);   // only the specific-mode switch
    CHECK_EQ(phy_params()->d, 4);

    // TA2 = 01 names T=1 although TD1 offers T=0 first: the session runs T=1.
    static const uint8_t specific_t1[] = { 0x3B, 0x90, 0x13, 0x90, 0x01, 0x01, 0x13 };
    CHECK(session_init_raw(specific_t1, sizeof specific_t1));
    fake_phy_clear();
    fake_phy_push_hex("00 00 05 00 11 00 00 00 14");
    session_run();
    tx = fake_phy_tx_log(&n);
    CHECK_HEX(tx, n, "3B 90 13 90 01 01 13  00 00 02 6D 00 6F");
    CHECK_EQ(session_protocol(), 1);
    CHECK_EQ(phy_params()->d, 4);

    // A broken ATR is refused and the previous one stays.
    static const uint8_t truncated[] = { 0x3B, 0x90, 0x13 };
    CHECK(!session_init_raw(truncated, sizeof truncated));
    atr = session_atr(&n);
    CHECK_HEX(atr, n, "3B 90 13 90 01 01 13");

    // Back to the card profile's ATR for the groups that follow.
    CHECK(session_init());
    atr = session_atr(&n);
    CHECK_HEX(atr, n, "3B 08 50 69 63 6F 37 38 31 36");
    fake_phy_clear();
}
