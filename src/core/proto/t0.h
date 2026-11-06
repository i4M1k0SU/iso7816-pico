// ISO 7816-3 T=0 transmission protocol, card side.
//
// Maps APDUs onto TPDUs: procedure bytes, 61 xx / GET RESPONSE, 6C xx, ENVELOPE for extended-length commands, NULL byte
// (60) for keep-alive.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Call once per card session (after the ATR / PPS).
void t0_session_begin(void);

// Waiting time integer announced in the ATR (TC2; 0 or not called: the default 10).  Bytes inside a TPDU (rest of the
// header, data field) must arrive within WWT = 960 x D x WI ETU, else the partial TPDU is dropped.  Call after
// t0_session_begin(), which resets it.
void t0_set_wi(uint8_t wi);

// Wait for one command TPDU, execute it and answer.  first_byte is the CLA if the session layer already consumed it
// (else -1).  Returns false when the session ended (reset asserted or I/O timeout).
bool t0_handle_command(int first_byte);

// Called by long-running handlers: sends the NULL procedure byte so that the reader keeps waiting (resets its work
// waiting time).
bool t0_keepalive(void);
