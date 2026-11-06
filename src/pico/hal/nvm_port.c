// NVM port for the Pico: the region is the last NVM_FLASH_BYTES of the flash, read through XIP and written with the
// SDK's flash functions under flash_safe_execute() (interrupts off, other core parked).
#include "hal/nvm_port.h"
#include "hal/nvm.h"

#include "iso7816_pico/config.h"
#include "iso7816_pico/log.h"

#include "hardware/flash.h"
#include "pico/error.h"
#include "pico/flash.h"

_Static_assert(NVM_FLASH_BYTES > 0 && NVM_FLASH_BYTES % FLASH_SECTOR_SIZE == 0,
               "NVM_FLASH_BYTES must be a non-zero multiple of the 4096-byte flash sector");
_Static_assert(NVM_FLASH_BYTES < PICO_FLASH_SIZE_BYTES, "NVM_FLASH_BYTES must be smaller than the board's flash");

#define NVM_OFFSET (PICO_FLASH_SIZE_BYTES - NVM_FLASH_BYTES)

typedef struct {
    uint32_t offset;
    const uint8_t *data;
    size_t len;
} op_t;

bool nvm_port_region_ok(uintptr_t *program_end, uintptr_t *region_start) {
    uintptr_t start = XIP_BASE + NVM_OFFSET;
#if PICO_NO_FLASH
    uintptr_t end = XIP_BASE; // the program runs from RAM and occupies no flash
#else
    extern char __flash_binary_end; // linker script: end of everything the image stores in flash
    uintptr_t end = (uintptr_t)&__flash_binary_end;
#endif
    if (program_end) *program_end = end;
    if (region_start) *region_start = start;
    return end <= start;
}

const uint8_t *nvm_port_map(void) { return (const uint8_t *)(XIP_BASE + NVM_OFFSET); }

static void do_erase(void *arg) {
    const op_t *op = arg;
    flash_range_erase(NVM_OFFSET + op->offset, op->len);
}

static void do_program(void *arg) {
    const op_t *op = arg;
    flash_range_program(NVM_OFFSET + op->offset, op->data, op->len);
}

// flash_safe_execute() with a reason in the log on failure: the generic "NVM: save FAILED" would not tell why.
static bool run_safe(void (*fn)(void *), op_t *op) {
    if (!nvm_port_region_ok(NULL, NULL)) return false; // reported once at boot by card_runtime_init()
    int rc = flash_safe_execute(fn, op, 1000);
    if (rc == PICO_OK) return true;
    log_printf("NVM: flash_safe_execute() failed (%d)%s", rc,
               rc == PICO_ERROR_NOT_PERMITTED || rc == PICO_ERROR_TIMEOUT
                   ? ": if core1 runs, it must call flash_safe_execute_core_init()"
                   : "");
    return false;
}

bool nvm_port_erase(uint32_t offset, size_t len) {
    if (offset % NVM_SECTOR_BYTES || len % NVM_SECTOR_BYTES || offset + len > NVM_FLASH_BYTES) return false;
    op_t op = { offset, NULL, len };
    return run_safe(do_erase, &op);
}

bool nvm_port_program(uint32_t offset, const uint8_t *data, size_t len) {
    if (offset % NVM_PAGE_BYTES || len % NVM_PAGE_BYTES || offset + len > NVM_FLASH_BYTES) return false;
    op_t op = { offset, data, len };
    return run_safe(do_program, &op);
}
