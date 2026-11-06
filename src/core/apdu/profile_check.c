// Read-only consistency check of the card profile; see profile_check.h.  Each rule cites the code that relies on it,
// so that a finding tells what the card will actually do.  Findings already reported elsewhere at boot are left out:
// sec_init() logs more than SEC_MAX_PINS PINs, lengths above SEC_PIN_MAX_LEN and pin_transform without plain_len
// ("SEC: ..."), session_init() an ATR that cannot be parsed or built ("session: invalid ATR in card profile").
#include "apdu/profile_check.h"

#include "hal/phy.h"
#include "iso7816_pico/atr.h"
#include "iso7816_pico/log.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// The console's built-in commands (s_builtin in src/pico/sys/console.c, which is not compiled on the host): they are
// looked up first, so a profile command of the same name never runs.  Keep the two lists in sync.
static const char *const s_builtin_console[] = {
    "help", "status", "card", "trace", "txperr", "gpio", "pins", "apdu", "save", "erase", "bootsel", "reboot",
};

// Most extra guard ETUs the PHY takes (PHY_MAX_EXTRA_GUARD in src/pico/hal/phy.c, the same limit in tests/fake_phy.c):
// phy_start_session() refuses a session with more.
#define MAX_EXTRA_GUARD 20

static unsigned s_count, s_errors;

// error: does not work as configured; warning: works, but probably not as meant; note: legal, for information.
enum { ERR, WARN, NOTE };
static const char *const s_severity[] = { "error", "warning", "note" };

static void report(int sev, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void report(int sev, const char *fmt, ...) {
    s_count++;
    if (sev == ERR) s_errors++;
    if (s_count > PROFILE_CHECK_MAX_LINES) return;
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    log_printf("profile: %s: %s", s_severity[sev], msg);
}

// ---------------------------------------------------------------------------
// defects that would make the framework access memory outside the profile's objects
// ---------------------------------------------------------------------------

static bool is_record_type(uint8_t t) {
    return t == FS_EF_LINEAR_FIXED || t == FS_EF_LINEAR_VARIABLE || t == FS_EF_CYCLIC;
}

// Is f one of files[0..n-1]?  Compared as integers: the pointer may belong to another object.
static bool in_files(const fs_file_t *files, size_t n, const fs_file_t *f) {
    uintptr_t base = (uintptr_t)files, p = (uintptr_t)f;
    return p >= base && p < base + n * sizeof *files && (p - base) % sizeof *files == 0;
}

static const char *own_defect(const fs_file_t *f) {
    if (f->type == FS_DF) return NULL;
    // fs.c reads the record header (data[0..3]: records in use, cyclic head) of every record EF it touches.
    if (is_record_type(f->type) && !f->data) return "record EF without data (fs.c reads the record header in data)";
    if (f->size && !f->data) return "size without data (READ / UPDATE BINARY would access data[0..size-1])";
    if (is_record_type(f->type) && f->size < FS_REC_HDR_SIZE) {
        return "record EF smaller than the 4-byte record header (fs.h: FS_*_BYTES)";
    }
    if (f->type == FS_EF_LINEAR_VARIABLE && f->rec_size > UINT16_MAX - 2) {
        return "rec_size + 2 overflows the 16-bit slot size of fs.c";
    }
    return NULL;
}

const char *profile_file_defect(const fs_file_t *files, size_t n, const fs_file_t *f) {
    const char *d = own_defect(f);
    if (d) return d;
    // SELECT parent (P1 = 03) and SELECT by FID (P1 = 00) climb from the current DF, so every ancestor must be usable.
    // The walk is bounded by n: a cycle is reported by profile_check() but is not a memory hazard.
    const fs_file_t *p = f->parent;
    for (size_t i = 0; p && i < n; i++, p = p->parent) {
        if (!in_files(files, n, p)) {
            return i == 0 ? "parent outside files[]" : "the parent of an ancestor is outside files[]";
        }
        if (own_defect(p)) return "an ancestor is unusable";
    }
    return NULL;
}

const char *profile_applet_defect(const applet_t *a) {
    if (!a) return "NULL entry";
    if (!a->aid && a->aid_len) return "aid_len without aid (SELECT by DF name compares aid)";
    if (!a->cmds && a->n_cmds) return "n_cmds without cmds (the dispatcher scans cmds)";
    return NULL;
}

// ---------------------------------------------------------------------------
// file system (fs.h, fs.c)
// ---------------------------------------------------------------------------

static bool pin_exists(const card_profile_t *p, uint8_t ref) {
    size_t n = p->pins ? (p->n_pins < SEC_MAX_PINS ? p->n_pins : SEC_MAX_PINS) : 0;
    for (size_t i = 0; i < n; i++) {
        if (p->pins[i].ref == ref) return true;
    }
    return false;
}

static void check_ac(const card_profile_t *p, uint16_t fid, const char *op, uint8_t ac) {
    if (ac == FS_AC_ALWAYS || ac == FS_AC_NEVER || p->hooks.check_access || pin_exists(p, ac)) return;
    report(ERR, "file %04X: %s condition %02X names no PIN in pins[]: never satisfied (sec_access_ok)", fid, op, ac);
}

// Where the EF's data lies relative to nvm_blob.  RAM-only files are legal; a range that straddles the blob's end is
// not (part of the file would be persisted, part not, and the bytes behind the blob belong to something else).
static void check_nvm_range(const card_profile_t *p, const fs_file_t *f) {
    if (!p->nvm_blob || !p->nvm_size || !f->data || !f->size) return;
    uintptr_t b = (uintptr_t)p->nvm_blob, e = b + p->nvm_size;
    uintptr_t d = (uintptr_t)f->data, de = d + f->size;
    if (d >= b && de <= e) return;
    if (de <= b || d >= e) {
        report(NOTE, "file %04X: data lies outside nvm_blob: changes are not persisted", f->fid);
    } else {
        report(ERR, "file %04X: data straddles the bounds of nvm_blob (%lu bytes at blob offset %ld)", f->fid,
               (unsigned long)f->size, (long)(d - b));
    }
}

static void check_df(const fs_file_t *files, size_t i) {
    const fs_file_t *f = &files[i];
    if (f->data || f->size) report(WARN, "file %04X: DF with data / size (ignored: only EFs have a body)", f->fid);
    if (f->rec_size || f->sfi) {
        report(WARN, "file %04X: DF with rec_size / SFI (ignored: fs_by_sfi finds EFs only)", f->fid);
    }
    if (!f->name && f->name_len) report(WARN, "file %04X: name_len without name (ignored by fs_by_name)", f->fid);
    if (!f->name || !f->name_len) return;
    if (f->name_len > 16) report(ERR, "file %04X: DF name of %u bytes (ISO 7816-4: at most 16)", f->fid, f->name_len);
    for (size_t j = 0; j < i; j++) {
        const fs_file_t *g = &files[j];
        if (g->type != FS_DF || !g->name || !g->name_len) continue;
        size_t k = f->name_len < g->name_len ? f->name_len : g->name_len;
        if (memcmp(f->name, g->name, k) != 0) continue;
        if (f->name_len == g->name_len) {
            report(ERR, "file %04X: same DF name as DF %04X: SELECT by name finds DF %04X first (fs_by_name)", f->fid,
                   g->fid, g->fid);
        } else if (f->name_len < g->name_len) {
            report(ERR,
                   "file %04X: DF name is a prefix of DF %04X's, listed earlier: SELECT by name (partial match) "
                   "finds DF %04X",
                   f->fid, g->fid, g->fid);
        } else {
            report(WARN, "file %04X: DF name starts with DF %04X's: a partial SELECT by name is ambiguous", f->fid,
                   g->fid);
        }
    }
}

static void check_record_ef(const fs_file_t *f) {
    bool var = f->type == FS_EF_LINEAR_VARIABLE;
    const char *macro = var                             ? "FS_LINEAR_VARIABLE_BYTES"
                        : f->type == FS_EF_LINEAR_FIXED ? "FS_LINEAR_FIXED_BYTES"
                                                        : "FS_CYCLIC_BYTES";
    if (!f->rec_size) {
        report(ERR, "file %04X: record EF with rec_size 0: no records (rec_capacity)", f->fid);
        return;
    }
    uint32_t slot = (uint32_t)f->rec_size + (var ? 2 : 0);
    uint32_t cap = (f->size - FS_REC_HDR_SIZE) / slot, spare = (f->size - FS_REC_HDR_SIZE) % slot;
    if (!cap) {
        report(ERR, "file %04X: size %lu holds no record of %u bytes: use %s(rec_size, count)", f->fid,
               (unsigned long)f->size, f->rec_size, macro);
        return;
    }
    if (spare) {
        report(WARN, "file %04X: size %lu is not %s(%u, n): %lu bytes unused", f->fid, (unsigned long)f->size, macro,
               f->rec_size, (unsigned long)spare);
    }
    if (cap > 254) {
        report(WARN,
               "file %04X: %lu records: only 1..254 can be addressed by number (P1) and SEARCH RECORD answers "
               "one-byte numbers",
               f->fid, (unsigned long)cap);
    }
    if (f->rec_size > 255) {
        report(WARN, "file %04X: rec_size %u: a short APDU cannot write a whole record (Lc <= 255)", f->fid,
               f->rec_size);
    }
    uint16_t used = (uint16_t)(f->data[0] | (f->data[1] << 8)), head = (uint16_t)(f->data[2] | (f->data[3] << 8));
    if (used > cap) {
        report(WARN, "file %04X: the record header says %u records in use, the file holds %lu (FS_REC_HDR)", f->fid,
               used, (unsigned long)cap);
    }
    if (f->type == FS_EF_CYCLIC && head >= cap) {
        report(WARN, "file %04X: cyclic head slot %u is beyond the %lu slots (FS_REC_HDR)", f->fid, head,
               (unsigned long)cap);
    }
}

static void check_ef(const card_profile_t *p, const fs_file_t *files, size_t i) {
    const fs_file_t *f = &files[i];
    if (f->name || f->name_len)
        report(WARN, "file %04X: name on an EF (ignored: only DFs are selected by name)", f->fid);
    if (f->sfi > 30) {
        report(ERR, "file %04X: SFI %u outside 1..30 (fs_by_sfi never finds it)", f->fid, f->sfi);
    } else if (f->sfi) {
        for (size_t j = 0; j < i; j++) {
            const fs_file_t *g = &files[j];
            if (g->parent == f->parent && g->type != FS_DF && g->sfi == f->sfi) {
                report(ERR, "file %04X: SFI %u already used by EF %04X in the same DF (fs_by_sfi finds that one)",
                       f->fid, f->sfi, g->fid);
                break;
            }
        }
    }
    if (f->type == FS_EF_TRANSPARENT) {
        if (f->rec_size) report(WARN, "file %04X: transparent EF with rec_size (ignored)", f->fid);
        if (f->size > 0xFFFF) {
            report(WARN, "file %04X: size %lu does not fit the 2-byte FCP tag 80", f->fid, (unsigned long)f->size);
        }
    } else {
        check_record_ef(f);
    }
    check_ac(p, f->fid, "read", f->ac_read);
    check_ac(p, f->fid, "update", f->ac_update);
    check_nvm_range(p, f);
}

static void check_files(const card_profile_t *p) {
    const fs_file_t *files = p->files;
    size_t n = p->n_files;
    if (!files || !n) {
        if (files || n) report(NOTE, "files / n_files: only one of them is set: no file system (fs_present)");
        return;
    }
    const fs_file_t *mf = &files[0];
    if (mf->type != FS_DF || mf->parent || mf->fid != 0x3F00) {
        report(ERR, "files[0] must be the MF: a DF with FID 3F00 and no parent (fs_init, SELECT 3F00)");
    }
    for (size_t i = 0; i < n; i++) {
        const fs_file_t *f = &files[i];
        const char *defect = profile_file_defect(files, n, f);
        if (defect) {
            report(ERR, "file %04X (files[%lu]): %s: %s", f->fid, (unsigned long)i, defect,
                   i == 0 ? "the file system is disabled" : "disabled (the lookups of fs.c skip it)");
            continue;
        }
        if (f->type != FS_DF && f->type != FS_EF_TRANSPARENT && !is_record_type(f->type)) {
            report(ERR, "file %04X: unknown type %02X (fs_type_t)", f->fid, f->type);
            continue;
        }
        if (i > 0 && !f->parent) {
            report(ERR, "file %04X (files[%lu]): no parent: only the MF has none; reachable by DF name at most", f->fid,
                   (unsigned long)i);
        }
        if (f->parent) {
            if (f->parent->type != FS_DF) {
                report(ERR, "file %04X: parent %04X is not a DF (SELECT makes the parent the current DF)", f->fid,
                       f->parent->fid);
            }
            const fs_file_t *a = f->parent;
            size_t steps = 0;
            while (a && steps <= n) {
                a = a->parent;
                steps++;
            }
            if (a) report(ERR, "file %04X: the parent chain loops and never reaches the MF", f->fid);
            if (f->fid == f->parent->fid) {
                report(WARN, "file %04X: same FID as its parent DF: SELECT P1=00 finds the DF first", f->fid);
            }
            for (size_t j = 0; j < i; j++) {
                if (files[j].parent == f->parent && files[j].fid == f->fid) {
                    report(ERR, "file %04X: FID used twice in DF %04X: fs_child finds files[%lu]", f->fid,
                           f->parent->fid, (unsigned long)j);
                    break;
                }
            }
        }
        if (i > 0 && f->fid == 0x3F00)
            report(ERR, "file %04X (files[%lu]): 3F00 is the MF's FID", f->fid, (unsigned long)i);
        if (f->fid == 0x3FFF || f->fid == 0xFFFF) {
            report(WARN, "file %04X: FID reserved by ISO 7816-4 (3FFF current DF in paths, FFFF RFU)", f->fid);
        }
        if (f->type == FS_DF)
            check_df(files, i);
        else
            check_ef(p, files, i);
    }
}

// ---------------------------------------------------------------------------
// PINs (sec.h, sec.c)
// ---------------------------------------------------------------------------

static void check_pins(const card_profile_t *p) {
    if (!p->pins) {
        if (p->n_pins)
            report(ERR, "pins is NULL with n_pins %lu: no PINs are used (sec_init)", (unsigned long)p->n_pins);
        return;
    }
    size_t n = p->n_pins < SEC_MAX_PINS ? p->n_pins : SEC_MAX_PINS; // the excess is logged by sec_init()
    for (size_t i = 0; i < n; i++) {
        const sec_pin_t *pin = &p->pins[i];
        for (size_t j = 0; j < i; j++) {
            if (p->pins[j].ref == pin->ref) {
                report(ERR, "PIN %02X (pins[%lu]): ref used twice: sec_find returns pins[%lu]", pin->ref,
                       (unsigned long)i, (unsigned long)j);
                break;
            }
        }
        if (pin->ref == FS_AC_ALWAYS || pin->ref == FS_AC_NEVER) {
            report(WARN, "PIN %02X: the ref is FS_AC_%s: no access condition can require it", pin->ref,
                   pin->ref == FS_AC_ALWAYS ? "ALWAYS" : "NEVER");
        }
        if (!pin->max_retries) {
            report(ERR, "PIN %02X: max_retries 0: the PIN is blocked after the first try (sec.c try_pin)", pin->ref);
        } else if (pin->retries > pin->max_retries) {
            report(WARN, "PIN %02X: retries %u above max_retries %u", pin->ref, pin->retries, pin->max_retries);
        }
        if (pin->puk_len && !pin->puk_max_retries) {
            report(ERR, "PIN %02X: PUK with puk_max_retries 0: blocked after the first try (sec.c try_puk)", pin->ref);
        } else if (pin->puk_len && pin->puk_retries > pin->puk_max_retries) {
            report(WARN, "PIN %02X: puk_retries %u above puk_max_retries %u", pin->ref, pin->puk_retries,
                   pin->puk_max_retries);
        }
        if (pin->min_len > pin->max_len) {
            report(ERR, "PIN %02X: min_len %u > max_len %u: no new value is accepted (6700)", pin->ref, pin->min_len,
                   pin->max_len);
        }
        if (!pin->len && !pin->disabled) report(WARN, "PIN %02X: empty stored value: VERIFY never matches", pin->ref);
    }
}

// ---------------------------------------------------------------------------
// applets (card.h, dispatch.c)
// ---------------------------------------------------------------------------

static const char *applet_name(const applet_t *a) { return a->name ? a->name : "(unnamed)"; }

static void check_cmds(const applet_t *a) {
    for (size_t k = 0; k < a->n_cmds; k++) {
        const apdu_cmd_desc_t *d = &a->cmds[k];
        const char *dn = d->name ? d->name : "?";
        if (d->cla & ~d->cla_mask) {
            report(WARN, "applet %s: cmds[%lu] %s: CLA %02X has bits outside cla_mask %02X: never matches",
                   applet_name(a), (unsigned long)k, dn, d->cla, d->cla_mask);
            continue;
        }
        for (size_t l = 0; l < k; l++) {
            const apdu_cmd_desc_t *e = &a->cmds[l];
            // Every CLA d matches also matches e, which applet_find tries first.
            if (e->ins != d->ins || (e->cla_mask & ~d->cla_mask) || (d->cla & e->cla_mask) != e->cla) continue;
            bool same = e->cla_mask == d->cla_mask && e->cla == d->cla;
            report(same ? ERR : WARN,
                   "applet %s: cmds[%lu] %s (CLA %02X/%02X INS %02X) %s cmds[%lu]: applet_find "
                   "takes the first match",
                   applet_name(a), (unsigned long)k, dn, d->cla, d->cla_mask, d->ins,
                   same ? "duplicates" : "is shadowed by", (unsigned long)l);
            break;
        }
        if (!d->handler && !a->process) {
            report(WARN,
                   "applet %s: cmds[%lu] %s has no handler and the applet no process(): it only classifies; "
                   "the command goes on to the ISO commands / hooks",
                   applet_name(a), (unsigned long)k, dn);
        }
    }
}

static void check_applets(const card_profile_t *p) {
    const applet_t *const *list = p->applets;
    size_t n = list ? p->n_applets : 0;
    if (!list && p->n_applets) {
        report(ERR, "applets is NULL with n_applets %lu: no applet is used (dispatch.c)", (unsigned long)p->n_applets);
    }
    bool default_listed = false;
    for (size_t i = 0; i < n; i++) {
        const applet_t *a = list[i];
        const char *defect = profile_applet_defect(a);
        if (defect) {
            report(ERR, "applets[%lu]%s%s: %s: disabled (dispatch.c skips it)", (unsigned long)i, a ? " " : "",
                   a ? applet_name(a) : "", defect);
            if (a == p->default_applet) default_listed = true;
            continue;
        }
        if (a == p->default_applet) default_listed = true;
        if (!a->aid_len) {
            report(ERR, "applet %s: empty AID: SELECT by DF name never finds it", applet_name(a));
        } else if (a->aid_len > 16) {
            report(ERR, "applet %s: AID of %u bytes (ISO 7816-4: at most 16; the default FCI answers 6F00)",
                   applet_name(a), a->aid_len);
        } else if (a->aid_len < 5) {
            report(WARN, "applet %s: AID of %u bytes (ISO 7816-5: the RID alone has 5)", applet_name(a), a->aid_len);
        }
        for (size_t j = 0; a->aid_len && j < i; j++) {
            const applet_t *b = list[j];
            if (profile_applet_defect(b) || !b->aid_len) continue;
            size_t k = a->aid_len < b->aid_len ? a->aid_len : b->aid_len;
            if (memcmp(a->aid, b->aid, k) != 0) continue;
            if (a->aid_len == b->aid_len) {
                report(ERR, "applet %s: same AID as %s, listed earlier: SELECT finds %s", applet_name(a),
                       applet_name(b), applet_name(b));
            } else if (a->aid_len < b->aid_len) {
                report(ERR,
                       "applet %s: AID is a prefix of %s's, listed earlier: SELECT by its AID (partial match) "
                       "finds %s; only SELECT next reaches it",
                       applet_name(a), applet_name(b), applet_name(b));
            } else {
                report(WARN, "applet %s: AID starts with %s's: a partial SELECT by name is ambiguous", applet_name(a),
                       applet_name(b));
            }
        }
        check_cmds(a);
    }
    const applet_t *d = p->default_applet;
    if (d && !default_listed) {
        const char *defect = profile_applet_defect(d);
        if (defect) {
            report(ERR, "default_applet: %s: not selected after the ATR", defect);
        } else {
            report(WARN, "default_applet %s is not in applets[]: its reset() is not called and SELECT cannot find it",
                   applet_name(d));
        }
    }
    if (!p->max_channels) {
        report(NOTE, "max_channels 0 is taken as 1 (dispatch.c)");
    } else if (p->max_channels > CARD_MAX_CHANNELS) {
        report(WARN, "max_channels %u above CARD_MAX_CHANNELS %d: only %d are opened", p->max_channels,
               CARD_MAX_CHANNELS, CARD_MAX_CHANNELS);
    }
}

// ---------------------------------------------------------------------------
// ATR and transmission parameters (session.c, pps.c, phy.h)
// ---------------------------------------------------------------------------

static void check_atr(const card_profile_t *p) {
    if (p->tx_extra_guard > MAX_EXTRA_GUARD) {
        report(ERR, "tx_extra_guard %u above %d: the PHY refuses every session (no ATR)", p->tx_extra_guard,
               MAX_EXTRA_GUARD);
    }
    if (p->atr && p->atr_desc) report(WARN, "both atr and atr_desc are set: atr is sent (session_init)");
    if (!p->atr != !p->atr_len) report(WARN, "only one of atr / atr_len is set: the raw ATR is ignored");

    uint8_t built[ATR_MAX_LEN];
    const uint8_t *atr = NULL;
    size_t len = 0;
    if (p->atr && p->atr_len) {
        atr = p->atr;
        len = p->atr_len;
    } else if (p->atr_desc) {
        atr = built;
        len = atr_build(p->atr_desc, built, sizeof built);
    }
    atr_info_t info;
    // No ATR: session_init() sends 3B 00.  An invalid one is logged by session_init().
    if (!atr || !len || len > ATR_MAX_LEN || !atr_parse(atr, len, &info)) return;

    if (info.protocols & ~((1u << 0) | (1u << 1) | (1u << 15))) {
        report(WARN, "the ATR offers protocols other than T=0 / T=1 (mask %04X): only those two are implemented",
               info.protocols);
    }
    if (info.specific_mode) {
        if (info.specific_protocol > 1) {
            report(ERR, "specific mode (TA2) names T=%u: only T=0 / T=1 are implemented", info.specific_protocol);
        }
        if (!phy_params_supported(info.f, info.d)) {
            report(ERR,
                   "specific mode (TA2) at F=%u D=%u: the PIO cannot do it (phy_params_supported); the card "
                   "stops after the ATR",
                   info.f, info.d);
        }
    } else if (info.f != 372 || info.d != 1) {
        if (!p->allow_pps) {
            report(NOTE, "TA1 offers F=%u D=%u but allow_pps is false: the reader stays at 372 / 1", info.f, info.d);
        } else if (!phy_params_supported(info.f, info.d)) {
            report(WARN, "TA1 offers F=%u D=%u, which the PIO cannot do: a PPS for it is refused (pps.c)", info.f,
                   info.d);
        }
    }
}

// ---------------------------------------------------------------------------
// console commands (card.h, src/pico/sys/console.c)
// ---------------------------------------------------------------------------

static void check_console(const card_profile_t *p) {
    if (!p->console_cmds) {
        if (p->n_console_cmds) {
            report(ERR, "console_cmds is NULL with n_console_cmds %lu (help and the lookup read it)",
                   (unsigned long)p->n_console_cmds);
        }
        return;
    }
    for (size_t i = 0; i < p->n_console_cmds; i++) {
        const console_cmd_t *c = &p->console_cmds[i];
        if (!c->name || !c->handler || !*c->name) {
            report(WARN, "console_cmds[%lu]: without a name or handler it never runs", (unsigned long)i);
            continue;
        }
        if (strpbrk(c->name, " \t")) {
            report(WARN, "console command '%s': the name contains a blank: it can never be typed", c->name);
        }
        for (size_t b = 0; b < sizeof s_builtin_console / sizeof s_builtin_console[0]; b++) {
            if (strcmp(c->name, s_builtin_console[b]) == 0) {
                report(WARN, "console command '%s': the built-in command of that name runs instead", c->name);
            }
        }
        for (size_t j = 0; j < i; j++) {
            const console_cmd_t *o = &p->console_cmds[j];
            if (o->name && o->handler && strcmp(o->name, c->name) == 0) {
                report(WARN, "console command '%s': also console_cmds[%lu], which runs instead", c->name,
                       (unsigned long)j);
                break;
            }
        }
        if (strlen(c->name) > 8) {
            report(NOTE, "console command '%s': longer than 8 characters: the help listing is misaligned", c->name);
        }
    }
}

unsigned profile_check(const card_profile_t *p) {
    s_count = s_errors = 0;
    if (!p) return 0;
    check_atr(p);
    check_files(p);
    check_pins(p);
    check_applets(p);
    check_console(p);
    if (s_count > PROFILE_CHECK_MAX_LINES) {
        log_printf("profile: ... %u more finding(s) not shown", s_count - PROFILE_CHECK_MAX_LINES);
    }
    if (s_count) {
        log_printf("profile: %u finding(s), %u error(s); see docs/card-profile.md", s_count, s_errors);
    }
    return s_count;
}
