#include "fake_phy.h"
#include "proto/pps.h"
#include "test.h"

static bool refuse_all(uint16_t f, uint8_t d, uint8_t t) {
    (void)f;
    (void)d;
    (void)t;
    return false;
}

void test_pps(void) {
    phy_params_t p = PHY_PARAMS_DEFAULT;
    pps_result_t r;
    size_t n;
    const uint8_t *tx;
    fake_phy_set_tx_hook(NULL);

    // PPS1 = 13 (F=372, D=4) accepted: the request is echoed.
    fake_phy_clear();
    fake_phy_push_hex("10 13 FC");
    CHECK(pps_handle(&p, 0x0001, NULL, &r));
    tx = fake_phy_tx_log(&n);
    CHECK_HEX(tx, n, "FF 10 13 FC");
    CHECK(r.fd_changed);
    CHECK_EQ(r.f, 372);
    CHECK_EQ(r.d, 4);
    CHECK_EQ(r.protocol, 0);

    // PPS1 with an RFU Di: PPS1 is not echoed, defaults stay.
    fake_phy_clear();
    fake_phy_push_hex("10 1B F4");
    CHECK(pps_handle(&p, 0x0001, NULL, &r));
    tx = fake_phy_tx_log(&n);
    CHECK_HEX(tx, n, "FF 00 FF");
    CHECK(!r.fd_changed);
    CHECK_EQ(r.d, 1);

    // The profile's accept callback can refuse a supported value.
    fake_phy_clear();
    fake_phy_push_hex("10 13 FC");
    CHECK(pps_handle(&p, 0x0001, refuse_all, &r));
    tx = fake_phy_tx_log(&n);
    CHECK_HEX(tx, n, "FF 00 FF");
    CHECK(!r.fd_changed);

    // Protocol only (no PPS1).
    fake_phy_clear();
    fake_phy_push_hex("01 FE");
    CHECK(pps_handle(&p, 0x0003, NULL, &r));
    tx = fake_phy_tx_log(&n);
    CHECK_HEX(tx, n, "FF 01 FE");
    CHECK_EQ(r.protocol, 1);

    // T=1 requested but only T=0 offered: stay mute.
    fake_phy_clear();
    fake_phy_push_hex("11 13 FD");
    CHECK(!pps_handle(&p, 0x0001, NULL, &r));
    fake_phy_tx_log(&n);
    CHECK_EQ(n, 0);

    // Bad PCK: ignored, nothing sent.
    fake_phy_clear();
    fake_phy_push_hex("10 13 00");
    CHECK(!pps_handle(&p, 0x0001, NULL, &r));
    fake_phy_tx_log(&n);
    CHECK_EQ(n, 0);

    // PPS2 / PPS3 present: never accepted, not echoed.
    fake_phy_clear();
    fake_phy_push_hex("70 13 01 02 9F");
    CHECK(pps_handle(&p, 0x0001, NULL, &r));
    tx = fake_phy_tx_log(&n);
    CHECK_HEX(tx, n, "FF 10 13 FC");
}
