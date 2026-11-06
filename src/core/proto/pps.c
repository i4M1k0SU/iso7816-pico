#include "pps.h"

#include "iso7816_pico/atr.h"

#include "iso7816_pico/log.h"

bool pps_handle(const phy_params_t *current, uint16_t offered_protocols,
                bool (*accept)(uint16_t f, uint8_t d, uint8_t t), pps_result_t *out) {
    uint8_t req[6] = { 0xFF };
    size_t n = 1;
    if (!phy_rx(&req[n++], false)) return false;              // PPS0
    uint8_t pps0 = req[1];
    for (int bit = 4; bit <= 6; bit++) {
        if (pps0 & (1u << bit)) {
            if (!phy_rx(&req[n++], false)) return false;      // PPS1..3
        }
    }
    if (!phy_rx(&req[n++], false)) return false;              // PCK
    uint8_t x = 0;
    for (size_t i = 0; i < n; i++) x ^= req[i];
    if (x != 0) {
        log_printf("PPS: bad PCK, ignoring");
        return false;
    }

    uint8_t t = pps0 & 0xF;
    if (!(offered_protocols & (1u << t))) {
        log_printf("PPS: T=%u not offered, staying mute", t);
        return false;
    }

    // Decide about PPS1 (F/D).  PPS2 (SPU) and PPS3 are never accepted: they are simply not echoed, which means "not
    // changed".
    uint8_t resp[4] = { 0xFF, (uint8_t)(pps0 & 0x0F) };
    size_t rn = 2;
    out->protocol = t;
    out->fd_changed = false;
    out->f = current->f;
    out->d = current->d;
    if (pps0 & 0x10) {
        uint8_t pps1 = req[2];
        uint16_t f = atr_fi(pps1 >> 4);
        uint8_t d = atr_di(pps1 & 0xF);
        bool ok = f && d && phy_params_supported(f, d) && (!accept || accept(f, d, t));
        if (ok) {
            resp[1] |= 0x10;
            resp[rn++] = pps1;
            out->fd_changed = true;
            out->f = f;
            out->d = d;
        }
    }
    uint8_t pck = 0;
    for (size_t i = 0; i < rn; i++) pck ^= resp[i];
    resp[rn++] = pck;
    if (!phy_tx(resp, rn)) return false;

    log_printf("PPS: T=%u F/D %u/%u -> %u/%u%s", t, current->f, current->d, out->f, out->d,
               (pps0 & 0x10) && !out->fd_changed ? " (PPS1 rejected)" : "");
    return true;
}
