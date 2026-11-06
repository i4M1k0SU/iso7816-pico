#pragma once

#include <stdbool.h>
#include <stdint.h>

extern bool g_verbose_log;       // print the card's log lines
extern uint8_t g_fake_protocol;  // what card_protocol() / card_keepalive() use (0 or 1)
extern unsigned g_log_lines;     // log_printf() lines so far
extern char g_log_last[200];     // the last one

void fake_clear_bootsel(void);
