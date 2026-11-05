// The Pico runtime: everything an application needs to run a card profile, in two calls.
//
//     int main(void) {
//         card_runtime_init(&my_profile);
//         card_runtime_run();
//     }
#pragma once

#include "iso7816_pico/card.h"

// Bring the card up: stdio (USB console), the PIO character layer and its loopback self-test, the NVM image, the
// file system and PINs, the ATR; then enable the watchdog (WATCHDOG_TIMEOUT_MS).
void card_runtime_init(const card_profile_t *profile);

// Serve the card forever: one session per reader activation (ATR, PPS, T=0 / T=1 commands), NVM write-back and
// the BOOTSEL paths (APDU, console, button) in between.  Never returns.
void card_runtime_run(void) __attribute__((noreturn));

// Extra work for the idle time (console, logs and the BOOTSEL button are already serviced there).  Called from the
// main loop between commands and while the card waits for the reader; keep it short.
void card_runtime_set_idle_hook(void (*hook)(void));
