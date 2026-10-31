// ISO 7816-4 file system: MF / DF / EF (transparent, linear fixed, linear variable, cyclic), SELECT and the BINARY /
// RECORD commands.
//
// The tree is a static table supplied by the card profile; the engine keeps a cursor (current DF / EF / record) per
// logical channel.
#pragma once

#include "apdu.h"

// File descriptor byte values (ISO 7816-4 table 14, b3..b1 for EFs).
typedef enum {
    FS_DF                 = 0x38,
    FS_EF_TRANSPARENT     = 0x01,
    FS_EF_LINEAR_FIXED    = 0x02,
    FS_EF_LINEAR_VARIABLE = 0x04,
    FS_EF_CYCLIC          = 0x06,
} fs_type_t;

// Access conditions: ALWAYS, NEVER, or the reference (P2 of VERIFY) of the PIN that must have been verified on the
// channel.
#define FS_AC_ALWAYS 0x00
#define FS_AC_NEVER  0xFF

#define FS_OP_READ   0
#define FS_OP_UPDATE 1

typedef struct fs_file {
    uint16_t fid;
    uint8_t type;                   // fs_type_t
    const struct fs_file *parent;   // NULL only for the MF
    const uint8_t *name;            // DF name / AID (DFs only, may be NULL)
    uint8_t name_len;
    uint8_t *data;                  // EF body (see layout below)
    uint32_t size;                  // bytes available in data
    uint16_t rec_size;              // record EFs: record length (max. for variable)
    uint8_t sfi;                    // short EF identifier 1..30, 0 = none
    uint8_t ac_read;                // FS_AC_* or PIN reference
    uint8_t ac_update;
} fs_file_t;

// Record EF storage layout inside `data`:
//   [0..1] number of records in use (little endian)
//   [2..3] cyclic EFs: slot index of the most recent record
//   [4..]  slots; a slot is rec_size bytes (fixed / cyclic) or
//          2 bytes length + rec_size bytes (linear variable)
#define FS_REC_HDR_SIZE 4
#define FS_REC_HDR(used, head) (uint8_t)((used) & 0xFF), (uint8_t)((used) >> 8), \
                               (uint8_t)((head) & 0xFF), (uint8_t)((head) >> 8)
#define FS_LINEAR_FIXED_BYTES(rec_size, count)    (FS_REC_HDR_SIZE + (rec_size) * (count))
#define FS_LINEAR_VARIABLE_BYTES(rec_size, count) (FS_REC_HDR_SIZE + ((rec_size) + 2) * (count))
#define FS_CYCLIC_BYTES(rec_size, count)          (FS_REC_HDR_SIZE + (rec_size) * (count))

typedef struct {
    const fs_file_t *df;      // current DF (never NULL once initialised)
    const fs_file_t *ef;      // current EF or NULL
    uint16_t record;          // current record number (1-based), 0 = none
} fs_cursor_t;

void fs_init(const fs_file_t *files, size_t n);     // files[0] must be the MF
void fs_reset(void);                                // all cursors back to the MF
void fs_cursor_reset(uint8_t ch);
fs_cursor_t *fs_cursor(uint8_t ch);
bool fs_present(void);

// Lookups
const fs_file_t *fs_mf(void);
const fs_file_t *fs_child(const fs_file_t *df, uint16_t fid);
const fs_file_t *fs_by_name(const uint8_t *name, size_t len, bool partial,
                            const fs_file_t *after);
const fs_file_t *fs_by_sfi(const fs_file_t *df, uint8_t sfi);
const fs_file_t *fs_by_path(const fs_file_t *from, const uint8_t *path, size_t len);

// Make a file current on a channel (as SELECT would, without a response).
void fs_select_file(uint8_t ch, const fs_file_t *f);

// Build the FCP (62 ...) / FCI (6F ...) / FMD (64 ...) of a file.
void fs_put_fcp(apdu_resp_t *resp, const fs_file_t *f, uint8_t template_tag);

// Records: n is 1-based.  Return false when the record does not exist.
uint16_t fs_record_count(const fs_file_t *ef);
bool fs_record_read(const fs_file_t *ef, uint16_t n, const uint8_t **out, uint16_t *len);
bool fs_record_write(const fs_file_t *ef, uint16_t n, const uint8_t *in, uint16_t len);
bool fs_record_append(const fs_file_t *ef, const uint8_t *in, uint16_t len, uint16_t *n_out);

// Access condition evaluation (hook first, then the PIN rule).
bool fs_access_ok(uint8_t ch, const fs_file_t *f, uint8_t op);

// Interindustry command handlers (apdu_handler_t).
uint16_t fs_cmd_select(const apdu_t *cmd, apdu_resp_t *resp);         // A4
uint16_t fs_cmd_read_binary(const apdu_t *cmd, apdu_resp_t *resp);    // B0
uint16_t fs_cmd_write_binary(const apdu_t *cmd, apdu_resp_t *resp);   // D0
uint16_t fs_cmd_update_binary(const apdu_t *cmd, apdu_resp_t *resp);  // D6
uint16_t fs_cmd_erase_binary(const apdu_t *cmd, apdu_resp_t *resp);   // 0E
uint16_t fs_cmd_read_record(const apdu_t *cmd, apdu_resp_t *resp);    // B2
uint16_t fs_cmd_write_record(const apdu_t *cmd, apdu_resp_t *resp);   // D2
uint16_t fs_cmd_update_record(const apdu_t *cmd, apdu_resp_t *resp);  // DC
uint16_t fs_cmd_append_record(const apdu_t *cmd, apdu_resp_t *resp);  // E2
uint16_t fs_cmd_erase_record(const apdu_t *cmd, apdu_resp_t *resp);   // 0C
uint16_t fs_cmd_search_record(const apdu_t *cmd, apdu_resp_t *resp);  // A2
