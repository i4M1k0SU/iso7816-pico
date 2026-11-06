#include "dispatch.h"

#include "iso7816_pico/card.h"
#include "iso7816_pico/fs.h"
#include "iso_cmds.h"
#include "iso7816_pico/sec.h"
#include "iso7816_pico/log.h"

#include <string.h>

static uint8_t s_cmd_buf[APDU_BUF_SIZE];
static uint8_t s_resp_buf[APDU_BUF_SIZE + 2];

// Command chaining (CLA b5): pieces are collected here until the last one.
static uint8_t s_chain_buf[APDU_BUF_SIZE];
static uint32_t s_chain_len;
static uint8_t s_chain_hdr[4];
static bool s_chain_active;

// Secure messaging scratch (plain command after sm_unwrap).
static uint8_t s_sm_buf[APDU_BUF_SIZE];

typedef struct {
    bool open;
    const applet_t *applet;
} channel_t;

static channel_t s_ch[CARD_MAX_CHANNELS];

uint8_t *dispatch_cmd_buf(void) { return s_cmd_buf; }
uint8_t *dispatch_resp_buf(void) { return s_resp_buf; }

static uint8_t max_channels(void) {
    uint8_t n = card_profile->max_channels ? card_profile->max_channels : 1;
    return n > CARD_MAX_CHANNELS ? CARD_MAX_CHANNELS : n;
}

static uint16_t implicit_select(uint8_t ch, const applet_t *a, const fs_cursor_t *start, apdu_resp_t *resp);

void dispatch_reset(void) {
    for (uint8_t ch = 0; ch < CARD_MAX_CHANNELS; ch++) {
        s_ch[ch].open = ch == 0;
        s_ch[ch].applet = NULL;
    }
    s_chain_active = false;
    s_chain_len = 0;
    fs_reset();
    sec_reset();
    for (size_t i = 0; i < card_profile->n_applets; i++) {
        const applet_t *a = card_profile->applets[i];
        if (a->reset) a->reset(a);
    }
    // The default applet is selected implicitly, exactly as an explicit SELECT would: its DF (if one shares its name)
    // becomes current and its select() runs.  Nothing is sent to the reader, so the response buffer is free here.
    const applet_t *a = card_profile->default_applet;
    if (a) {
        apdu_resp_t resp = APDU_RESP_INIT(s_resp_buf, APDU_BUF_SIZE);
        implicit_select(0, a, NULL, &resp);
    }
}

// ---------------------------------------------------------------------------
// CLA decoding (ISO 7816-4 5.1.1)
// ---------------------------------------------------------------------------

static void decode_cla(uint8_t cla, apdu_t *c) {
    c->cla = cla;
    c->interindustry = !(cla & 0x80);
    if (!c->interindustry && !card_profile->proprietary_cla_decode) {
        // Proprietary class: the low bits mean whatever the applet says.
        c->channel = 0;
        c->chaining = false;
        c->sm = 0;
        return;
    }
    if (!(cla & 0x40)) {                      // first interindustry values / proprietary
        c->channel = cla & 0x03;
        c->chaining = cla & 0x10;
        c->sm = (cla >> 2) & 0x03;
    } else {                                  // further interindustry values (channels 4..19)
        c->channel = (uint8_t)(4 + (cla & 0x0F));
        c->chaining = cla & 0x10;
        c->sm = (cla & 0x20) ? 2 : 0;
    }
}

// ---------------------------------------------------------------------------
// lookups
// ---------------------------------------------------------------------------

static const apdu_cmd_desc_t *applet_find(const applet_t *a, uint8_t cla, uint8_t ins) {
    if (!a) return NULL;
    for (size_t i = 0; i < a->n_cmds; i++) {
        const apdu_cmd_desc_t *d = &a->cmds[i];
        if (d->ins == ins && (cla & d->cla_mask) == d->cla) return d;
    }
    return NULL;
}

static const apdu_cmd_desc_t *find_desc(uint8_t cla, uint8_t ins) {
    apdu_t c;
    decode_cla(cla, &c);
    const applet_t *a = c.channel < CARD_MAX_CHANNELS ? s_ch[c.channel].applet : NULL;
    const apdu_cmd_desc_t *d = applet_find(a, cla, ins);
    if (d) return d;
    if (card_profile->iso_commands) {
        d = iso_cmds_find(cla, ins);
        if (d) return d;
    }
    return NULL;
}

// Order: the table of the applet selected on the addressed channel, that applet's classify(), the interindustry table,
// SELECT by name, the profile-wide hook.  Applet before interindustry, as in route().
apdu_case_t dispatch_classify(uint8_t cla, uint8_t ins, uint8_t p1, uint8_t p2) {
    if (cla == 0xFF) return APDU_CASE_UNKNOWN;
    apdu_t c;
    decode_cla(cla, &c);
    const applet_t *a = c.channel < CARD_MAX_CHANNELS ? s_ch[c.channel].applet : NULL;
    const apdu_cmd_desc_t *d = applet_find(a, cla, ins);
    if (d) return d->kind;
    if (a && a->classify) {
        apdu_case_t k = a->classify(a, cla, ins, p1, p2);
        if (k != APDU_CASE_UNKNOWN) return k;
    }
    if (card_profile->iso_commands) {
        d = iso_cmds_find(cla, ins);
        if (d) return d->kind;
    }
    if (ins == 0xA4 && (p1 & 0x04)) return APDU_CASE4;     // SELECT by name: applets
    if (card_profile->hooks.classify) return card_profile->hooks.classify(cla, ins, p1, p2);
    return APDU_CASE_UNKNOWN;
}

const char *dispatch_name(uint8_t cla, uint8_t ins) {
    const apdu_cmd_desc_t *d = find_desc(cla, ins);
    if (d && d->name) return d->name;
    if (ins == 0xA4) return "SELECT";
    return "?";
}

static bool table_has_other_cla(const apdu_cmd_desc_t *t, size_t n, uint8_t cla, uint8_t ins) {
    for (size_t i = 0; i < n; i++) {
        if (t[i].ins == ins && t[i].handler && (cla & t[i].cla_mask) != t[i].cla) return true;
    }
    return false;
}

// 6E00 "class not supported" (ISO 7816-4 5.6) is the right answer only when the card would have executed this INS
// under another CLA *in the current context*: a table handler of the applet selected on the addressed channel, an
// interindustry handler (iso_commands), or a command the dispatcher itself serves (SELECT by name, MANAGE CHANNEL).
// Classification-only entries, handlers of applets that are not selected and handlers that declined (same CLA, they
// returned APDU_UNHANDLED) do not count: middleware takes 6E00 as "retry with another CLA", which would be pointless.
bool dispatch_cla_mismatch(const apdu_t *cmd) {
    const applet_t *a = cmd->channel < CARD_MAX_CHANNELS ? s_ch[cmd->channel].applet : NULL;
    if (a && table_has_other_cla(a->cmds, a->n_cmds, cmd->cla, cmd->ins)) return true;
    if (card_profile->iso_commands) {
        size_t n;
        const apdu_cmd_desc_t *t = iso_cmds_table(&n);
        if (table_has_other_cla(t, n, cmd->cla, cmd->ins)) return true;
    }
    return !cmd->interindustry && (cmd->ins == 0xA4 || cmd->ins == 0x70);
}

// ---------------------------------------------------------------------------
// applet selection
// ---------------------------------------------------------------------------

// 6F (2 + n) { 84 n AID }.  An AID has at most 16 bytes (ISO 7816-4 8.2.1.2), so both lengths fit in one byte.  A
// longer one is a profile error: it is logged and the response is flagged as overflowing, so the SELECT fails with
// 6F00 instead of answering a malformed FCI.
void card_put_default_fci(apdu_resp_t *resp, const uint8_t *aid, size_t aid_len) {
    if (aid_len > 16) {
        log_printf("default FCI: AID of %u bytes, ISO 7816-4 allows at most 16", (unsigned)aid_len);
        resp->overflow = true;
        return;
    }
    resp_put_u8(resp, 0x6F);
    resp_put_u8(resp, (uint8_t)(2 + aid_len));
    resp_put_u8(resp, 0x84);
    resp_put_u8(resp, (uint8_t)aid_len);
    resp_put(resp, aid, aid_len);
}

static void deselect_applet(uint8_t ch) {
    const applet_t *old = s_ch[ch].applet;
    if (old && old->deselect) old->deselect(old, ch);
    s_ch[ch].applet = NULL;
}

// Find the n-th applet whose AID starts with (or equals) the given name.
static const applet_t *find_applet(const uint8_t *name, size_t len, bool partial,
                                   const applet_t *after) {
    bool passed = after == NULL;
    for (size_t i = 0; i < card_profile->n_applets; i++) {
        const applet_t *a = card_profile->applets[i];
        if (!passed) {
            if (a == after) passed = true;
            continue;
        }
        if (len == 0 || len > a->aid_len) continue;
        if (partial ? memcmp(a->aid, name, len) == 0
                    : (len == a->aid_len && memcmp(a->aid, name, len) == 0)) {
            return a;
        }
    }
    return NULL;
}

// SW of select() that leaves the applet selected: 9000 and the warnings 62xx / 63xx (ISO 7816-4 5.6: the command was
// executed).  Anything else is a failed selection.
static bool select_succeeded(uint16_t sw) {
    return sw == SW_OK || (sw >> 8) == 0x62 || (sw >> 8) == 0x63;
}

// Make applet `a` the selected one on channel ch: the cursor starts at `start` (NULL: the DF of the same name if there
// is one, else the MF), then a->select() runs.  The channel state (applet, cursor) is committed only when select()
// succeeds; when it fails (Java Card semantics) no applet is selected on the channel, the cursor is put back, the
// security state of the channel is cleared and the response data is dropped.  The caller deselects the previous applet
// first.
static uint16_t activate_applet(uint8_t ch, const applet_t *a, const apdu_t *cmd, apdu_resp_t *resp,
                                const fs_cursor_t *start) {
    fs_cursor_t saved = *fs_cursor(ch);
    if (start) {
        *fs_cursor(ch) = *start;
    } else {
        // A DF of the same name becomes the current DF, so the applet can use the file system below it.
        const fs_file_t *df = fs_by_name(a->aid, a->aid_len, false, NULL);
        if (df) fs_select_file(ch, df);
        else fs_cursor_reset(ch);
    }
    s_ch[ch].applet = a;    // already visible to select(), e.g. for a nested dispatch_classify()

    uint16_t sw = SW_OK;
    if (a->select) {
        apdu_t c = *cmd;
        c.applet = a;
        sw = a->select(&c, resp);
    }
    else if ((cmd->p2 & 0x0C) != 0x0C) card_put_default_fci(resp, a->aid, a->aid_len);
    if (select_succeeded(sw) && resp->overflow) {
        // An incomplete FCI is not a successful selection.
        log_printf("%s: response overflow (cap %u)", a->name ? a->name : "applet", (unsigned)resp->cap);
        sw = SW_UNKNOWN;
    }
    if (select_succeeded(sw)) return sw;

    s_ch[ch].applet = NULL;
    *fs_cursor(ch) = saved;
    sec_clear_channel(ch);
    resp->len = 0;
    return sw == APDU_UNHANDLED ? SW_UNKNOWN : sw;
}

// Select an applet without a SELECT from the reader (default applet after the ATR, MANAGE CHANNEL open): select() gets
// a synthetic SELECT by DF name with P2 = 0C (no response data); its response is discarded.
static uint16_t implicit_select(uint8_t ch, const applet_t *a, const fs_cursor_t *start, apdu_resp_t *resp) {
    apdu_t cmd = {
        .cla = ch < 4 ? ch : (uint8_t)(0x40 | (ch - 4)), .ins = 0xA4, .p1 = 0x04, .p2 = 0x0C,
        .data = a->aid, .lc = a->aid_len, .channel = ch, .interindustry = true, .applet = a,
    };
    uint16_t sw = activate_applet(ch, a, &cmd, resp, start);
    resp_reset(resp);
    if (sw != SW_OK) {
        log_printf("%s: implicit selection on channel %u: SW %04X%s", a->name ? a->name : "applet", (unsigned)ch,
                   (unsigned)sw, s_ch[ch].applet ? "" : ", not selected");
    }
    return sw;
}

static uint16_t select_by_name(const apdu_t *cmd, apdu_resp_t *resp) {
    uint8_t ch = cmd->channel;
    bool next = (cmd->p2 & 0x03) == 0x02;
    const applet_t *a = find_applet(cmd->data, cmd->lc, true, next ? s_ch[ch].applet : NULL);
    if (!a) {
        // Not an applet: maybe a DF with that name in the file system.  A failed lookup changes nothing.
        if (fs_present()) {
            uint16_t sw = fs_cmd_select(cmd, resp);
            if (sw == SW_OK) deselect_applet(ch);
            return sw;
        }
        return SW_FILE_NOT_FOUND;
    }
    if (s_ch[ch].applet != a) {
        deselect_applet(ch);
        sec_clear_channel(ch);
    }
    return activate_applet(ch, a, cmd, resp, NULL);
}

// ---------------------------------------------------------------------------
// MANAGE CHANNEL (00 70 P1 P2 [01])
// ---------------------------------------------------------------------------

static uint16_t manage_channel(const apdu_t *cmd, apdu_resp_t *resp) {
    uint8_t n = max_channels();
    if (cmd->p1 == 0x00) {                               // open
        uint8_t ch = cmd->p2;
        if (ch == 0) {
            for (ch = 1; ch < n && s_ch[ch].open; ch++) {}
            if (ch >= n) return SW_LOGICAL_CHANNEL_NOT_SUPP;
        } else if (ch >= n || s_ch[ch].open) {
            return SW_WRONG_P1P2;
        }
        // ISO 7816-4 (5.4.2 / 7.1.2, MANAGE CHANNEL): a channel opened from the basic channel starts with the MF or the
        // default application selected; one opened from another channel gets the application and current DF / EF of
        // that channel.  The security state is never copied.  The applet's select() runs on the new channel in both
        // cases (so it can set up per-channel state); if it fails, the channel is not opened and its SW is returned.
        // When copying, the origin's current files are put back after select(), whatever select() made current.
        s_ch[ch].open = true;
        s_ch[ch].applet = NULL;
        fs_cursor_reset(ch);
        sec_clear_channel(ch);
        const applet_t *a;
        fs_cursor_t origin = *fs_cursor(cmd->channel);
        const fs_cursor_t *start = NULL;
        if (cmd->channel == 0) {
            a = card_profile->default_applet;
        } else {
            a = s_ch[cmd->channel].applet;
            start = &origin;
            *fs_cursor(ch) = origin;
        }
        if (a) {
            uint16_t sw = implicit_select(ch, a, start, resp);
            if (!s_ch[ch].applet) {
                s_ch[ch].open = false;
                fs_cursor_reset(ch);
                return sw;
            }
            if (start) *fs_cursor(ch) = origin;
        }
        if (cmd->p2 == 0) resp_put_u8(resp, ch);
        return SW_OK;
    }
    if (cmd->p1 == 0x80) {                               // close
        uint8_t ch = cmd->p2 ? cmd->p2 : cmd->channel;
        if (ch == 0 || ch >= n || !s_ch[ch].open) return SW_WRONG_P1P2;
        deselect_applet(ch);
        s_ch[ch].open = false;
        sec_clear_channel(ch);
        fs_cursor_reset(ch);
        return SW_OK;
    }
    return SW_WRONG_P1P2;
}

// ---------------------------------------------------------------------------
// main entry
// ---------------------------------------------------------------------------

static uint16_t route(const apdu_t *cmd, apdu_resp_t *resp) {
    uint8_t ch = cmd->channel;

    if (cmd->ins == 0xA4 && (cmd->p1 & 0x04) && cmd->interindustry) {
        return select_by_name(cmd, resp);
    }
    if (cmd->ins == 0x70 && cmd->interindustry) return manage_channel(cmd, resp);

    const applet_t *a = s_ch[ch].applet;
    if (a) {
        const apdu_cmd_desc_t *d = applet_find(a, cmd->cla, cmd->ins);
        if (d && d->handler) {
            uint16_t sw = d->handler(cmd, resp);
            if (sw != APDU_UNHANDLED) return sw;
        }
        if (a->process) {
            uint16_t sw = a->process(cmd, resp);
            if (sw != APDU_UNHANDLED) return sw;
        }
    }
    if (card_profile->iso_commands && cmd->interindustry) {
        uint16_t sw = iso_cmds_process(cmd, resp);
        if (sw != APDU_UNHANDLED) return sw;
    }
    if (card_profile->hooks.unhandled) {
        uint16_t sw = card_profile->hooks.unhandled(cmd, resp);
        if (sw != APDU_UNHANDLED) return sw;
    }
    // GET RESPONSE and ENVELOPE are T=0 transport commands that t0.c consumes; they arrive here only under T=1 (or from
    // the console, or inside an ENVELOPE).  Nothing is ever pending outside T=0, so GET RESPONSE gets 6985 "conditions
    // of use not satisfied", the same SW t0.c gives when no response data is left (ISO 7816-4 5.6).  ENVELOPE has no
    // meaning above the transport unless an applet or hook implements it, so it ends in 6D00 below like any other INS
    // nobody serves (its interindustry table entry only classifies and does not make it a 6E00 candidate).
    if (cmd->ins == 0xC0 && cmd->interindustry) return SW_CONDITIONS_NOT_SAT;
    return dispatch_cla_mismatch(cmd) ? SW_CLA_NOT_SUPPORTED : SW_INS_NOT_SUPPORTED;
}

// SW after which the response data is sent (ISO 7816-4 5.6: normal processing and warnings).
static bool sw_has_data(uint16_t sw) {
    uint8_t sw1 = (uint8_t)(sw >> 8);
    return sw == SW_OK || sw1 == 0x61 || sw1 == 0x62 || sw1 == 0x63;
}

// A handler whose response did not fit must not report success with part of it: 6F00, no data.
static uint16_t check_overflow(const apdu_t *cmd, apdu_resp_t *resp, uint16_t sw) {
    if (!resp->overflow || !sw_has_data(sw)) return sw;
    log_printf("%s: response overflow (cap %u)", dispatch_name(cmd->cla, cmd->ins), (unsigned)resp->cap);
    resp_reset(resp);
    return SW_UNKNOWN;
}

uint16_t dispatch_process(const apdu_t *in, apdu_resp_t *resp) {
    apdu_t cmd = *in;
    resp_reset(resp);
    decode_cla(in->cla, &cmd);
    cmd.applet = NULL;

    if (in->cla == 0xFF) return SW_CLA_NOT_SUPPORTED;
    if (cmd.channel >= max_channels() || !s_ch[cmd.channel].open) {
        return SW_LOGICAL_CHANNEL_NOT_SUPP;
    }
    // The applet in charge as the command arrives; it stays so for the whole command (sm_wrap included), even when the
    // command is a SELECT that changes it.
    cmd.applet = s_ch[cmd.channel].applet;

    // Secure messaging: unwrap first so that chaining sees plain data.
    if (cmd.sm) {
        if (!card_profile->hooks.sm_unwrap) return SW_SM_NOT_SUPPORTED;
        uint16_t sw = card_profile->hooks.sm_unwrap(&cmd, s_sm_buf, sizeof s_sm_buf);
        if (sw != SW_OK) return sw;
    }

    // Command chaining: collect the pieces, run the whole thing with the last.
    if (cmd.chaining || s_chain_active) {
        uint8_t hdr[4] = { (uint8_t)(cmd.cla & ~0x10), cmd.ins, cmd.p1, cmd.p2 };
        if (s_chain_active && memcmp(hdr, s_chain_hdr, 4) != 0) {
            s_chain_active = false;
            s_chain_len = 0;
            return SW_LAST_CMD_EXPECTED;
        }
        if (s_chain_len + cmd.lc > sizeof s_chain_buf) {
            s_chain_active = false;
            s_chain_len = 0;
            return SW_WRONG_LENGTH;
        }
        memcpy(s_chain_buf + s_chain_len, cmd.data, cmd.lc);
        s_chain_len += cmd.lc;
        if (cmd.chaining) {
            memcpy(s_chain_hdr, hdr, 4);
            s_chain_active = true;
            return SW_OK;
        }
        s_chain_active = false;
        cmd.data = s_chain_buf;
        cmd.lc = s_chain_len;
        s_chain_len = 0;
    }

    uint16_t sw = route(&cmd, resp);
    if (sw == APDU_UNHANDLED) sw = SW_UNKNOWN;
    sw = check_overflow(&cmd, resp, sw);
    if (cmd.sm && card_profile->hooks.sm_wrap) {
        sw = card_profile->hooks.sm_wrap(&cmd, resp, sw);
        sw = check_overflow(&cmd, resp, sw);
    }
    return sw;
}
