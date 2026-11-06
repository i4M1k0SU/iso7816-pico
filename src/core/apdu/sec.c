#include "iso7816_pico/sec.h"

#include "iso7816_pico/card.h"
#include "iso7816_pico/fs.h"
#include "iso7816_pico/log.h"
#include "hal/nvm.h"

#include "pico/rand.h"

#include <string.h>

static sec_pin_t *s_pins;
static size_t s_n;
static uint32_t s_verified[CARD_MAX_CHANNELS];   // bit i = pin i verified
_Static_assert(SEC_MAX_PINS <= 32, "s_verified has one bit per PIN");
static uint8_t s_challenge[SEC_CHALLENGE_MAX];
static size_t s_challenge_len;

// Profile errors are logged once at boot; the commands then refuse what cannot work instead of consuming tries.
void sec_init(sec_pin_t *pins, size_t n) {
    s_pins = pins;
    s_n = n;
    if (n > SEC_MAX_PINS) {
        log_printf("SEC: the profile has %lu PINs, only the first %d are used (SEC_MAX_PINS)", (unsigned long)n,
                   SEC_MAX_PINS);
        s_n = SEC_MAX_PINS;
    }
    bool transform = card_profile && card_profile->hooks.pin_transform;
    for (size_t i = 0; i < s_n; i++) {
        const sec_pin_t *p = &pins[i];
        if (p->len > SEC_PIN_MAX_LEN || p->puk_len > SEC_PIN_MAX_LEN || p->max_len > SEC_PIN_MAX_LEN) {
            log_printf("SEC: PIN %02X: a length is above SEC_PIN_MAX_LEN (%d); new values are capped there", p->ref,
                       SEC_PIN_MAX_LEN);
        }
        if (transform && !p->plain_len) {
            log_printf("SEC: PIN %02X: pin_transform without plain_len: CHANGE REFERENCE DATA P1=00 and RESET RETRY "
                       "COUNTER P1=00/01 answer 6F00", p->ref);
        }
    }
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
    if (card_profile->hooks.random_bytes) {
        card_profile->hooks.random_bytes(out, n);
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
    if (card_profile->hooks.pin_transform) return card_profile->hooks.pin_transform(p, in, len, out, SEC_PIN_MAX_LEN);
    if (len > SEC_PIN_MAX_LEN) return 0;
    memcpy(out, in, len);
    return len;
}

// Plaintext length of the stored PIN / PUK, needed to split "old + new" data fields.  0 with a pin_transform means the
// profile did not set plain_len (logged by sec_init()): the commands answer 6F00 without consuming a try.
static size_t plain_len(const sec_pin_t *p, size_t stored_len) {
    return card_profile->hooks.pin_transform ? p->plain_len : stored_len;
}

// Can a candidate of this length match at all?  Checked before a try is consumed, so a malformed command costs no try:
// an empty value never matches, and neither does one longer than a stored PIN can be (SEC_PIN_MAX_LEN) or, with a
// pin_transform and plain_len, one of another length.  None of this depends on the stored value.
static bool candidate_len_ok(const sec_pin_t *p, size_t len) {
    if (len == 0) return false;
    if (card_profile->hooks.pin_transform) return !p->plain_len || len == p->plain_len;
    return len <= SEC_PIN_MAX_LEN;
}

// Make a decremented retry counter durable before the candidate is compared.  Otherwise a wrong try followed by a
// power cut (before the runtime's deferred save) would leave the old counter in flash and the PIN could be guessed
// without limit.  The whole blob is written (any other pending change goes with it), which takes ~50-100 ms with
// interrupts off: well inside the waiting time of the default parameters (T=0 WWT ~0.7 s, T=1 BWT ~1.2 s at
// 4.8 MHz), so no keepalive is sent.  Only effective when the sec_pin_t array lives inside the profile's NVM blob (see
// docs/card-profile.md).  The tries are still counted in RAM when the save fails, which is logged.
static void persist_counter(const char *why) {
    if (!card_profile->nvm_blob || !card_profile->nvm_size) {
        nvm_mark_dirty();
        return;
    }
    nvm_take_dirty();   // other pending changes go out with this save
    if (nvm_save_logged(card_profile->nvm_blob, card_profile->nvm_size, why) == NVM_SAVE_FAILED) {
        nvm_mark_dirty();   // retried by the runtime's deferred save
    }
}

// Compare a candidate with the stored PIN, maintaining the retry counter (decremented and saved to flash before the
// comparison so that tearing cannot gain a try).
static uint16_t try_pin(uint8_t ch, sec_pin_t *p, const uint8_t *cand, size_t len) {
    if (p->retries == 0) return SW_AUTH_BLOCKED;
    p->retries--;
    persist_counter("(PIN try)");
    uint8_t form[SEC_PIN_MAX_LEN];
    size_t n = stored_form(p, cand, len, form);
    if (n && n == p->len && equal(form, p->value, n)) {
        p->retries = p->max_retries;   // restored in RAM; saved by the deferred save
        nvm_mark_dirty();
        sec_set_verified(ch, p->ref, true);
        return SW_OK;
    }
    sec_set_verified(ch, p->ref, false);
    return p->retries ? SW_WARN_COUNTER(p->retries) : SW_AUTH_BLOCKED;
}

// Compare a candidate with the PUK, maintaining the PUK retry counter like try_pin().
static uint16_t try_puk(sec_pin_t *p, const uint8_t *cand, size_t len) {
    if (p->puk_retries == 0) return SW_AUTH_BLOCKED;
    p->puk_retries--;
    persist_counter("(PUK try)");
    uint8_t form[SEC_PIN_MAX_LEN];
    size_t n = stored_form(p, cand, len, form);
    if (!n || n != p->puk_len || !equal(form, p->puk, n)) {
        return p->puk_retries ? SW_WARN_COUNTER(p->puk_retries) : SW_AUTH_BLOCKED;
    }
    p->puk_retries = p->puk_max_retries;
    nvm_mark_dirty();
    return SW_OK;
}

// A new PIN value checked and converted to its stored form, before anything is changed.
typedef struct {
    uint8_t form[SEC_PIN_MAX_LEN];
    size_t len;
} new_pin_t;

static uint16_t prepare_pin(const sec_pin_t *p, const uint8_t *v, size_t len, new_pin_t *out) {
    if (len < p->min_len || len > p->max_len || len > SEC_PIN_MAX_LEN) return SW_WRONG_LENGTH;
    if (card_profile->hooks.pin_transform && p->plain_len && len != p->plain_len) return SW_WRONG_LENGTH;
    out->len = stored_form(p, v, len, out->form);
    return out->len ? SW_OK : SW_WRONG_DATA;
}

static void commit_pin(sec_pin_t *p, const new_pin_t *v) {
    memcpy(p->value, v->form, v->len);
    p->len = (uint8_t)v->len;
    p->retries = p->max_retries;
    nvm_mark_dirty();
}

static uint16_t set_pin(sec_pin_t *p, const uint8_t *v, size_t len) {
    new_pin_t np;
    uint16_t sw = prepare_pin(p, v, len, &np);
    if (sw == SW_OK) commit_pin(p, &np);
    return sw;
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
    if (!candidate_len_ok(p, cmd->lc)) return SW_WRONG_LENGTH;
    return try_pin(cmd->channel, p, cmd->data, cmd->lc);
}

// 00 24 P1 P2 Lc data: P1 = 00 -> old PIN + new PIN, P1 = 01 -> new PIN only (requires a verified status).  For
// P1 = 00 the whole data field (both lengths, the new value's form) is checked before the old PIN costs a try.
uint16_t sec_cmd_change_reference_data(const apdu_t *cmd, apdu_resp_t *resp) {
    (void)resp;
    sec_pin_t *p = sec_find(cmd->p2);
    if (!p) return SW_REF_DATA_NOT_FOUND;
    if (cmd->p1 == 0x00) {
        size_t old_len = plain_len(p, p->len);
        if (old_len == 0) return card_profile->hooks.pin_transform ? SW_UNKNOWN : SW_REF_DATA_INVALID;
        if (cmd->lc <= old_len) return SW_WRONG_LENGTH;
        new_pin_t np;
        uint16_t sw = prepare_pin(p, cmd->data + old_len, cmd->lc - old_len, &np);
        if (sw != SW_OK) return sw;
        sw = try_pin(cmd->channel, p, cmd->data, old_len);
        if (sw != SW_OK) return sw;
        commit_pin(p, &np);
        return SW_OK;
    }
    if (cmd->p1 == 0x01) {
        if (!sec_is_verified(cmd->channel, p->ref)) return SW_SECURITY_STATUS;
        return set_pin(p, cmd->data, cmd->lc);
    }
    return SW_WRONG_P1P2;
}

// 00 2C P1 P2 [Lc data]: P1 = 00 PUK + new PIN, 01 PUK only (unblock), 02 new PIN (verified status required), 03 no
// data (verified status required).  Every length (and for P1 = 00 the new PIN's form) is checked before the PUK costs a
// try or anything changes.
uint16_t sec_cmd_reset_retry_counter(const apdu_t *cmd, apdu_resp_t *resp) {
    (void)resp;
    sec_pin_t *p = sec_find(cmd->p2);
    if (!p) return SW_REF_DATA_NOT_FOUND;
    switch (cmd->p1) {
    case 0x00:
    case 0x01: {
        if (!p->puk_len) return SW_REF_DATA_NOT_FOUND;
        size_t puk_plain = plain_len(p, p->puk_len);
        if (puk_plain == 0) return SW_UNKNOWN;   // pin_transform without plain_len (logged at boot)
        new_pin_t np = { .len = 0 };
        if (cmd->p1 == 0x00) {
            if (cmd->lc <= puk_plain) return SW_WRONG_LENGTH;
            uint16_t sw = prepare_pin(p, cmd->data + puk_plain, cmd->lc - puk_plain, &np);
            if (sw != SW_OK) return sw;
        } else if (cmd->lc != puk_plain) {
            return SW_WRONG_LENGTH;
        }
        uint16_t sw = try_puk(p, cmd->data, puk_plain);
        if (sw != SW_OK) return sw;
        if (cmd->p1 == 0x00) commit_pin(p, &np);   // also restores the PIN retry counter
        p->retries = p->max_retries;
        nvm_mark_dirty();
        return SW_OK;
    }
    case 0x02:
        if (!sec_is_verified(cmd->channel, p->ref)) return SW_SECURITY_STATUS;
        return set_pin(p, cmd->data, cmd->lc);
    case 0x03:
        if (cmd->lc) return SW_WRONG_LENGTH;
        if (!sec_is_verified(cmd->channel, p->ref)) return SW_SECURITY_STATUS;
        p->retries = p->max_retries;
        nvm_mark_dirty();
        return SW_OK;
    default:
        return SW_WRONG_P1P2;
    }
}

// 00 26 / 00 28 P1 P2 [Lc PIN]: P1 = 00 with the PIN (checked like VERIFY; an empty field costs no try), P1 = 01
// without data (verified status required).
static uint16_t set_disabled(const apdu_t *cmd, bool disabled) {
    sec_pin_t *p = sec_find(cmd->p2);
    if (!p) return SW_REF_DATA_NOT_FOUND;
    if (cmd->p1 == 0x00) {
        if (!candidate_len_ok(p, cmd->lc)) return SW_WRONG_LENGTH;
        uint16_t sw = try_pin(cmd->channel, p, cmd->data, cmd->lc);
        if (sw != SW_OK) return sw;
    } else if (cmd->p1 == 0x01) {
        if (cmd->lc) return SW_WRONG_LENGTH;
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
    if (!card_profile->hooks.external_authenticate) return SW_FUNC_NOT_SUPPORTED;
    uint16_t sw = card_profile->hooks.external_authenticate(cmd, resp);
    s_challenge_len = 0;                 // a challenge is good for one attempt
    return sw;
}

uint16_t sec_cmd_internal_authenticate(const apdu_t *cmd, apdu_resp_t *resp) {
    if (!card_profile->hooks.internal_authenticate) return SW_FUNC_NOT_SUPPORTED;
    return card_profile->hooks.internal_authenticate(cmd, resp);
}
