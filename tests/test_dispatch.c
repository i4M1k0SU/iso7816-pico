#include "apdu/dispatch.h"
#include "fake_phy.h"
#include "fakes.h"
#include "iso7816_pico/sec.h"
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

void test_dispatch(void) {
    g_fake_protocol = 0;
    dispatch_reset();
    t0_session_begin();
    t0r_begin();

    // The default applet is selected implicitly *with* its select(): demo_select made EF 0101 current, so READ BINARY
    // without a SELECT works right after the ATR.
    CHECK_EQ(sw("00 B0 00 00 10"), 0x9000);
    CHECK_EQ(r.len, 16);

    // Command chaining must keep CLA/INS/P1/P2; a different command in the middle is refused.
    CHECK_EQ(sw("10 D6 00 00 02 41 42"), 0x9000);
    CHECK_EQ(sw("00 B0 00 00 05"), 0x6883);
    CHECK_EQ(sw("00 B0 81 00 05"), 0x9000); // the chain state was discarded (EF 0101 via SFI)

    // Logical channels: closed channel, explicit open, close, basic channel cannot close.  Opened from the basic
    // channel, a channel starts with the default applet (its select() makes EF 0101 current), not with channel 0's EF.
    CHECK_EQ(sw("02 B0 00 00 05"), 0x6881);
    CHECK_EQ(sw("00 A4 02 0C 02 01 02"), 0x9000); // channel 0: record EF 0102
    CHECK_EQ(sw("00 B0 00 00 05"), 0x6981);
    CHECK_EQ(sw("00 70 00 02 00"), 0x9000);
    CHECK_EQ(r.len, 0);
    CHECK_EQ(sw("02 B0 00 00 05"), 0x9000); // EF 0101 from demo_select
    // Opened from another channel, it gets that channel's applet and current file.
    CHECK_EQ(sw("02 A4 02 0C 02 01 02"), 0x9000);
    CHECK_EQ(sw("02 70 00 03 00"), 0x9000);
    CHECK_EQ(sw("03 B0 00 00 05"), 0x6981); // EF 0102 copied from channel 2
    CHECK_EQ(sw("03 B2 01 04 05"), 0x9000);
    CHECK_EQ(sw("00 70 80 03 00"), 0x9000);
    CHECK_EQ(sw("00 70 00 02 00"), 0x6A86); // already open
    CHECK_EQ(sw("00 70 80 02 00"), 0x9000);
    CHECK_EQ(sw("02 B0 00 00 05"), 0x6881);
    CHECK_EQ(sw("00 70 80 00 00"), 0x6A86);
    CHECK_EQ(sw("00 70 00 00 01"), 0x9000); // assigns 1
    CHECK_EQ(r.data[0], 1);
    CHECK_EQ(sw("00 70 00 00 01"), 0x9000); // 2
    CHECK_EQ(sw("00 70 00 00 01"), 0x9000); // 3
    CHECK_EQ(sw("00 70 00 00 01"), 0x6881); // none left
    CHECK_EQ(sw("40 B0 00 00 05"), 0x6881); // extended coding: channel 4 does not exist here
    CHECK_EQ(sw("00 70 80 01 00"), 0x9000);
    CHECK_EQ(sw("00 70 80 02 00"), 0x9000);
    CHECK_EQ(sw("00 70 80 03 00"), 0x9000);

    // Secure messaging bits without a hook.
    CHECK_EQ(sw("0C A4 04 00 05 F0 50 49 43 4F"), 0x6882);

    // Security status is per channel and forgotten at reset; the retry counter is not.
    CHECK_EQ(sw("00 20 00 01 04 30 30 30 30"), 0x63C2);
    CHECK_EQ(sw("00 20 00 01 04 31 32 33 34"), 0x9000);
    CHECK(sec_is_verified(0, 0x01));
    CHECK(!sec_is_verified(1, 0x01));
    CHECK_EQ(sw("00 70 00 00 01"), 0x9000);
    CHECK_EQ(sw("01 20 00 01 00"), 0x63C3);
    CHECK_EQ(sw("00 70 80 01 00"), 0x9000);
    CHECK_EQ(sw("00 20 00 01 04 30 30 30 30"), 0x63C2); // one wrong try, counter now 2
    dispatch_reset();
    t0_session_begin();
    CHECK(!sec_is_verified(0, 0x01));
    CHECK_EQ(sw("00 20 00 01 00"), 0x63C2);
    CHECK_EQ(sw("00 20 00 01 04 31 32 33 34"), 0x9000); // back to 3

    // Unknown reference data, wrong P1.
    CHECK_EQ(sw("00 20 00 05 04 31 32 33 34"), 0x6A88);
    CHECK_EQ(sw("00 20 01 01 04 31 32 33 34"), 0x6A86);

    // The default applet is selected after reset, with its DF current: SFI access works at once.
    CHECK_EQ(sw("80 10 00 00 00"), 0x9000);
    CHECK_EQ(sw("00 B0 81 00 05"), 0x9000);
    CHECK_EQ(r.len, 5);
    fake_phy_clear();
}
