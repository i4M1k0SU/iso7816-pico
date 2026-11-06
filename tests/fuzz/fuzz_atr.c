// atr_parse() on arbitrary bytes, and atr_build() -> atr_parse() on a description made from the same bytes: what the
// parser reads back must be what the description says.
#include "iso7816_pico/atr.h"

#include <stdlib.h>
#include <string.h>

// Description from the input: byte 0 = inverse (bit 7) and levels (bits 1..0, + 1); per level present / TA / TB / TC /
// protocol; then the historical bytes.  Missing bytes are 0.
static size_t take(const uint8_t *data, size_t size, size_t *pos, uint8_t *out, size_t n) {
    size_t got = 0;
    for (; got < n && *pos < size; got++) out[got] = data[(*pos)++];
    for (size_t i = got; i < n; i++) out[i] = 0;
    return got;
}

static void check_round_trip(const uint8_t *data, size_t size) {
    size_t pos = 0;
    uint8_t b[5];
    atr_desc_t d;
    memset(&d, 0, sizeof d);
    take(data, size, &pos, b, 1);
    d.inverse = b[0] & 0x80;
    d.levels = (uint8_t)((b[0] & 3) + 1);
    for (int i = 0; i < 4; i++) {
        take(data, size, &pos, b, 5);
        d.level[i].present = b[0] & 7;
        d.level[i].ta = b[1];
        d.level[i].tb = b[2];
        d.level[i].tc = b[3];
        d.level[i].protocol = b[4] & 0xF;
    }
    d.historical_len = (uint8_t)take(data, size, &pos, d.historical, 15);

    uint8_t atr[ATR_MAX_LEN];
    size_t n = atr_build(&d, atr, sizeof atr);
    if (n == 0 || n > ATR_MAX_LEN) abort(); // every description above is valid and fits
    if (atr_build(&d, atr, n - 1) != 0) abort();

    atr_info_t info;
    if (!atr_parse(atr, n, &info) || !info.valid) abort();
    if (info.inverse != d.inverse) abort();
    if (atr[0] != (d.inverse ? 0x3F : 0x3B) || (atr[1] & 0xF) != d.historical_len) abort();
    if (memcmp(&atr[n - d.historical_len - (info.protocols != 1 ? 1 : 0)], d.historical, d.historical_len) != 0)
        abort();

    // Protocols: one bit per TD(i), T=0 alone without any TD.
    uint16_t protocols = 0;
    for (int i = 1; i < d.levels; i++) protocols |= (uint16_t)(1u << d.level[i].protocol);
    if (d.levels == 1) protocols = 1;
    if (info.protocols != protocols) abort();
    if (d.levels > 1 && info.first_protocol != d.level[1].protocol) abort();

    // TC1 = N; TA2 = specific mode; TC2 = WI.
    if (info.n != ((d.level[0].present & ATR_TC) ? d.level[0].tc : 0)) abort();
    bool specific = d.levels > 1 && (d.level[1].present & ATR_TA);
    if (info.specific_mode != specific) abort();
    if (specific && info.specific_protocol != (d.level[1].ta & 0xF)) abort();
    if (info.wi != ((d.levels > 1 && (d.level[1].present & ATR_TC)) ? d.level[1].tc : 10)) abort();

    // F / D from TA1 when it codes a defined pair, unless TA2 asks for the implicit values.
    uint16_t f = 372;
    uint8_t dd = 1;
    if ((d.level[0].present & ATR_TA) && atr_fi(d.level[0].ta >> 4) && atr_di(d.level[0].ta & 0xF)) {
        f = atr_fi(d.level[0].ta >> 4);
        dd = atr_di(d.level[0].ta & 0xF);
    }
    if (specific && (d.level[1].ta & 0x10)) {
        f = 372;
        dd = 1;
    }
    if (info.f != f || info.d != dd) abort();

    // T=1 parameters: from the first level >= 3 that T=1 introduces.
    uint8_t ifsc = 32, cwi = 13, bwi = 4;
    bool crc = false;
    for (int i = 2; i < d.levels; i++) {
        if (d.level[i].protocol != 1) continue;
        const atr_level_t *lv = &d.level[i];
        if (lv->present & ATR_TA) ifsc = lv->ta;
        if (lv->present & ATR_TB) {
            cwi = lv->tb & 0xF;
            bwi = lv->tb >> 4;
        }
        if (lv->present & ATR_TC) crc = lv->tc & 1;
        break;
    }
    if (info.ifsc != ifsc || info.cwi != cwi || info.bwi != bwi || info.crc != crc) abort();

    // Any truncation of a valid ATR is rejected (the historical bytes and the TCK are counted by the parser).
    if (atr_parse(atr, n - 1, &info)) abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 256) return 0;
    // Raw parse: only memory safety and self-consistency.
    atr_info_t info;
    if (atr_parse(data, size, &info)) {
        if (!info.valid || !(info.protocols & (1u << info.first_protocol)) || !info.f || !info.d) abort();
    }
    check_round_trip(data, size);
    return 0;
}
