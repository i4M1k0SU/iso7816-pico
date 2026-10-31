// One card session: ATR, optional PPS, then T=0 or T=1 until the reader resets or deactivates the card.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Prepare the ATR from the card profile (build or parse).  Logs it.
bool session_init(void);

const uint8_t *session_atr(size_t *len);

// Called from the main loop when RST is high and no session is running. Returns when the session has ended (reset or
// failure).
void session_run(void);

// Protocol of the running session (0 / 1).
uint8_t session_protocol(void);

// Keep the reader waiting (NULL byte / S(WTX)).
void session_keepalive(void);
