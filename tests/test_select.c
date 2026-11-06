// Applet selection and status-word choice of the dispatcher, against a test profile of its own: interindustry commands
// on but no file system, several applets (one whose select() can fail, one with a warning, one decoding its commands in
// a switch with an applet-level classify()), a PIN, four logical channels.
#include "apdu/dispatch.h"
#include "hal/nvm.h"
#include "iso7816_pico/card.h"
#include "iso7816_pico/fs.h"
#include "iso7816_pico/sec.h"
#include "fake_phy.h"
#include "fakes.h"
#include "proto/t0.h"
#include "reader.h"
#include "test.h"

// ---- applet A: the default applet, table-driven ----------------------------------------------------------------------

static const uint8_t s_aid_a[] = { 0xA0, 0x00, 0x00, 0x00, 0x01 };
static int s_a_selects, s_a_deselects;
static uint8_t s_a_last_ch, s_a_last_p2;
static bool s_a_fail;

static uint16_t a_select(const apdu_t *cmd, apdu_resp_t *resp) {
    s_a_selects++;
    s_a_last_ch = cmd->channel;
    s_a_last_p2 = cmd->p2;
    resp_put(resp, "A", 1);   // response data must be discarded on implicit selection
    return s_a_fail ? 0x6999 : SW_OK;
}

static void a_deselect(uint8_t ch) {
    (void)ch;
    s_a_deselects++;
}

static uint16_t a_hello(const apdu_t *cmd, apdu_resp_t *resp) {
    (void)cmd;
    resp_put(resp, "a", 1);
    return SW_OK;
}

static const apdu_cmd_desc_t s_cmds_a[] = {
    APDU_CMD(0x80, 0xF0, 0x10, APDU_CASE2, "A HELLO", a_hello),
    APDU_CMD(0x80, 0xF0, 0x50, APDU_CASE2, "A CLASSIFY ONLY", NULL),
};

static const applet_t s_applet_a = {
    .name = "A", .aid = s_aid_a, .aid_len = sizeof s_aid_a,
    .cmds = s_cmds_a, .n_cmds = 2,
    .select = a_select, .deselect = a_deselect,
};

// ---- applet B: another table-driven applet ----------------------------------------------------------------------------

static const uint8_t s_aid_b[] = { 0xA0, 0x00, 0x00, 0x00, 0x02 };
static int s_b_selects, s_b_deselects;
static uint8_t s_b_last_ch, s_b_last_deselect_ch;

static uint16_t b_select(const apdu_t *cmd, apdu_resp_t *resp) {
    (void)resp;
    s_b_selects++;
    s_b_last_ch = cmd->channel;
    return SW_OK;
}

static void b_deselect(uint8_t ch) {
    s_b_deselects++;
    s_b_last_deselect_ch = ch;
}

static uint16_t b_hello(const apdu_t *cmd, apdu_resp_t *resp) {
    (void)cmd;
    resp_put(resp, "b", 1);
    return SW_OK;
}

static const apdu_cmd_desc_t s_cmds_b[] = {
    APDU_CMD(0x80, 0xF0, 0x30, APDU_CASE2, "B HELLO", b_hello),
};

static const applet_t s_applet_b = {
    .name = "B", .aid = s_aid_b, .aid_len = sizeof s_aid_b,
    .cmds = s_cmds_b, .n_cmds = 1,
    .select = b_select, .deselect = b_deselect,
};

// ---- applet C: switch in process() + classify() ---------------------------------------------------------------------

static const uint8_t s_aid_c[] = { 0xA0, 0x00, 0x00, 0x00, 0x03 };
static uint32_t s_c_last_lc;

static apdu_case_t c_classify(uint8_t cla, uint8_t ins, uint8_t p1, uint8_t p2) {
    (void)p1;
    (void)p2;
    if ((cla & 0xF0) != 0x80) return APDU_CASE_UNKNOWN;
    switch (ins) {
    case 0x42: return APDU_CASE3;   // STORE: data in
    case 0x44: return APDU_CASE4;   // ECHO: data in and out
    default: return APDU_CASE_UNKNOWN;
    }
}

static uint16_t c_process(const apdu_t *cmd, apdu_resp_t *resp) {
    if ((cmd->cla & 0xF0) != 0x80) return APDU_UNHANDLED;
    switch (cmd->ins) {
    case 0x42:
        s_c_last_lc = cmd->lc;
        return SW_OK;
    case 0x44:
        resp_put(resp, cmd->data, cmd->lc);
        return SW_OK;
    default:
        return APDU_UNHANDLED;
    }
}

static const applet_t s_applet_c = {
    .name = "C", .aid = s_aid_c, .aid_len = sizeof s_aid_c,
    .classify = c_classify, .process = c_process,
};

// ---- applet D: select() always fails; applet E: select() answers with a warning -------------------------------------

static const uint8_t s_aid_d[] = { 0xA0, 0x00, 0x00, 0x00, 0x04 };
static const uint8_t s_aid_e[] = { 0xA0, 0x00, 0x00, 0x00, 0x05 };

static uint16_t d_select(const apdu_t *cmd, apdu_resp_t *resp) {
    (void)cmd;
    resp_put(resp, "DD", 2);
    return 0x6999;
}

static uint16_t e_select(const apdu_t *cmd, apdu_resp_t *resp) {
    (void)cmd;
    resp_put(resp, "EE", 2);
    return SW_WARN_FILE_DEACTIVATED;
}

static uint16_t de_hello(const apdu_t *cmd, apdu_resp_t *resp) {
    (void)cmd;
    resp_put(resp, "e", 1);
    return SW_OK;
}

static const apdu_cmd_desc_t s_cmds_de[] = {
    APDU_CMD(0x80, 0xF0, 0x72, APDU_CASE2, "D/E HELLO", de_hello),
};

static const applet_t s_applet_d = {
    .name = "D", .aid = s_aid_d, .aid_len = sizeof s_aid_d, .cmds = s_cmds_de, .n_cmds = 1, .select = d_select,
};
static const applet_t s_applet_e = {
    .name = "E", .aid = s_aid_e, .aid_len = sizeof s_aid_e, .cmds = s_cmds_de, .n_cmds = 1, .select = e_select,
};

// ---- the profile ------------------------------------------------------------------------------------------------------

static const applet_t *const s_applets[] = { &s_applet_a, &s_applet_b, &s_applet_c, &s_applet_d, &s_applet_e };

static sec_pin_t s_pins[] = {
    { .ref = 0x01, .value = "1234", .len = 4, .min_len = 4, .max_len = 8, .max_retries = 3, .retries = 3 },
};

static const card_profile_t s_profile = {
    .name = "select-test",
    .applets = s_applets,
    .n_applets = sizeof s_applets / sizeof s_applets[0],
    .default_applet = &s_applet_a,
    .files = NULL,
    .pins = s_pins,
    .n_pins = 1,
    .iso_commands = true,
    .max_channels = 4,
    .proprietary_cla_decode = true,   // CLA 81 / 82 address channels 1 / 2 (CLA 90 would be 80 + chaining)
};

// ---- helpers ----------------------------------------------------------------------------------------------------------

static reader_result_t r;
static bool s_t1;

static uint16_t sw(const char *hex) {
    uint8_t buf[600];
    size_t n = hex_parse(hex, buf, sizeof buf);
    if (!(s_t1 ? t1r_apdu(buf, n, &r) : t0r_apdu(buf, n, &r))) return 0xFFFF;
    return r.sw;
}

static void session(bool t1) {
    s_t1 = t1;
    g_fake_protocol = t1 ? 1 : 0;
    dispatch_reset();
    if (t1) {
        t1r_begin(254);
    } else {
        t0_session_begin();
        t0r_begin();
    }
}

static void use_profile(const card_profile_t *p) {
    card_profile = p;
    fs_init(p->files, p->n_files);
    sec_init(p->pins, p->n_pins);
}

void test_select(void) {
    const card_profile_t *saved = card_profile;
    use_profile(&s_profile);

    // --- implicit selection after the ATR runs select() (P2 = 0C, response dropped) ---------------------------------
    s_a_selects = 0;
    session(false);
    CHECK_EQ(s_a_selects, 1);
    CHECK_EQ(s_a_last_ch, 0);
    CHECK_EQ(s_a_last_p2, 0x0C);
    CHECK_EQ(sw("80 10 00 00 01"), 0x9000);
    CHECK_HEX(r.data, r.len, "61");

    // --- iso_commands without a file system: real status words instead of 6E00 --------------------------------------
    CHECK_EQ(sw("00 A4 00 00 02 3F 00"), 0x6A82);   // SELECT MF
    CHECK_EQ(sw("00 A4 00 0C 00"), 0x6A82);
    CHECK_EQ(sw("00 A4 08 04 02 01 01"), 0x6A82);   // path
    CHECK_EQ(sw("00 B0 00 00 10"), 0x6986);         // no current EF
    CHECK_EQ(sw("00 B0 81 00 10"), 0x6A82);         // short EF identifier not found
    CHECK_EQ(sw("00 D6 00 00 02 01 02"), 0x6986);
    CHECK_EQ(sw("00 B2 01 04 10"), 0x6986);
    CHECK_EQ(sw("00 B2 01 0C 10"), 0x6A82);         // SFI 1
    CHECK_EQ(sw("00 E2 00 00 02 01 02"), 0x6986);
    CHECK_EQ(sw("00 A4 04 00 02 11 22"), 0x6A82);   // unknown AID, no DF names either
    CHECK_EQ(sw("80 10 00 00 01"), 0x9000);         // ... and A is still selected

    // --- 6E00 only for a handler under another CLA in the current context ------------------------------------------
    CHECK_EQ(sw("A0 10 00 00 01"), 0x6E00);         // A's INS 10 exists under CLA 80
    CHECK_EQ(sw("80 30 00 00 01"), 0x6D00);         // B's INS: B is not selected
    CHECK_EQ(sw("80 50 00 00 01"), 0x6D00);         // A's classification-only entry
    CHECK_EQ(sw("80 B0 00 00 10"), 0x6E00);         // READ BINARY exists under CLA 00
    CHECK_EQ(sw("80 A4 04 00 05 A0 00 00 00 02"), 0x6E00);   // SELECT and MANAGE CHANNEL: interindustry only
    CHECK_EQ(sw("80 70 00 00 01"), 0x6E00);
    CHECK_EQ(sw("00 11 00 00 00"), 0x6D00);
    CHECK_EQ(sw("80 11 00 00 00"), 0x6D00);

    // --- explicit SELECT: deselect the previous applet, select the new one ------------------------------------------
    s_a_deselects = 0;
    s_b_selects = s_b_deselects = 0;
    CHECK_EQ(sw("00 A4 04 00 05 A0 00 00 00 02"), 0x9000);
    CHECK_EQ(s_a_deselects, 1);
    CHECK_EQ(s_b_selects, 1);
    CHECK_EQ(sw("80 30 00 00 01"), 0x9000);
    CHECK_EQ(sw("A0 10 00 00 01"), 0x6D00);         // nothing with INS 10 in this context any more

    // --- a failed select() leaves no applet selected and clears the security state ----------------------------------
    CHECK_EQ(sw("00 20 00 01 04 31 32 33 34"), 0x9000);
    CHECK(sec_is_verified(0, 0x01));
    CHECK_EQ(sw("00 A4 04 00 05 A0 00 00 00 04"), 0x6999);
    CHECK_EQ(r.len, 0);                             // its response data is dropped
    CHECK_EQ(s_b_deselects, 1);                     // the previous applet was deselected
    CHECK(!sec_is_verified(0, 0x01));
    CHECK_EQ(sw("80 72 00 00 01"), 0x6D00);         // D's command does not reach D
    CHECK_EQ(sw("80 30 00 00 01"), 0x6D00);         // nor B's to B
    CHECK_EQ(sw("00 A4 04 00 05 A0 00 00 00 02"), 0x9000);
    CHECK_EQ(s_b_deselects, 1);                     // nothing was selected, so nobody to deselect
    CHECK_EQ(sw("80 30 00 00 01"), 0x9000);

    // --- a warning keeps the applet selected (and its response data) ------------------------------------------------
    CHECK_EQ(sw("00 A4 04 00 05 A0 00 00 00 05"), 0x6283);
    CHECK_HEX(r.data, r.len, "45 45");
    CHECK_EQ(sw("80 72 00 00 01"), 0x9000);
    CHECK_HEX(r.data, r.len, "65");

    // --- switch-style applet: its classify() tells T=0 that P3 is Lc --------------------------------------------------
    CHECK_EQ(sw("00 A4 04 0C 05 A0 00 00 00 03"), 0x9000);
    s_c_last_lc = 0;
    CHECK_EQ(sw("80 42 00 00 03 01 02 03"), 0x9000);
    CHECK_EQ(s_c_last_lc, 3);
    CHECK_EQ(sw("80 44 00 00 02 AA BB"), 0x9000);
    CHECK_HEX(r.data, r.len, "AA BB");
    CHECK_EQ(dispatch_classify(0x80, 0x42, 0, 0), APDU_CASE3);
    CHECK_EQ(dispatch_classify(0x80, 0x45, 0, 0), APDU_CASE_UNKNOWN);   // "not mine"
    CHECK_EQ(dispatch_classify(0x81, 0x42, 0, 0), APDU_CASE_UNKNOWN);   // channel 1: nothing selected
    CHECK_EQ(dispatch_classify(0x00, 0xB0, 0, 0), APDU_CASE2);          // interindustry table still used
    CHECK_EQ(sw("80 48 00 00 00"), 0x6D00);         // process() declined

    // --- MANAGE CHANNEL open from the basic channel: the default applet, select() on the new channel ----------------
    s_a_selects = 0;
    CHECK_EQ(sw("00 70 00 00 01"), 0x9000);
    CHECK_HEX(r.data, r.len, "01");
    CHECK_EQ(s_a_selects, 1);
    CHECK_EQ(s_a_last_ch, 1);
    CHECK_EQ(sw("81 10 00 00 01"), 0x9000);         // A on channel 1 (channel 0 has C)
    CHECK_EQ(sw("81 42 00 00 01 00"), 0x6D00);
    CHECK_EQ(sw("80 42 00 00 01 00"), 0x9000);      // C stays on channel 0

    // --- ... and from another channel: that channel's applet ----------------------------------------------------------
    s_b_selects = 0;
    CHECK_EQ(sw("01 A4 04 00 05 A0 00 00 00 02"), 0x9000);
    CHECK_EQ(s_b_selects, 1);
    CHECK_EQ(sw("01 70 00 00 01"), 0x9000);
    CHECK_HEX(r.data, r.len, "02");
    CHECK_EQ(s_b_selects, 2);
    CHECK_EQ(s_b_last_ch, 2);
    CHECK_EQ(sw("82 30 00 00 01"), 0x9000);
    s_b_deselects = 0;
    CHECK_EQ(sw("00 70 80 02 00"), 0x9000);
    CHECK_EQ(s_b_deselects, 1);
    CHECK_EQ(s_b_last_deselect_ch, 2);

    // --- a select() failing during MANAGE CHANNEL open: the channel is not opened ------------------------------------
    s_a_fail = true;
    CHECK_EQ(sw("00 70 00 03 00"), 0x6999);
    CHECK_EQ(sw("03 B0 00 00 01"), 0x6881);
    CHECK_EQ(sw("00 70 00 00 01"), 0x6999);
    s_a_fail = false;
    CHECK_EQ(sw("00 70 00 00 01"), 0x9000);
    CHECK_HEX(r.data, r.len, "02");

    // --- failed implicit selection after the ATR: no applet --------------------------------------------------------
    s_a_fail = true;
    session(false);
    CHECK_EQ(sw("80 10 00 00 01"), 0x6D00);
    s_a_fail = false;

    // --- T=1: GET RESPONSE / ENVELOPE reach the dispatcher ------------------------------------------------------------
    session(true);
    CHECK_EQ(sw("00 C0 00 00 00"), 0x6985);
    CHECK_EQ(sw("00 C2 00 00 02 01 02"), 0x6D00);
    CHECK_EQ(sw("80 C0 00 00 00"), 0x6D00);
    CHECK_EQ(sw("00 B0 00 00 10"), 0x6986);
    CHECK_EQ(sw("80 10 00 00 01"), 0x9000);

    // Back to the demo card for the groups that follow.
    use_profile(saved);
    session(false);
    nvm_take_dirty();
    fake_phy_clear();
}
