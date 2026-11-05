#include "apdu.h"

#include "card/card.h"

// Defined here so that every build that has the core (firmware and host tests) has exactly one copy.
const card_profile_t *card_profile;

size_t tlv_len_size(size_t n) { return n < 128 ? 1 : n < 256 ? 2 : 3; }

void resp_put_tlv(apdu_resp_t *r, uint8_t tag, const void *val, size_t n) {
    resp_put_u8(r, tag);
    if (n < 128) {
        resp_put_u8(r, (uint8_t)n);
    } else if (n < 256) {
        resp_put_u8(r, 0x81);
        resp_put_u8(r, (uint8_t)n);
    } else {
        resp_put_u8(r, 0x82);
        resp_put_u8(r, (uint8_t)(n >> 8));
        resp_put_u8(r, (uint8_t)n);
    }
    resp_put(r, val, n);
}

// Decode a complete command APDU (short or extended length, ISO 7816-4 5.1).  `data` in *out points into buf.  Returns
// false if the length fields do not add up.
bool apdu_decode(const uint8_t *buf, size_t len, apdu_t *out) {
    if (len < 4) return false;
    out->cla = buf[0];
    out->ins = buf[1];
    out->p1 = buf[2];
    out->p2 = buf[3];
    out->data = NULL;
    out->lc = 0;
    out->le = 0;
    out->le_present = false;
    out->extended = false;

    if (len == 4) return true;                                   // case 1
    if (len == 5) {                                              // case 2S
        out->le = buf[4] ? buf[4] : 256;
        out->le_present = true;
        return true;
    }
    if (buf[4] != 0) {                                           // short Lc
        uint32_t lc = buf[4];
        if (len == 5 + lc) {                                     // case 3S
            out->lc = lc;
            out->data = &buf[5];
            return true;
        }
        if (len == 6 + lc) {                                     // case 4S
            out->lc = lc;
            out->data = &buf[5];
            out->le = buf[5 + lc] ? buf[5 + lc] : 256;
            out->le_present = true;
            return true;
        }
        return false;
    }
    out->extended = true;
    if (len == 7) {                                              // case 2E
        out->le = ((uint32_t)buf[5] << 8) | buf[6];
        if (!out->le) out->le = 65536;
        out->le_present = true;
        return true;
    }
    if (len < 7) return false;
    uint32_t lc = ((uint32_t)buf[5] << 8) | buf[6];
    if (!lc) return false;
    if (len == 7 + lc) {                                         // case 3E
        out->lc = lc;
        out->data = &buf[7];
        return true;
    }
    if (len == 9 + lc) {                                         // case 4E
        out->lc = lc;
        out->data = &buf[7];
        out->le = ((uint32_t)buf[7 + lc] << 8) | buf[8 + lc];
        if (!out->le) out->le = 65536;
        out->le_present = true;
        return true;
    }
    return false;
}
