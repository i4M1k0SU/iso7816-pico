// RAM-backed NVM port with flash semantics: erase sets bytes to FF, programming can only clear bits, and both must be
// aligned.  Lets the tests inspect and corrupt the "flash".
#include "fake_nvm.h"
#include "hal/nvm.h"

#include "iso7816_pico/config.h"

#include <string.h>

static uint8_t s_flash[NVM_FLASH_BYTES];
static uint32_t s_erases, s_programs;

void fake_nvm_reset(void) {
    memset(s_flash, 0xFF, sizeof s_flash);
    s_erases = s_programs = 0;
}

uint8_t *fake_nvm_raw(void) { return s_flash; }
uint32_t fake_nvm_erase_count(void) { return s_erases; }
uint32_t fake_nvm_program_count(void) { return s_programs; }

const uint8_t *nvm_port_map(void) { return s_flash; }

bool nvm_port_erase(uint32_t offset, size_t len) {
    if (offset % NVM_SECTOR_BYTES || len % NVM_SECTOR_BYTES || offset + len > NVM_FLASH_BYTES) return false;
    memset(s_flash + offset, 0xFF, len);
    s_erases++;
    return true;
}

bool nvm_port_program(uint32_t offset, const uint8_t *data, size_t len) {
    if (offset % NVM_PAGE_BYTES || len % NVM_PAGE_BYTES || offset + len > NVM_FLASH_BYTES) return false;
    for (size_t i = 0; i < len; i++) s_flash[offset + i] &= data[i]; // bits only go 1 -> 0
    s_programs++;
    return true;
}
