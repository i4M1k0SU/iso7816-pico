// ============================================================================
//  Example applet: shows how an application plugs into the framework.
//
//  Copy this file to start your own applet, give it an AID, list its
//  commands in the table at the bottom and add it to card_profile->applets.
// ============================================================================
#include "demo_applet.h"

#include "iso7816_pico/fs.h"

#include "pico/stdlib.h"

#include <string.h>

static const uint8_t s_aid[] = { 0xF0, 'P', 'I', 'C', 'O' };
static uint32_t s_counter;


// ---------------------------------------------------------------------------
// life cycle
// ---------------------------------------------------------------------------

// After every ATR: volatile state back to its defaults.
static void demo_reset(void) {
    // s_counter deliberately survives a warm reset.
}

// SELECT by AID, and the implicit selection after every ATR (this is the default applet).  The dispatcher already made
// the DF with the same name current; here we also make its first EF current so that READ BINARY works right away, and
// answer with the default FCI.  Returning an error instead would leave no applet selected on the channel.
static uint16_t demo_select(const apdu_t *cmd, apdu_resp_t *resp) {
    fs_cursor_t *c = fs_cursor(cmd->channel);
    const fs_file_t *ef = fs_child(c->df, 0x0101);
    if (ef) fs_select_file(cmd->channel, ef);
    if ((cmd->p2 & 0x0C) != 0x0C) card_put_default_fci(resp, s_aid, sizeof s_aid);
    return SW_OK;
}

// ---------------------------------------------------------------------------
// command handlers
// ---------------------------------------------------------------------------

// 80 10 00 00 Le  -> "<name> <version>" from the card profile
static uint16_t h_get_version(const apdu_t *cmd, apdu_resp_t *resp) {
    (void)cmd;
    const char *name = card_profile->name ? card_profile->name : "iso7816-pico";
    resp_put(resp, name, strlen(name));
    if (card_profile->version) {
        resp_put_u8(resp, ' ');
        resp_put(resp, card_profile->version, strlen(card_profile->version));
    }
    return SW_OK;
}

// 80 20 00 00 Lc <data> [Le] -> same data back
static uint16_t h_echo(const apdu_t *cmd, apdu_resp_t *resp) {
    resp_put(resp, cmd->data, cmd->lc);
    return SW_OK;
}

// 80 30 00 00 04  -> 32-bit big-endian counter, incremented on every call
static uint16_t h_get_counter(const apdu_t *cmd, apdu_resp_t *resp) {
    (void)cmd;
    s_counter++;
    uint8_t b[4] = { (uint8_t)(s_counter >> 24), (uint8_t)(s_counter >> 16),
                     (uint8_t)(s_counter >> 8), (uint8_t)s_counter };
    resp_put(resp, b, 4);
    return SW_OK;
}

// 80 40 00 00 00  -> takes ~2 s, using the keep-alive so the reader waits
static uint16_t h_slow(const apdu_t *cmd, apdu_resp_t *resp) {
    (void)cmd;
    for (int i = 0; i < 4; i++) {
        sleep_ms(500);
        card_keepalive();
    }
    resp_put(resp, "done", 4);
    return SW_OK;
}

// 80 E0 00 00 00  -> 90 00, then the Pico reboots into BOOTSEL (USB mass storage)
static uint16_t h_bootsel(const apdu_t *cmd, apdu_resp_t *resp) {
    (void)cmd;
    (void)resp;
    card_request_bootsel();
    return SW_OK;
}

// ---------------------------------------------------------------------------
// command table: CLA 80 exactly (mask F0 keeps channel / SM bits out of the comparison but rejects e.g. CLA 90), INS,
// T=0 case, name, handler.
// ---------------------------------------------------------------------------

static const apdu_cmd_desc_t s_cmds[] = {
    APDU_CMD(0x80, 0xF0, 0x10, APDU_CASE2, "GET VERSION",    h_get_version),
    APDU_CMD(0x80, 0xF0, 0x20, APDU_CASE4, "ECHO",           h_echo),
    APDU_CMD(0x80, 0xF0, 0x30, APDU_CASE2, "GET COUNTER",    h_get_counter),
    APDU_CMD(0x80, 0xF0, 0x40, APDU_CASE2, "SLOW",           h_slow),
    APDU_CMD(0x80, 0xF0, 0xE0, APDU_CASE1, "REBOOT BOOTSEL", h_bootsel),
};

const applet_t demo_applet = {
    .name = "demo",
    .aid = s_aid,
    .aid_len = sizeof s_aid,
    .cmds = s_cmds,
    .n_cmds = sizeof s_cmds / sizeof s_cmds[0],
    .select = demo_select,
    .deselect = NULL,
    .classify = NULL,     // every command is in the table, which also gives the T=0 case
    .process = NULL,      // everything is in the table; ISO commands fall through.  A switch on INS here instead
                          // needs .classify as well, or T=0 drops the data field of the commands not in the table
    .reset = demo_reset,
};
