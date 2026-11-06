// The boot-time profile validator (apdu/profile_check.c): the demo profile is clean, every broken profile below gives
// the expected number of findings with the expected message, and the defects that would make the framework access
// memory outside the profile's objects make fs.c / dispatch.c / sec.c leave that object alone.
#include "apdu/dispatch.h"
#include "apdu/profile_check.h"
#include "fakes.h"
#include "iso7816_pico/card.h"
#include "test.h"

extern const card_profile_t demo_card_profile;

// profile_check(prof) must report n findings, one of them containing `needle`, followed by the summary line.
#define EXPECT(prof, n, needle)                                                                                        \
    do {                                                                                                               \
        unsigned since_ = g_log_lines;                                                                                 \
        CHECK_EQ(profile_check(prof), (n));                                                                            \
        CHECK(fake_log_find(since_, needle));                                                                          \
        CHECK(fake_log_find(since_, "profile: "));                                                                     \
        CHECK(strstr(g_log_last, "finding(s)") != NULL);                                                               \
    } while (0)

static card_profile_t base(void) {
    card_profile_t p;
    memset(&p, 0, sizeof p);
    p.max_channels = 1;
    return p;
}

static uint16_t h_ok(const apdu_t *cmd, apdu_resp_t *resp) {
    (void)cmd;
    (void)resp;
    return SW_OK;
}

static void handler_ok(const char *args) { (void)args; }

static bool access_any(uint8_t ch, const fs_file_t *f, uint8_t op) {
    (void)ch;
    (void)f;
    (void)op;
    return true;
}

static uint8_t s_pool[1024]; // file bodies of the broken trees (no nvm_blob, so where they lie does not matter)

// clang-format off
static const fs_file_t s_ok_files[] = {
    { .fid = 0x3F00, .type = FS_DF },
    { .fid = 0x0101, .type = FS_EF_TRANSPARENT, .parent = &s_ok_files[0], .data = s_pool, .size = 16, .sfi = 1,
      .ac_update = 0x01 },
    { .fid = 0x0102, .type = FS_EF_LINEAR_FIXED, .parent = &s_ok_files[0], .data = s_pool + 16,
      .size = FS_LINEAR_FIXED_BYTES(8, 2), .rec_size = 8, .sfi = 2 },
};

static const fs_file_t s_no_data[] = {
    { .fid = 0x3F00, .type = FS_DF },
    { .fid = 0x0101, .type = FS_EF_TRANSPARENT, .parent = &s_no_data[0], .size = 16, .sfi = 1 },
    { .fid = 0x0102, .type = FS_EF_TRANSPARENT, .parent = &s_no_data[0], .data = s_pool, .size = 16, .sfi = 2 },
};

static const fs_file_t s_foreign_df = { .fid = 0x5000, .type = FS_DF };
static const fs_file_t s_orphans[] = {
    { .fid = 0x3F00, .type = FS_DF },
    { .fid = 0x7F00, .type = FS_DF, .parent = &s_foreign_df, .name = (const uint8_t *)"ORPHAN", .name_len = 6 },
    { .fid = 0x0101, .type = FS_EF_TRANSPARENT, .parent = &s_orphans[1], .data = s_pool, .size = 16 },
};

static const fs_file_t s_bad_mf[] = {
    { .fid = 0x3F01, .type = FS_DF },
};

static const fs_file_t s_tree[] = {
    [0] = { .fid = 0x3F00, .type = FS_DF },
    [1] = { .fid = 0x0101, .type = FS_EF_TRANSPARENT, .parent = &s_tree[0], .data = s_pool, .size = 16, .sfi = 1 },
    [2] = { .fid = 0x0101, .type = FS_EF_TRANSPARENT, .parent = &s_tree[0], .data = s_pool, .size = 16, .sfi = 1 },
    [3] = { .fid = 0x0103, .type = FS_EF_TRANSPARENT, .parent = &s_tree[0], .data = s_pool, .size = 16, .sfi = 31 },
    [4] = { .fid = 0x0104, .type = FS_EF_TRANSPARENT, .parent = &s_tree[1], .data = s_pool, .size = 16 },
    [5] = { .fid = 0x7F10, .type = FS_DF, .parent = &s_tree[0], .data = s_pool, .size = 4 },
    [6] = { .fid = 0x3F00, .type = FS_EF_TRANSPARENT, .parent = &s_tree[5] },
};

static const fs_file_t s_records[] = {
    [0] = { .fid = 0x3F00, .type = FS_DF },
    [1] = { .fid = 0x0101, .type = FS_EF_LINEAR_FIXED, .parent = &s_records[0], .data = s_pool,
            .size = FS_LINEAR_FIXED_BYTES(8, 2) + 3, .rec_size = 8 },
    [2] = { .fid = 0x0102, .type = FS_EF_CYCLIC, .parent = &s_records[0], .data = s_pool + 32, .size = 12 },
    [3] = { .fid = 0x0103, .type = FS_EF_LINEAR_VARIABLE, .parent = &s_records[0], .data = s_pool + 64,
            .size = FS_REC_HDR_SIZE, .rec_size = 8 },
    [4] = { .fid = 0x0104, .type = FS_EF_LINEAR_FIXED, .parent = &s_records[0], .data = s_pool + 96, .size = 2,
            .rec_size = 1 },
    [5] = { .fid = 0x0105, .type = FS_EF_LINEAR_FIXED, .parent = &s_records[0], .data = s_pool + 128,
            .size = FS_LINEAR_FIXED_BYTES(300, 1), .rec_size = 300 },
    [6] = { .fid = 0x0106, .type = FS_EF_LINEAR_FIXED, .parent = &s_records[0], .data = s_pool + 512,
            .size = FS_LINEAR_FIXED_BYTES(8, 2), .rec_size = 8 }, // header claims 5 records (set below)
};

static const fs_file_t s_cycle[] = {
    { .fid = 0x3F00, .type = FS_DF },
    { .fid = 0x7F01, .type = FS_DF, .parent = &s_cycle[2] },
    { .fid = 0x7F02, .type = FS_DF, .parent = &s_cycle[1] },
};

static const fs_file_t s_names[] = {
    { .fid = 0x3F00, .type = FS_DF },
    { .fid = 0x7F01, .type = FS_DF, .parent = &s_names[0], .name = (const uint8_t *)"ABCDE", .name_len = 5 },
    { .fid = 0x7F02, .type = FS_DF, .parent = &s_names[0], .name = (const uint8_t *)"ABCDE", .name_len = 5 },
    { .fid = 0x7F03, .type = FS_DF, .parent = &s_names[0], .name = (const uint8_t *)"ABC", .name_len = 3 },
    { .fid = 0x7F05, .type = FS_DF, .parent = &s_names[0], .name = (const uint8_t *)"XXXXXXXXXXXXXXXXX",
      .name_len = 17 },
};

static const fs_file_t s_ac[] = {
    { .fid = 0x3F00, .type = FS_DF },
    { .fid = 0x0101, .type = FS_EF_TRANSPARENT, .parent = &s_ac[0], .data = s_pool, .size = 16, .ac_read = 0x05 },
};

static struct {
    uint8_t ef[16];
    uint8_t other[16];
} s_blob;
static const fs_file_t s_nvm_files[] = {
    { .fid = 0x3F00, .type = FS_DF },
    { .fid = 0x0101, .type = FS_EF_TRANSPARENT, .parent = &s_nvm_files[0], .data = s_blob.ef, .size = 16 },
    { .fid = 0x0102, .type = FS_EF_TRANSPARENT, .parent = &s_nvm_files[0], .data = s_pool, .size = 16 },
    { .fid = 0x0103, .type = FS_EF_TRANSPARENT, .parent = &s_nvm_files[0], .data = s_blob.ef + 8, .size = 16 },
};
// clang-format on

#define N(a) (sizeof(a) / sizeof((a)[0]))

static void files_checks(void) {
    card_profile_t p = base();
    p.files = s_ok_files;
    p.n_files = N(s_ok_files);
    static sec_pin_t pin = { .ref = 0x01, .len = 4, .min_len = 4, .max_len = 8, .max_retries = 3, .retries = 3 };
    p.pins = &pin;
    p.n_pins = 1;
    unsigned since = g_log_lines;
    CHECK_EQ(profile_check(&p), 0);
    CHECK_EQ(g_log_lines, since);

    // An EF with a size but no data is refused by the lookups; its neighbour is not.
    p.files = s_no_data;
    p.n_files = N(s_no_data);
    EXPECT(&p, 1, "file 0101 (files[1]): size without data");
    CHECK(fake_log_find(since, "disabled (the lookups of fs.c skip it)"));
    fs_init(s_no_data, N(s_no_data));
    CHECK(fs_child(fs_mf(), 0x0101) == NULL);
    CHECK(fs_by_sfi(fs_mf(), 1) == NULL);
    CHECK(fs_child(fs_mf(), 0x0102) == &s_no_data[2]);

    // A parent outside files[]: the DF and everything below it.
    p.files = s_orphans;
    p.n_files = N(s_orphans);
    since = g_log_lines;
    EXPECT(&p, 2, "file 7F00 (files[1]): parent outside files[]");
    CHECK(fake_log_find(since, "file 0101 (files[2]): the parent of an ancestor is outside files[]"));
    fs_init(s_orphans, N(s_orphans));
    CHECK(fs_by_name((const uint8_t *)"ORPHAN", 6, false, NULL) == NULL);

    p.files = s_bad_mf;
    p.n_files = N(s_bad_mf);
    EXPECT(&p, 1, "files[0] must be the MF");

    p.files = s_tree;
    p.n_files = N(s_tree);
    since = g_log_lines;
    EXPECT(&p, 6, "file 0101: FID used twice in DF 3F00");
    CHECK(fake_log_find(since, "file 0101: SFI 1 already used by EF 0101"));
    CHECK(fake_log_find(since, "file 0103: SFI 31 outside 1..30"));
    CHECK(fake_log_find(since, "file 0104: parent 0101 is not a DF"));
    CHECK(fake_log_find(since, "file 7F10: DF with data / size"));
    CHECK(fake_log_find(since, "file 3F00 (files[6]): 3F00 is the MF's FID"));

    s_pool[512] = 5; // FS_REC_HDR(5, 0) in EF 0106
    p.files = s_records;
    p.n_files = N(s_records);
    since = g_log_lines;
    EXPECT(&p, 6, "file 0101: size 23 is not FS_LINEAR_FIXED_BYTES(8, n): 3 bytes unused");
    CHECK(fake_log_find(since, "file 0102: record EF with rec_size 0"));
    CHECK(fake_log_find(since, "file 0103: size 4 holds no record of 8 bytes: use FS_LINEAR_VARIABLE_BYTES"));
    CHECK(fake_log_find(since, "file 0104 (files[4]): record EF smaller than the 4-byte record header"));
    CHECK(fake_log_find(since, "file 0105: rec_size 300"));
    CHECK(fake_log_find(since, "file 0106: the record header says 5 records in use, the file holds 2"));
    s_pool[512] = 0;

    p.files = s_cycle;
    p.n_files = N(s_cycle);
    EXPECT(&p, 2, "file 7F01: the parent chain loops");

    p.files = s_names;
    p.n_files = N(s_names);
    since = g_log_lines;
    EXPECT(&p, 4, "file 7F02: same DF name as DF 7F01");
    CHECK(fake_log_find(since, "file 7F03: DF name is a prefix of DF 7F01's"));
    CHECK(fake_log_find(since, "file 7F05: DF name of 17 bytes"));

    // An access condition naming no PIN is never met, unless hooks.check_access decides instead.
    p.files = s_ac;
    p.n_files = N(s_ac);
    EXPECT(&p, 1, "file 0101: read condition 05 names no PIN in pins[]");
    p.hooks.check_access = access_any;
    CHECK_EQ(profile_check(&p), 0);
    p.hooks.check_access = NULL;

    // EF data in, outside, and across the NVM blob.
    p.files = s_nvm_files;
    p.n_files = N(s_nvm_files);
    p.nvm_blob = s_blob.ef;
    p.nvm_size = sizeof s_blob.ef;
    since = g_log_lines;
    EXPECT(&p, 2, "note: file 0102: data lies outside nvm_blob");
    CHECK(fake_log_find(since, "error: file 0103: data straddles the bounds of nvm_blob"));

    // Only one of files / n_files.
    p = base();
    p.n_files = 3;
    EXPECT(&p, 1, "only one of them is set");

    fs_init(card_profile->files, card_profile->n_files);
}

static void pins_checks(void) {
    card_profile_t p = base();
    // clang-format off
#define PIN(r, len_, min_, max_, tries_, puk_len_)                                                                     \
    { .ref = (r), .len = (len_), .min_len = (min_), .max_len = (max_), .max_retries = (tries_), .retries = (tries_),   \
      .puk_len = (puk_len_) }
    static sec_pin_t pins[] = {
        PIN(0x01, 4, 4, 8, 3, 0),
        PIN(0x01, 4, 4, 8, 3, 0),
        PIN(0x02, 4, 4, 8, 0, 0), // max_retries 0
        PIN(0x03, 4, 8, 4, 3, 0), // min_len > max_len
        PIN(0x00, 4, 4, 8, 3, 0), // ref FS_AC_ALWAYS
        PIN(0x06, 0, 4, 8, 3, 0), // empty
        PIN(0x07, 4, 4, 8, 3, 4), // a PUK without tries
    };
#undef PIN
    // clang-format on
    p.pins = pins;
    p.n_pins = N(pins);
    unsigned since = g_log_lines;
    EXPECT(&p, 6, "PIN 01 (pins[1]): ref used twice: sec_find returns pins[0]");
    CHECK(fake_log_find(since, "PIN 02: max_retries 0"));
    CHECK(fake_log_find(since, "PIN 03: min_len 8 > max_len 4"));
    CHECK(fake_log_find(since, "PIN 00: the ref is FS_AC_ALWAYS"));
    CHECK(fake_log_find(since, "PIN 06: empty stored value"));
    CHECK(fake_log_find(since, "PIN 07: PUK with puk_max_retries 0"));

    // pins NULL with a count: reported, and sec_init uses no PINs.
    p.pins = NULL;
    p.n_pins = 2;
    EXPECT(&p, 1, "pins is NULL with n_pins 2");
    sec_init(NULL, 2);
    CHECK(sec_find(0x01) == NULL);
    sec_init(card_profile->pins, card_profile->n_pins);
}

// clang-format off
static const apdu_cmd_desc_t s_cmds[] = {
    APDU_CMD(0x80, 0xF0, 0x10, APDU_CASE1, "ONE", h_ok),
    APDU_CMD(0x80, 0xF0, 0x10, APDU_CASE1, "AGAIN", h_ok),
    APDU_CMD(0x80, 0xFF, 0x10, APDU_CASE1, "NARROW", h_ok),
    APDU_CMD(0x81, 0xF0, 0x20, APDU_CASE1, "NEVER", h_ok),
    APDU_CMD(0x80, 0xF0, 0x30, APDU_CASE1, "CLASSIFY", NULL),
};
static const uint8_t s_aid6[] = { 0xF0, 1, 2, 3, 4, 5 };
static const uint8_t s_aid17[17] = { 0xF1 };
static const applet_t s_a_cmds = { .name = "cmds", .aid = s_aid6, .aid_len = 6, .cmds = s_cmds, .n_cmds = N(s_cmds) };
static const applet_t s_a_twin = { .name = "twin", .aid = s_aid6, .aid_len = 6 };
static const applet_t s_a_long = { .name = "long", .aid = s_aid6, .aid_len = 6 };
static const applet_t s_a_short = { .name = "short", .aid = s_aid6, .aid_len = 5 };
static const applet_t s_a_tiny = { .name = "tiny", .aid = s_aid6, .aid_len = 3 };
static const applet_t s_a_huge = { .name = "huge", .aid = s_aid17, .aid_len = 17 };
static const applet_t s_a_empty = { .name = "empty", .aid = s_aid6, .aid_len = 0 };
static const applet_t s_a_noaid = { .name = "noaid", .aid = NULL, .aid_len = 5 };
// clang-format on

static void applets_checks(void) {
    card_profile_t p = base();
    const applet_t *list1[] = { &s_a_cmds, &s_a_twin };
    p.applets = list1;
    p.n_applets = N(list1);
    unsigned since = g_log_lines;
    EXPECT(&p, 5, "applet cmds: cmds[1] AGAIN (CLA 80/F0 INS 10) duplicates cmds[0]");
    CHECK(fake_log_find(since, "applet cmds: cmds[2] NARROW (CLA 80/FF INS 10) is shadowed by cmds[0]"));
    CHECK(fake_log_find(since, "applet cmds: cmds[3] NEVER: CLA 81 has bits outside cla_mask F0"));
    CHECK(fake_log_find(since, "applet cmds: cmds[4] CLASSIFY has no handler"));
    CHECK(fake_log_find(since, "applet twin: same AID as cmds"));

    const applet_t *list2[] = { &s_a_long, &s_a_short };
    p.applets = list2;
    p.n_applets = N(list2);
    EXPECT(&p, 1, "error: applet short: AID is a prefix of long's, listed earlier");
    const applet_t *list3[] = { &s_a_short, &s_a_long };
    p.applets = list3;
    EXPECT(&p, 1, "warning: applet long: AID starts with short's");

    const applet_t *list4[] = { &s_a_tiny, &s_a_huge, &s_a_empty, NULL };
    p.applets = list4;
    p.n_applets = N(list4);
    since = g_log_lines;
    EXPECT(&p, 4, "applet tiny: AID of 3 bytes (ISO 7816-5");
    CHECK(fake_log_find(since, "applet huge: AID of 17 bytes"));
    CHECK(fake_log_find(since, "applet empty: empty AID"));
    CHECK(fake_log_find(since, "applets[3]: NULL entry: disabled"));

    p.applets = NULL;
    p.n_applets = 1;
    EXPECT(&p, 1, "applets is NULL with n_applets 1");

    p.n_applets = 0;
    p.default_applet = &s_a_long;
    EXPECT(&p, 1, "default_applet long is not in applets[]");
    p.default_applet = NULL;

    p.max_channels = 0;
    EXPECT(&p, 1, "max_channels 0 is taken as 1");
    p.max_channels = CARD_MAX_CHANNELS + 1;
    EXPECT(&p, 1, "above CARD_MAX_CHANNELS");
    p.max_channels = 1;

    // An applet without its AID is reported and never reset or selected, not even as the default applet; the others
    // still work.
    const applet_t *list5[] = { &s_a_noaid, &s_a_long };
    p.applets = list5;
    p.n_applets = N(list5);
    p.default_applet = &s_a_noaid;
    EXPECT(&p, 1, "applets[0] noaid: aid_len without aid");
    const card_profile_t *saved = card_profile;
    card_profile = &p;
    dispatch_reset();
    uint8_t buf[64];
    apdu_resp_t resp = APDU_RESP_INIT(buf, sizeof buf);
    apdu_t sel = { .ins = 0xA4, .p1 = 0x04, .p2 = 0x0C, .data = s_aid6, .lc = 6 };
    CHECK_EQ(dispatch_process(&sel, &resp), SW_OK);
    sel.lc = 5; // a prefix of long's AID: found as before
    CHECK_EQ(dispatch_process(&sel, &resp), SW_OK);
    card_profile = saved;
    dispatch_reset();
}

static void atr_checks(void) {
    card_profile_t p = base();
    static const uint8_t raw[] = { 0x3B, 0x00 };
    static atr_desc_t desc = { .levels = 1 };
    p.atr = raw;
    p.atr_len = sizeof raw;
    p.atr_desc = &desc;
    EXPECT(&p, 1, "both atr and atr_desc are set");
    p.atr = NULL;
    p.atr_len = 2;
    EXPECT(&p, 1, "only one of atr / atr_len is set");
    p.atr_len = 0;
    CHECK_EQ(profile_check(&p), 0);

    // TA1 = 95 (F 512, D 16): pointless without PPS, fine with it; TA1 = 19 (F 372, D 20) is not an integer ETU.
    desc.level[0].present = ATR_TA;
    desc.level[0].ta = 0x95;
    EXPECT(&p, 1, "note: TA1 offers F=512 D=16 but allow_pps is false");
    p.allow_pps = true;
    CHECK_EQ(profile_check(&p), 0);
    desc.level[0].ta = 0x19;
    EXPECT(&p, 1, "TA1 offers F=372 D=20, which the PIO cannot do");

    // Specific mode (TA2) at parameters the PIO cannot do, and with a protocol the framework lacks.
    desc.levels = 2;
    desc.level[1].present = ATR_TA;
    desc.level[1].ta = 0x00;
    desc.level[1].protocol = 0;
    EXPECT(&p, 1, "specific mode (TA2) at F=372 D=20: the PIO cannot do it");
    desc.level[0].ta = 0x11;
    desc.level[1].ta = 0x02;
    EXPECT(&p, 1, "specific mode (TA2) names T=2");

    p = base();
    p.tx_extra_guard = 21;
    EXPECT(&p, 1, "tx_extra_guard 21 above 20");
}

static void console_checks(void) {
    card_profile_t p = base();
    static const console_cmd_t cmds[] = {
        { "status", "", handler_ok },     { "x", "", handler_ok },   { "x", "", handler_ok },
        { "longername", "", handler_ok }, { "a b", "", handler_ok }, { "y", "", NULL },
    };
    p.console_cmds = cmds;
    p.n_console_cmds = N(cmds);
    unsigned since = g_log_lines;
    EXPECT(&p, 5, "console command 'status': the built-in command of that name runs instead");
    CHECK(fake_log_find(since, "console command 'x': also console_cmds[1]"));
    CHECK(fake_log_find(since, "console command 'longername': longer than 8 characters"));
    CHECK(fake_log_find(since, "console command 'a b': the name contains a blank"));
    CHECK(fake_log_find(since, "console_cmds[5]: without a name or handler"));

    p.console_cmds = NULL;
    EXPECT(&p, 1, "console_cmds is NULL with n_console_cmds 6");

    // Past PROFILE_CHECK_MAX_LINES the findings are counted, not printed.
    static console_cmd_t many[PROFILE_CHECK_MAX_LINES + 6];
    static char names[N(many)][16];
    for (size_t i = 0; i < N(many); i++) {
        snprintf(names[i], sizeof names[i], "command%02u", (unsigned)i);
        many[i] = (console_cmd_t){ names[i], "", handler_ok };
    }
    p.console_cmds = many;
    p.n_console_cmds = N(many);
    since = g_log_lines;
    EXPECT(&p, N(many), "console command 'command00': longer than 8 characters");
    CHECK_EQ(g_log_lines - since, PROFILE_CHECK_MAX_LINES + 2);
    CHECK(fake_log_find(since, "profile: ... 6 more finding(s) not shown"));
}

void test_profile_check(void) {
    // The demo is the template users copy: it must be clean, and print nothing.
    unsigned since = g_log_lines;
    CHECK_EQ(profile_check(&demo_card_profile), 0);
    CHECK_EQ(g_log_lines, since);

    files_checks();
    pins_checks();
    applets_checks();
    atr_checks();
    console_checks();
}
