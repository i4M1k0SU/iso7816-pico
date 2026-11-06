// pps_handle() fed through the fake PHY.  Byte 0 chooses the protocols the ATR offers (bit t = T=t for t = 0..6) and
// bit 7 a pps_accept hook that refuses half of the F/D pairs, byte 1 the current F/D (TA1 coding), byte 2 bit 0
// whether the hook is passed at all; the rest is what the reader sends after PPSS.  An accepted request must be
// answered with a well-formed PPS response that only grants what was asked.
#include "fake_phy.h"
#include "fuzz_common.h"
#include "iso7816_pico/atr.h"
#include "proto/pps.h"

#include <stdlib.h>

static bool s_accept_odd;

static bool accept(uint16_t f, uint8_t d, uint8_t t) {
    (void)t;
    return !s_accept_odd || ((f ^ d) & 1) == 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 3 || size > FUZZ_MAX_INPUT) return 0;
    fuzz_card_reset(0, false, 32);
    uint16_t offered = data[0] & 0x7F;
    s_accept_odd = data[0] & 0x80;
    phy_params_t cur = PHY_PARAMS_DEFAULT;
    cur.f = atr_fi(data[1] >> 4) ? atr_fi(data[1] >> 4) : 372;
    cur.d = atr_di(data[1] & 0xF) ? atr_di(data[1] & 0xF) : 1;
    bool with_hook = data[2] & 1;
    fuzz_phy_push(data + 3, size - 3);

    pps_result_t r;
    bool ok = pps_handle(&cur, offered, with_hook ? accept : NULL, &r);
    size_t n;
    const uint8_t *tx = fake_phy_tx_log(&n);
    if (!ok) {
        if (n) abort(); // a refused request is never answered (the card stays mute)
        return 0;
    }
    // PPSS PPS0 [PPS1] PCK
    if (n < 3 || n > 4 || tx[0] != 0xFF) abort();
    uint8_t x = 0;
    for (size_t i = 0; i < n; i++) x ^= tx[i];
    if (x) abort();
    uint8_t pps0 = tx[1];
    if ((pps0 & 0xE0) || (pps0 & 0xF) != r.protocol || !(offered & (1u << r.protocol))) abort();
    if (r.protocol != (data[3] & 0xF)) abort();
    if (((pps0 & 0x10) != 0) != (n == 4) || r.fd_changed != (n == 4)) abort();
    if (r.fd_changed) {
        // Granted PPS1 = requested PPS1, and the result is what it codes.
        if (!(data[3] & 0x10) || tx[2] != data[4]) abort();
        if (r.f != atr_fi(tx[2] >> 4) || r.d != atr_di(tx[2] & 0xF)) abort();
        if (!phy_params_supported(r.f, r.d)) abort();
        if (with_hook && !accept(r.f, r.d, r.protocol)) abort();
    } else if (r.f != cur.f || r.d != cur.d) {
        abort();
    }
    return 0;
}
