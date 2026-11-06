// APDU dispatcher: CLA decoding (channels, chaining, secure messaging), applet selection, routing to applets /
// interindustry commands / hooks. This is the boundary between the transports (T=0, T=1) and the card.
#pragma once

#include "iso7816_pico/apdu.h"

// Forget all session state (selected applets, security status, chaining). Called after every ATR.
void dispatch_reset(void);

// Classify a command header for T=0 (which needs to know whether P3 is Lc or Le): the table and classify() of the
// applet selected on the addressed channel, the interindustry table, then hooks.classify.  APDU_CASE_UNKNOWN when
// nobody knows the INS; T=0 then treats it as case 1 (P3 ignored, no data received).
apdu_case_t dispatch_classify(uint8_t cla, uint8_t ins, uint8_t p1, uint8_t p2);

// Name of the command for logging ("?" if unknown).
const char *dispatch_name(uint8_t cla, uint8_t ins);

// Execute one complete command APDU.  Returns the status word; response data (if any) is in *resp.  Never returns
// APDU_UNHANDLED.  A success / warning SW whose response overflowed resp->cap becomes 6F00 without data.
uint16_t dispatch_process(const apdu_t *cmd, apdu_resp_t *resp);

// True when nobody served the command but its INS has a handler under another CLA in the current context (applet
// selected on the addressed channel, interindustry commands, SELECT / MANAGE CHANNEL): 6E00 instead of 6D00.
bool dispatch_cla_mismatch(const apdu_t *cmd);

// Buffers owned by the dispatcher: the transports fill cmd_buf with the command data field and hand resp_buf to
// dispatch_process().
uint8_t *dispatch_cmd_buf(void);
// The response buffer is APDU_BUF_SIZE + 2 bytes, but transports pass cap = APDU_BUF_SIZE to dispatch_process(): the
// two spare bytes let a transport append SW1 SW2 right after the response data (T=1 sends data + SW from here).  The
// contents stay valid until the next dispatch_process() call.
uint8_t *dispatch_resp_buf(void);
