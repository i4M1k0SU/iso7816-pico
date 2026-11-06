// Shared set-up for the libFuzzer harnesses: the demo card profile over the fake PHY and the RAM "flash" of the host
// tests, brought back to the same state before every input so that a run depends on nothing but its input.
#pragma once

#include "iso7816_pico/card.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// A harness check that does not hold.  A bare abort() shows up in libFuzzer's output only as "deadly signal", so say
// which check it was first.
#define FUZZ_FAIL() (fprintf(stderr, "fuzz check failed at %s:%d\n", __FILE__, __LINE__), abort())

// Inputs larger than this are skipped: everything the transports can buffer fits easily, and the fake PHY queue
// (64 KB) must hold the input plus the bytes a harness adds to it.
#define FUZZ_MAX_INPUT 16384

// The mutable copy of the demo profile that card_profile points at (a harness may change hooks or versions in it;
// fuzz_card_reset() restores it).
extern card_profile_t g_fuzz_profile;

// Restore the profile, the NVM blob (files and PINs), the RAM flash, the fake PHY, the RNG and every session layer
// (dispatcher, fs / sec cursors and security status, T=0, T=1 with the given configuration), as after a fresh ATR.
// protocol is what card_protocol() / card_keepalive() report (0 or 1).
void fuzz_card_reset(uint8_t protocol, bool t1_crc, uint8_t t1_ifsc);

// Push the whole input into the fake PHY queue in one go.
void fuzz_phy_push(const uint8_t *data, size_t len);
