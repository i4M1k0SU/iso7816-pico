// ============================================================================
//  CARD PROFILE  --  this is the file to edit to make the card yours.
//
//  Sections:
//    1. ATR and transmission parameters   (ISO 7816-3)
//    2. Persistent data (NVM blob)
//    3. File system                        (ISO 7816-4)
//    4. PINs                               (ISO 7816-4 security)
//    5. Applets                            (SELECT by AID)
//    6. Hooks                              (GET DATA, authentication, SM ...)
//    7. Console commands
//    8. The profile itself (demo_card_profile, handed to the framework by main.c)
// ============================================================================
#include "iso7816_pico/card.h"

#include "applets/demo_applet.h"
#include "iso7816_pico/log.h"

#include <stdio.h>
#include <string.h>

// ----------------------------------------------------------------------------
// 1. ATR
//
// Either paste a raw ATR (`.atr` / `.atr_len`, e.g. copied from a real card) or describe one; the framework reads the
// protocol, F/D, guard time, T=1 parameters etc. back out of the bytes, so both ways configure the same things.
// ----------------------------------------------------------------------------

// T=0 only, default F/D, 8 historical bytes "Pico7816":
//   3B 08 50 69 63 6F 37 38 31 36
static const atr_desc_t s_atr_t0 __attribute__((unused)) = {
    .inverse = false,
    .levels = 1,
    .level = { { .present = 0 } },
    .historical = { 'P', 'i', 'c', 'o', '7', '8', '1', '6' },
    .historical_len = 8,
};

// T=1 only, with IFSC = 254 and LRC:
//   3B 88 81 31 FE 45 50 69 63 6F 37 38 31 36 xx
static const atr_desc_t s_atr_t1 __attribute__((unused)) = {
    .levels = 3,
    .level = {
        { .present = 0 },                                   // level 1: defaults
        { .present = 0, .protocol = 1 },                    // TD1 -> T=1
        { .present = ATR_TA | ATR_TB, .ta = 0xFE, .tb = 0x45, .protocol = 1 },  // IFSC, CWI/BWI
    },
    .historical = { 'P', 'i', 'c', 'o', '7', '8', '1', '6' },
    .historical_len = 8,
};

// T=0 first and T=1 as alternative, plus TA1 = 0x13 (F=372, D=4): readers that support it negotiate 4x speed with a
// PPS.
static const atr_desc_t s_atr_negotiable __attribute__((unused)) = {
    .levels = 3,
    .level = {
        { .present = ATR_TA, .ta = 0x13 },
        { .present = 0, .protocol = 0 },                    // TD1 -> T=0
        { .present = ATR_TA | ATR_TB, .ta = 0xFE, .tb = 0x45, .protocol = 1 },  // TD2 -> T=1
    },
    .historical = { 'P', 'i', 'c', 'o', '7', '8', '1', '6' },
    .historical_len = 8,
};

// All three were verified with an SCR3310 under Windows (T=0, T=1, and the negotiable one ends up at T=1 with F/D =
// 372/4 after a PPS).  The build can pick one without editing this file: `cmake -DDEMO_ATR=t1` (t0 / t1 / negotiable)
// defines DEMO_ATR_IN_USE (see CMakeLists.txt; tools/hw_regression.ps1 uses it); otherwise the #define below applies.
#ifdef DEMO_ATR_IN_USE
#define ATR_IN_USE DEMO_ATR_IN_USE
#else
#define ATR_IN_USE s_atr_t0 // <- pick s_atr_t0 / s_atr_t1 / s_atr_negotiable
#endif

// ----------------------------------------------------------------------------
// 2. Persistent data
//
// Everything in this struct survives power cycles: it is loaded from flash at boot and written back when the card is
// idle after something changed (UPDATE BINARY, ...); PIN / PUK counters are saved at once on every try.
//
// Bump NVM_VERSION whenever this struct changes (members, sizes, meaning): the flash image written by the previous
// firmware is then not reinterpreted.  Without hooks.nvm_migrate the card starts from these defaults (once, logged);
// with it, the hook can carry the old data over (see card.h).  A size change alone is caught too, but a change that
// keeps the size is not: that is what the version is for.
// ----------------------------------------------------------------------------

#define NVM_VERSION   0

#define EF_HELLO_SIZE 256

typedef struct {
    uint8_t ef_hello[EF_HELLO_SIZE];                   // transparent EF 0101
    uint8_t ef_records[FS_LINEAR_FIXED_BYTES(16, 4)];  // linear fixed EF 0102, 4 x 16 bytes
    uint8_t ef_log[FS_CYCLIC_BYTES(8, 5)];             // cyclic EF 0103, 5 x 8 bytes
    uint8_t ef_notes[FS_LINEAR_VARIABLE_BYTES(32, 3)]; // linear variable EF 0104
    sec_pin_t pins[2];
} nvm_data_t;

_Static_assert(NVM_BLOB_FITS(sizeof(nvm_data_t)), "the NVM journal needs two slots: raise NVM_FLASH_BYTES");

static nvm_data_t s_nvm = {
    .ef_hello = "Hello from iso7816-pico! This 256-byte file lives in the card's NVM; "
                "VERIFY the PIN 1234 and UPDATE BINARY to change it.",
    .ef_records = {
        FS_REC_HDR(2, 0),
        'r', 'e', 'c', 'o', 'r', 'd', ' ', '1', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
        'r', 'e', 'c', 'o', 'r', 'd', ' ', '2', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
    },
    .ef_log = { FS_REC_HDR(0, 0) },
    .ef_notes = { FS_REC_HDR(0, 0) },
    .pins = {
        { .ref = 0x01, .value = "1234", .len = 4, .min_len = 4, .max_len = 8,
          .max_retries = 3, .retries = 3,
          .puk = "12345678", .puk_len = 8, .puk_max_retries = 10, .puk_retries = 10 },
        { .ref = 0x81, .value = "0000", .len = 4, .min_len = 4, .max_len = 8,
          .max_retries = 3, .retries = 3 },
    },
};

// ----------------------------------------------------------------------------
// 3. File system
//
//   MF 3F00
//    └─ DF 7F10  (name = F0 "PICO", the demo applet's AID)
//        ├─ EF 0101  transparent, 256 B, SFI 1   read: always   update: PIN 01
//        ├─ EF 0102  linear fixed 4 x 16, SFI 2  read: always   update: PIN 01
//        ├─ EF 0103  cyclic 5 x 8, SFI 3         read: always   update: PIN 01
//        └─ EF 0104  linear variable 3 x 32      read: PIN 81   update: PIN 81
// ----------------------------------------------------------------------------

static const uint8_t s_df_name[] = { 0xF0, 'P', 'I', 'C', 'O' };

// clang-format off
static const fs_file_t s_files[] = {
    [0] = { .fid = 0x3F00, .type = FS_DF, .parent = NULL },
    [1] = { .fid = 0x7F10, .type = FS_DF, .parent = &s_files[0],
            .name = s_df_name, .name_len = sizeof s_df_name },
    [2] = { .fid = 0x0101, .type = FS_EF_TRANSPARENT, .parent = &s_files[1],
            .data = s_nvm.ef_hello, .size = sizeof s_nvm.ef_hello, .sfi = 1,
            .ac_read = FS_AC_ALWAYS, .ac_update = 0x01 },
    [3] = { .fid = 0x0102, .type = FS_EF_LINEAR_FIXED, .parent = &s_files[1],
            .data = s_nvm.ef_records, .size = sizeof s_nvm.ef_records, .rec_size = 16, .sfi = 2,
            .ac_read = FS_AC_ALWAYS, .ac_update = 0x01 },
    [4] = { .fid = 0x0103, .type = FS_EF_CYCLIC, .parent = &s_files[1],
            .data = s_nvm.ef_log, .size = sizeof s_nvm.ef_log, .rec_size = 8, .sfi = 3,
            .ac_read = FS_AC_ALWAYS, .ac_update = 0x01 },
    [5] = { .fid = 0x0104, .type = FS_EF_LINEAR_VARIABLE, .parent = &s_files[1],
            .data = s_nvm.ef_notes, .size = sizeof s_nvm.ef_notes, .rec_size = 32,
            .ac_read = 0x81, .ac_update = 0x81 },
};
// clang-format on

// ----------------------------------------------------------------------------
// 5. Applets
// ----------------------------------------------------------------------------

static const applet_t *const s_applets[] = {
    &demo_applet,
};

// ----------------------------------------------------------------------------
// 6. Hooks  (delete the ones you do not need; NULL = default behaviour)
// ----------------------------------------------------------------------------

// 00 CA P1 P2 Le: P1P2 = 0100 -> card serial, 0200 -> firmware name.
static uint16_t hook_get_data(const apdu_t *cmd, apdu_resp_t *resp) {
    switch (apdu_p1p2(cmd)) {
    case 0x0100: {
        static const uint8_t serial[] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77 };
        resp_put(resp, serial, sizeof serial);
        return SW_OK;
    }
    case 0x0200:
        resp_put(resp, "iso7816-pico", 12);
        return SW_OK;
    default:
        return SW_REF_DATA_NOT_FOUND;
    }
}

// 00 88 00 00 Lc <challenge> Le: placeholder "signature" = challenge XOR 5A. Replace with real cryptography (pico-sdk
// ships mbedtls: link pico_mbedtls).
static uint16_t hook_internal_authenticate(const apdu_t *cmd, apdu_resp_t *resp) {
    for (uint32_t i = 0; i < cmd->lc; i++) resp_put_u8(resp, cmd->data[i] ^ 0x5A);
    return SW_OK;
}

// 00 82 00 00 Lc <cryptogram>: accept if it equals the last GET CHALLENGE output XOR 5A (same placeholder "algorithm"
// as above).
static uint16_t hook_external_authenticate(const apdu_t *cmd, apdu_resp_t *resp) {
    (void)resp;
    size_t n;
    const uint8_t *ch = sec_last_challenge(&n);
    if (!n) return SW_CONDITIONS_NOT_SAT; // GET CHALLENGE first
    if (cmd->lc != n) return SW_WRONG_LENGTH;
    for (size_t i = 0; i < n; i++) {
        if ((ch[i] ^ 0x5A) != cmd->data[i]) return SW_SECURITY_STATUS;
    }
    sec_set_verified(cmd->channel, 0x81, true); // counts as the app PIN
    return SW_OK;
}

// ----------------------------------------------------------------------------
// 7. Console commands  (extra lines on the USB serial console; `help` lists them)
// ----------------------------------------------------------------------------

// `dump`: show what the demo card keeps in NVM.
static void cmd_dump(const char *args) {
    (void)args;
    char hex[3 * 16 + 1] = "";
    for (int i = 0; i < 16; i++) sprintf(hex + strlen(hex), i ? " %02X" : "%02X", s_nvm.ef_hello[i]);
    log_printf("EF 0101[0..15]: %s", hex);
    log_printf("EF 0103 (cyclic): %u record(s) in use", s_nvm.ef_log[0] | (s_nvm.ef_log[1] << 8));
    for (size_t i = 0; i < 2; i++) {
        log_printf("PIN %02X: %u/%u tries left%s", s_nvm.pins[i].ref, s_nvm.pins[i].retries, s_nvm.pins[i].max_retries,
                   s_nvm.pins[i].disabled ? " (verification disabled)" : "");
    }
}

// `unblock`: reset the PIN retry counters without the PUK (a development convenience; remove it from a real card).
static void cmd_unblock(const char *args) {
    (void)args;
    for (size_t i = 0; i < 2; i++) s_nvm.pins[i].retries = s_nvm.pins[i].max_retries;
    card_nvm_changed();
    log_printf("PIN retry counters reset (saved when the card is idle)");
}

static const console_cmd_t s_console_cmds[] = {
    { "dump", "show the demo card's NVM (EF 0101, cyclic EF, PIN counters)", cmd_dump },
    { "unblock", "reset the PIN retry counters", cmd_unblock },
};

// ----------------------------------------------------------------------------
// 8. The profile
// ----------------------------------------------------------------------------

const card_profile_t demo_card_profile = {
    // identity (console banner, `card` command, the demo's GET VERSION)
    .name = "Pico7816 demo",
    .version = "1.0",

    // ISO 7816-3
    .atr = NULL, .atr_len = 0,          // raw ATR, or ...
    .atr_desc = &ATR_IN_USE,            // ... described ATR
    .allow_pps = true,
    .pps_accept = NULL,                 // accept every F/D the PIO can do
    .tx_extra_guard = 0,

    // ISO 7816-4
    .applets = s_applets,
    .n_applets = sizeof s_applets / sizeof s_applets[0],
    .default_applet = &demo_applet,     // implicitly selected after the ATR (NULL = none)
    .files = s_files,
    .n_files = sizeof s_files / sizeof s_files[0],
    .pins = s_nvm.pins,
    .n_pins = 2,
    .iso_commands = true,
    .max_channels = 4,
    .proprietary_cla_decode = false,    // CLA 90 is not "80 + chaining" here

    .hooks = {
        .get_data = hook_get_data,
        .internal_authenticate = hook_internal_authenticate,
        .external_authenticate = hook_external_authenticate,
        // .sm_unwrap / .sm_wrap / .perform_security_operation / .nvm_migrate / ... : see card.h
    },

    // persistence
    .nvm_blob = &s_nvm,
    .nvm_size = sizeof s_nvm,
    .nvm_version = NVM_VERSION,

    // console
    .console_cmds = s_console_cmds,
    .n_console_cmds = sizeof s_console_cmds / sizeof s_console_cmds[0],
};
