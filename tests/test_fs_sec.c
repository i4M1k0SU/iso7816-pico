// Edge cases of the file system and PIN commands: short EF identifiers 0 / 31, records that do not exist, and the
// rule that a malformed PIN / PUK command is refused before it costs a try.  The handlers are called directly on
// channel 0 against the demo card profile; the whole NVM image is put back at the end.
#include "apdu/dispatch.h"
#include "iso7816_pico/card.h"
#include "iso7816_pico/fs.h"
#include "iso7816_pico/sec.h"
#include "fake_nvm.h"
#include "fakes.h"
#include "hal/nvm.h"
#include "test.h"

typedef uint16_t (*handler_fn)(const apdu_t *, apdu_resp_t *);

static uint8_t s_out[300];
static apdu_resp_t s_resp;

static uint16_t run(handler_fn fn, const char *hex) {
    static uint8_t buf[300];
    size_t n = hex_parse(hex, buf, sizeof buf);
    apdu_t a;
    memset(&a, 0, sizeof a);
    if (!apdu_decode(buf, n, &a)) return 0xFFFF;
    s_resp = (apdu_resp_t){ s_out, sizeof s_out, 0 };
    return fn(&a, &s_resp);
}

static const fs_file_t *df_7f10(void) { return fs_child(fs_mf(), 0x7F10); }
static uint16_t used(const fs_file_t *ef) { return (uint16_t)(ef->data[0] | (ef->data[1] << 8)); }

static void sfi_checks(void) {
    const fs_file_t *df = df_7f10();
    CHECK(df != NULL);
    if (!df) return;
    fs_select_file(0, df);

    // fs_by_sfi() knows only 1..30; EF 0104 has no SFI (sfi = 0) and must not be found as "SFI 0".
    CHECK(fs_by_sfi(df, 0) == NULL);
    CHECK(fs_by_sfi(df, 31) == NULL);
    CHECK(fs_by_sfi(df, 1) == fs_child(df, 0x0101));

    // BINARY: P1 = 80 (SFI 0) and 9F (SFI 31) are wrong P1-P2, not a file.
    CHECK_EQ(run(fs_cmd_read_binary, "00 B0 80 00 00"), 0x6A86);
    CHECK_EQ(run(fs_cmd_read_binary, "00 B0 9F 00 00"), 0x6A86);
    CHECK_EQ(run(fs_cmd_update_binary, "00 D6 80 00 01 41"), 0x6A86);
    CHECK_EQ(run(fs_cmd_update_binary, "00 D6 9F 00 01 41"), 0x6A86);
    CHECK_EQ(run(fs_cmd_erase_binary, "00 0E 80 00 00"), 0x6A86);
    CHECK(fs_cursor(0)->ef == NULL);                              // nothing was selected on the way
    CHECK_EQ(run(fs_cmd_read_binary, "00 B0 81 00 04"), 0x9000);  // SFI 1 still works
    CHECK_EQ(run(fs_cmd_read_binary, "00 B0 9E 00 00"), 0x6A82);  // SFI 30: valid, no such file

    // RECORD: P2 b8..b4 = 31 is reserved; 0 is the current EF.
    CHECK_EQ(run(fs_cmd_read_record, "00 B2 01 FC 00"), 0x6A86);
    CHECK_EQ(run(fs_cmd_update_record, "00 DC 01 FC 01 41"), 0x6A86);
    CHECK_EQ(run(fs_cmd_read_record, "00 B2 01 04 00"), 0x6981);   // SFI 0: the current EF, 0101 (selected by SFI 1)
    fs_select_file(0, fs_child(df, 0x0102));
    CHECK_EQ(run(fs_cmd_read_record, "00 B2 01 04 10"), 0x9000);   // SFI 0 = current EF (linear fixed 0102)
    CHECK_EQ(s_resp.len, 16);
}

static void record_checks(void) {
    const fs_file_t *df = df_7f10();
    const fs_file_t *fixed = fs_child(df, 0x0102), *cyc = fs_child(df, 0x0103), *var = fs_child(df, 0x0104);
    CHECK(fixed && cyc && var);
    if (!fixed || !cyc || !var) return;
    memset(cyc->data, 0, cyc->size);   // both empty
    memset(var->data, 0, var->size);
    CHECK_EQ(run(sec_cmd_verify, "00 20 00 01 04 31 32 33 34"), 0x9000);
    CHECK_EQ(run(sec_cmd_verify, "00 20 00 81 04 30 30 30 30"), 0x9000);

    // Linear variable EF 0104 (3 x 32): UPDATE / WRITE RECORD address only records that exist.
    fs_select_file(0, var);
    CHECK_EQ(run(fs_cmd_update_record, "00 DC 01 04 03 61 62 63"), 0x6A83);
    CHECK_EQ(run(fs_cmd_update_record, "00 DC 00 00 01 41"), 0x6A83);   // "first" of an empty EF
    CHECK_EQ(run(fs_cmd_update_record, "00 DC 00 01 01 41"), 0x6A83);   // "last"
    CHECK_EQ(used(var), 0);
    CHECK_EQ(run(fs_cmd_append_record, "00 E2 00 00 03 61 62 63"), 0x9000);
    CHECK_EQ(used(var), 1);
    CHECK_EQ(run(fs_cmd_update_record, "00 DC 03 04 01 41"), 0x6A83);   // beyond the records in use
    CHECK_EQ(run(fs_cmd_write_record, "00 D2 02 04 01 41"), 0x6A83);
    CHECK_EQ(used(var), 1);
    CHECK_EQ(run(fs_cmd_read_record, "00 B2 02 04 00"), 0x6A83);
    CHECK_EQ(run(fs_cmd_update_record, "00 DC 01 04 02 41 42"), 0x9000);
    CHECK_EQ(run(fs_cmd_read_record, "00 B2 01 04 00"), 0x9000);
    CHECK_HEX(s_out, s_resp.len, "41 42");
    // "next" past the last record: not found, and the record pointer stays on record 1.
    CHECK_EQ(run(fs_cmd_update_record, "00 DC 00 02 01 43"), 0x6A83);
    CHECK_EQ(used(var), 1);
    CHECK_EQ(fs_cursor(0)->record, 1);
    CHECK_EQ(run(fs_cmd_update_record, "00 DC 00 04 01 44"), 0x9000);   // P1 = 00: the current record
    CHECK_EQ(run(fs_cmd_read_record, "00 B2 01 04 00"), 0x9000);
    CHECK_HEX(s_out, s_resp.len, "44");
    // APPEND still adds records up to the capacity; after that the new ones can be updated.
    CHECK_EQ(run(fs_cmd_append_record, "00 E2 00 00 01 32"), 0x9000);
    CHECK_EQ(run(fs_cmd_append_record, "00 E2 00 00 01 33"), 0x9000);
    CHECK_EQ(run(fs_cmd_append_record, "00 E2 00 00 01 34"), 0x6A84);
    CHECK_EQ(used(var), 3);
    CHECK_EQ(run(fs_cmd_update_record, "00 DC 03 04 02 45 46"), 0x9000);
    CHECK_EQ(run(fs_cmd_read_record, "00 B2 03 04 00"), 0x9000);
    CHECK_HEX(s_out, s_resp.len, "45 46");
    CHECK_EQ(run(fs_cmd_update_record, "00 DC 04 04 01 41"), 0x6A83);
    // The library calls follow the same rule.
    CHECK(!fs_record_write(var, 4, (const uint8_t *)"x", 1));
    CHECK(!fs_record_erase(var, 4));

    // Cyclic EF 0103 (SFI 3, 5 x 8): empty, so nothing to update until a record is appended.
    CHECK_EQ(run(fs_cmd_update_record, "00 DC 01 1C 08 01 02 03 04 05 06 07 08"), 0x6A83);
    CHECK_EQ(used(cyc), 0);
    CHECK_EQ(run(fs_cmd_append_record, "00 E2 00 18 08 01 02 03 04 05 06 07 08"), 0x9000);
    CHECK_EQ(run(fs_cmd_update_record, "00 DC 02 1C 08 11 12 13 14 15 16 17 18"), 0x6A83);
    CHECK_EQ(run(fs_cmd_update_record, "00 DC 01 1C 08 11 12 13 14 15 16 17 18"), 0x9000);
    // "next" after record 1 in the current EF (an SFI would select the EF again and reset the record pointer).
    CHECK_EQ(run(fs_cmd_update_record, "00 DC 00 02 08 21 22 23 24 25 26 27 28"), 0x6A83);
    CHECK_EQ(used(cyc), 1);
    CHECK_EQ(run(fs_cmd_read_record, "00 B2 01 1C 08"), 0x9000);
    CHECK_HEX(s_out, s_resp.len, "11 12 13 14 15 16 17 18");
    CHECK_EQ(used(cyc), 1);

    // Linear fixed EF 0102 (SFI 2, 4 x 16) is fully allocated: every record up to the capacity can be updated.
    CHECK_EQ(fs_record_count(fixed), 4);
    CHECK_EQ(run(fs_cmd_update_record, "00 DC 04 14 01 41"), 0x9000);
    CHECK_EQ(run(fs_cmd_update_record, "00 DC 05 14 01 41"), 0x6A83);
}

static void pin_checks(void) {
    sec_pin_t *p = sec_find(0x01);
    CHECK(p != NULL);
    if (!p) return;
    sec_reset();
    CHECK_EQ(p->retries, 3);
    CHECK_EQ(p->puk_retries, 10);

    // A malformed VERIFY costs nothing: longer than any PIN can be.
    CHECK_EQ(run(sec_cmd_verify, "00 20 00 01 11 31 32 33 34 31 32 33 34 31 32 33 34 31 32 33 34 31"), 0x6700);
    CHECK_EQ(p->retries, 3);

    // ENABLE / DISABLE VERIFICATION REQUIREMENT P1 = 00 with no data: 6700, the counter is untouched.
    CHECK_EQ(run(sec_cmd_disable_verification, "00 26 00 01"), 0x6700);
    CHECK_EQ(run(sec_cmd_enable_verification, "00 28 00 01"), 0x6700);
    CHECK_EQ(run(sec_cmd_disable_verification, "00 26 00 01 00"), 0x6700);   // Le only, still no data
    CHECK_EQ(p->retries, 3);
    CHECK(!p->disabled);
    CHECK_EQ(run(sec_cmd_disable_verification, "00 26 01 01 04 31 32 33 34"), 0x6700);   // P1 = 01 takes no data
    CHECK(!p->disabled);
    CHECK_EQ(p->retries, 3);

    // CHANGE REFERENCE DATA P1 = 00: a new PIN of a bad length is refused before the old PIN costs a try.
    CHECK_EQ(run(sec_cmd_change_reference_data, "00 24 00 01 06 31 32 33 34 35 36"), 0x6700);   // new: 2 bytes
    CHECK_EQ(run(sec_cmd_change_reference_data, "00 24 00 01 06 39 39 39 39 35 36"), 0x6700);   // wrong old, too
    CHECK_EQ(run(sec_cmd_change_reference_data,
                 "00 24 00 01 0D 31 32 33 34 31 32 33 34 35 36 37 38 39"), 0x6700);               // new: 9 > max 8
    CHECK_EQ(run(sec_cmd_change_reference_data, "00 24 00 01 04 31 32 33 34"), 0x6700);         // no new PIN
    CHECK_EQ(p->retries, 3);
    CHECK_EQ(run(sec_cmd_verify, "00 20 00 01 04 31 32 33 34"), 0x9000);   // still 1234
    sec_reset();

    // Block the PIN.
    CHECK_EQ(run(sec_cmd_verify, "00 20 00 01 04 30 30 30 30"), 0x63C2);
    CHECK_EQ(run(sec_cmd_verify, "00 20 00 01 04 30 30 30 30"), 0x63C1);
    CHECK_EQ(run(sec_cmd_verify, "00 20 00 01 04 30 30 30 30"), 0x6983);
    CHECK_EQ(p->retries, 0);

    // RESET RETRY COUNTER P1 = 01 (PUK only): the correct PUK with an extra byte, a short and an empty field are all
    // 6700 with the PUK counter untouched and the PIN still blocked.
    CHECK_EQ(run(sec_cmd_reset_retry_counter, "00 2C 01 01 09 31 32 33 34 35 36 37 38 39"), 0x6700);
    CHECK_EQ(run(sec_cmd_reset_retry_counter, "00 2C 01 01 07 31 32 33 34 35 36 37"), 0x6700);
    CHECK_EQ(run(sec_cmd_reset_retry_counter, "00 2C 01 01"), 0x6700);
    CHECK_EQ(p->puk_retries, 10);
    CHECK_EQ(p->retries, 0);
    // P1 = 00 (PUK + new PIN): no new PIN, a new PIN that is too short or too long.
    CHECK_EQ(run(sec_cmd_reset_retry_counter, "00 2C 00 01 08 31 32 33 34 35 36 37 38"), 0x6700);
    CHECK_EQ(run(sec_cmd_reset_retry_counter, "00 2C 00 01 0A 31 32 33 34 35 36 37 38 35 36"), 0x6700);
    CHECK_EQ(run(sec_cmd_reset_retry_counter,
                 "00 2C 00 01 11 31 32 33 34 35 36 37 38 31 32 33 34 35 36 37 38 39"), 0x6700);
    CHECK_EQ(run(sec_cmd_reset_retry_counter, "00 2C 00 01 07 31 32 33 34 35 36 37"), 0x6700);
    CHECK_EQ(p->puk_retries, 10);
    CHECK_EQ(p->retries, 0);
    CHECK_EQ(run(sec_cmd_verify, "00 20 00 01 04 31 32 33 34"), 0x6983);   // still blocked
    // P1 = 02 / 03 need a verified PIN; P1 = 03 takes no data.
    CHECK_EQ(run(sec_cmd_reset_retry_counter, "00 2C 02 01 04 35 36 37 38"), 0x6982);
    CHECK_EQ(run(sec_cmd_reset_retry_counter, "00 2C 03 01"), 0x6982);
    CHECK_EQ(run(sec_cmd_reset_retry_counter, "00 2C 03 01 01 00"), 0x6700);
    CHECK_EQ(run(sec_cmd_reset_retry_counter, "00 2C 04 01"), 0x6A86);
    CHECK_EQ(p->retries, 0);
    CHECK_EQ(p->puk_retries, 10);

    // The correct PUK after all of that still unblocks.
    CHECK_EQ(run(sec_cmd_reset_retry_counter, "00 2C 01 01 08 31 32 33 34 35 36 37 38"), 0x9000);
    CHECK_EQ(p->retries, 3);
    CHECK_EQ(p->puk_retries, 10);
    CHECK_EQ(run(sec_cmd_verify, "00 20 00 01 04 31 32 33 34"), 0x9000);

    // Verified: P1 = 02 checks the new PIN's length, P1 = 03 restores the counter.
    CHECK_EQ(run(sec_cmd_reset_retry_counter, "00 2C 02 01 02 35 36"), 0x6700);
    CHECK_EQ(run(sec_cmd_reset_retry_counter, "00 2C 03 01 01 00"), 0x6700);
    CHECK_EQ(run(sec_cmd_verify, "00 20 00 01 04 30 30 30 30"), 0x63C2);
    CHECK_EQ(run(sec_cmd_verify, "00 20 00 01 04 31 32 33 34"), 0x9000);
    CHECK_EQ(run(sec_cmd_verify, "00 20 00 01 04 30 30 30 30"), 0x63C2);   // drops the verified status
    CHECK_EQ(run(sec_cmd_reset_retry_counter, "00 2C 03 01"), 0x6982);
    CHECK_EQ(run(sec_cmd_verify, "00 20 00 01 04 31 32 33 34"), 0x9000);
    CHECK_EQ(run(sec_cmd_reset_retry_counter, "00 2C 03 01"), 0x9000);
    CHECK_EQ(p->retries, 3);

    // P1 = 00 with the PUK and a new PIN, then back to 1234.
    CHECK_EQ(run(sec_cmd_reset_retry_counter, "00 2C 00 01 0C 31 32 33 34 35 36 37 38 35 36 37 38"), 0x9000);
    CHECK_EQ(run(sec_cmd_verify, "00 20 00 01 04 35 36 37 38"), 0x9000);
    CHECK_EQ(run(sec_cmd_reset_retry_counter, "00 2C 00 01 0C 31 32 33 34 35 36 37 38 31 32 33 34"), 0x9000);
    CHECK_EQ(run(sec_cmd_verify, "00 20 00 01 04 31 32 33 34"), 0x9000);
    CHECK_EQ(p->retries, 3);
    CHECK_EQ(p->puk_retries, 10);
}

// A pin_transform profile: the stored form is the plaintext XOR 0x55 (enough to tell stored from plain).
static size_t xor_transform(const sec_pin_t *pin, const uint8_t *in, size_t len, uint8_t *out, size_t cap) {
    (void)pin;
    if (len > cap) return 0;
    for (size_t i = 0; i < len; i++) out[i] = in[i] ^ 0x55;
    return len;
}

static void transform_checks(void) {
    static card_profile_t prof;
    const card_profile_t *saved = card_profile;
    prof = *saved;
    prof.hooks.pin_transform = xor_transform;
    card_profile = &prof;

    // "1234" / PUK "12345678" in stored form; the first entry lacks plain_len (a profile error).
    static sec_pin_t pins[2];
    memset(pins, 0, sizeof pins);
    for (int i = 0; i < 2; i++) {
        sec_pin_t *q = &pins[i];
        q->ref = (uint8_t)(0x11 + i);
        for (int k = 0; k < 4; k++) q->value[k] = (uint8_t)(('1' + k) ^ 0x55);
        q->len = 4;
        for (int k = 0; k < 8; k++) q->puk[k] = (uint8_t)(('1' + k) ^ 0x55);
        q->puk_len = 8;
        q->min_len = 4;
        q->max_len = 8;
        q->max_retries = q->retries = 3;
        q->puk_max_retries = q->puk_retries = 5;
        q->plain_len = i ? 4 : 0;
    }
    unsigned logs = g_log_lines;
    sec_init(pins, 2);
    CHECK_EQ(g_log_lines, logs + 1);   // PIN 11: pin_transform without plain_len
    CHECK(strstr(g_log_last, "PIN 11") && strstr(g_log_last, "plain_len"));

    // Without plain_len the old PIN / the PUK cannot be split off: 6F00, and no try is consumed.
    CHECK_EQ(run(sec_cmd_change_reference_data, "00 24 00 11 08 31 32 33 34 35 36 37 38"), 0x6F00);
    CHECK_EQ(run(sec_cmd_reset_retry_counter, "00 2C 01 11 08 31 32 33 34 35 36 37 38"), 0x6F00);
    CHECK_EQ(run(sec_cmd_reset_retry_counter, "00 2C 00 11 0C 31 32 33 34 35 36 37 38 35 36 37 38"), 0x6F00);
    CHECK_EQ(pins[0].retries, 3);
    CHECK_EQ(pins[0].puk_retries, 5);
    CHECK_EQ(run(sec_cmd_verify, "00 20 00 11 04 31 32 33 34"), 0x9000);   // VERIFY needs no split

    // With plain_len = 4 a candidate of another length cannot match: 6700 without a try.
    CHECK_EQ(run(sec_cmd_verify, "00 20 00 12 05 31 32 33 34 35"), 0x6700);
    CHECK_EQ(run(sec_cmd_disable_verification, "00 26 00 12 03 31 32 33"), 0x6700);
    CHECK_EQ(pins[1].retries, 3);
    CHECK_EQ(run(sec_cmd_change_reference_data, "00 24 00 12 0A 31 32 33 34 35 36 37 38 39 30"), 0x6700);
    CHECK_EQ(pins[1].retries, 3);
    CHECK_EQ(run(sec_cmd_change_reference_data, "00 24 00 12 08 31 32 33 34 35 36 37 38"), 0x9000);
    CHECK_EQ(pins[1].value[0], '5' ^ 0x55);
    CHECK_EQ(run(sec_cmd_verify, "00 20 00 12 04 35 36 37 38"), 0x9000);

    // More PINs than the security status can hold: logged, only SEC_MAX_PINS are used.
    static sec_pin_t many[SEC_MAX_PINS + 1];
    memset(many, 0, sizeof many);
    for (int i = 0; i <= SEC_MAX_PINS; i++) {
        many[i].ref = (uint8_t)(0x20 + i);
        many[i].plain_len = 4;
    }
    logs = g_log_lines;
    sec_init(many, SEC_MAX_PINS + 1);
    CHECK_EQ(g_log_lines, logs + 1);
    CHECK(strstr(g_log_last, "SEC_MAX_PINS") != NULL);
    CHECK(sec_find(0x20 + SEC_MAX_PINS - 1) != NULL);
    CHECK(sec_find(0x20 + SEC_MAX_PINS) == NULL);

    card_profile = saved;
    sec_init(card_profile->pins, card_profile->n_pins);
}

void test_fs_sec(void) {
    static uint8_t saved[4096];
    CHECK(card_profile->nvm_size <= sizeof saved);
    if (card_profile->nvm_size > sizeof saved) return;
    memcpy(saved, card_profile->nvm_blob, card_profile->nvm_size);
    dispatch_reset();
    sec_reset();
    fs_reset();

    sfi_checks();
    record_checks();
    pin_checks();
    transform_checks();

    memcpy(card_profile->nvm_blob, saved, card_profile->nvm_size);
    sec_reset();
    fs_reset();
    dispatch_reset();
    nvm_take_dirty();
    fake_nvm_reset();
}
