#include "console.h"

#include "apdu/fs.h"
#include "card/card.h"
#include "config.h"
#include "hal/nvm.h"
#include "hal/phy.h"
#include "log.h"
#include "proto/session.h"

#include "hardware/flash.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/stdio.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"
#include "tusb.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// log ring buffer
// ---------------------------------------------------------------------------

static char s_ring[LOG_RING_SIZE];
static size_t s_head, s_tail;   // head = write, tail = read
static bool s_overflow;

static size_t ring_used(void) { return (s_head - s_tail + LOG_RING_SIZE) % LOG_RING_SIZE; }
static size_t ring_free(void) { return LOG_RING_SIZE - 1 - ring_used(); }

static void ring_put(const char *s, size_t n) {
    if (n >= LOG_RING_SIZE) return;
    if (n > ring_free()) {
        // Keep the newest lines: drop the oldest ones to make room.
        s_overflow = true;
        size_t drop = n - ring_free();
        s_tail = (s_tail + drop) % LOG_RING_SIZE;
    }
    for (size_t i = 0; i < n; i++) {
        s_ring[s_head] = s[i];
        s_head = (s_head + 1) % LOG_RING_SIZE;
    }
}

static void log_vprintf(const char *fmt, va_list ap) {
    char line[420];
    int n = vsnprintf(line, sizeof line - 2, fmt, ap);
    if (n < 0) return;
    if ((size_t)n > sizeof line - 3) n = sizeof line - 3;
    line[n++] = '\r';
    line[n++] = '\n';
    ring_put(line, (size_t)n);
}

void log_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_vprintf(fmt, ap);
    va_end(ap);
}

// Bounded string builder: appends never run past the buffer.
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} strbuf_t;

static void sb_printf(strbuf_t *sb, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_printf(strbuf_t *sb, const char *fmt, ...) {
    if (sb->len + 1 >= sb->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(sb->buf + sb->len, sb->cap - sb->len, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    sb->len += (size_t)n;
    if (sb->len >= sb->cap) sb->len = sb->cap - 1;
}

static void sb_hex(strbuf_t *sb, const uint8_t *src, size_t n, size_t show_max) {
    size_t show = n > show_max ? show_max : n;
    for (size_t i = 0; i < show; i++) {
        sb_printf(sb, i ? " %02X" : "%02X", src[i]);
    }
    if (show < n) sb_printf(sb, " ..(%u bytes)", (unsigned)n);
}

void log_apdu(const char *name, uint8_t cla, uint8_t ins, uint8_t p1, uint8_t p2,
              const uint8_t *in, size_t in_len, const uint8_t *out, size_t out_len, uint16_t sw) {
    char buf[200];
    strbuf_t sb = { buf, sizeof buf, 0 };
    buf[0] = 0;
    sb_printf(&sb, "APDU %-22s %02X %02X %02X %02X", name, cla, ins, p1, p2);
    if (in_len) {
        sb_printf(&sb, " | ");
        sb_hex(&sb, in, in_len, 16);
    }
    if (out_len) {
        sb_printf(&sb, " -> ");
        sb_hex(&sb, out, out_len, 16);
    }
    if (sw) sb_printf(&sb, " SW %04X", sw);
    log_printf("%s", buf);
}

void log_drain(void) {
    // While nobody is listening the ring keeps the most recent history, so a terminal opened later still shows what
    // happened.
    if (!stdio_usb_connected()) return;
    if (s_overflow && ring_free() > 32) {
        s_overflow = false;
        ring_put("[log: older lines dropped]\r\n", 28);
    }
    size_t avail = tud_cdc_write_available();
    while (avail && s_tail != s_head) {
        size_t chunk = s_head > s_tail ? s_head - s_tail : LOG_RING_SIZE - s_tail;
        if (chunk > avail) chunk = avail;
        stdio_put_string(&s_ring[s_tail], (int)chunk, false, false);
        s_tail = (s_tail + chunk) % LOG_RING_SIZE;
        avail -= chunk;
    }
}

// ---------------------------------------------------------------------------
// commands
// ---------------------------------------------------------------------------

static void print_help(void) {
    log_printf("iso7816-pico console");
    log_printf("  h  this help");
    log_printf("  s  status / statistics");
    log_printf("  c  card profile (ATR, protocol, files, PINs)");
    log_printf("  t  dump TX/RX trace of the last session start");
    log_printf("  g  watch all GPIOs for 15 s (find the contacts)");
    log_printf("  p  watch contact pins for 5 s (RST/CLK/IO)");
    log_printf("  n  save NVM now");
    log_printf("  x  erase NVM (factory defaults) and reboot");
    log_printf("  b  reboot into BOOTSEL (UF2 drive)");
    log_printf("  r  reboot (run firmware again)");
}

static void print_status(void) {
    const phy_stats_t *st = phy_stats();
    const phy_params_t *pp = phy_params();
    log_printf("state      : %s (RST=%d)", phy_in_reset() ? "reset" : "active", phy_rst_high());
    log_printf("CLK        : %lu Hz, F/D %u/%u, ETU = %lu us, T=%u", (unsigned long)st->clk_hz,
               pp->f, pp->d, (unsigned long)phy_etu_us(), session_protocol());
    log_printf("RST edges  : %lu rising, %lu falling; ATR attempts %lu",
               (unsigned long)st->rst_rises, (unsigned long)st->rst_falls,
               (unsigned long)st->atr_attempts);
    phy_pins_t pins;
    phy_sample_pins(&pins);
    log_printf("pins now   : RST=%d CLK=%d (%lu toggles/2ms) IO=%d", pins.rst, pins.clk,
               (unsigned long)pins.clk_toggles, pins.io);
    log_printf("sessions   : %lu", (unsigned long)st->sessions);
    log_printf("rx/tx chars: %lu / %lu", (unsigned long)st->rx_chars, (unsigned long)st->tx_chars);
    log_printf("errors     : rx parity %lu, tx echo %lu, tx timeout %lu",
               (unsigned long)st->rx_parity_errors, (unsigned long)st->tx_echo_errors,
               (unsigned long)st->tx_timeouts);
    log_printf("last ATR   : %s", st->last_error ? st->last_error : "ok");
    log_printf("  first ATR echo %lu us after start; CLK by PWM counter %lu Hz",
               (unsigned long)st->dbg_delay_us, (unsigned long)st->dbg_clk_pwm_hz);
    log_printf("  first echo mismatch at byte %lu: expected %02lX got 0x%03lX",
               (unsigned long)st->dbg_echo_index, (unsigned long)st->dbg_echo_expected,
               (unsigned long)st->dbg_echo_got);
}

static void print_card(void) {
    size_t n;
    const uint8_t *atr = session_atr(&n);
    char hex[3 * ATR_MAX_LEN + 1];
    strbuf_t sb = { hex, sizeof hex, 0 };
    hex[0] = 0;
    sb_hex(&sb, atr, n, n);
    log_printf("ATR        : %s", hex);
    log_printf("PPS        : %s", card_profile.allow_pps ? "allowed" : "refused");
    log_printf("channels   : %u, ISO commands %s", card_profile.max_channels,
               card_profile.iso_commands ? "on" : "off");
    log_printf("applets    : %u", (unsigned)card_profile.n_applets);
    for (size_t i = 0; i < card_profile.n_applets; i++) {
        const applet_t *a = card_profile.applets[i];
        char aid[3 * 16 + 1];
        strbuf_t s2 = { aid, sizeof aid, 0 };
        aid[0] = 0;
        sb_hex(&s2, a->aid, a->aid_len, 16);
        log_printf("  %-12s AID %s, %u commands", a->name ? a->name : "?", aid, (unsigned)a->n_cmds);
    }
    log_printf("files      : %u", (unsigned)card_profile.n_files);
    for (size_t i = 0; i < card_profile.n_files; i++) {
        const fs_file_t *f = &card_profile.files[i];
        log_printf("  %04X type %02X parent %04X size %lu rec %u sfi %u ac %02X/%02X", f->fid, f->type,
                   f->parent ? f->parent->fid : 0, (unsigned long)f->size, f->rec_size, f->sfi,
                   f->ac_read, f->ac_update);
    }
    log_printf("PINs       : %u", (unsigned)card_profile.n_pins);
    for (size_t i = 0; i < card_profile.n_pins; i++) {
        const sec_pin_t *p = &card_profile.pins[i];
        log_printf("  ref %02X len %u retries %u/%u%s", p->ref, p->len, p->retries, p->max_retries,
                   p->disabled ? " (disabled)" : "");
    }
    log_printf("NVM        : %u bytes%s", (unsigned)card_profile.nvm_size,
               card_profile.nvm_blob ? "" : " (none)");
}

void console_init(void) {}

void console_poll(void) {
    static bool was_connected;
    bool connected = stdio_usb_connected();
    if (connected && !was_connected) {
        log_printf("%s", "");
        log_printf("=== iso7816-pico: ISO 7816 smart card framework (RP2040 PIO) ===");
        print_help();
    }
    was_connected = connected;

    int c = getchar_timeout_us(0);
    if (c == PICO_ERROR_TIMEOUT || c < 0) return;
    switch (c) {
    case 'h': case 'H': case '?':
        print_help();
        break;
    case 's': case 'S':
        print_status();
        break;
    case 'c': case 'C':
        print_card();
        break;
    case 't': case 'T': {
        size_t n;
        const phy_trace_t *tr = phy_trace(&n);
        log_printf("trace of last session start (%u entries):", (unsigned)n);
        for (size_t i = 0; i < n; i++) {
            log_printf("  %7lu us  %c %03X", (unsigned long)tr[i].t_us, tr[i].kind, tr[i].value);
        }
        break;
    }
    case 'g': case 'G': {
        // Wiring check: watch every GPIO for 15 s and report which ones moved, with the order of their first edges (ISO
        // 7816-3 activation raises I/O before RST, deactivation lowers RST before I/O).
        log_printf("watching all GPIOs for 15 s - trigger the reader now");
        log_drain();
        uint32_t first = gpio_get_all(), changed = 0, seen_high = 0;
        absolute_time_t t_start = get_absolute_time();
        absolute_time_t end = make_timeout_time_ms(15000);
        absolute_time_t next_report = make_timeout_time_ms(1000);
        uint32_t last = first;
        int64_t first_rise[30], first_fall[30];
        int64_t first_change = -1;
        for (int g = 0; g < 30; g++) { first_rise[g] = first_fall[g] = -1; }
        while (!time_reached(end)) {
            uint32_t now = gpio_get_all();
            uint32_t diff = now ^ last;
            if (diff) {
                int64_t t = absolute_time_diff_us(t_start, get_absolute_time());
                if (first_change < 0) first_change = t;
                for (int g = 0; g < 30; g++) {
                    if (!(diff & (1u << g))) continue;
                    if ((now >> g) & 1) { if (first_rise[g] < 0) first_rise[g] = t; }
                    else                { if (first_fall[g] < 0) first_fall[g] = t; }
                }
            }
            changed |= diff;
            seen_high |= now;
            last = now;
            watchdog_update();
            if (time_reached(next_report)) {
                next_report = make_timeout_time_ms(1000);
                log_printf("  now=0x%08lx changed=0x%08lx", (unsigned long)now,
                           (unsigned long)changed);
                log_drain();
            }
        }
        log_printf("GPIO changed  : 0x%08lx", (unsigned long)changed);
        log_printf("GPIO seen high: 0x%08lx", (unsigned long)seen_high);
        for (int g = 0; g < 30; g++) {
            if (changed & (1u << g)) {
                log_printf("  GP%-2d first rise %+lld us, first fall %+lld us (ends %s)", g,
                           (long long)(first_rise[g] < 0 ? -1 : first_rise[g] - first_change),
                           (long long)(first_fall[g] < 0 ? -1 : first_fall[g] - first_change),
                           (last >> g) & 1 ? "high" : "low");
            }
        }
        break;
    }
    case 'p': case 'P': {
        for (int i = 0; i < 50; i++) {
            phy_pins_t pins;
            phy_sample_pins(&pins);
            log_printf("t=%4d ms RST=%d CLK=%d (%lu toggles/2ms) IO=%d", i * 100, pins.rst,
                       pins.clk, (unsigned long)pins.clk_toggles, pins.io);
            log_drain();
            watchdog_update();
            sleep_ms(98);
        }
        break;
    }
    case 'n': case 'N':
        if (card_profile.nvm_blob) {
            bool ok = nvm_save(card_profile.nvm_blob, card_profile.nvm_size);
            log_printf("NVM save: %s", ok ? "ok" : "FAILED");
        }
        break;
    case 'x': case 'X': {
        log_printf("erasing NVM and rebooting ...");
        log_drain();
        sleep_ms(50);
        uint32_t ints = save_and_disable_interrupts();
        flash_range_erase(PICO_FLASH_SIZE_BYTES - NVM_FLASH_BYTES, NVM_FLASH_BYTES);
        restore_interrupts(ints);
        watchdog_reboot(0, 0, 0);
        break;
    }
    case 'b': case 'B':
        log_printf("rebooting into BOOTSEL ...");
        log_drain();
        sleep_ms(50);
        reset_usb_boot(0, 0);
        break;
    case 'r': case 'R':
        log_printf("rebooting ...");
        log_drain();
        sleep_ms(50);
        watchdog_reboot(0, 0, 0);
        break;
    default:
        break;
    }
}
