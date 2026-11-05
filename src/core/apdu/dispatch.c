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

void dispatch_reset(void) {
    for (uint8_t ch = 0; ch < CARD_MAX_CHANNELS; ch++) {
        s_ch[ch].open = ch == 0;
        s_ch[ch].applet = ch == 0 ? card_profile->default_applet : NULL;
    }
    s_chain_active = false;
    s_chain_len = 0;
    fs_reset();
    sec_reset();
    for (size_t i = 0; i < card_profile->n_applets; i++) {
        if (card_profile->applets[i]->reset) card_profile->applets[i]->reset();
    }
    // The implicitly selected applet also gets its DF (if one shares its name).
    const applet_t *a = card_profile->default_applet;
    if (a) {
        const fs_file_t *df = fs_by_name(a->aid, a->aid_len, false, NULL);
        if (df) fs_select_file(0, df);
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

apdu_case_t dispatch_classify(uint8_t cla, uint8_t ins, uint8_t p1, uint8_t p2) {
    if (cla == 0xFF) return APDU_CASE_UNKNOWN;
    const apdu_cmd_desc_t *d = find_desc(cla, ins);
    if (d) return d->kind;
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

bool dispatch_ins_known(uint8_t ins) {
    for (size_t i = 0; i < card_profile->n_applets; i++) {
        const applet_t *a = card_profile->applets[i];
        for (size_t k = 0; k < a->n_cmds; k++) {
            if (a->cmds[k].ins == ins) return true;
        }
    }
    if (card_profile->iso_commands) {
        size_t n;
        const apdu_cmd_desc_t *t = iso_cmds_table(&n);
        for (size_t i = 0; i < n; i++) {
            if (t[i].ins == ins) return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// applet selection
// ---------------------------------------------------------------------------

void card_put_default_fci(apdu_resp_t *resp, const uint8_t *aid, size_t aid_len) {
    uint8_t body[2 + 16];
    apdu_resp_t b = { body, sizeof body, 0 };
    resp_put_tlv(&b, 0x84, aid, aid_len);
    resp_put_tlv(resp, 0x6F, body, b.len);
}

static void deselect_applet(uint8_t ch) {
    const applet_t *old = s_ch[ch].applet;
    if (old && old->deselect) old->deselect(ch);
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

static uint16_t select_by_name(const apdu_t *cmd, apdu_resp_t *resp) {
    uint8_t ch = cmd->channel;
    bool next = (cmd->p2 & 0x03) == 0x02;
    const applet_t *a = find_applet(cmd->data, cmd->lc, true, next ? s_ch[ch].applet : NULL);
    if (!a) {
        // Not an applet: maybe a DF with that name in the file system.
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
    s_ch[ch].applet = a;
    // A DF of the same name (if any) becomes the current DF, so the applet can use the file system below it.
    const fs_file_t *df = fs_by_name(a->aid, a->aid_len, false, NULL);
    if (df) fs_select_file(ch, df);
    else fs_cursor_reset(ch);

    if (a->select) return a->select(cmd, resp);
    if ((cmd->p2 & 0x0C) != 0x0C) card_put_default_fci(resp, a->aid, a->aid_len);
    return SW_OK;
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
        s_ch[ch].open = true;
        s_ch[ch].applet = s_ch[cmd->channel].applet;       // inherits the current application
        fs_cursor_reset(ch);
        *fs_cursor(ch) = *fs_cursor(cmd->channel);
        sec_clear_channel(ch);
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
    return dispatch_ins_known(cmd->ins) ? SW_CLA_NOT_SUPPORTED : SW_INS_NOT_SUPPORTED;
}

uint16_t dispatch_process(const apdu_t *in, apdu_resp_t *resp) {
    apdu_t cmd = *in;
    resp->len = 0;
    decode_cla(in->cla, &cmd);

    if (in->cla == 0xFF) return SW_CLA_NOT_SUPPORTED;
    if (cmd.channel >= max_channels() || !s_ch[cmd.channel].open) {
        return SW_LOGICAL_CHANNEL_NOT_SUPP;
    }

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
    if (cmd.sm && card_profile->hooks.sm_wrap) sw = card_profile->hooks.sm_wrap(&cmd, resp, sw);
    return sw;
}
