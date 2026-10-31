// Fake PHY for host-side tests: a byte queue from the "reader" to the card, a log of everything the card transmits,
// and a hook that lets a reader model react to each transmitted byte (so that it can queue the next bytes the card is
// going to wait for).  Everything is single threaded: the card code runs inside the test, the reader model runs inside
// the hook and in between calls.
#pragma once

#include "hal/phy.h"

// Bytes the card will receive next (reader -> card).
void fake_phy_push(const uint8_t *data, size_t len);
void fake_phy_push_hex(const char *hex);
size_t fake_phy_pending(void);
void fake_phy_clear(void);

// Everything the card transmitted since the last clear (card -> reader).
const uint8_t *fake_phy_tx_log(size_t *len);
void fake_phy_tx_clear(void);

// Called for every byte the card transmits.
void fake_phy_set_tx_hook(void (*hook)(uint8_t b));

// Simulate the reader asserting RST: phy_rx / phy_tx fail from now on.
void fake_phy_set_reset(bool asserted);

// Counters.
uint32_t fake_phy_apply_count(void);     // phy_apply_params() calls
uint32_t fake_phy_rx_timeouts(void);     // phy_rx_timeout() calls that found the queue empty
