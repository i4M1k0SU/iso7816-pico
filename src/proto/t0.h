// ISO 7816-3 T=0 transmission protocol, card side.
//
// Maps APDUs onto TPDUs: procedure bytes, 61 xx / GET RESPONSE, 6C xx, ENVELOPE for extended-length commands, NULL byte
// (60) for keep-alive.
#pragma once

#include <stdbool.h>

// Call once per card session (after the ATR / PPS).
void t0_session_begin(void);

// Wait for one command TPDU, execute it and answer.  first_byte is the CLA if the session layer already consumed it
// (else -1).  Returns false when the session ended (reset asserted or I/O timeout).
bool t0_handle_command(int first_byte);

// Called by long-running handlers: sends the NULL procedure byte so that the reader keeps waiting (resets its work
// waiting time).
bool t0_keepalive(void);
