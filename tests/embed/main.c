// Smallest possible card: a T=0 ATR, one applet with one command, no file system.  Only the public headers are used.
#include "iso7816_pico/runtime.h"

#include <string.h>

static const uint8_t s_aid[] = { 0xF0, 'E', 'M', 'B' };

// The greeting is per applet instance (applet_t.ctx), which the handler reaches through cmd->applet.
static uint16_t h_hello(const apdu_t *cmd, apdu_resp_t *resp) {
    const char *greeting = cmd->applet && cmd->applet->ctx ? cmd->applet->ctx : "hello";
    resp_put(resp, greeting, strlen(greeting));
    return SW_OK;
}

static void embed_reset(const applet_t *self) { (void)self; }

static void embed_deselect(const applet_t *self, uint8_t channel) {
    (void)self;
    (void)channel;
}

static const apdu_cmd_desc_t s_cmds[] = {
    APDU_CMD(0x80, 0xF0, 0x10, APDU_CASE2, "HELLO", h_hello),
};

static const applet_t s_applet = {
    .name = "embed",
    .aid = s_aid,
    .aid_len = sizeof s_aid,
    .ctx = "hello",
    .cmds = s_cmds,
    .n_cmds = 1,
    .deselect = embed_deselect,
    .reset = embed_reset,
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
