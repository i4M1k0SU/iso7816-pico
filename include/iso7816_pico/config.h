// Build-time configuration of the framework (pins, buffer sizes, limits) with its defaults.
//
// Every value can be overridden from CMake before add_subdirectory():
//     set(ISO7816_PIN_IO 4 CACHE STRING "")          # or -DISO7816_PIN_IO=4 on the command line
// The root CMakeLists.txt turns the cache variables into compile definitions on the library targets, so the same
// values reach the framework, the application and the public headers.
//
// Card behaviour (ATR, files, PINs, applets, hooks) is NOT here: it lives in the application's card profile
// (see iso7816_pico/card.h and examples/demo/card_profile.c).
#pragma once

#include <stdint.h>

// ---- Card contact wiring (Pico GPIO numbers) --------------------------------------------------------------------
//   C1 VCC  -> VSYS (via diode)   : powers the Pico when no USB is attached
//   C2 RST  -> GP2 (1 kOhm)
//   C3 CLK  -> GP1 (1 kOhm)       : patched into the PIO programs at start-up
//   C5 GND  -> GND
//   C7 I/O  -> GP4 (1 kOhm)
//
// These defaults are the wiring of the development board.  The adapter was documented as RST=GP4, CLK=GP3, I/O=GP2,
// but watching the GPIOs during a reader activation (console command `gpio`) showed: GP4 released first, the clock
// on GP1 12 us later, GP2 raised 40000 clocks later and lowered first at deactivation.  That is the ISO 7816-3 order
// I/O, CLK, RST, hence the mapping below.  Run `gpio` again if the board is ever rewired.
#ifndef ISO7816_PIN_IO
#define ISO7816_PIN_IO   4
#endif
#ifndef ISO7816_PIN_CLK
#define ISO7816_PIN_CLK  1
#endif
#ifndef ISO7816_PIN_RST
#define ISO7816_PIN_RST  2
#endif

// Loopback self-test at every boot (phy_selftest(), result in the boot log): a test clock generated on
// ISO7816_SELFTEST_PIN_CLK and the I/O line looped on ISO7816_SELFTEST_PIN_IO run the whole ATR path for a few
// milliseconds without touching the card contacts.  Both pins must be unconnected (the clock pin is driven), and both
// are left unassigned (GPIO_FUNC_NULL) afterwards.  An application that needs them (GP16 / GP17 are the SDK's default
// SPI0 pins) chooses other spare pins here or sets either value to -1, which skips the test.
//
// The pins are checked when the framework is compiled: all of them GP0..GP29, the three contacts distinct, and the
// self-test pins distinct from each other and from the contacts.
#ifndef ISO7816_SELFTEST_PIN_CLK
#define ISO7816_SELFTEST_PIN_CLK 16
#endif
#ifndef ISO7816_SELFTEST_PIN_IO
#define ISO7816_SELFTEST_PIN_IO  17
#endif
#define ISO7816_SELFTEST_ENABLED (ISO7816_SELFTEST_PIN_CLK >= 0 && ISO7816_SELFTEST_PIN_IO >= 0)

// ---- Protocol limits --------------------------------------------------------------------------------------------
// Largest command data / response data the APDU layer buffers.  Extended length APDUs up to this size work over T=1
// (and over T=0 via ENVELOPE / GET RESPONSE).  65535 is the ISO maximum; RAM is the only limit.
#ifndef APDU_BUF_SIZE
#define APDU_BUF_SIZE     4096
#endif

// Logical channels (ISO 7816-4 MANAGE CHANNEL).  1 = basic channel only. Up to 4 with the basic CLA coding, up to 20
// with the extended coding.
#ifndef CARD_MAX_CHANNELS
#define CARD_MAX_CHANNELS 4
#endif

// Longest PIN / reference data the security module stores.
#ifndef SEC_PIN_MAX_LEN
#define SEC_PIN_MAX_LEN   16
#endif

// ---- Non-volatile memory -------------------------------------------------------------------------------------------
// Flash area (at the very end of the chip) used by the NVM module to persist the card's data between power cycles.
// It is divided into slots of (header + blob) rounded up to 4 KB sectors and the slots are written in turn, so with a
// blob under 4 KB these 64 KB give 16 slots, i.e. 16 times the erase endurance of a single sector.  Must be a multiple
// of 4096.
#ifndef NVM_FLASH_BYTES
#define NVM_FLASH_BYTES   (64 * 1024)
#endif

// A dirty NVM blob is written back once the card has seen no character traffic for this long while waiting for the
// next command (and, as a fallback, when RST goes low).
#ifndef NVM_IDLE_SAVE_MS
#define NVM_IDLE_SAVE_MS  200
#endif

// Slot size for a blob of `size` bytes (16-byte header + blob, rounded up to 4 KB sectors), and whether the region
// holds the two slots the journal needs: with a single slot a save would have to erase the only image before
// rewriting it, so such a blob is refused (not persisted).  Usable in a static assertion, e.g.
//   _Static_assert(NVM_BLOB_FITS(sizeof(my_nvm_t)), "NVM blob too large for NVM_FLASH_BYTES");
#define NVM_SLOT_BYTES(size) ((((size) + 16u) + 4095u) / 4096u * 4096u)
#define NVM_BLOB_FITS(size)  (NVM_SLOT_BYTES(size) * 2u <= (NVM_FLASH_BYTES))

// ---- Misc ---------------------------------------------------------------------------------------------------------
#ifndef LOG_RING_SIZE
#define LOG_RING_SIZE     4096
#endif
// If the main loop ever stops servicing the card for this long, the watchdog reboots the Pico so that it never needs
// a manual reset to come back.  0 disables the watchdog.
#ifndef WATCHDOG_TIMEOUT_MS
#define WATCHDOG_TIMEOUT_MS 3000
#endif
