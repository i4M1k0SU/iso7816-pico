// Non-volatile storage for the card's persistent data (EF contents, PIN retry counters, keys ...) in the last
// NVM_FLASH_BYTES of the flash.
//
// The card profile names one RAM blob (card_profile.nvm_blob / nvm_size); main() loads it at boot and writes it back
// whenever something marked it dirty and the card is not in a session (RST low), so a flash write never delays a
// command.
#pragma once

#include <stdbool.h>
#include <stddef.h>

// Copy the stored image into blob if a valid one exists.  Returns false (blob untouched) when the flash holds no image
// of this size.
bool nvm_load(void *blob, size_t size);

// Erase + program the image.  Interrupts are off for a few ms.
bool nvm_save(const void *blob, size_t size);

// Dirty flag: set by anyone who changed the blob, consumed by main().
void nvm_mark_dirty(void);
bool nvm_take_dirty(void);
