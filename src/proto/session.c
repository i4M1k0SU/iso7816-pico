#include "session.h"

#include "apdu/dispatch.h"
#include "atr.h"
#include "card/card.h"
#include "hal/phy.h"
#include "pps.h"
#include "sys/log.h"
#include "t0.h"
#include "t1.h"

#include <string.h>

static uint8_t s_atr[ATR_MAX_LEN];
static size_t s_atr_len;
static atr_info_t s_info;
static uint8_t s_protocol;

bool session_init(void) {
    if (card_profile.atr && card_profile.atr_len) {
        if (card_profile.atr_len > ATR_MAX_LEN) return false;
        memcpy(s_atr, card_profile.atr, card_profile.atr_len);
        s_atr_len = card_profile.atr_len;
    } else if (card_profile.atr_desc) {
        s_atr_len = atr_build(card_profile.atr_desc, s_atr, sizeof s_atr);
    } else {
        static const uint8_t fallback[] = { 0x3B, 0x00 };
        memcpy(s_atr, fallback, sizeof fallback);
        s_atr_len = sizeof fallback;
    }
    if (!s_atr_len || !atr_parse(s_atr, s_atr_len, &s_info)) {
        log_printf("session: invalid ATR in card profile");
        return false;
    }
    return true;
}

const uint8_t *session_atr(size_t *len) {
    *len = s_atr_len;
    return s_atr;
}

uint8_t session_protocol(void) { return s_protocol; }

static void set_protocol_params(phy_params_t *p, uint8_t t) {
    p->error_signal = t == 0;              // character repetition is a T=0 thing
    p->turnaround_etu = t == 0 ? 16 : 22;  // BGT for T=1
}

void session_keepalive(void) {
    if (s_protocol == 1) t1_keepalive();
    else t0_keepalive();
}

void session_run(void) {
    phy_params_t p = PHY_PARAMS_DEFAULT;
    p.inverse = s_info.inverse;
    p.extra_guard = card_profile.tx_extra_guard;
    s_protocol = s_info.first_protocol;
    set_protocol_params(&p, s_protocol);

    if (!phy_start_session(&p) || !phy_tx(s_atr, s_atr_len)) {
        const phy_stats_t *st = phy_stats();
        log_printf("ATR aborted: %s (CLK ~ %lu Hz, echo errors %lu)",
                   st->last_error ? st->last_error : "?", (unsigned long)st->clk_hz,
                   (unsigned long)st->tx_echo_errors);
        return;
    }
    log_printf("ATR sent, CLK ~ %lu Hz, T=%u%s", (unsigned long)phy_stats()->clk_hz, s_protocol,
               s_info.specific_mode ? " (specific mode)" : "");

    dispatch_reset();
    if (card_profile.hooks.on_atr) card_profile.hooks.on_atr();
    t0_session_begin();
    t1_config_t t1c = { s_info.ifsc, s_info.crc };
    t1_session_begin(&t1c);

    bool pps_allowed = card_profile.allow_pps && !s_info.specific_mode;
    bool first = true;

    for (;;) {
        uint8_t b;
        if (!phy_rx(&b, true)) break;

        if (b == 0xFF && first && pps_allowed) {
            pps_result_t r;
            if (!pps_handle(&p, s_info.protocols, card_profile.pps_accept, &r)) {
                // Invalid / unsupported request: stay mute until the next reset.
                log_printf("PPS failed, waiting for reset");
                while (!phy_reset_pending()) {
                    uint8_t junk;
                    phy_rx(&junk, true);
                }
                break;
            }
            p.f = r.f;
            p.d = r.d;
            s_protocol = r.protocol;
            set_protocol_params(&p, s_protocol);
            phy_apply_params(&p);
            continue;              // PPS may be repeated once more by some readers
        }
        first = false;

        bool ok = s_protocol == 1 ? t1_handle_block(b) : t0_handle_command(b);
        if (!ok) break;
        if (card_bootsel_requested()) return;
    }
    log_printf("session ended (reset)");
}
