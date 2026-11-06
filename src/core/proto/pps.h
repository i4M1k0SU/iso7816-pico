// ISO 7816-3 Protocol and Parameters Selection (card side).
//
// A PPS request starts with FF, which can never be the first byte of a T=0 command (CLA FF is invalid) nor of a T=1
// block (NAD FF is invalid), so the session layer calls pps_handle() when it sees FF before the first command.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "hal/phy.h"

typedef struct {
    uint8_t protocol; // T selected by PPS0
    bool fd_changed;  // PPS1 accepted: f / d below apply
    uint16_t f;
    uint8_t d;
} pps_result_t;

// Receive the rest of the request (PPSS = FF already consumed), decide, answer, and apply the new parameters to the
// PHY.  Returns false when the session ended or the request was invalid (the card stays mute then, which makes the
// reader fall back to a reset).
bool pps_handle(const phy_params_t *current, uint16_t offered_protocols,
                bool (*accept)(uint16_t f, uint8_t d, uint8_t t), pps_result_t *out);
