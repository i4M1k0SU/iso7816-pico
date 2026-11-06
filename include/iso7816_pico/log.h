// Non-blocking logging to the USB serial console (ring buffer, drained in idle time).  Implemented in src/pico/sys/console.c.
#pragma once

#include <stddef.h>
#include <stdint.h>

// Queue a log line (printf style, "\r\n" appended).  Never blocks.
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Queue a one-line APDU trace.
void log_apdu(const char *name, uint8_t cla, uint8_t ins, uint8_t p1, uint8_t p2,
              const uint8_t *in, size_t in_len, const uint8_t *out, size_t out_len, uint16_t sw);

// Push queued log bytes to the host, but only as many as fit right now.
void log_drain(void);
