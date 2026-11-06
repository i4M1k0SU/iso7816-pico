#include "iso7816_pico/apdu.h"
#include "iso7816_pico/card.h"
#include "test.h"

static bool decode(const char *hex, apdu_t *a) {
    static uint8_t buf[8192];
    size_t n = hex_parse(hex, buf, sizeof buf);
    return apdu_decode(buf, n, a);
}

void test_apdu(void) {
    apdu_t a;

    CHECK(decode("00 A4 04 00", &a));                           // case 1
    CHECK_EQ(a.lc, 0);
    CHECK(!a.le_present);

    CHECK(decode("00 B0 00 00 10", &a));                        // case 2S
    CHECK_EQ(a.le, 0x10);
    CHECK(a.le_present && !a.extended);
    CHECK(decode("00 B0 00 00 00", &a));
    CHECK_EQ(a.le, 256);

    CHECK(decode("00 20 00 01 04 31 32 33 34", &a));            // case 3S
    CHECK_EQ(a.lc, 4);
    CHECK_EQ(a.data[3], 0x34);
    CHECK(!a.le_present);

    CHECK(decode("00 A4 04 00 02 11 22 00", &a));               // case 4S, Le = 00 -> 256
    CHECK_EQ(a.lc, 2);
    CHECK_EQ(a.le, 256);

    CHECK(decode("00 B0 00 00 00 01 00", &a));                  // case 2E
    CHECK(a.extended && a.le_present);
    CHECK_EQ(a.le, 256);
    CHECK(decode("00 B0 00 00 00 00 00", &a));
    CHECK_EQ(a.le, 65536);

    CHECK(decode("80 20 00 00 00 00 03 01 02 03", &a));         // case 3E
    CHECK_EQ(a.lc, 3);
    CHECK(!a.le_present);

    CHECK(decode("80 20 00 00 00 00 03 01 02 03 01 00", &a));   // case 4E
    CHECK_EQ(a.lc, 3);
    CHECK_EQ(a.le, 256);

    // Length fields that do not add up.
    CHECK(!decode("00 A4 04", &a));
    CHECK(!decode("00 A4 04 00 05 01 02", &a));
    CHECK(!decode("00 A4 04 00 02 01 02 03 04", &a));
    CHECK(!decode("00 A4 04 00 00 00 00 01", &a));             // extended Lc = 0
    CHECK(!decode("00 A4 04 00 00 00 02 01 02 03", &a));

    // TLV writer length encodings.
    uint8_t buf[600];
    apdu_resp_t r = APDU_RESP_INIT(buf, sizeof buf);
    uint8_t v[300];
    memset(v, 0xAA, sizeof v);
    resp_put_tlv(&r, 0x84, v, 127);
    CHECK_EQ(r.len, 2 + 127);
    CHECK_EQ(buf[1], 127);
    r.len = 0;
    resp_put_tlv(&r, 0x84, v, 128);
    CHECK_EQ(buf[1], 0x81);
    CHECK_EQ(buf[2], 128);
    r.len = 0;
    resp_put_tlv(&r, 0x84, v, 300);
    CHECK_EQ(buf[1], 0x82);
    CHECK_EQ(buf[2], 0x01);
    CHECK_EQ(buf[3], 0x2C);
    CHECK_EQ(r.len, 4 + 300);

    CHECK(!r.overflow);

    // resp_put never overruns the buffer, and says so when it had to drop bytes.
    apdu_resp_t small = APDU_RESP_INIT(buf, 10);
    resp_put(&small, v, 9);
    CHECK(!small.overflow);
    resp_put_u8(&small, 1);
    CHECK_EQ(small.len, 10);
    CHECK(!small.overflow);                                     // exactly full is fine
    resp_put(&small, v, 0);
    CHECK(!small.overflow);
    resp_put_u8(&small, 2);
    CHECK(small.overflow);
    CHECK_EQ(small.len, 10);
    resp_reset(&small);
    CHECK(!small.overflow);
    CHECK_EQ(small.len, 0);
    resp_put(&small, v, 300);
    CHECK_EQ(small.len, 10);
    CHECK(small.overflow);
    resp_put_u8(&small, 3);                                     // sticky
    CHECK(small.overflow);
    resp_reset(&small);
    resp_put_tlv(&small, 0x84, v, 9);                           // 2 + 9 bytes
    CHECK(small.overflow);
    CHECK_EQ(small.len, 10);

    // The default FCI: 6F (2 + n) { 84 n AID } up to the ISO limit of 16 bytes, refused beyond.
    uint8_t aid[17];
    for (int i = 0; i < 17; i++) aid[i] = (uint8_t)(0xA0 + i);
    apdu_resp_t f = APDU_RESP_INIT(buf, sizeof buf);
    card_put_default_fci(&f, aid, 5);
    CHECK_HEX(buf, f.len, "6F 07 84 05 A0 A1 A2 A3 A4");
    resp_reset(&f);
    card_put_default_fci(&f, aid, 16);
    CHECK_EQ(f.len, 20);
    CHECK_EQ(buf[1], 18);
    CHECK_EQ(buf[3], 16);
    CHECK_EQ(buf[19], 0xAF);
    CHECK(!f.overflow);
    resp_reset(&f);
    card_put_default_fci(&f, aid, 17);
    CHECK_EQ(f.len, 0);
    CHECK(f.overflow);
}
