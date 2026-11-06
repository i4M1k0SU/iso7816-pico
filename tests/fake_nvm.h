#pragma once

#include <stdint.h>

void fake_nvm_reset(void);   // all FF, counters cleared
uint8_t *fake_nvm_raw(void); // the simulated flash region (NVM_FLASH_BYTES)
uint32_t fake_nvm_erase_count(void);
uint32_t fake_nvm_program_count(void);
