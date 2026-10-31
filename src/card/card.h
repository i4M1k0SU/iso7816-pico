// ============================================================================
//  The card profile: everything that makes *your* card what it is.
//
//  Fill in `card_profile` in card_profile.c (ATR, transmission parameters,
//  file system, PINs, applets, hooks).  The framework does the rest:
//
//    hal/phy.c        PIO character layer (ETU timing, parity, error signal)
//    proto/atr.c      ATR builder / parser
//    proto/pps.c      PPS (F/D and protocol negotiation)
//    proto/t0.c       T=0 (procedure bytes, 61xx / 6Cxx, GET RESPONSE, ENVELOPE)
//    proto/t1.c       T=1 (I/R/S blocks, chaining, IFS, RESYNCH, WTX)
//    apdu/dispatch.c  CLA decoding, logical channels, command chaining,
//                     secure messaging hooks, applet selection, routing
//    apdu/fs.c        MF/DF/EF, SELECT, READ/UPDATE/WRITE/ERASE BINARY,
//                     READ/UPDATE/WRITE/APPEND/ERASE/SEARCH RECORD
//    apdu/sec.c       PINs, VERIFY, CHANGE REFERENCE DATA, RESET RETRY
//                     COUNTER, ENABLE/DISABLE VERIFICATION, GET CHALLENGE
//    apdu/iso_cmds.c  the interindustry command table and -8/-9 hooks
//    hal/nvm.c        persistence of your data in flash
// ============================================================================
#pragma once

#include "apdu/apdu.h"
#include "apdu/fs.h"
#include "apdu/sec.h"
#include "proto/atr.h"

// ---------------------------------------------------------------------------
// Applet: an application selectable by AID (ISO 7816-4 SELECT by DF name).
// ---------------------------------------------------------------------------
typedef struct applet {
    const char *name;
    const uint8_t *aid;
    uint8_t aid_len;

    // Command table: classifies commands for T=0 and, where `handler` is set, routes them.  Commands without a handler
    // (or not in the table) go to `process`.
    const apdu_cmd_desc_t *cmds;
    size_t n_cmds;

    // Optional.  Called by SELECT; return the SW and put the FCI in resp. When NULL the framework answers 6F len { 84
    // len AID } (unless P2 says "no response data").
    uint16_t (*select)(const apdu_t *cmd, apdu_resp_t *resp);

    // Optional.  Another applet or DF is being selected on this channel.
    void (*deselect)(uint8_t channel);

    // Optional.  Catch-all for commands without a table handler.  Return APDU_UNHANDLED to let the interindustry
    // commands / hooks try.
    uint16_t (*process)(const apdu_t *cmd, apdu_resp_t *resp);

    // Optional.  Called after every ATR (volatile state back to defaults).
    void (*reset)(void);
} applet_t;

// ---------------------------------------------------------------------------
// Hooks: places where card-specific behaviour plugs into the framework. Every member is optional (NULL = default
// behaviour).
// ---------------------------------------------------------------------------
typedef struct {
    // --- life cycle ---------------------------------------------------------
    void (*on_atr)(void);                           // after the ATR went out
    void (*on_deactivate)(void);                    // RST fell / session ended

    // --- file system / access control ---------------------------------------
    // Override the access condition bytes.  op = FS_OP_READ / FS_OP_UPDATE.
    bool (*check_access)(uint8_t channel, const fs_file_t *f, uint8_t op);
    void (*on_select)(uint8_t channel, const fs_file_t *f);
    void (*on_file_changed)(const fs_file_t *f);    // after UPDATE/WRITE/ERASE/APPEND

    // --- commands -------------------------------------------------------------
    // T=0 only: tell which case an INS not in any table has.
    apdu_case_t (*classify)(uint8_t cla, uint8_t ins, uint8_t p1, uint8_t p2);
    // Last resort for commands nobody handled (return APDU_UNHANDLED for 6D00).
    uint16_t (*unhandled)(const apdu_t *cmd, apdu_resp_t *resp);

    uint16_t (*get_data)(const apdu_t *cmd, apdu_resp_t *resp);               // 00 CA
    uint16_t (*put_data)(const apdu_t *cmd, apdu_resp_t *resp);               // 00 DA
    uint16_t (*internal_authenticate)(const apdu_t *cmd, apdu_resp_t *resp);  // 00 88
    uint16_t (*external_authenticate)(const apdu_t *cmd, apdu_resp_t *resp);  // 00 82
    uint16_t (*manage_security_environment)(const apdu_t *cmd, apdu_resp_t *resp); // 00 22
    uint16_t (*perform_security_operation)(const apdu_t *cmd, apdu_resp_t *resp);  // 00 2A
    uint16_t (*generate_asymmetric_key_pair)(const apdu_t *cmd, apdu_resp_t *resp);// 00 46
    uint16_t (*create_file)(const apdu_t *cmd, apdu_resp_t *resp);            // 00 E0
    uint16_t (*delete_file)(const apdu_t *cmd, apdu_resp_t *resp);            // 00 E4

    // --- secure messaging (CLA b4 b3 != 00) -----------------------------------
    // Unwrap: decode the SM data objects in cmd->data, place the plain command data in scratch and point cmd->data / lc
    // / le at it.
    uint16_t (*sm_unwrap)(apdu_t *cmd, uint8_t *scratch, size_t cap);
    // Wrap: replace resp (and possibly the SW) by the protected response.
    uint16_t (*sm_wrap)(const apdu_t *cmd, apdu_resp_t *resp, uint16_t sw);

    // --- PIN storage ----------------------------------------------------------------------------------------------
    // Transform reference data before it is stored (CHANGE REFERENCE DATA, RESET RETRY COUNTER) and before a
    // candidate is compared (VERIFY, the PUK, ...), e.g. a salted hash so that no PIN sits in flash in clear.  Return
    // the stored length (0 = reject).  The initial values in the profile must already be in the stored form, and
    // sec_pin_t.plain_len must give the fixed plaintext length of the PIN and PUK (the commands that carry an old and
    // a new value in one data field split them there).  NULL = store and compare the plain value.
    size_t (*pin_transform)(const sec_pin_t *pin, const uint8_t *in, size_t len, uint8_t *out, size_t cap);

    // --- misc -----------------------------------------------------------------
    void (*random_bytes)(uint8_t *out, size_t n);   // default: hardware RNG
} card_hooks_t;

// ---------------------------------------------------------------------------
// The profile.
// ---------------------------------------------------------------------------
typedef struct {
    // --- ISO 7816-3 -----------------------------------------------------------
    const uint8_t *atr;             // raw ATR to send (e.g. copied from a real card) ...
    size_t atr_len;
    const atr_desc_t *atr_desc;     // ... or a description to build one from (atr == NULL)
    bool allow_pps;                 // answer PPS requests (ignored in specific mode)
    bool (*pps_accept)(uint16_t f, uint8_t d, uint8_t t);  // NULL = accept what the PIO can do
    uint8_t tx_extra_guard;         // extra guard ETUs after our own characters (0..20)

    // --- ISO 7816-4 -----------------------------------------------------------
    const applet_t *const *applets;
    size_t n_applets;
    const applet_t *default_applet; // implicitly selected after ATR (may be NULL)
    const fs_file_t *files;         // file tree, files[0] = MF (NULL = no file system)
    size_t n_files;
    sec_pin_t *pins;                // PIN objects (NULL = none)
    size_t n_pins;
    bool iso_commands;              // serve the interindustry commands of apdu/iso_cmds.c
    uint8_t max_channels;           // 1..CARD_MAX_CHANNELS logical channels
    bool proprietary_cla_decode;    // also decode channel / chaining / SM bits of CLA 8X..FX

    card_hooks_t hooks;

    // --- persistence -----------------------------------------------------------
    void *nvm_blob;                 // RAM image saved to flash when marked dirty
    size_t nvm_size;
} card_profile_t;

extern const card_profile_t card_profile;

// ---------------------------------------------------------------------------
// Services for handlers.
// ---------------------------------------------------------------------------

// Tell the reader to keep waiting (T=0: NULL byte, T=1: S(WTX)).  Call it from handlers that take longer than ~1 s.
void card_keepalive(void);

// Protocol in use for the current session (0 or 1).
uint8_t card_protocol(void);

// Mark the NVM blob changed; it is written to flash once the session ends.
void card_nvm_changed(void);

// Reboot into BOOTSEL (UF2 drive) once the current response has been sent.
void card_request_bootsel(void);
bool card_bootsel_requested(void);

// Default FCI for an applet: 6F len { 84 len AID }.
void card_put_default_fci(apdu_resp_t *resp, const uint8_t *aid, size_t aid_len);
