#include "atr.h"

#include <string.h>

static const uint16_t s_fi[16] = { 372, 372, 558, 744, 1116, 1488, 1860, 0,
                                   0, 512, 768, 1024, 1536, 2048, 0, 0 };
static const uint8_t s_di[16] = { 0, 1, 2, 4, 8, 16, 32, 64, 12, 20, 0, 0, 0, 0, 0, 0 };

uint16_t atr_fi(uint8_t code) { return s_fi[code & 0xF]; }
uint8_t atr_di(uint8_t code) { return s_di[code & 0xF]; }

size_t atr_build(const atr_desc_t *d, uint8_t *out, size_t cap) {
    if (!d || d->levels < 1 || d->levels > 4 || d->historical_len > 15) return 0;
    uint8_t buf[ATR_MAX_LEN];
    size_t n = 0;
    bool need_tck = false;

    buf[n++] = d->inverse ? 0x3F : 0x3B;
    // T0: Y1 (presence of TA1..TD1) and K.
    uint8_t y1 = d->level[0].present & 7;
    if (d->levels > 1) y1 |= 0x8;
    buf[n++] = (uint8_t)((y1 << 4) | d->historical_len);

    for (uint8_t i = 0; i < d->levels; i++) {
        const atr_level_t *lv = &d->level[i];
        if (lv->present & ATR_TA) buf[n++] = lv->ta;
        if (lv->present & ATR_TB) buf[n++] = lv->tb;
        if (lv->present & ATR_TC) buf[n++] = lv->tc;
        if (i + 1 < d->levels) {
            const atr_level_t *next = &d->level[i + 1];
            uint8_t y = next->present & 7;
            if (i + 2 < d->levels) y |= 0x8;
            buf[n++] = (uint8_t)((y << 4) | (next->protocol & 0xF));
            if ((next->protocol & 0xF) != 0) need_tck = true;
        }
    }
    memcpy(&buf[n], d->historical, d->historical_len);
    n += d->historical_len;
    if (need_tck) {
        uint8_t tck = 0;
        for (size_t i = 1; i < n; i++) tck ^= buf[i];
        buf[n++] = tck;
    }
    if (n > cap) return 0;
    memcpy(out, buf, n);
    return n;
}

bool atr_parse(const uint8_t *atr, size_t len, atr_info_t *info) {
    memset(info, 0, sizeof *info);
    info->f = 372;
    info->d = 1;
    info->wi = 10;
    info->ifsc = 32;
    info->cwi = 13;
    info->bwi = 4;
    if (len < 2) return false;
    info->inverse = atr[0] == 0x3F;

    size_t i = 2;
    uint8_t y = atr[1] >> 4;
    uint8_t k = atr[1] & 0xF;
    uint8_t level = 1;
    uint8_t proto = 0;          // protocol introducing the current level
    bool t1_params_done = false;
    info->protocols = 0;
    bool first_td = true;

    while (y) {
        uint8_t ta = 0, tb = 0, tc = 0, td = 0;
        bool has_ta = y & 1, has_tb = y & 2, has_tc = y & 4, has_td = y & 8;
        if (has_ta) { if (i >= len) return false; ta = atr[i++]; }
        if (has_tb) { if (i >= len) return false; tb = atr[i++]; }
        if (has_tc) { if (i >= len) return false; tc = atr[i++]; }
        if (has_td) { if (i >= len) return false; td = atr[i++]; }

        if (level == 1) {
            if (has_ta) {
                uint16_t f = atr_fi(ta >> 4);
                uint8_t d = atr_di(ta & 0xF);
                if (f && d) { info->f = f; info->d = d; }
            }
            if (has_tc) info->n = tc;
        } else if (level == 2) {
            if (has_ta) {
                info->specific_mode = true;
                if (!(ta & 0x10)) { /* F/D from TA1 */ } else { info->f = 372; info->d = 1; }
            }
            if (has_tc) info->wi = tc;
        } else if (proto == 1 && !t1_params_done) {
            if (has_ta) info->ifsc = ta;
            if (has_tb) { info->cwi = tb & 0xF; info->bwi = tb >> 4; }
            if (has_tc) info->crc = tc & 1;
            t1_params_done = true;
        }

        if (has_td) {
            uint8_t t = td & 0xF;
            if (first_td) { info->first_protocol = t; first_td = false; }
            info->protocols |= (uint16_t)(1u << t);
            proto = t;
            y = td >> 4;
        } else {
            y = 0;
        }
        level++;
    }
    if (first_td) info->protocols = 1;   // no TD1: T=0 only
    if (i + k > len) return false;
    // TCK present unless only T=0 is offered.
    if (info->protocols != 1) {
        if (i + k + 1 > len) return false;
        uint8_t tck = 0;
        for (size_t j = 1; j < i + k + 1; j++) tck ^= atr[j];
        if (tck != 0) return false;
    }
    info->valid = true;
    return true;
}
