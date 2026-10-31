#include "proto/atr.h"
#include "test.h"

static const uint8_t HIST[8] = { 'P', 'i', 'c', 'o', '7', '8', '1', '6' };

static atr_desc_t base(void) {
    atr_desc_t d = { 0 };
    d.levels = 1;
    memcpy(d.historical, HIST, 8);
    d.historical_len = 8;
    return d;
}

void test_atr(void) {
    uint8_t out[ATR_MAX_LEN];
    atr_info_t info;

    // T=0 only, default parameters.
    atr_desc_t d = base();
    size_t n = atr_build(&d, out, sizeof out);
    CHECK_HEX(out, n, "3B 08 50 69 63 6F 37 38 31 36");
    CHECK(atr_parse(out, n, &info));
    CHECK_EQ(info.protocols, 0x0001);
    CHECK_EQ(info.first_protocol, 0);
    CHECK_EQ(info.f, 372);
    CHECK_EQ(info.d, 1);
    CHECK(!info.specific_mode);
    CHECK(!info.inverse);

    // T=1 only with IFSC / CWI / BWI, TCK appended.
    d = base();
    d.levels = 3;
    d.level[1].protocol = 1;
    d.level[2].present = ATR_TA | ATR_TB;
    d.level[2].ta = 0xFE;
    d.level[2].tb = 0x45;
    d.level[2].protocol = 1;
    n = atr_build(&d, out, sizeof out);
    CHECK_HEX(out, n, "3B 88 81 31 FE 45 50 69 63 6F 37 38 31 36 BE");
    CHECK(atr_parse(out, n, &info));
    CHECK_EQ(info.protocols, 0x0002);
    CHECK_EQ(info.first_protocol, 1);
    CHECK_EQ(info.ifsc, 254);
    CHECK_EQ(info.cwi, 5);
    CHECK_EQ(info.bwi, 4);
    CHECK(!info.crc);

    // T=0 first, T=1 offered, TA1 = 13 (F=372, D=4).
    d = base();
    d.levels = 3;
    d.level[0].present = ATR_TA;
    d.level[0].ta = 0x13;
    d.level[1].protocol = 0;
    d.level[2].present = ATR_TA | ATR_TB;
    d.level[2].ta = 0xFE;
    d.level[2].tb = 0x45;
    d.level[2].protocol = 1;
    n = atr_build(&d, out, sizeof out);
    CHECK_HEX(out, n, "3B 98 13 80 31 FE 45 50 69 63 6F 37 38 31 36 BC");
    CHECK(atr_parse(out, n, &info));
    CHECK_EQ(info.protocols, 0x0003);
    CHECK_EQ(info.first_protocol, 0);
    CHECK_EQ(info.f, 372);
    CHECK_EQ(info.d, 4);

    // Inverse convention and TC1 (extra guard time).
    d = base();
    d.inverse = true;
    d.level[0].present = ATR_TC;
    d.level[0].tc = 0xFF;
    n = atr_build(&d, out, sizeof out);
    CHECK_EQ(out[0], 0x3F);
    CHECK_EQ(out[1], 0x48);
    CHECK_EQ(out[2], 0xFF);
    CHECK(atr_parse(out, n, &info));
    CHECK(info.inverse);
    CHECK_EQ(info.n, 0xFF);

    // Specific mode (TA2) from a raw ATR: 3B 90 11 10 00.
    static const uint8_t specific[] = { 0x3B, 0x90, 0x11, 0x10, 0x00 };
    CHECK(atr_parse(specific, sizeof specific, &info));
    CHECK(info.specific_mode);
    CHECK_EQ(info.protocols, 0x0001);

    // Broken ATRs.
    static const uint8_t bad_tck[] = { 0x3B, 0x88, 0x81, 0x31, 0xFE, 0x45, 'P', 'i', 'c', 'o', '7', '8', '1', '6',
                                       0x00 };
    CHECK(!atr_parse(bad_tck, sizeof bad_tck, &info));
    CHECK(!atr_parse(bad_tck, 6, &info));           // truncated
    CHECK(!atr_parse(bad_tck, 1, &info));
    d = base();
    d.levels = 0;
    CHECK_EQ(atr_build(&d, out, sizeof out), 0);
    d = base();
    d.historical_len = 16;
    CHECK_EQ(atr_build(&d, out, sizeof out), 0);
    d = base();
    CHECK_EQ(atr_build(&d, out, 4), 0);              // buffer too small

    // Fi / Di tables.
    CHECK_EQ(atr_fi(0x1), 372);
    CHECK_EQ(atr_fi(0x9), 512);
    CHECK_EQ(atr_fi(0x7), 0);
    CHECK_EQ(atr_di(0x3), 4);
    CHECK_EQ(atr_di(0x8), 12);
    CHECK_EQ(atr_di(0x0), 0);
}
