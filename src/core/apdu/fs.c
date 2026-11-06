#include "iso7816_pico/fs.h"

#include "apdu/profile_check.h"
#include "hal/nvm.h"
#include "iso7816_pico/card.h"
#include "iso7816_pico/sec.h"

#include <string.h>

static const fs_file_t *s_files;
static size_t s_n;
static fs_cursor_t s_cursor[CARD_MAX_CHANNELS];

// ---------------------------------------------------------------------------
// setup / cursors
// ---------------------------------------------------------------------------

// A file profile_file_defect() objects to (it would make the commands access memory outside the profile's objects;
// profile_check() logs it at boot) is skipped by the lookups below, so it can never become current.  Without a usable
// MF there is no file system.
void fs_init(const fs_file_t *files, size_t n) {
    s_files = files;
    s_n = files && n && !profile_file_defect(files, n, &files[0]) ? n : 0;
    fs_reset();
}

static bool usable(const fs_file_t *f) { return !profile_file_defect(s_files, s_n, f); }

bool fs_present(void) { return s_files && s_n; }

const fs_file_t *fs_mf(void) { return fs_present() ? &s_files[0] : NULL; }

void fs_cursor_reset(uint8_t ch) {
    if (ch >= CARD_MAX_CHANNELS) return;
    s_cursor[ch].df = fs_mf();
    s_cursor[ch].ef = NULL;
    s_cursor[ch].record = 0;
}

void fs_reset(void) {
    for (uint8_t ch = 0; ch < CARD_MAX_CHANNELS; ch++) fs_cursor_reset(ch);
}

fs_cursor_t *fs_cursor(uint8_t ch) { return &s_cursor[ch < CARD_MAX_CHANNELS ? ch : 0]; }

static bool is_df(const fs_file_t *f) { return f && f->type == FS_DF; }
static bool is_record_ef(const fs_file_t *f) {
    return f && (f->type == FS_EF_LINEAR_FIXED || f->type == FS_EF_LINEAR_VARIABLE || f->type == FS_EF_CYCLIC);
}

// ---------------------------------------------------------------------------
// lookups
// ---------------------------------------------------------------------------

const fs_file_t *fs_child(const fs_file_t *df, uint16_t fid) {
    for (size_t i = 0; i < s_n; i++) {
        if (s_files[i].parent == df && s_files[i].fid == fid && usable(&s_files[i])) return &s_files[i];
    }
    return NULL;
}

const fs_file_t *fs_by_name(const uint8_t *name, size_t len, bool partial, const fs_file_t *after) {
    bool passed = after == NULL;
    for (size_t i = 0; i < s_n; i++) {
        const fs_file_t *f = &s_files[i];
        if (!passed) {
            if (f == after) passed = true;
            continue;
        }
        if (!is_df(f) || !f->name || len == 0) continue;
        if (partial ? (len <= f->name_len && memcmp(f->name, name, len) == 0)
                    : (len == f->name_len && memcmp(f->name, name, len) == 0)) {
            if (usable(f)) return f;
        }
    }
    return NULL;
}

// Short EF identifiers are 1..30: 0 means "no SFI" in fs_file_t (and "current EF" in the commands), 31 is reserved.
const fs_file_t *fs_by_sfi(const fs_file_t *df, uint8_t sfi) {
    if (sfi == 0 || sfi > 30) return NULL;
    for (size_t i = 0; i < s_n; i++) {
        if (s_files[i].parent == df && s_files[i].sfi == sfi && !is_df(&s_files[i]) && usable(&s_files[i])) {
            return &s_files[i];
        }
    }
    return NULL;
}

const fs_file_t *fs_by_path(const fs_file_t *from, const uint8_t *path, size_t len) {
    if (len == 0 || len % 2) return NULL;
    const fs_file_t *f = from;
    for (size_t i = 0; i < len; i += 2) {
        uint16_t fid = (uint16_t)((path[i] << 8) | path[i + 1]);
        if (i == 0 && fid == 0x3F00 && from == fs_mf()) continue; // leading MF id
        f = fs_child(f, fid);
        if (!f) return NULL;
    }
    return f;
}

void fs_select_file(uint8_t ch, const fs_file_t *f) {
    fs_cursor_t *c = fs_cursor(ch);
    if (is_df(f)) {
        c->df = f;
        c->ef = NULL;
    } else {
        c->df = f->parent;
        c->ef = f;
    }
    c->record = 0;
    if (card_profile->hooks.on_select) card_profile->hooks.on_select(ch, f);
}

// ---------------------------------------------------------------------------
// records
// ---------------------------------------------------------------------------

static uint16_t rec_used(const fs_file_t *ef) { return (uint16_t)(ef->data[0] | (ef->data[1] << 8)); }
static uint16_t rec_head(const fs_file_t *ef) { return (uint16_t)(ef->data[2] | (ef->data[3] << 8)); }
static void rec_set_used(const fs_file_t *ef, uint16_t v) {
    ef->data[0] = (uint8_t)v;
    ef->data[1] = (uint8_t)(v >> 8);
}
static void rec_set_head(const fs_file_t *ef, uint16_t v) {
    ef->data[2] = (uint8_t)v;
    ef->data[3] = (uint8_t)(v >> 8);
}
static uint16_t slot_size(const fs_file_t *ef) {
    return (uint16_t)(ef->rec_size + (ef->type == FS_EF_LINEAR_VARIABLE ? 2 : 0));
}
static uint16_t rec_capacity(const fs_file_t *ef) {
    if (!is_record_ef(ef) || !ef->rec_size || ef->size < FS_REC_HDR_SIZE) return 0;
    return (uint16_t)((ef->size - FS_REC_HDR_SIZE) / slot_size(ef));
}
static uint8_t *slot_ptr(const fs_file_t *ef, uint16_t slot) {
    return ef->data + FS_REC_HDR_SIZE + (uint32_t)slot * slot_size(ef);
}

uint16_t fs_record_count(const fs_file_t *ef) {
    if (!is_record_ef(ef)) return 0;
    if (ef->type == FS_EF_LINEAR_FIXED) return rec_capacity(ef); // fully allocated: every record exists
    uint16_t used = rec_used(ef), cap = rec_capacity(ef);
    return used < cap ? used : cap;
}

// Record number (1-based) -> slot index; false for a record that does not exist (linear variable / cyclic: beyond the
// records in use).  Cyclic EFs number from the most recently written record.
static bool rec_slot(const fs_file_t *ef, uint16_t n, uint16_t *slot) {
    uint16_t cap = rec_capacity(ef);
    if (n == 0 || n > fs_record_count(ef) || cap == 0) return false;
    if (ef->type == FS_EF_CYCLIC) {
        *slot = (uint16_t)((rec_head(ef) + cap - (n - 1)) % cap);
    } else {
        *slot = (uint16_t)(n - 1);
    }
    return true;
}

bool fs_record_read(const fs_file_t *ef, uint16_t n, const uint8_t **out, uint16_t *len) {
    uint16_t slot;
    if (!rec_slot(ef, n, &slot)) return false;
    uint8_t *p = slot_ptr(ef, slot);
    if (ef->type == FS_EF_LINEAR_VARIABLE) {
        *len = (uint16_t)(p[0] | (p[1] << 8));
        if (*len > ef->rec_size) *len = ef->rec_size;
        *out = p + 2;
    } else {
        *len = ef->rec_size;
        *out = p;
    }
    return true;
}

// Overwrite an existing record.  Only fs_record_append() adds records.
bool fs_record_write(const fs_file_t *ef, uint16_t n, const uint8_t *in, uint16_t len) {
    uint16_t slot;
    if (!rec_slot(ef, n, &slot) || len > ef->rec_size) return false;
    uint8_t *p = slot_ptr(ef, slot);
    if (ef->type == FS_EF_LINEAR_VARIABLE) {
        p[0] = (uint8_t)len;
        p[1] = (uint8_t)(len >> 8);
        p += 2;
    } else {
        memset(p, 0, ef->rec_size);
    }
    if (len) memcpy(p, in, len);
    // Linear fixed: every record exists; "in use" is only the high-water mark APPEND RECORD continues from.
    if (ef->type == FS_EF_LINEAR_FIXED && n > rec_used(ef)) rec_set_used(ef, n);
    return true;
}

bool fs_record_erase(const fs_file_t *ef, uint16_t n) {
    uint16_t slot;
    if (!rec_slot(ef, n, &slot)) return false;
    // Erasing does not create a record: the number of records in use and the cyclic head stay as they are.
    memset(slot_ptr(ef, slot), 0, slot_size(ef)); // linear variable: the stored length becomes 0 as well
    return true;
}

bool fs_record_append(const fs_file_t *ef, const uint8_t *in, uint16_t len, uint16_t *n_out) {
    uint16_t cap = rec_capacity(ef);
    if (!cap || len > ef->rec_size) return false;
    uint16_t used = rec_used(ef);
    if (ef->type == FS_EF_CYCLIC) {
        uint16_t head = used ? (uint16_t)((rec_head(ef) + 1) % cap) : 0;
        rec_set_head(ef, head);
        if (used < cap) rec_set_used(ef, (uint16_t)(used + 1));
        *n_out = 1;
        return fs_record_write(ef, 1, in, len);
    }
    if (used >= cap) return false;
    *n_out = (uint16_t)(used + 1);
    rec_set_used(ef, *n_out); // the new record exists from here on
    return fs_record_write(ef, *n_out, in, len);
}

// ---------------------------------------------------------------------------
// FCP / FCI
// ---------------------------------------------------------------------------

void fs_put_fcp(apdu_resp_t *resp, const fs_file_t *f, uint8_t template_tag) {
    uint8_t body[64];
    apdu_resp_t b = APDU_RESP_INIT(body, sizeof body);
    uint8_t tmp[8];

    if (template_tag != 0x64) {
        if (f->type == FS_EF_TRANSPARENT) {
            tmp[0] = (uint8_t)(f->size >> 8);
            tmp[1] = (uint8_t)f->size;
            resp_put_tlv(&b, 0x80, tmp, 2);
        }
        if (is_record_ef(f)) {
            uint16_t cap = rec_capacity(f);
            tmp[0] = f->type;
            tmp[1] = 0x21;
            tmp[2] = (uint8_t)(f->rec_size >> 8);
            tmp[3] = (uint8_t)f->rec_size;
            tmp[4] = (uint8_t)(cap > 255 ? 255 : cap);
            resp_put_tlv(&b, 0x82, tmp, 5);
        } else {
            tmp[0] = f->type;
            resp_put_tlv(&b, 0x82, tmp, 1);
        }
        tmp[0] = (uint8_t)(f->fid >> 8);
        tmp[1] = (uint8_t)f->fid;
        resp_put_tlv(&b, 0x83, tmp, 2);
        if (is_df(f) && f->name && f->name_len) resp_put_tlv(&b, 0x84, f->name, f->name_len);
        if (f->sfi) {
            tmp[0] = (uint8_t)(f->sfi << 3);
            resp_put_tlv(&b, 0x88, tmp, 1);
        }
        tmp[0] = 0x05; // operational state (activated)
        resp_put_tlv(&b, 0x8A, tmp, 1);
        if (!is_df(f)) {
            tmp[0] = f->ac_read; // proprietary: read / update conditions
            tmp[1] = f->ac_update;
            resp_put_tlv(&b, 0x86, tmp, 2);
        }
    }
    resp_put_tlv(resp, template_tag, body, b.len);
    if (b.overflow) resp->overflow = true;
}

// ---------------------------------------------------------------------------
// access
// ---------------------------------------------------------------------------

bool fs_access_ok(uint8_t ch, const fs_file_t *f, uint8_t op) {
    if (card_profile->hooks.check_access) return card_profile->hooks.check_access(ch, f, op);
    return sec_access_ok(ch, op == FS_OP_READ ? f->ac_read : f->ac_update);
}

static void file_changed(const fs_file_t *f) {
    nvm_mark_dirty();
    if (card_profile->hooks.on_file_changed) card_profile->hooks.on_file_changed(f);
}

// Without a file system (profile.files == NULL) the commands below still answer with a status word instead of
// passing the command on, so that the reader does not get 6D00 / 6E00 for a command the card knows.  ISO 7816-4
// (5.6, table 6) gives 6A82 "file or application not found" when a short EF identifier (or, for SELECT, a file)
// cannot be found, and 6986 "command not allowed (no current EF)" when the command addresses the current EF and
// there is none.  The cursor of a profile without files is { NULL, NULL }, so binary_target / record_target give
// exactly these.

// Resolve the EF a BINARY command refers to (short EF id in P1 or current).  P1 = 100x xxxx carries an SFI of 1..30:
// SFI 0 is not a short identifier (the current EF is addressed with P1 b8 = 0) and 31 is reserved.
static uint16_t binary_target(const apdu_t *cmd, const fs_file_t **ef, uint32_t *offset) {
    fs_cursor_t *c = fs_cursor(cmd->channel);
    if (cmd->p1 & 0x80) {
        uint8_t sfi = cmd->p1 & 0x1F;
        if ((cmd->p1 & 0x60) || sfi == 0 || sfi == 31) return SW_WRONG_P1P2;
        *ef = fs_by_sfi(c->df, sfi);
        if (!*ef) return SW_FILE_NOT_FOUND;
        *offset = cmd->p2;
        fs_select_file(cmd->channel, *ef);
    } else {
        *ef = c->ef;
        if (!*ef) return SW_NO_CURRENT_EF;
        *offset = apdu_p1p2(cmd);
    }
    if ((*ef)->type != FS_EF_TRANSPARENT) return SW_INCOMPATIBLE_FILE;
    return SW_OK;
}

// Resolve the EF a RECORD command refers to (short EF id in P2 b8..b4: 0 = current EF, 31 reserved).
static uint16_t record_target(const apdu_t *cmd, const fs_file_t **ef) {
    fs_cursor_t *c = fs_cursor(cmd->channel);
    uint8_t sfi = cmd->p2 >> 3;
    if (sfi) {
        if (sfi == 31) return SW_WRONG_P1P2;
        *ef = fs_by_sfi(c->df, sfi);
        if (!*ef) return SW_FILE_NOT_FOUND;
        fs_select_file(cmd->channel, *ef);
    } else {
        *ef = c->ef;
        if (!*ef) return SW_NO_CURRENT_EF;
    }
    if (!is_record_ef(*ef)) return SW_INCOMPATIBLE_FILE;
    return SW_OK;
}

// ---------------------------------------------------------------------------
// SELECT
// ---------------------------------------------------------------------------

uint16_t fs_cmd_select(const apdu_t *cmd, apdu_resp_t *resp) {
    if (!fs_present()) return SW_FILE_NOT_FOUND; // no file system: nothing to select (see above)
    fs_cursor_t *c = fs_cursor(cmd->channel);
    const fs_file_t *f = NULL;
    uint16_t fid = cmd->lc >= 2 ? (uint16_t)((cmd->data[0] << 8) | cmd->data[1]) : 0;

    switch (cmd->p1) {
    case 0x00: // MF, DF or EF by identifier
        if (cmd->lc == 0 || (cmd->lc == 2 && fid == 0x3F00)) {
            f = fs_mf();
        } else if (cmd->lc == 2) {
            if (c->df->fid == fid) f = c->df;
            if (!f) f = fs_child(c->df, fid);
            if (!f && c->df->parent && c->df->parent->fid == fid) f = c->df->parent;
        } else {
            return SW_WRONG_LENGTH;
        }
        break;
    case 0x01: // child DF
        if (cmd->lc != 2) return SW_WRONG_LENGTH;
        f = fs_child(c->df, fid);
        if (f && !is_df(f)) f = NULL;
        break;
    case 0x02: // EF under the current DF
        if (cmd->lc != 2) return SW_WRONG_LENGTH;
        f = fs_child(c->df, fid);
        if (f && is_df(f)) f = NULL;
        break;
    case 0x03: // parent DF
        if (cmd->lc != 0) return SW_WRONG_LENGTH;
        f = c->df->parent ? c->df->parent : c->df;
        break;
    case 0x04: { // by DF name
        bool next = (cmd->p2 & 0x03) == 0x02;
        f = fs_by_name(cmd->data, cmd->lc, true, next ? c->df : NULL);
        break;
    }
    case 0x08: // path from the MF
        f = fs_by_path(fs_mf(), cmd->data, cmd->lc);
        break;
    case 0x09: // path from the current DF
        f = fs_by_path(c->df, cmd->data, cmd->lc);
        break;
    default:
        return SW_WRONG_P1P2;
    }
    if (!f) return SW_FILE_NOT_FOUND;

    fs_select_file(cmd->channel, f);
    switch (cmd->p2 & 0x0C) {
    case 0x00:
        fs_put_fcp(resp, f, 0x6F);
        break; // FCI
    case 0x04:
        fs_put_fcp(resp, f, 0x62);
        break; // FCP
    case 0x08:
        fs_put_fcp(resp, f, 0x64);
        break; // FMD
    default:
        break; // no response data
    }
    return SW_OK;
}

// ---------------------------------------------------------------------------
// BINARY
// ---------------------------------------------------------------------------

uint16_t fs_cmd_read_binary(const apdu_t *cmd, apdu_resp_t *resp) {
    const fs_file_t *ef;
    uint32_t off;
    uint16_t sw = binary_target(cmd, &ef, &off);
    if (sw != SW_OK) return sw;
    if (!fs_access_ok(cmd->channel, ef, FS_OP_READ)) return SW_SECURITY_STATUS;
    if (off >= ef->size) return SW_WRONG_P1P2_2;
    uint32_t n = ef->size - off;
    uint32_t le = cmd->le ? cmd->le : 256;
    if (n > le) n = le;
    // Le is a maximum: an extended Le larger than the response buffer (e.g. 65536, "as much as possible") gets what
    // fits, which is a complete answer, not an overflow.
    if (n > resp->cap - resp->len) n = resp->cap - resp->len;
    resp_put(resp, ef->data + off, n);
    return SW_OK;
}

static uint16_t write_binary(const apdu_t *cmd) {
    const fs_file_t *ef;
    uint32_t off;
    uint16_t sw = binary_target(cmd, &ef, &off);
    if (sw != SW_OK) return sw;
    if (!fs_access_ok(cmd->channel, ef, FS_OP_UPDATE)) return SW_SECURITY_STATUS;
    if (off > ef->size) return SW_WRONG_P1P2_2;
    if (off + cmd->lc > ef->size) return SW_WRONG_LENGTH;
    if (cmd->lc) {
        memcpy(ef->data + off, cmd->data, cmd->lc);
        file_changed(ef);
    }
    return SW_OK;
}

uint16_t fs_cmd_update_binary(const apdu_t *cmd, apdu_resp_t *resp) {
    (void)resp;
    return write_binary(cmd);
}

uint16_t fs_cmd_write_binary(const apdu_t *cmd, apdu_resp_t *resp) {
    (void)resp;
    return write_binary(cmd);
}

// 0E: erase from offset P1P2 up to the offset in the data field (2 bytes) or to the end of the file.
uint16_t fs_cmd_erase_binary(const apdu_t *cmd, apdu_resp_t *resp) {
    (void)resp;
    const fs_file_t *ef;
    uint32_t off;
    uint16_t sw = binary_target(cmd, &ef, &off);
    if (sw != SW_OK) return sw;
    if (!fs_access_ok(cmd->channel, ef, FS_OP_UPDATE)) return SW_SECURITY_STATUS;
    uint32_t end = ef->size;
    if (cmd->lc == 2)
        end = (uint32_t)((cmd->data[0] << 8) | cmd->data[1]);
    else if (cmd->lc)
        return SW_WRONG_LENGTH;
    if (off > ef->size || end > ef->size || end < off) return SW_WRONG_P1P2_2;
    memset(ef->data + off, 0, end - off);
    file_changed(ef);
    return SW_OK;
}

// ---------------------------------------------------------------------------
// RECORD
// ---------------------------------------------------------------------------

// Turn P1 / P2 b3..b1 into a record number, updating the record pointer for the relative modes.  Returns 0 when there
// is no such record: reading and writing address only existing records (a linear variable or cyclic EF has as many
// as are in use; APPEND RECORD adds them).
static uint16_t resolve_record(const apdu_t *cmd, const fs_file_t *ef) {
    fs_cursor_t *c = fs_cursor(cmd->channel);
    uint16_t count = fs_record_count(ef);
    uint16_t n = 0;
    switch (cmd->p2 & 0x07) {
    case 0x00:
        n = count ? 1 : 0;
        break; // first
    case 0x01:
        n = count;
        break; // last
    case 0x02:
        n = (uint16_t)(c->record + 1);
        break; // next
    case 0x03:
        n = c->record > 1 ? (uint16_t)(c->record - 1) : 0;
        break; // previous
    case 0x04:
        n = cmd->p1 ? cmd->p1 : c->record;
        break; // by number (00 = current)
    default:
        return 0;
    }
    if (n == 0 || n > count) return 0;
    c->record = n;
    return n;
}

uint16_t fs_cmd_read_record(const apdu_t *cmd, apdu_resp_t *resp) {
    const fs_file_t *ef;
    uint16_t sw = record_target(cmd, &ef);
    if (sw != SW_OK) return sw;
    if (!fs_access_ok(cmd->channel, ef, FS_OP_READ)) return SW_SECURITY_STATUS;

    uint8_t mode = cmd->p2 & 0x07;
    if (mode == 0x05 || mode == 0x06) {
        // Read all records from P1 up to the last (05) / from the last down to P1 (06).
        uint16_t count = fs_record_count(ef);
        uint16_t first = cmd->p1 ? cmd->p1 : 1;
        if (first > count) return SW_RECORD_NOT_FOUND;
        uint16_t total = (uint16_t)(count - first + 1);
        for (uint16_t i = 0; i < total; i++) {
            uint16_t n = mode == 0x05 ? (uint16_t)(first + i) : (uint16_t)(count - i);
            const uint8_t *p;
            uint16_t len;
            if (!fs_record_read(ef, n, &p, &len)) break;
            resp_put(resp, p, len);
        }
        return SW_OK;
    }
    uint16_t n = resolve_record(cmd, ef);
    if (!n) return SW_RECORD_NOT_FOUND;
    const uint8_t *p;
    uint16_t len;
    if (!fs_record_read(ef, n, &p, &len)) return SW_RECORD_NOT_FOUND;
    uint32_t le = cmd->le ? cmd->le : 256;
    resp_put(resp, p, len < le ? len : le);
    return SW_OK;
}

static uint16_t write_record(const apdu_t *cmd) {
    const fs_file_t *ef;
    uint16_t sw = record_target(cmd, &ef);
    if (sw != SW_OK) return sw;
    if (!fs_access_ok(cmd->channel, ef, FS_OP_UPDATE)) return SW_SECURITY_STATUS;
    if (cmd->lc > ef->rec_size) return SW_WRONG_LENGTH;
    if (ef->type == FS_EF_CYCLIC && (cmd->p2 & 0x07) == 0x03) {
        // "previous" on a cyclic EF: write the next (oldest) record.
        uint16_t n;
        if (!fs_record_append(ef, cmd->data, (uint16_t)cmd->lc, &n)) return SW_NOT_ENOUGH_MEMORY;
        fs_cursor(cmd->channel)->record = 1;
        file_changed(ef);
        return SW_OK;
    }
    uint16_t n = resolve_record(cmd, ef);
    if (!n) return SW_RECORD_NOT_FOUND;
    if (!fs_record_write(ef, n, cmd->data, (uint16_t)cmd->lc)) return SW_RECORD_NOT_FOUND;
    file_changed(ef);
    return SW_OK;
}

uint16_t fs_cmd_update_record(const apdu_t *cmd, apdu_resp_t *resp) {
    (void)resp;
    return write_record(cmd);
}

uint16_t fs_cmd_write_record(const apdu_t *cmd, apdu_resp_t *resp) {
    (void)resp;
    return write_record(cmd);
}

uint16_t fs_cmd_append_record(const apdu_t *cmd, apdu_resp_t *resp) {
    (void)resp;
    if (cmd->p1 != 0) return SW_WRONG_P1P2;
    const fs_file_t *ef;
    uint16_t sw = record_target(cmd, &ef);
    if (sw != SW_OK) return sw;
    if (!fs_access_ok(cmd->channel, ef, FS_OP_UPDATE)) return SW_SECURITY_STATUS;
    if (cmd->lc > ef->rec_size) return SW_WRONG_LENGTH;
    uint16_t n;
    if (!fs_record_append(ef, cmd->data, (uint16_t)cmd->lc, &n)) return SW_NOT_ENOUGH_MEMORY;
    fs_cursor(cmd->channel)->record = n;
    file_changed(ef);
    return SW_OK;
}

uint16_t fs_cmd_erase_record(const apdu_t *cmd, apdu_resp_t *resp) {
    (void)resp;
    const fs_file_t *ef;
    uint16_t sw = record_target(cmd, &ef);
    if (sw != SW_OK) return sw;
    if (!fs_access_ok(cmd->channel, ef, FS_OP_UPDATE)) return SW_SECURITY_STATUS;
    uint16_t count = fs_record_count(ef);
    uint16_t first, last;
    if ((cmd->p2 & 0x07) == 0x04) {
        first = last = cmd->p1 ? cmd->p1 : fs_cursor(cmd->channel)->record;
    } else if ((cmd->p2 & 0x07) == 0x05) {
        first = cmd->p1 ? cmd->p1 : 1;
        last = count;
    } else {
        return SW_WRONG_P1P2;
    }
    if (!first || first > count) return SW_RECORD_NOT_FOUND;
    for (uint16_t n = first; n <= last; n++) fs_record_erase(ef, n);
    file_changed(ef);
    return SW_OK;
}

// A2: simple search (P2 b3..b1 = 100) forward from record P1 for records starting with the search string; returns the
// matching record numbers.
uint16_t fs_cmd_search_record(const apdu_t *cmd, apdu_resp_t *resp) {
    const fs_file_t *ef;
    uint16_t sw = record_target(cmd, &ef);
    if (sw != SW_OK) return sw;
    if (!fs_access_ok(cmd->channel, ef, FS_OP_READ)) return SW_SECURITY_STATUS;
    if ((cmd->p2 & 0x07) != 0x04) return SW_FUNC_NOT_SUPPORTED;
    uint16_t count = fs_record_count(ef);
    uint16_t first = cmd->p1 ? cmd->p1 : 1;
    for (uint16_t n = first; n <= count; n++) {
        const uint8_t *p;
        uint16_t len;
        if (!fs_record_read(ef, n, &p, &len)) continue;
        if (len >= cmd->lc && memcmp(p, cmd->data, cmd->lc) == 0) resp_put_u8(resp, (uint8_t)n);
    }
    return SW_OK;
}
