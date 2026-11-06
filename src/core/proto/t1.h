// ISO 7816-3 T=1 block transmission protocol, card side.
//
// I-blocks with chaining in both directions, R-blocks (retransmission), S-blocks (RESYNCH, IFS, ABORT, WTX), LRC or CRC
// error detection.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t ifsc; // our maximum information field size (from ATR TA3)
    bool crc;     // CRC instead of LRC (from ATR TC3)
    uint8_t cwi;  // character waiting time integer (from ATR TB3, default 13): CWT = (11 + 2^CWI) ETU
} t1_config_t;

// Call once per card session (after the ATR / PPS).
void t1_session_begin(const t1_config_t *cfg);

// Receive one block and react to it (which may include executing a command and sending the response).  first_byte is
// the NAD if the session layer already consumed it (else -1).  Returns false when the session ended.
bool t1_handle_block(int first_byte);

// Called by long-running handlers: S(WTX request) so that the reader extends its block waiting time.  Returns false
// when the session ended.
bool t1_keepalive(void);
