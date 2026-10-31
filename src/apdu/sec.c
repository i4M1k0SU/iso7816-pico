#include "sec.h"

#include "card/card.h"
#include "fs.h"
#include "hal/nvm.h"

#include "pico/rand.h"

#include <string.h>

static sec_pin_t *s_pins;
static size_t s_n;
static uint32_t s_verified[CARD_MAX_CHANNELS];   // bit i = pin i verified
static uint8_t s_challenge[SEC_CHALLENGE_MAX];
static size_t s_challenge_len;

void sec_init(sec_pin_t *pins, size_t n) {
    s_pins = pins;
    s_n = n > 32 ? 32 : n;
    sec_reset();
}

void sec_reset(void) {
    memset(s_verified, 0, sizeof s_verified);
    s_challenge_len = 0;
}

void sec_clear_channel(uint8_t ch) {
    if (ch < CARD_MAX_CHANNELS) s_verified[ch] = 0;
}

sec_pin_t *sec_find(uint8_t ref) {
    for (size_t i = 0; i < s_n; i++) {
        if (s_pins[i].ref == ref) return &s_pins[i];
    }
    return NULL;
}

static int pin_index(const sec_pin_t *p) { return (int)(p - s_pins); }

bool sec_is_verified(uint8_t ch, uint8_t ref) {
    const sec_pin_t *p = sec_find(ref);
    if (!p || ch >= CARD_MAX_CHANNELS) return false;
    if (p->disabled) return true;
    return (s_verified[ch] >> pin_index(p)) & 1;
}

void sec_set_verified(uint8_t ch, uint8_t ref, bool verified) {
    const sec_pin_t *p = sec_find(ref);
    if (!p || ch >= CARD_MAX_CHANNELS) return;
    uint32_t bit = 1u << pin_index(p);
    if (verified) s_verified[ch] |= bit;
    else s_verified[ch] &= ~bit;
}

bool sec_access_ok(uint8_t ch, uint8_t ac) {
    if (ac == FS_AC_ALWAYS) return true;
    if (ac == FS_AC_NEVER) return false;
    return sec_is_verified(ch, ac);
}

const uint8_t *sec_last_challenge(size_t *len) {
    *len = s_challenge_len;
    return s_challenge;
}

void sec_random(uint8_t *out, size_t n) {
    if (card_profile.hooks.random_bytes) {
        card_profile.hooks.random_bytes(out, n);
        return;
    }
    while (n) {
        uint32_t r = get_rand_32();
        size_t k = n < 4 ? n : 4;
        memcpy(out, &r, k);
        out += k;
        n -= k;
    }
}

// Constant-time comparison.
static bool equal(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
    return d == 0;
}

// Stored form of a value: the profile's pin_transform (e.g. a hash) or the value itself.  Returns the stored length,
// 0 when the transform rejects it.
static size_t stored_form(const sec_pin_t *p, const uint8_t *in, size_t len, uint8_t *out) {
    if (card_profile.hooks.pin_transform) return card_profile.hooks.pin_transform(p, in, len, out, SEC_PIN_MAX_LEN);
    if (len > SEC_PIN_MAX_LEN) return 0;
    memcpy(out, in, len);
    return len;
}

// Plaintext length of the stored PIN / PUK, needed to split "old + new" data fields.
static size_t plain_len(const sec_pin_t *p, size_t stored_len) {
    return card_profile.hooks.pin_transform ? p->plain_len : stored_len;
}

// Compare a candidate with the stored PIN, maintaining the retry counter (decremented before the comparison so that
// tearing cannot gain a try).
static uint16_t try_pin(uint8_t ch, sec_pin_t *p, const uint8_t *cand, size_t len) {
    if (p->retries == 0) return SW_AUTH_BLOCKED;
    p->retries--;
    nvm_mark_dirty();
    uint8_t form[SEC_PIN_MAX_LEN];
    size_t n = stored_form(p, cand, len, form);
    if (n && n == p->len && equal(form, p->value, n)) {
        p->retries = p->max_retries;
        sec_set_verified(ch, p->ref, true);
        return SW_OK;
    }
    sec_set_verified(ch, p->ref, false);
    return p->retries ? SW_WARN_COUNTER(p->retries) : SW_AUTH_BLOCKED;
}

static uint16_t set_pin(sec_pin_t *p, const uint8_t *v, size_t len) {
    if (len < p->min_len || len > p->max_len || len > SEC_PIN_MAX_LEN) return SW_WRONG_LENGTH;
    uint8_t form[SEC_PIN_MAX_LEN];
    size_t n = stored_form(p, v, len, form);
    if (!n) return SW_WRONG_DATA;
    memcpy(p->value, form, n);
    p->len = (uint8_t)n;
    p->retries = p->max_retries;
    nvm_mark_dirty();
    return SW_OK;
}

// ---------------------------------------------------------------------------
// commands
// ---------------------------------------------------------------------------

// 00 20 P1 P2 [Lc PIN]: P1 = 00 verify, P1 = FF reset the verification status.
uint16_t sec_cmd_verify(const apdu_t *cmd, apdu_resp_t *resp) {
    (void)resp;
    sec_pin_t *p = sec_find(cmd->p2);
    if (!p) return SW_REF_DATA_NOT_FOUND;
    if (cmd->p1 == 0xFF) {
        sec_set_verified(cmd->channel, p->ref, false);
        return SW_OK;
    }
    if (cmd->p1 != 0x00) return SW_WRONG_P1P2;
    if (cmd->lc == 0) {
        if (sec_is_verified(cmd->channel, p->ref)) return SW_OK;
        return p->retries ? SW_WARN_COUNTER(p->retries) : SW_AUTH_BLOCKED;
    }
    return try_pin(cmd->channel, p, cmd->data, cmd->lc);
}

// 00 24 P1 P2 Lc data: P1 = 00 -> old PIN + new PIN, P1 = 01 -> new PIN only (requires a verified status).
uint16_t sec_cmd_change_reference_data(const apdu_t *cmd, apdu_resp_t *resp) {
    (void)resp;
    sec_pin_t *p = sec_find(cmd->p2);
    if (!p) return SW_REF_DATA_NOT_FOUND;
    if (cmd->p1 == 0x00) {
        size_t old_len = plain_len(p, p->len);
        if (cmd->lc <= old_len) return SW_WRONG_LENGTH;
        uint16_t sw = try_pin(cmd->channel, p, cmd->data, old_len);
        if (sw != SW_OK) return sw;
        return set_pin(p, cmd->data + old_len, cmd->lc - old_len);
    }
    if (cmd->p1 == 0x01) {
        if (!sec_is_verified(cmd->channel, p->ref)) return SW_SECURITY_STATUS;
        return set_pin(p, cmd->data, cmd->lc);
    }
    return SW_WRONG_P1P2;
}

// 00 2C P1 P2 [Lc data]: P1 = 00 PUK + new PIN, 01 PUK only (unblock), 02 new PIN (verified status required), 03 no
// data (verified status required).
uint16_t sec_cmd_reset_retry_counter(const apdu_t *cmd, apdu_resp_t *resp) {
    (void)resp;
    sec_pin_t *p = sec_find(cmd->p2);
    if (!p) return SW_REF_DATA_NOT_FOUND;
    switch (cmd->p1) {
    case 0x00:
    case 0x01: {
        if (!p->puk_len) return SW_REF_DATA_NOT_FOUND;
        if (p->puk_retries == 0) return SW_AUTH_BLOCKED;
        size_t puk_plain = plain_len(p, p->puk_len);
        if (cmd->lc < puk_plain) return SW_WRONG_LENGTH;
        p->puk_retries--;
        nvm_mark_dirty();
        uint8_t form[SEC_PIN_MAX_LEN];
        size_t n = stored_form(p, cmd->data, puk_plain, form);
        if (!n || n != p->puk_len || !equal(form, p->puk, n)) {
            return p->puk_retries ? SW_WARN_COUNTER(p->puk_retries) : SW_AUTH_BLOCKED;
        }
        p->puk_retries = p->puk_max_retries;
        if (cmd->p1 == 0x00) {
            uint16_t sw = set_pin(p, cmd->data + puk_plain, cmd->lc - puk_plain);
            if (sw != SW_OK) return sw;
        } else if (cmd->lc != puk_plain) {
            return SW_WRONG_LENGTH;
        }
        p->retries = p->max_retries;
        return SW_OK;
    }
    case 0x02:
        if (!sec_is_verified(cmd->channel, p->ref)) return SW_SECURITY_STATUS;
        return set_pin(p, cmd->data, cmd->lc);
    case 0x03:
        if (!sec_is_verified(cmd->channel, p->ref)) return SW_SECURITY_STATUS;
        if (cmd->lc) return SW_WRONG_LENGTH;
        p->retries = p->max_retries;
        nvm_mark_dirty();
        return SW_OK;
    default:
        return SW_WRONG_P1P2;
    }
}

static uint16_t set_disabled(const apdu_t *cmd, bool disabled) {
    sec_pin_t *p = sec_find(cmd->p2);
    if (!p) return SW_REF_DATA_NOT_FOUND;
    if (cmd->p1 == 0x00) {
        uint16_t sw = try_pin(cmd->channel, p, cmd->data, cmd->lc);
        if (sw != SW_OK) return sw;
    } else if (cmd->p1 == 0x01) {
        if (!sec_is_verified(cmd->channel, p->ref)) return SW_SECURITY_STATUS;
    } else {
        return SW_WRONG_P1P2;
    }
    p->disabled = disabled;
    nvm_mark_dirty();
    return SW_OK;
}

uint16_t sec_cmd_disable_verification(const apdu_t *cmd, apdu_resp_t *resp) {
    (void)resp;
    return set_disabled(cmd, true);
}

uint16_t sec_cmd_enable_verification(const apdu_t *cmd, apdu_resp_t *resp) {
    (void)resp;
    return set_disabled(cmd, false);
}

// 00 84 00 00 Le
uint16_t sec_cmd_get_challenge(const apdu_t *cmd, apdu_resp_t *resp) {
    if (cmd->p1 || cmd->p2) return SW_WRONG_P1P2;
    size_t n = cmd->le ? cmd->le : 8;
    if (n > SEC_CHALLENGE_MAX) n = SEC_CHALLENGE_MAX;
    sec_random(s_challenge, n);
    s_challenge_len = n;
    resp_put(resp, s_challenge, n);
    return SW_OK;
}

uint16_t sec_cmd_external_authenticate(const apdu_t *cmd, apdu_resp_t *resp) {
    if (!card_profile.hooks.external_authenticate) return SW_FUNC_NOT_SUPPORTED;
    uint16_t sw = card_profile.hooks.external_authenticate(cmd, resp);
    s_challenge_len = 0;                 // a challenge is good for one attempt
    return sw;
}

uint16_t sec_cmd_internal_authenticate(const apdu_t *cmd, apdu_resp_t *resp) {
    if (!card_profile.hooks.internal_authenticate) return SW_FUNC_NOT_SUPPORTED;
    return card_profile.hooks.internal_authenticate(cmd, resp);
}
