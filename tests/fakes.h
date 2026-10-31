#pragma once

#include <stdbool.h>
#include <stdint.h>

extern bool g_verbose_log;       // print the card's log lines
extern uint8_t g_fake_protocol;  // what card_protocol() / card_keepalive() use (0 or 1)

void fake_clear_bootsel(void);
