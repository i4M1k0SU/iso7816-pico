// Board-level configuration for iso7816-pico (pins, buffer sizes, limits).
//
// Card behaviour (ATR, files, PINs, applets, hooks) is NOT here: it lives in src/card/card_profile.c, which is the part
// you are meant to edit.
#pragma once

#include <stdint.h>

// ---- Card contact wiring (Pico GPIO numbers) ------------------------------
//   C1 VCC  -> VSYS (via diode)   : powers the Pico when no USB is attached
//   C2 RST  -> GP2 (1 kOhm)
//   C3 CLK  -> GP1 (1 kOhm)       : patched into the PIO programs at start-up
//   C5 GND  -> GND
//   C7 I/O  -> GP4 (1 kOhm)
//
// The adapter was documented as RST=GP4, CLK=GP3, I/O=GP2, but watching the GPIOs during a reader activation (console
// command `g`) showed: GP4 released first, the clock on GP1 12 us later, GP2 raised 40000 clocks later and lowered
// first at deactivation.  That is the ISO 7816-3 order I/O, CLK, RST, hence the mapping below.  Run `g` again if the
// board is ever rewired.
#define ISO7816_PIN_IO   4
#define ISO7816_PIN_CLK  1
#define ISO7816_PIN_RST  2

// ---- Protocol limits -----------------------------------------------------------
// Largest command data / response data the APDU layer buffers.  Extended length APDUs up to this size work over T=1
// (and over T=0 via ENVELOPE / GET RESPONSE).  65535 is the ISO maximum; RAM is the only limit.
#define APDU_BUF_SIZE     4096

// Logical channels (ISO 7816-4 MANAGE CHANNEL).  1 = basic channel only. Up to 4 with the basic CLA coding, up to 20
// with the extended coding.
#define CARD_MAX_CHANNELS 4

// Longest PIN / reference data the security module stores.
#define SEC_PIN_MAX_LEN   16

// ---- Non-volatile memory ---------------------------------------------------------
// Flash area (at the very end of the chip) used by hal/nvm.c to persist the card's data between power cycles.  It is
// divided into slots of (header + blob) rounded up to 4 KB sectors and the slots are written in turn, so with a blob
// under 4 KB these 64 KB give 16 slots, i.e. 16 times the erase endurance of a single sector.  Must be a multiple of
// 4096.
#define NVM_FLASH_BYTES   (64 * 1024)

// ---- Misc ----------------------------------------------------------------------
#define LOG_RING_SIZE     4096
#define WATCHDOG_TIMEOUT_MS 3000
