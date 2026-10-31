#include "nvm.h"

#include "config.h"

#include <string.h>

#define NVM_MAGIC 0x4D564E37u   // "7NVM"

typedef struct {
    uint32_t magic;
    uint32_t size;    // blob size (an image of another size is ignored)
    uint32_t seq;     // increases with every write; the highest valid one wins
    uint32_t crc;     // CRC-32 of the blob
} nvm_header_t;

static bool s_dirty;
static nvm_status_t s_status;

static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

// Slot size for a blob: header + blob rounded up to whole sectors.  0 when it does not fit.
static uint32_t slot_bytes(size_t size) {
    size_t need = sizeof(nvm_header_t) + size;
    uint32_t bytes = (uint32_t)((need + NVM_SECTOR_BYTES - 1) / NVM_SECTOR_BYTES * NVM_SECTOR_BYTES);
    return bytes <= NVM_FLASH_BYTES ? bytes : 0;
}

static const uint8_t *slot_ptr(uint32_t slot, uint32_t bytes) { return nvm_port_map() + (size_t)slot * bytes; }

// Header of a slot if it holds a valid image of this size.
static bool slot_valid(uint32_t slot, uint32_t bytes, size_t size, nvm_header_t *h) {
    const uint8_t *p = slot_ptr(slot, bytes);
    memcpy(h, p, sizeof *h);
    if (h->magic != NVM_MAGIC || h->size != size) return false;
    return crc32(p + sizeof *h, size) == h->crc;
}

// Sequence numbers wrap: "a is newer than b" with the usual signed trick.
static bool newer(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }

// Find the newest valid slot.  Returns false when there is none.
static bool find_current(size_t size, uint32_t bytes, uint32_t *slot, uint32_t *seq) {
    bool found = false;
    uint32_t n = NVM_FLASH_BYTES / bytes;
    for (uint32_t i = 0; i < n; i++) {
        nvm_header_t h;
        if (!slot_valid(i, bytes, size, &h)) continue;
        if (!found || newer(h.seq, *seq)) {
            *slot = i;
            *seq = h.seq;
            found = true;
        }
    }
    return found;
}

bool nvm_load(void *blob, size_t size) {
    uint32_t bytes = slot_bytes(size);
    s_status = (nvm_status_t){ 0 };
    if (!bytes) return false;
    s_status.slots = NVM_FLASH_BYTES / bytes;
    uint32_t slot, seq;
    if (!find_current(size, bytes, &slot, &seq)) return false;
    memcpy(blob, slot_ptr(slot, bytes) + sizeof(nvm_header_t), size);
    s_status.valid = true;
    s_status.slot = slot;
    s_status.seq = seq;
    return true;
}

nvm_save_result_t nvm_save(const void *blob, size_t size) {
    uint32_t bytes = slot_bytes(size);
    if (!bytes) return NVM_SAVE_FAILED;
    uint32_t n = NVM_FLASH_BYTES / bytes;
    s_status.slots = n;

    // Where the current image is (nvm_load() may not have run, or the flash may have changed since).
    uint32_t slot = 0, seq = 0;
    bool have = find_current(size, bytes, &slot, &seq);
    if (have && memcmp(slot_ptr(slot, bytes) + sizeof(nvm_header_t), blob, size) == 0) {
        s_status.valid = true;
        s_status.slot = slot;
        s_status.seq = seq;
        return NVM_SAVE_UNCHANGED;
    }

    uint32_t next = have ? (slot + 1) % n : 0;
    uint32_t next_seq = have ? seq + 1 : 1;
    uint32_t base = next * bytes;
    if (!nvm_port_erase(base, bytes)) return NVM_SAVE_FAILED;

    // Program page by page: the header shares the first page with the start of the blob.
    nvm_header_t h = { NVM_MAGIC, (uint32_t)size, next_seq, crc32(blob, size) };
    const uint8_t *src = blob;
    size_t total = sizeof h + size;
    uint8_t page[NVM_PAGE_BYTES];
    for (size_t off = 0; off < total; off += NVM_PAGE_BYTES) {
        memset(page, 0xFF, sizeof page);
        for (size_t i = 0; i < NVM_PAGE_BYTES && off + i < total; i++) {
            size_t pos = off + i;
            page[i] = pos < sizeof h ? ((const uint8_t *)&h)[pos] : src[pos - sizeof h];
        }
        if (!nvm_port_program((uint32_t)(base + off), page, sizeof page)) return NVM_SAVE_FAILED;
    }

    nvm_header_t check;
    if (!slot_valid(next, bytes, size, &check)) return NVM_SAVE_FAILED;   // read back
    s_status.valid = true;
    s_status.slot = next;
    s_status.seq = next_seq;
    return NVM_SAVE_WRITTEN;
}

bool nvm_erase_all(void) {
    s_status = (nvm_status_t){ 0 };
    return nvm_port_erase(0, NVM_FLASH_BYTES);
}

const nvm_status_t *nvm_status(void) { return &s_status; }

void nvm_mark_dirty(void) { s_dirty = true; }

bool nvm_take_dirty(void) {
    bool d = s_dirty;
    s_dirty = false;
    return d;
}
