#pragma once

#include <stdbool.h>
#include <stdint.h>

extern bool g_verbose_log;      // print the card's log lines
extern uint8_t g_fake_protocol; // what card_protocol() / card_keepalive() use (0 or 1)
extern unsigned g_log_lines;    // log_printf() lines so far
extern char g_log_last[200];    // the last one

// Is `needle` part of one of the log lines numbered since..g_log_lines-1 (only the last FAKE_LOG_KEEP are kept)?
#define FAKE_LOG_KEEP 64
bool fake_log_find(unsigned since, const char *needle);

void fake_clear_bootsel(void);
