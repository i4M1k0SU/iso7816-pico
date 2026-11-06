// Non-volatile storage for the card's persistent data (EF contents, PIN retry counters, keys ...) in the last
// NVM_FLASH_BYTES of the flash.
//
// The card profile names one RAM blob (card_profile->nvm_blob / nvm_size); the runtime loads it at boot and writes it
// back when something marked it dirty: while the card is idle in a session (no character traffic for
// NVM_IDLE_SAVE_MS) and, as a fallback, when RST goes low.  Security counters (PIN / PUK tries) do not wait: sec.c
// saves them synchronously before comparing a candidate (see try_pin()).
//
// Wear levelling: the region is divided into slots (each a multiple of the 4 KB erase sector, big enough for the
// header plus the blob).  Every save goes to the next slot with an increasing sequence number and only that slot is
// erased, so each location sees NVM_FLASH_BYTES / slot size times fewer erase cycles.  nvm_load() picks the valid
// slot with the highest sequence number, so a save interrupted by a power loss leaves the previous image intact.  A
// save whose image equals the stored one is skipped altogether.  That guarantee needs at least two slots, so a blob
// for which the region offers only one (NVM_BLOB_FITS() false) is refused by nvm_load() and nvm_save().
//
// Layout id: the header records which layout the blob was written with, nvm_layout_id() =
// NVM_LAYOUT_FRAMEWORK << 16 | card_profile->nvm_version.  nvm_load() takes the newest valid image only when its
// layout id and size are the ones of this firmware; otherwise it hands it to card_profile->hooks.nvm_migrate (if set)
// or keeps the defaults, logs both layout ids and marks the blob dirty so that the new image supersedes the old one.
// Images of the first header format (16 bytes, no layout id) count as layout 0.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Layout version of the framework-owned structs that live inside the application's blob (today sec_pin_t, see
// iso7816_pico/sec.h).  Bump it whenever one of them changes size or meaning: every deployed card then sees a layout
// change at the next boot (migration hook or defaults) instead of reinterpreting the old bytes.
#define NVM_LAYOUT_FRAMEWORK 1u

// The layout id this firmware writes and accepts.
uint32_t nvm_layout_id(void);

typedef enum {
    NVM_SAVE_FAILED,    // flash error or the blob does not fit in one slot
    NVM_SAVE_WRITTEN,   // a new slot was written
    NVM_SAVE_UNCHANGED, // the stored image already matches, nothing written
} nvm_save_result_t;

typedef struct {
    bool valid;     // a stored image was found (by nvm_load or nvm_save)
    uint32_t slot;  // slot holding the current image
    uint32_t seq;   // its sequence number
    uint32_t slots; // number of slots the region offers for this blob size
    // Set by nvm_load(): the newest image in flash (stored_*; 0 when there is none) and what became of it when its
    // layout id or size was not this firmware's (layout_changed): migrated by hooks.nvm_migrate, or defaults.
    uint32_t stored_layout;
    uint32_t stored_size;
    bool layout_changed;
    bool migrated;
} nvm_status_t;

// Load the profile's blob (blob / size are card_profile->nvm_blob / nvm_size): copy the newest valid stored image
// into it when it has this firmware's layout id and size, and return true.  When the newest image has another layout
// id or size, card_profile->hooks.nvm_migrate (if set) gets it; if the hook returns true, the image it wrote into the
// blob is used (true, marked dirty).  Otherwise the blob keeps its defaults (false, marked dirty, logged).  False with
// the blob untouched when the flash holds no image at all.
bool nvm_load(void *blob, size_t size);

// Write the image to the next slot unless it is identical to the stored one.  On the Pico, interrupts are off for a
// few ms per erased sector and a few hundred us per 256-byte page.
nvm_save_result_t nvm_save(const void *blob, size_t size);

// nvm_save() plus one log line: "NVM: saved (slot n, seq m)" / "unchanged" / "save FAILED", followed by `why` when
// it is not NULL (e.g. "PIN try").
nvm_save_result_t nvm_save_logged(const void *blob, size_t size, const char *why);

// True when the region offers the two slots the journal needs for a blob of this size.
bool nvm_fits(size_t size);

// Erase the whole region (factory defaults on the next boot).
bool nvm_erase_all(void);

const nvm_status_t *nvm_status(void);

// Dirty flag: set by anyone who changed the blob, consumed by the runtime.
void nvm_mark_dirty(void);
bool nvm_is_dirty(void);
bool nvm_take_dirty(void);

// ---- Port: the three flash operations, implemented in hal/nvm_port.c (Pico) or tests/fake_nvm.c (host) -----------
// Offsets are relative to the start of the region; erase works in 4096-byte sectors, program in 256-byte pages.
#define NVM_SECTOR_BYTES 4096u
#define NVM_PAGE_BYTES   256u
const uint8_t *nvm_port_map(void); // read-only view of the whole region
bool nvm_port_erase(uint32_t offset, size_t len);
bool nvm_port_program(uint32_t offset, const uint8_t *data, size_t len);
