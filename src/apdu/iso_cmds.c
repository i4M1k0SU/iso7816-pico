#include "iso_cmds.h"

#include "card/card.h"
#include "fs.h"
#include "sec.h"

// Hook-backed commands (ISO 7816-4 GET/PUT DATA, -8 security operations, -9 file management).  6A81 "function not
// supported" when the profile provides no implementation.
#define HOOK_HANDLER(fn, member)                                                   \
    static uint16_t fn(const apdu_t *cmd, apdu_resp_t *resp) {                    \
        if (!card_profile.hooks.member) return SW_FUNC_NOT_SUPPORTED;             \
        return card_profile.hooks.member(cmd, resp);                              \
    }

HOOK_HANDLER(h_get_data, get_data)
HOOK_HANDLER(h_put_data, put_data)
HOOK_HANDLER(h_mse, manage_security_environment)
HOOK_HANDLER(h_pso, perform_security_operation)
HOOK_HANDLER(h_gen_key, generate_asymmetric_key_pair)
HOOK_HANDLER(h_create_file, create_file)
HOOK_HANDLER(h_delete_file, delete_file)

// Interindustry class: CLA b8 = 0 (00..7F, including the extended channel coding 4X..7X).  Channel / SM / chaining bits
// are ignored by the mask.
#define ISO(ins, kind, name, handler) APDU_CMD(0x00, 0x80, ins, kind, name, handler)

static const apdu_cmd_desc_t s_table[] = {
    // --- ISO 7816-4 file / data -----------------------------------------------
    ISO(0xA4, APDU_CASE4, "SELECT",                 fs_cmd_select),
    ISO(0xB0, APDU_CASE2, "READ BINARY",            fs_cmd_read_binary),
    ISO(0xD0, APDU_CASE3, "WRITE BINARY",           fs_cmd_write_binary),
    ISO(0xD6, APDU_CASE3, "UPDATE BINARY",          fs_cmd_update_binary),
    ISO(0x0E, APDU_CASE3, "ERASE BINARY",           fs_cmd_erase_binary),
    ISO(0xB2, APDU_CASE2, "READ RECORD",            fs_cmd_read_record),
    ISO(0xD2, APDU_CASE3, "WRITE RECORD",           fs_cmd_write_record),
    ISO(0xDC, APDU_CASE3, "UPDATE RECORD",          fs_cmd_update_record),
    ISO(0xE2, APDU_CASE3, "APPEND RECORD",          fs_cmd_append_record),
    ISO(0x0C, APDU_CASE1, "ERASE RECORD",           fs_cmd_erase_record),
    ISO(0xA2, APDU_CASE4, "SEARCH RECORD",          fs_cmd_search_record),
    ISO(0xCA, APDU_CASE2, "GET DATA",               h_get_data),
    ISO(0xDA, APDU_CASE3, "PUT DATA",               h_put_data),
    // --- ISO 7816-4 security ---------------------------------------------------
    ISO(0x20, APDU_CASE3, "VERIFY",                 sec_cmd_verify),
    ISO(0x24, APDU_CASE3, "CHANGE REFERENCE DATA",  sec_cmd_change_reference_data),
    ISO(0x26, APDU_CASE3, "DISABLE VERIFICATION",   sec_cmd_disable_verification),
    ISO(0x28, APDU_CASE3, "ENABLE VERIFICATION",    sec_cmd_enable_verification),
    ISO(0x2C, APDU_CASE3, "RESET RETRY COUNTER",    sec_cmd_reset_retry_counter),
    ISO(0x84, APDU_CASE2, "GET CHALLENGE",          sec_cmd_get_challenge),
    ISO(0x82, APDU_CASE3, "EXTERNAL AUTHENTICATE",  sec_cmd_external_authenticate),
    ISO(0x88, APDU_CASE4, "INTERNAL AUTHENTICATE",  sec_cmd_internal_authenticate),
    // --- ISO 7816-8 ------------------------------------------------------------
    ISO(0x22, APDU_CASE3, "MANAGE SECURITY ENV",    h_mse),
    ISO(0x2A, APDU_CASE4, "PERFORM SECURITY OP",    h_pso),
    ISO(0x46, APDU_CASE4, "GENERATE KEY PAIR",      h_gen_key),
    // --- ISO 7816-9 ------------------------------------------------------------
    ISO(0xE0, APDU_CASE3, "CREATE FILE",            h_create_file),
    ISO(0xE4, APDU_CASE3, "DELETE FILE",            h_delete_file),
    // --- transport / dispatcher level (classification only) -----------------------
    ISO(0x70, APDU_CASE2, "MANAGE CHANNEL",         NULL),
    ISO(0xC0, APDU_CASE2, "GET RESPONSE",           NULL),
    ISO(0xC2, APDU_CASE3, "ENVELOPE",               NULL),
};

const apdu_cmd_desc_t *iso_cmds_table(size_t *count) {
    *count = sizeof s_table / sizeof s_table[0];
    return s_table;
}

const apdu_cmd_desc_t *iso_cmds_find(uint8_t cla, uint8_t ins) {
    for (size_t i = 0; i < sizeof s_table / sizeof s_table[0]; i++) {
        const apdu_cmd_desc_t *d = &s_table[i];
        if (d->ins == ins && (cla & d->cla_mask) == d->cla) return d;
    }
    return NULL;
}

uint16_t iso_cmds_process(const apdu_t *cmd, apdu_resp_t *resp) {
    const apdu_cmd_desc_t *d = iso_cmds_find(cmd->cla, cmd->ins);
    if (!d || !d->handler) return APDU_UNHANDLED;
    return d->handler(cmd, resp);
}
