// ISO 7816-4 APDU types and status words shared by the transports, the dispatcher and the applets.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "iso7816_pico/config.h"

// ---- Command / response -------------------------------------------------------

struct applet;   // applet_t, defined in iso7816_pico/card.h

typedef struct {
    uint8_t cla, ins, p1, p2;
    const uint8_t *data;   // command data field (NULL when lc == 0)
    uint32_t lc;           // 0..APDU_BUF_SIZE
    uint32_t le;           // expected response length; 0 = none (case 1/3),
                           // 256 / 65536 for a wildcard short / extended Le
    bool le_present;
    bool extended;         // extended length fields were used
    // Decoded CLA (ISO 7816-4 5.1.1)
    uint8_t channel;       // logical channel 0..19
    bool chaining;         // command chaining: more commands follow
    uint8_t sm;            // secure messaging indication 0..3 (0 = none)
    bool interindustry;    // CLA bit 8 = 0
    // The applet selected on the addressed channel when the dispatcher took the command (NULL when none), so that one
    // implementation can serve several applets (cmd->applet->ctx).  Inside an applet's select() it is the applet being
    // selected.  Filled by the dispatcher before any handler, hook, select() or process() runs.
    const struct applet *applet;
} apdu_t;

typedef struct {
    uint8_t *data;         // response data buffer (APDU_BUF_SIZE bytes)
    uint32_t cap;
    uint32_t len;
} apdu_resp_t;

// Handler result: a status word, or APDU_UNHANDLED to let the next layer try.
#define APDU_UNHANDLED 0x0000

typedef uint16_t (*apdu_handler_t)(const apdu_t *cmd, apdu_resp_t *resp);

// T=0 needs to know, from the header alone, whether P3 is Lc or Le.
typedef enum {
    APDU_CASE_UNKNOWN = 0,
    APDU_CASE1,   // no data in, no data out
    APDU_CASE2,   // data out            (P3 = Le)
    APDU_CASE3,   // data in             (P3 = Lc)
    APDU_CASE4,   // data in + data out  (P3 = Lc, out via 61 xx)
} apdu_case_t;

// Command descriptor: used both to classify (T=0) and, when handler is set, to route the command directly.
typedef struct {
    uint8_t cla;           // compared as (CLA & cla_mask) == cla
    uint8_t cla_mask;      // 0 = any CLA; 0xF0 ignores channel/SM/chaining bits
    uint8_t ins;
    apdu_case_t kind;
    const char *name;      // for the console trace (may be NULL)
    apdu_handler_t handler; // may be NULL (descriptor only classifies)
} apdu_cmd_desc_t;

#define APDU_CMD(cla_, mask_, ins_, kind_, name_, handler_) \
    { (cla_), (mask_), (ins_), (kind_), (name_), (handler_) }

// ---- Helpers ------------------------------------------------------------------------

static inline uint16_t apdu_p1p2(const apdu_t *c) { return (uint16_t)((c->p1 << 8) | c->p2); }

static inline void resp_put(apdu_resp_t *r, const void *src, size_t n) {
    if (r->len + n > r->cap) n = r->cap - r->len;
    const uint8_t *s = src;
    for (size_t i = 0; i < n; i++) r->data[r->len + i] = s[i];
    r->len += n;
}

static inline void resp_put_u8(apdu_resp_t *r, uint8_t b) { resp_put(r, &b, 1); }

// Decode a complete command APDU (short or extended length).
bool apdu_decode(const uint8_t *buf, size_t len, apdu_t *out);

// Simple TLV writer (1-byte tag, 1- or 2-byte length).
void resp_put_tlv(apdu_resp_t *r, uint8_t tag, const void *val, size_t n);
size_t tlv_len_size(size_t n);

// ---- Status words (ISO 7816-4 5.1.3) -------------------------------------------

#define SW_OK                          0x9000
#define SW_BYTES_REMAINING(n)          (0x6100 | ((n) & 0xFF))
#define SW_WARN_NV_UNCHANGED           0x6200
#define SW_WARN_EOF                    0x6282   // end of file/record reached before Le
#define SW_WARN_FILE_DEACTIVATED       0x6283
#define SW_WARN_NV_CHANGED             0x6300
#define SW_WARN_COUNTER(n)             (0x63C0 | ((n) & 0xF))
#define SW_WRONG_LENGTH                0x6700
#define SW_CLA_FUNC_NOT_SUPPORTED      0x6800
#define SW_LOGICAL_CHANNEL_NOT_SUPP    0x6881
#define SW_SM_NOT_SUPPORTED            0x6882
#define SW_LAST_CMD_EXPECTED           0x6883
#define SW_CHAINING_NOT_SUPPORTED      0x6884
#define SW_COMMAND_NOT_ALLOWED         0x6900
#define SW_INCOMPATIBLE_FILE           0x6981
#define SW_SECURITY_STATUS             0x6982
#define SW_AUTH_BLOCKED                0x6983
#define SW_REF_DATA_INVALID            0x6984
#define SW_CONDITIONS_NOT_SAT          0x6985
#define SW_NO_CURRENT_EF               0x6986
#define SW_SM_DO_MISSING               0x6987
#define SW_SM_DO_INCORRECT             0x6988
#define SW_WRONG_PARAMS                0x6A00
#define SW_WRONG_DATA                  0x6A80
#define SW_FUNC_NOT_SUPPORTED          0x6A81
#define SW_FILE_NOT_FOUND              0x6A82
#define SW_RECORD_NOT_FOUND            0x6A83
#define SW_NOT_ENOUGH_MEMORY           0x6A84
#define SW_LC_TLV_MISMATCH             0x6A85
#define SW_WRONG_P1P2                  0x6A86
#define SW_LC_P1P2_MISMATCH            0x6A87
#define SW_REF_DATA_NOT_FOUND          0x6A88
#define SW_FILE_EXISTS                 0x6A89
#define SW_WRONG_P1P2_2                0x6B00
#define SW_WRONG_LE(n)                 (0x6C00 | ((n) & 0xFF))
#define SW_INS_NOT_SUPPORTED           0x6D00
#define SW_CLA_NOT_SUPPORTED           0x6E00
#define SW_UNKNOWN                     0x6F00
