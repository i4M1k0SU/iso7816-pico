// NVM port for the Pico: the region is the last NVM_FLASH_BYTES of the flash, read through XIP and written with the
// SDK's flash functions under flash_safe_execute() (interrupts off, other core parked).
#include "nvm.h"

#include "config.h"

#include "hardware/flash.h"
#include "pico/flash.h"

#define NVM_OFFSET (PICO_FLASH_SIZE_BYTES - NVM_FLASH_BYTES)

typedef struct {
    uint32_t offset;
    const uint8_t *data;
    size_t len;
} op_t;

const uint8_t *nvm_port_map(void) { return (const uint8_t *)(XIP_BASE + NVM_OFFSET); }

static void do_erase(void *arg) {
    const op_t *op = arg;
    flash_range_erase(NVM_OFFSET + op->offset, op->len);
}

static void do_program(void *arg) {
    const op_t *op = arg;
    flash_range_program(NVM_OFFSET + op->offset, op->data, op->len);
}

bool nvm_port_erase(uint32_t offset, size_t len) {
    if (offset % NVM_SECTOR_BYTES || len % NVM_SECTOR_BYTES || offset + len > NVM_FLASH_BYTES) return false;
    op_t op = { offset, NULL, len };
    return flash_safe_execute(do_erase, &op, 1000) == PICO_OK;
}

bool nvm_port_program(uint32_t offset, const uint8_t *data, size_t len) {
    if (offset % NVM_PAGE_BYTES || len % NVM_PAGE_BYTES || offset + len > NVM_FLASH_BYTES) return false;
    op_t op = { offset, data, len };
    return flash_safe_execute(do_program, &op, 1000) == PICO_OK;
}
