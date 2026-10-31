#include "nvm.h"

#include "config.h"

#include "hardware/flash.h"
#include "pico/flash.h"

#include <string.h>

#define NVM_OFFSET (PICO_FLASH_SIZE_BYTES - NVM_FLASH_BYTES)
#define NVM_MAGIC  0x4D564E37u   // "7NVM"

typedef struct {
    uint32_t magic;
    uint32_t size;
    uint32_t crc;
} nvm_header_t;

static bool s_dirty;

static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

bool nvm_load(void *blob, size_t size) {
    const uint8_t *base = (const uint8_t *)(XIP_BASE + NVM_OFFSET);
    nvm_header_t h;
    memcpy(&h, base, sizeof h);
    if (h.magic != NVM_MAGIC || h.size != size) return false;
    if (sizeof h + size > NVM_FLASH_BYTES) return false;
    if (crc32(base + sizeof h, size) != h.crc) return false;
    memcpy(blob, base + sizeof h, size);
    return true;
}

typedef struct {
    const uint8_t *image;
    size_t len;
} save_args_t;

static void do_save(void *arg) {
    const save_args_t *a = arg;
    flash_range_erase(NVM_OFFSET, NVM_FLASH_BYTES);
    flash_range_program(NVM_OFFSET, a->image, a->len);
}

bool nvm_save(const void *blob, size_t size) {
    static uint8_t image[NVM_FLASH_BYTES];   // page-aligned, multiple of 256
    if (sizeof(nvm_header_t) + size > NVM_FLASH_BYTES) return false;
    nvm_header_t h = { NVM_MAGIC, (uint32_t)size, crc32(blob, size) };
    memset(image, 0xFF, sizeof image);
    memcpy(image, &h, sizeof h);
    memcpy(image + sizeof h, blob, size);
    size_t len = (sizeof h + size + 255u) & ~(size_t)255u;
    save_args_t a = { image, len };
    return flash_safe_execute(do_save, &a, 1000) == PICO_OK;
}

void nvm_mark_dirty(void) { s_dirty = true; }

bool nvm_take_dirty(void) {
    bool d = s_dirty;
    s_dirty = false;
    return d;
}
