#include "session.h"

#include "apdu/dispatch.h"
#include "iso7816_pico/atr.h"
#include "iso7816_pico/card.h"
#include "hal/phy.h"
#include "pps.h"
#include "iso7816_pico/log.h"
#include "t0.h"
#include "t1.h"

#include <string.h>

static uint8_t s_atr[ATR_MAX_LEN];
static size_t s_atr_len;
static atr_info_t s_info;
static uint8_t s_protocol;

bool session_init_raw(const uint8_t *atr, size_t len) {
    atr_info_t info;
    if (!len || len > ATR_MAX_LEN || !atr_parse(atr, len, &info)) {
        log_printf("session: invalid ATR in card profile");
        return false;
    }
    s_info = info;
    memcpy(s_atr, atr, len);
    s_atr_len = len;
    return true;
}

bool session_init(void) {
    if (card_profile->atr && card_profile->atr_len) return session_init_raw(card_profile->atr, card_profile->atr_len);
    if (card_profile->atr_desc) {
        uint8_t built[ATR_MAX_LEN];
        return session_init_raw(built, atr_build(card_profile->atr_desc, built, sizeof built));
    }
    static const uint8_t fallback[] = { 0x3B, 0x00 };
    return session_init_raw(fallback, sizeof fallback);
}

const uint8_t *session_atr(size_t *len) {
    *len = s_atr_len;
    return s_atr;
}

uint8_t session_protocol(void) { return s_protocol; }

// The PIO can only count the reader's clock when the system clock is fast enough (phy.h); a port that lowers the
// system clock to save power must stay above the ratio for the readers it meets.
static void warn_slow_sysclk(void) {
    const phy_stats_t *st = phy_stats();
    if (st->sys_clk_hz && st->clk_hz && st->sys_clk_hz < (uint64_t)PHY_MIN_SYSCLK_PER_CLK * st->clk_hz) {
        log_printf("WARNING: system clock %lu Hz is below %u x CLK (%lu Hz); the PIO cannot follow this reader",
                   (unsigned long)st->sys_clk_hz, (unsigned)PHY_MIN_SYSCLK_PER_CLK, (unsigned long)st->clk_hz);
    }
}

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
    p.extra_guard = card_profile->tx_extra_guard;
    // Specific mode (TA2): the protocol is the one TA2 names, not necessarily the first TD.
    s_protocol = s_info.specific_mode ? s_info.specific_protocol : s_info.first_protocol;
    set_protocol_params(&p, s_protocol);

    if (!phy_start_session(&p) || !phy_tx(s_atr, s_atr_len)) {
        const phy_stats_t *st = phy_stats();
        log_printf("ATR aborted: %s (CLK ~ %lu Hz, echo errors %lu)",
                   st->last_error ? st->last_error : "?", (unsigned long)st->clk_hz,
                   (unsigned long)st->tx_echo_errors);
        warn_slow_sysclk();
        return;
    }
    log_printf("ATR sent, CLK ~ %lu Hz, T=%u%s", (unsigned long)phy_stats()->clk_hz, s_protocol,
               s_info.specific_mode ? " (specific mode)" : "");
    warn_slow_sysclk();

    // Specific mode has no PPS: the ATR goes out at the default F/D and the parameters it announces apply from the
    // next character on.
    if (s_info.specific_mode && (s_info.f != p.f || s_info.d != p.d)) {
        p.f = s_info.f;
        p.d = s_info.d;
        if (!phy_apply_params(&p)) {
            log_printf("specific mode: F=%u D=%u not supported, waiting for reset", (unsigned)s_info.f,
                       (unsigned)s_info.d);
            uint8_t junk;
            while (phy_rx(&junk, true)) {}
            return;
        }
        log_printf("specific mode: F=%u D=%u", (unsigned)s_info.f, (unsigned)s_info.d);
    }

    dispatch_reset();
    if (card_profile->hooks.on_atr) card_profile->hooks.on_atr();
    t0_session_begin();
    t0_set_wi(s_info.wi);
    t1_config_t t1c = { s_info.ifsc, s_info.crc, s_info.cwi };
    t1_session_begin(&t1c);

    bool pps_allowed = card_profile->allow_pps && !s_info.specific_mode;
    bool first = true;

    for (;;) {
        uint8_t b;
        if (!phy_rx(&b, true)) break;

        if (b == 0xFF && first && pps_allowed) {
            pps_result_t r;
            if (!pps_handle(&p, s_info.protocols, card_profile->pps_accept, &r)) {
                // Invalid / unsupported request: stay mute until the next reset.
                log_printf("PPS failed, waiting for reset");
                uint8_t junk;
                while (phy_rx(&junk, true)) {}
                break;
            }
            p.f = r.f;
            p.d = r.d;
            s_protocol = r.protocol;
            set_protocol_params(&p, s_protocol);
            phy_apply_params(&p);
            first = false;         // ISO 7816-3 9.1: one PPS exchange, directly after the ATR
            continue;
        }
        first = false;

        bool ok = s_protocol == 1 ? t1_handle_block(b) : t0_handle_command(b);
        if (!ok) break;
        if (card_bootsel_requested()) return;
    }
    log_printf("session ended (reset)");
}
