// Non-volatile storage for the card's persistent data (EF contents, PIN retry counters, keys ...) in the last
// NVM_FLASH_BYTES of the flash.
//
// The card profile names one RAM blob (card_profile->nvm_blob / nvm_size); main() loads it at boot and writes it back
// whenever something marked it dirty and the card is not in a session (RST low), so a flash write never delays a
// command.
//
// Wear levelling: the region is divided into slots (each a multiple of the 4 KB erase sector, big enough for the
// header plus the blob).  Every save goes to the next slot with an increasing sequence number and only that slot is
// erased, so each location sees NVM_FLASH_BYTES / slot size times fewer erase cycles.  nvm_load() picks the valid
// slot with the highest sequence number, so a save interrupted by a power loss leaves the previous image intact.  A
// save whose image equals the stored one is skipped altogether.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    NVM_SAVE_FAILED,      // flash error or the blob does not fit in one slot
    NVM_SAVE_WRITTEN,     // a new slot was written
    NVM_SAVE_UNCHANGED,   // the stored image already matches, nothing written
} nvm_save_result_t;

typedef struct {
    bool valid;           // a stored image was found (by nvm_load or nvm_save)
    uint32_t slot;        // slot holding the current image
    uint32_t seq;         // its sequence number
    uint32_t slots;       // number of slots the region offers for this blob size
} nvm_status_t;

// Copy the newest valid stored image into blob.  Returns false (blob untouched) when the flash holds no image of this
// size.
bool nvm_load(void *blob, size_t size);

// Write the image to the next slot unless it is identical to the stored one.  On the Pico, interrupts are off for a
// few ms per erased sector and a few hundred us per 256-byte page.
nvm_save_result_t nvm_save(const void *blob, size_t size);

// Erase the whole region (factory defaults on the next boot).
bool nvm_erase_all(void);

const nvm_status_t *nvm_status(void);

// Dirty flag: set by anyone who changed the blob, consumed by main().
void nvm_mark_dirty(void);
bool nvm_take_dirty(void);

// ---- Port: the three flash operations, implemented in hal/nvm_port.c (Pico) or tests/fake_nvm.c (host) -----------
// Offsets are relative to the start of the region; erase works in 4096-byte sectors, program in 256-byte pages.
#define NVM_SECTOR_BYTES 4096u
#define NVM_PAGE_BYTES   256u
const uint8_t *nvm_port_map(void);                                        // read-only view of the whole region
bool nvm_port_erase(uint32_t offset, size_t len);
bool nvm_port_program(uint32_t offset, const uint8_t *data, size_t len);
