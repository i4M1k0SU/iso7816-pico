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

#include "iso7816_pico/apdu.h"
#include "iso7816_pico/fs.h"
#include "iso7816_pico/sec.h"
#include "iso7816_pico/atr.h"

// ---------------------------------------------------------------------------
// Applet: an application selectable by AID (ISO 7816-4 SELECT by DF name).
// ---------------------------------------------------------------------------
typedef struct applet {
    const char *name;
    const uint8_t *aid;
    uint8_t aid_len;               // 1..16 (ISO 7816-4)

    // Opaque, for the application: e.g. the instance state when one implementation serves several applets / AIDs.
    // Handlers reach it as cmd->applet->ctx, the life-cycle callbacks as self->ctx.  The framework never touches it.
    void *ctx;

    // Command table: classifies commands for T=0 and, where `handler` is set, routes them.  Commands without a handler
    // (or not in the table) go to `process`.
    const apdu_cmd_desc_t *cmds;
    size_t n_cmds;

    // Optional.  T=0 only: the case (whether P3 is Lc or Le) of a command that is not in `cmds`, for an applet that
    // decodes its commands in `process` (a switch on INS).  Consulted for the applet selected on the channel the CLA
    // addresses, after its table and before the interindustry table and hooks.classify.  Return APDU_CASE_UNKNOWN for
    // "not mine".  Without it such a command is case 1 under T=0: P3 is ignored and the data field is not received.
    // `self` is this applet.
    apdu_case_t (*classify)(const struct applet *self, uint8_t cla, uint8_t ins, uint8_t p1, uint8_t p2);

    // Optional.  Called by SELECT (and by the implicit selections: the default applet after every ATR, MANAGE CHANNEL
    // open, which pass a SELECT by DF name with P2 = 0C and discard the response).  The DF named like the AID (if any)
    // is already current on cmd->channel; for a channel opened from a channel other than the basic one, the current
    // files of that channel are, and they are put back after select().  Return the SW and put the FCI in resp.  9000
    // or a warning (62xx / 63xx) keeps the applet selected; any other SW leaves the channel with no applet selected
    // (the previous one was already deselected, the security state is cleared, the current file is unchanged by the
    // SELECT, the response data is dropped; a failing MANAGE CHANNEL open does not open the channel).  When NULL the
    // framework answers 6F len { 84 len AID } (unless P2 says "no response data").  cmd->applet is this applet.
    uint16_t (*select)(const apdu_t *cmd, apdu_resp_t *resp);

    // Optional.  Another applet or DF is being selected on this channel, or the channel is being closed.  `self` is
    // this applet.
    void (*deselect)(const struct applet *self, uint8_t channel);

    // Optional.  Catch-all for commands without a table handler.  Return APDU_UNHANDLED to let the interindustry
    // commands / hooks try.  A switch-style applet should also provide `classify` (T=0).
    uint16_t (*process)(const apdu_t *cmd, apdu_resp_t *resp);

    // Optional.  Called after every ATR (volatile state back to defaults), for every applet of the profile.  `self` is
    // this applet.
    void (*reset)(const struct applet *self);
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
    // T=0 only: tell which case an INS not in any table (nor known to the applet's classify) has.
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

    // --- persistence ------------------------------------------------------------------------------------------------
    // The newest image in flash was written with another layout id (old_layout = NVM_LAYOUT_FRAMEWORK << 16 |
    // nvm_version of the firmware that wrote it; 0 for an image from before layout ids existed) or another size.
    // old_blob (old_size bytes, read-only, in flash, 8-byte aligned) is that image; nvm_blob still holds the defaults.
    // Convert what can be kept into nvm_blob and return true to use it (it is saved soon after), or return false
    // without touching nvm_blob to start from the defaults.  NULL = always the defaults.  Runs at boot, before the
    // file system and the PINs are initialised.
    bool (*nvm_migrate)(uint32_t old_layout, const void *old_blob, size_t old_size);

    // --- misc -----------------------------------------------------------------
    void (*random_bytes)(uint8_t *out, size_t n);   // default: hardware RNG
} card_hooks_t;

// ---------------------------------------------------------------------------
// Console command: a line "name [arguments]" typed on the USB serial console (src/pico/sys/console.c) runs the handler
// with the rest of the line (trimmed, possibly empty); it reports through log_printf() (iso7816_pico/log.h).  The
// framework's own names (help status card trace gpio pins apdu save erase bootsel reboot) take precedence.  Handlers
// run in the idle loop, i.e. between commands and while the card waits for the reader; keep them short or call
// watchdog_update() / log_drain() in long loops (see the `gpio` command for an example).
// ---------------------------------------------------------------------------
typedef struct {
    const char *name;               // matched exactly (case-sensitive); up to 8 characters keep the help aligned
    const char *help;               // one line for the `help` listing (shown under the card's name)
    void (*handler)(const char *args);
} console_cmd_t;

// ---------------------------------------------------------------------------
// The profile.
// ---------------------------------------------------------------------------
typedef struct {
    // --- identity ---------------------------------------------------------------
    // Shown in the console banner and by the `card` command.  NULL falls back to the program name / version given
    // to CMake (pico_set_program_name / pico_set_program_version).
    const char *name;
    const char *version;

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
    const applet_t *default_applet; // implicitly selected (select() runs) after ATR (may be NULL)
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
    // Layout version of the blob: bump it whenever the struct behind nvm_blob changes, so that the images written by
    // the previous firmware are not reinterpreted (they go to hooks.nvm_migrate, or the defaults are used).
    uint16_t nvm_version;

    // --- console ---------------------------------------------------------------
    const console_cmd_t *console_cmds;   // extra console commands (NULL = none)
    size_t n_console_cmds;
} card_profile_t;

// The profile in use.  Set by the runtime (card_runtime_init) or, on the host, by the tests before anything else runs.
extern const card_profile_t *card_profile;

// ---------------------------------------------------------------------------
// Services for handlers.
// ---------------------------------------------------------------------------

// Tell the reader to keep waiting (T=0: NULL byte, T=1: S(WTX)).  Call it from handlers that take longer than ~1 s.
void card_keepalive(void);

// Protocol in use for the current session (0 or 1).
uint8_t card_protocol(void);

// Mark the NVM blob changed; it is written to flash once the card is idle (NVM_IDLE_SAVE_MS without traffic) or RST
// goes low.
void card_nvm_changed(void);

// Reboot into BOOTSEL (UF2 drive) once the current response has been sent.
void card_request_bootsel(void);
bool card_bootsel_requested(void);

// Default FCI for an applet: 6F len { 84 len AID }.
void card_put_default_fci(apdu_resp_t *resp, const uint8_t *aid, size_t aid_len);
