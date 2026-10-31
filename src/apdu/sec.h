// ISO 7816-4 / -8 security: PIN objects, security status per logical channel, VERIFY / CHANGE REFERENCE DATA / RESET
// RETRY COUNTER / ENABLE / DISABLE VERIFICATION / GET CHALLENGE, and the authentication hooks.
#pragma once

#include "apdu.h"

typedef struct {
    uint8_t ref;                       // reference data qualifier (P2 of VERIFY)
    uint8_t value[SEC_PIN_MAX_LEN];
    uint8_t len;
    uint8_t min_len, max_len;          // accepted lengths for new values
    uint8_t max_retries;
    uint8_t retries;                   // remaining tries (0 = blocked)
    uint8_t puk[SEC_PIN_MAX_LEN];      // unblocking key (len 0 = none)
    uint8_t puk_len;
    uint8_t puk_max_retries;
    uint8_t puk_retries;
    bool disabled;                     // verification switched off (always verified)
} sec_pin_t;

#define SEC_CHALLENGE_MAX 32

void sec_init(sec_pin_t *pins, size_t n);
void sec_reset(void);                  // forget all verifications (ATR)
void sec_clear_channel(uint8_t ch);    // channel closed / application changed

sec_pin_t *sec_find(uint8_t ref);
bool sec_is_verified(uint8_t ch, uint8_t ref);
void sec_set_verified(uint8_t ch, uint8_t ref, bool verified);

// Evaluate an access condition byte (FS_AC_ALWAYS / FS_AC_NEVER / PIN ref).
bool sec_access_ok(uint8_t ch, uint8_t ac);

// Last challenge handed out by GET CHALLENGE (for EXTERNAL AUTHENTICATE).
const uint8_t *sec_last_challenge(size_t *len);
void sec_random(uint8_t *out, size_t n);

// Interindustry command handlers (apdu_handler_t).
uint16_t sec_cmd_verify(const apdu_t *cmd, apdu_resp_t *resp);                 // 20
uint16_t sec_cmd_change_reference_data(const apdu_t *cmd, apdu_resp_t *resp);  // 24
uint16_t sec_cmd_disable_verification(const apdu_t *cmd, apdu_resp_t *resp);   // 26
uint16_t sec_cmd_enable_verification(const apdu_t *cmd, apdu_resp_t *resp);    // 28
uint16_t sec_cmd_reset_retry_counter(const apdu_t *cmd, apdu_resp_t *resp);    // 2C
uint16_t sec_cmd_get_challenge(const apdu_t *cmd, apdu_resp_t *resp);          // 84
uint16_t sec_cmd_external_authenticate(const apdu_t *cmd, apdu_resp_t *resp);  // 82 (hook)
uint16_t sec_cmd_internal_authenticate(const apdu_t *cmd, apdu_resp_t *resp);  // 88 (hook)
