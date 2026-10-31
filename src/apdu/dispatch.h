// APDU dispatcher: CLA decoding (channels, chaining, secure messaging), applet selection, routing to applets /
// interindustry commands / hooks. This is the boundary between the transports (T=0, T=1) and the card.
#pragma once

#include "apdu.h"

// Forget all session state (selected applets, security status, chaining). Called after every ATR.
void dispatch_reset(void);

// Classify a command header for T=0 (which needs to know whether P3 is Lc or Le).  APDU_CASE_UNKNOWN when nobody knows
// the INS; T=0 then treats it as case 1 and the dispatcher answers 6D00 / 6E00.
apdu_case_t dispatch_classify(uint8_t cla, uint8_t ins, uint8_t p1, uint8_t p2);

// Name of the command for logging ("?" if unknown).
const char *dispatch_name(uint8_t cla, uint8_t ins);

// Execute one complete command APDU.  Returns the status word; response data (if any) is in *resp.  Never returns
// APDU_UNHANDLED.
uint16_t dispatch_process(const apdu_t *cmd, apdu_resp_t *resp);

// True when some command with this INS exists (6E00 instead of 6D00).
bool dispatch_ins_known(uint8_t ins);

// Buffers owned by the dispatcher: the transports fill cmd_buf with the command data field and hand resp_buf to
// dispatch_process().
uint8_t *dispatch_cmd_buf(void);
uint8_t *dispatch_resp_buf(void);
