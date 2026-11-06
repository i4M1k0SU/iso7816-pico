#include "console.h"

#include "iso7816_pico/apdu.h"
#include "apdu/dispatch.h"
#include "iso7816_pico/fs.h"
#include "iso7816_pico/card.h"
#include "iso7816_pico/config.h"
#include "hal/nvm.h"
#include "hal/phy.h"
#include "iso7816_pico/log.h"
#include "proto/session.h"

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
// commands: one line each, "name [arguments]", terminated by Enter
// ---------------------------------------------------------------------------

static void print_status(void) {
    const phy_stats_t *st = phy_stats();
    const phy_params_t *pp = phy_params();
    log_printf("state      : %s (RST=%d)", phy_in_reset() ? "reset" : "active", phy_rst_high());
    log_printf("CLK        : %lu Hz, F/D %u/%u, ETU = %lu us, T=%u", (unsigned long)st->clk_hz,
               pp->f, pp->d, (unsigned long)phy_etu_us(), session_protocol());
    log_printf("sysclk     : %lu Hz (PIO needs >= %u x CLK)", (unsigned long)st->sys_clk_hz,
               (unsigned)PHY_MIN_SYSCLK_PER_CLK);
    log_printf("RST edges  : %lu rising, %lu falling; ATR attempts %lu",
               (unsigned long)st->rst_rises, (unsigned long)st->rst_falls,
               (unsigned long)st->atr_attempts);
    phy_pins_t pins;
    phy_sample_pins(&pins);
    log_printf("pins now   : RST=%d CLK=%d (%lu toggles/2ms) IO=%d", pins.rst, pins.clk,
               (unsigned long)pins.clk_toggles, pins.io);
    log_printf("sessions   : %lu", (unsigned long)st->sessions);
    log_printf("rx/tx chars: %lu / %lu", (unsigned long)st->rx_chars, (unsigned long)st->tx_chars);
    log_printf("errors     : rx parity %lu, rx overrun %lu, tx echo %lu, tx timeout %lu",
               (unsigned long)st->rx_parity_errors, (unsigned long)st->rx_overruns,
               (unsigned long)st->tx_echo_errors, (unsigned long)st->tx_timeouts);
    log_printf("last ATR   : %s", st->last_error ? st->last_error : "ok");
    log_printf("  first ATR echo %lu us after start; CLK by PIO counter %lu Hz",
               (unsigned long)st->dbg_delay_us, (unsigned long)st->dbg_clk_counted_hz);
    log_printf("  first echo mismatch at byte %lu: expected %02lX got 0x%03lX",
               (unsigned long)st->dbg_echo_index, (unsigned long)st->dbg_echo_expected,
               (unsigned long)st->dbg_echo_got);
}

#ifndef PICO_PROGRAM_NAME
#define PICO_PROGRAM_NAME "iso7816-pico"
#endif
#ifndef PICO_PROGRAM_VERSION_STRING
#define PICO_PROGRAM_VERSION_STRING ""
#endif

// "<name> <version>" from the card profile, or from CMake when the profile leaves them NULL.
static void print_identity(void) {
    const char *name = card_profile->name ? card_profile->name : PICO_PROGRAM_NAME;
    const char *version = card_profile->version ? card_profile->version : PICO_PROGRAM_VERSION_STRING;
    log_printf("=== %s%s%s ===", name, *version ? " " : "", version);
}

static void print_card(void) {
    size_t n;
    print_identity();
    const uint8_t *atr = session_atr(&n);
    char hex[3 * ATR_MAX_LEN + 1];
    strbuf_t sb = { hex, sizeof hex, 0 };
    hex[0] = 0;
    sb_hex(&sb, atr, n, n);
    log_printf("ATR        : %s", hex);
    log_printf("PPS        : %s", card_profile->allow_pps ? "allowed" : "refused");
    log_printf("channels   : %u, ISO commands %s", card_profile->max_channels,
               card_profile->iso_commands ? "on" : "off");
    log_printf("applets    : %u", (unsigned)card_profile->n_applets);
    for (size_t i = 0; i < card_profile->n_applets; i++) {
        const applet_t *a = card_profile->applets[i];
        char aid[3 * 16 + 1];
        strbuf_t s2 = { aid, sizeof aid, 0 };
        aid[0] = 0;
        sb_hex(&s2, a->aid, a->aid_len, 16);
        log_printf("  %-12s AID %s, %u commands", a->name ? a->name : "?", aid, (unsigned)a->n_cmds);
    }
    log_printf("files      : %u", (unsigned)card_profile->n_files);
    for (size_t i = 0; i < card_profile->n_files; i++) {
        const fs_file_t *f = &card_profile->files[i];
        log_printf("  %04X type %02X parent %04X size %lu rec %u sfi %u ac %02X/%02X", f->fid, f->type,
                   f->parent ? f->parent->fid : 0, (unsigned long)f->size, f->rec_size, f->sfi,
                   f->ac_read, f->ac_update);
    }
    log_printf("PINs       : %u", (unsigned)card_profile->n_pins);
    for (size_t i = 0; i < card_profile->n_pins; i++) {
        const sec_pin_t *p = &card_profile->pins[i];
        log_printf("  ref %02X len %u retries %u/%u%s", p->ref, p->len, p->retries, p->max_retries,
                   p->disabled ? " (disabled)" : "");
    }
    log_printf("NVM        : %u bytes%s", (unsigned)card_profile->nvm_size,
               card_profile->nvm_blob ? "" : " (none)");
}

static void cmd_help(const char *args);

static void cmd_status(const char *args) {
    (void)args;
    print_status();
}

static void cmd_card(const char *args) {
    (void)args;
    print_card();
}

static void cmd_trace(const char *args) {
    (void)args;
    size_t n;
    const phy_trace_t *tr = phy_trace(&n);
    log_printf("trace of last session start (%u entries):", (unsigned)n);
    for (size_t i = 0; i < n; i++) {
        log_printf("  %7lu us  %c %03X", (unsigned long)tr[i].t_us, tr[i].kind, tr[i].value);
    }
}

// Wiring check: watch every GPIO for 15 s and report which ones moved, with the order of their first edges (ISO 7816-3
// activation raises I/O before RST, deactivation lowers RST before I/O).
static void cmd_gpio(const char *args) {
    (void)args;
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
            log_printf("  now=0x%08lx changed=0x%08lx", (unsigned long)now, (unsigned long)changed);
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
}

// Sample the contacts every 100 ms for 5 s; the card is not serviced meanwhile, so use this only for wiring checks.
static void cmd_pins(const char *args) {
    (void)args;
    for (int i = 0; i < 50; i++) {
        phy_pins_t pins;
        phy_sample_pins(&pins);
        log_printf("t=%4d ms RST=%d CLK=%d (%lu toggles/2ms) IO=%d", i * 100, pins.rst, pins.clk,
                   (unsigned long)pins.clk_toggles, pins.io);
        log_drain();
        watchdog_update();
        sleep_ms(98);
    }
}

// Parse "00 A4 04 00 ..." (spaces optional) into bytes; returns the count or 0 when malformed.
static size_t parse_hex(const char *s, uint8_t *out, size_t cap) {
    size_t n = 0;
    int hi = -1;
    for (; *s; s++) {
        int v;
        if (*s >= '0' && *s <= '9') v = *s - '0';
        else if (*s >= 'a' && *s <= 'f') v = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') v = *s - 'A' + 10;
        else if (*s == ' ' || *s == ':') continue;
        else return 0;
        if (hi < 0) {
            hi = v;
        } else {
            if (n >= cap) return 0;
            out[n++] = (uint8_t)((hi << 4) | v);
            hi = -1;
        }
    }
    return hi < 0 ? n : 0;
}

// Run a command APDU through the dispatcher without a reader: handy for trying handlers from the console.  Refused
// while a reader session is active (it would disturb that session's state); the dispatcher state is reset when the
// first console APDU after a reader session runs, and kept between console APDUs so that VERIFY etc. carry over.
static void cmd_apdu(const char *args) {
    static uint32_t s_seen_sessions = 0xFFFFFFFFu;
    static uint8_t buf[APDU_BUF_SIZE + 16];
    if (!phy_in_reset()) {
        log_printf("refused: a card session is active");
        return;
    }
    size_t n = parse_hex(args, buf, sizeof buf);
    apdu_t cmd;
    if (!n || !apdu_decode(buf, n, &cmd)) {
        log_printf("usage: apdu <CLA INS P1 P2 [Lc data] [Le]> (hex)");
        return;
    }
    if (phy_stats()->sessions != s_seen_sessions) {
        dispatch_reset();
        s_seen_sessions = phy_stats()->sessions;
    }
    apdu_resp_t resp = { dispatch_resp_buf(), APDU_BUF_SIZE, 0 };
    uint16_t sw = dispatch_process(&cmd, &resp);
    char line[3 * 32 + 1];
    for (size_t off = 0; off < resp.len; off += 32) {
        strbuf_t sb = { line, sizeof line, 0 };
        line[0] = 0;
        size_t chunk = resp.len - off < 32 ? resp.len - off : 32;
        sb_hex(&sb, resp.data + off, chunk, chunk);
        log_printf("  %s", line);
    }
    log_printf("SW %04X (%u data byte%s)", sw, (unsigned)resp.len, resp.len == 1 ? "" : "s");
}

static void cmd_save(const char *args) {
    (void)args;
    if (!card_profile->nvm_blob) {
        log_printf("this profile has no NVM blob");
        return;
    }
    nvm_take_dirty();
    if (nvm_save_logged(card_profile->nvm_blob, card_profile->nvm_size, "(console)") == NVM_SAVE_FAILED) {
        nvm_mark_dirty();
    }
}

static void cmd_erase(const char *args) {
    (void)args;
    log_printf("erasing NVM and rebooting ...");
    log_drain();
    sleep_ms(50);
    nvm_erase_all();
    watchdog_reboot(0, 0, 0);
}

static void cmd_bootsel(const char *args) {
    (void)args;
    log_printf("rebooting into BOOTSEL ...");
    log_drain();
    sleep_ms(50);
    reset_usb_boot(0, 0);
}

static void cmd_reboot(const char *args) {
    (void)args;
    log_printf("rebooting ...");
    log_drain();
    sleep_ms(50);
    watchdog_reboot(0, 0, 0);
}

static const console_cmd_t s_builtin[] = {
    { "help",    "list the commands",                                          cmd_help    },
    { "status",  "statistics (clock, sessions, errors, last ATR)",            cmd_status  },
    { "card",    "card profile (ATR, applets, files, PINs, NVM)",             cmd_card    },
    { "trace",   "TX/RX trace of the last session start",                     cmd_trace   },
    { "gpio",    "watch all GPIOs for 15 s (find the contacts)",              cmd_gpio    },
    { "pins",    "watch RST/CLK/IO for 5 s",                                  cmd_pins    },
    { "apdu",    "<hex>  run a command APDU through the dispatcher (no reader session)", cmd_apdu },
    { "save",    "write the NVM now",                                         cmd_save    },
    { "erase",   "erase the NVM (factory defaults) and reboot",               cmd_erase   },
    { "bootsel", "reboot into BOOTSEL (UF2 drive)",                           cmd_bootsel },
    { "reboot",  "reboot (run the firmware again)",                           cmd_reboot  },
};

// Two groups: the built-in commands, then the card profile's under the card's own name, so that a user of a finished
// card can tell which commands belong to that card.
static void cmd_help(const char *args) {
    (void)args;
    log_printf("commands (type a name and Enter; Enter alone shows this list):");
    log_printf("built-in:");
    for (size_t i = 0; i < sizeof s_builtin / sizeof s_builtin[0]; i++) {
        log_printf("  %-8s %s", s_builtin[i].name, s_builtin[i].help);
    }
    if (card_profile->n_console_cmds) {
        log_printf("%s:", card_profile->name ? card_profile->name : PICO_PROGRAM_NAME);
        for (size_t i = 0; i < card_profile->n_console_cmds; i++) {
            const console_cmd_t *cmd = &card_profile->console_cmds[i];
            log_printf("  %-8s %s", cmd->name, cmd->help ? cmd->help : "");
        }
    }
}

// Built-in names first, then the card profile's.
static const console_cmd_t *find_command(const char *name) {
    for (size_t i = 0; i < sizeof s_builtin / sizeof s_builtin[0]; i++) {
        if (strcmp(s_builtin[i].name, name) == 0) return &s_builtin[i];
    }
    for (size_t i = 0; i < card_profile->n_console_cmds; i++) {
        const console_cmd_t *cmd = &card_profile->console_cmds[i];
        if (cmd->name && cmd->handler && strcmp(cmd->name, name) == 0) return cmd;
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// line input
// ---------------------------------------------------------------------------

static char s_line[96];
static size_t s_line_len;

static void echo(const char *s, size_t n) {
    if (stdio_usb_connected()) stdio_put_string(s, (int)n, false, false);
}

static void run_line(char *line) {
    while (*line == ' ' || *line == '\t') line++;
    if (!*line) return;
    char *args = line;
    while (*args && *args != ' ' && *args != '\t') args++;
    if (*args) *args++ = 0;
    while (*args == ' ' || *args == '\t') args++;
    size_t n = strlen(args);
    while (n && (args[n - 1] == ' ' || args[n - 1] == '\t')) args[--n] = 0;

    const console_cmd_t *cmd = find_command(line);
    if (!cmd) {
        log_printf("unknown command '%s' (type help)", line);
        return;
    }
    cmd->handler(args);
}

void console_init(void) {}

void console_poll(void) {
    static bool was_connected;
    bool connected = stdio_usb_connected();
    if (connected && !was_connected) {
        log_printf("%s", "");
        print_identity();
        log_printf("iso7816-pico: ISO 7816 smart card framework");
        cmd_help(NULL);
        s_line_len = 0;
    }
    was_connected = connected;

    static int prev;   // previous input byte: the LF of a CR LF pair is not a second Enter
    for (;;) {
        int c = getchar_timeout_us(0);
        if (c == PICO_ERROR_TIMEOUT || c < 0) return;
        int last = prev;
        prev = c;
        if (c == '\r' || c == '\n') {
            if (c == '\n' && last == '\r') continue;
            echo("\r\n", 2);
            if (s_line_len == 0) {                    // Enter alone: show the commands
                cmd_help(NULL);
                continue;
            }
            s_line[s_line_len] = 0;
            s_line_len = 0;
            run_line(s_line);
        } else if (c == 0x08 || c == 0x7F) {
            if (s_line_len) {
                s_line_len--;
                echo("\b \b", 3);
            }
        } else if (c >= 0x20 && c < 0x7F && s_line_len < sizeof s_line - 1) {
            char ch = (char)c;
            s_line[s_line_len++] = ch;
            echo(&ch, 1);
        }
    }
}
