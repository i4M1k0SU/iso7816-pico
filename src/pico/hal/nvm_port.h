// Pico-specific parts of the NVM port (the portable interface is in hal/nvm.h).
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Whether the NVM region (the last NVM_FLASH_BYTES of PICO_FLASH_SIZE_BYTES) lies entirely behind the program image.
// When it does not, nvm_port_erase() / nvm_port_program() refuse every operation, so a large program or a wrong board
// flash size can never erase its own code.  *program_end / *region_start (XIP addresses) are filled in when not NULL.
bool nvm_port_region_ok(uintptr_t *program_end, uintptr_t *region_start);
