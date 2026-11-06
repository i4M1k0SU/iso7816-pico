// Smallest possible card: a T=0 ATR, one applet with one command, no file system.  Only the public headers are used.
#include "iso7816_pico/runtime.h"

#include <string.h>

static const uint8_t s_aid[] = { 0xF0, 'E', 'M', 'B' };

static uint16_t h_hello(const apdu_t *cmd, apdu_resp_t *resp) {
    (void)cmd;
    resp_put(resp, "hello", 5);
    return SW_OK;
}

static const apdu_cmd_desc_t s_cmds[] = {
    APDU_CMD(0x80, 0xF0, 0x10, APDU_CASE2, "HELLO", h_hello),
};

static const applet_t s_applet = {
    .name = "embed",
    .aid = s_aid,
    .aid_len = sizeof s_aid,
    .cmds = s_cmds,
    .n_cmds = 1,
};

static const applet_t *const s_applets[] = { &s_applet };

static const atr_desc_t s_atr = {
    .levels = 1,
    .historical = { 'E', 'm', 'b', 'e', 'd' },
    .historical_len = 5,
};

static const card_profile_t s_profile = {
    .name = "embed-test",
    .version = "0.1",
    .atr_desc = &s_atr,
    .allow_pps = true,
    .applets = s_applets,
    .n_applets = 1,
    .default_applet = &s_applet,
    .max_channels = 1,
};

int main(void) {
    card_runtime_init(&s_profile);
    card_runtime_run();
}
