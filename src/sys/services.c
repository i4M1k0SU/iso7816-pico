// Services offered to card code (declared in card/card.h).
#include "card/card.h"

#include "hal/nvm.h"
#include "proto/session.h"

static bool s_bootsel;

void card_keepalive(void) { session_keepalive(); }

uint8_t card_protocol(void) { return session_protocol(); }

void card_nvm_changed(void) { nvm_mark_dirty(); }

void card_request_bootsel(void) { s_bootsel = true; }

bool card_bootsel_requested(void) { return s_bootsel; }
