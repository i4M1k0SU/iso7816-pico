// Interindustry commands of ISO 7816-4 (and the hooks for -8 / -9 ones):
// the descriptor table used for T=0 classification and the fallback handler the dispatcher calls when no applet took
// the command.
#pragma once

#include "iso7816_pico/apdu.h"

// Table of all interindustry commands this framework knows.
const apdu_cmd_desc_t *iso_cmds_table(size_t *count);

// Find the descriptor for (CLA, INS) or NULL.
const apdu_cmd_desc_t *iso_cmds_find(uint8_t cla, uint8_t ins);

// Execute if known; APDU_UNHANDLED otherwise.
uint16_t iso_cmds_process(const apdu_t *cmd, apdu_resp_t *resp);
