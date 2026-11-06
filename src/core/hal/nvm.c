#include "hal/nvm.h"

#include "iso7816_pico/card.h"
#include "iso7816_pico/config.h"
#include "iso7816_pico/log.h"

#include <string.h>

#define NVM_MAGIC    0x324E5637u // "7VN2": header with a layout id
#define NVM_MAGIC_V1 0x4D564E37u // "7NVM": the first format, 16-byte header without a layout id (layout 0)

typedef struct {
    uint32_t magic;
    uint32_t size;     // blob size
    uint32_t seq;      // increases with every write; the newest valid image wins
    uint32_t crc;      // CRC-32 of the blob
    uint32_t layout;   // nvm_layout_id() of the firmware that wrote it
    uint32_t reserved; // FFFFFFFF; keeps the blob 8-byte aligned in flash
} nvm_header_t;

#define NVM_HEADER_V1_BYTES 16u

_Static_assert(sizeof(nvm_header_t) == 24, "NVM_SLOT_BYTES() in config.h assumes a 24-byte header");
_Static_assert(NVM_SECTOR_BYTES == 4096u, "NVM_SLOT_BYTES() in config.h assumes 4 KB sectors");
_Static_assert(NVM_FLASH_BYTES % NVM_SECTOR_BYTES == 0, "NVM_FLASH_BYTES must be a multiple of 4096");

// A valid image found in flash, in either header format.
typedef struct {
    uint32_t off; // offset of its header in the region (a sector boundary)
    uint32_t hdr; // header bytes (24, or 16 for the first format)
    uint32_t size, seq, layout;
} nvm_image_t;

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

uint32_t nvm_layout_id(void) {
    uint32_t app = card_profile ? card_profile->nvm_version : 0;
    return ((uint32_t)NVM_LAYOUT_FRAMEWORK << 16) | app;
}

bool nvm_fits(size_t size) { return size <= NVM_FLASH_BYTES && NVM_BLOB_FITS(size); }

// Slot size for a blob: header + blob rounded up to whole sectors.  0 when the region does not hold two such slots:
// with one slot every save would erase the only image before rewriting it, so a power loss in between would lose
// everything.  Logged once.
static uint32_t slot_bytes(size_t size) {
    if (!nvm_fits(size)) {
        static bool logged;
        if (!logged) {
            logged = true;
            log_printf("NVM: %lu-byte blob needs 2 slots of %lu bytes but NVM_FLASH_BYTES is %lu; not persisted",
                       (unsigned long)size, (unsigned long)NVM_SLOT_BYTES(size), (unsigned long)NVM_FLASH_BYTES);
        }
        return 0;
    }
    return (uint32_t)NVM_SLOT_BYTES(size);
}

// Header at a sector boundary, if it looks like one (magic, plausible size).  The CRC is checked separately.
static bool header_at(uint32_t off, nvm_image_t *im, uint32_t *crc) {
    const uint8_t *p = nvm_port_map() + off;
    nvm_header_t h;
    memcpy(&h, p, sizeof h);
    if (h.magic == NVM_MAGIC) {
        im->hdr = sizeof h;
        im->layout = h.layout;
    } else if (h.magic == NVM_MAGIC_V1) {
        im->hdr = NVM_HEADER_V1_BYTES;
        im->layout = 0;
    } else {
        return false;
    }
    if (h.size == 0 || h.size > NVM_FLASH_BYTES - off - im->hdr) return false;
    im->off = off;
    im->size = h.size;
    im->seq = h.seq;
    *crc = h.crc;
    return true;
}

static const uint8_t *image_blob(const nvm_image_t *im) { return nvm_port_map() + im->off + im->hdr; }

// Sequence numbers wrap: "a is newer than b" with the usual signed trick.
static bool newer(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }

// The newest valid image in the region, whatever its size, layout or header format.  Every slot starts at a sector
// boundary for any blob size, so the sectors are scanned one by one; only a candidate newer than the best so far has
// its CRC computed.  Returns false when there is none.
static bool find_newest(nvm_image_t *best) {
    bool found = false;
    for (uint32_t off = 0; off < NVM_FLASH_BYTES; off += NVM_SECTOR_BYTES) {
        nvm_image_t im;
        uint32_t crc;
        if (!header_at(off, &im, &crc)) continue;
        if (found && !newer(im.seq, best->seq)) continue;
        if (crc32(image_blob(&im), im.size) != crc) continue;
        *best = im;
        found = true;
    }
    return found;
}

// The image is one this firmware writes: current header format, same layout id, same size.
static bool image_matches(const nvm_image_t *im, size_t size) {
    return im->hdr == sizeof(nvm_header_t) && im->layout == nvm_layout_id() && im->size == size;
}

bool nvm_load(void *blob, size_t size) {
    uint32_t bytes = slot_bytes(size);
    s_status = (nvm_status_t){ 0 };
    if (!bytes) return false;
    s_status.slots = NVM_FLASH_BYTES / bytes;
    nvm_image_t im;
    if (!find_newest(&im)) return false;
    s_status.slot = im.off / bytes;
    s_status.seq = im.seq;
    s_status.stored_layout = im.layout;
    s_status.stored_size = im.size;
    if (image_matches(&im, size)) {
        memcpy(blob, image_blob(&im), size);
        s_status.valid = true;
        return true;
    }

    // The newest image was written for another layout (or size): never reinterpret it as this one.  An older image
    // that would match is stale by definition and is not used either.
    s_status.layout_changed = true;
    uint32_t want = nvm_layout_id();
    bool (*migrate)(uint32_t, const void *, size_t) = card_profile ? card_profile->hooks.nvm_migrate : NULL;
    if (migrate && migrate(im.layout, image_blob(&im), im.size)) {
        log_printf("NVM: image of layout %08lX (%lu bytes) migrated to layout %08lX (%lu bytes)",
                   (unsigned long)im.layout, (unsigned long)im.size, (unsigned long)want, (unsigned long)size);
        s_status.migrated = true;
        nvm_mark_dirty();
        return true;
    }
    log_printf("NVM: stored image has layout %08lX (%lu bytes), firmware expects %08lX (%lu bytes): %s, "
               "using defaults",
               (unsigned long)im.layout, (unsigned long)im.size, (unsigned long)want, (unsigned long)size,
               migrate ? "migration refused" : "no nvm_migrate hook");
    // Save the defaults at the next opportunity, so that they supersede the old image and this happens only once.
    nvm_mark_dirty();
    return false;
}

nvm_save_result_t nvm_save(const void *blob, size_t size) {
    uint32_t bytes = slot_bytes(size);
    if (!bytes) return NVM_SAVE_FAILED;
    uint32_t n = NVM_FLASH_BYTES / bytes;
    s_status.slots = n;

    // Where the newest image is (nvm_load() may not have run, or the flash may have changed since).
    nvm_image_t im;
    bool have = find_newest(&im);
    if (have && image_matches(&im, size) && memcmp(image_blob(&im), blob, size) == 0) {
        s_status.valid = true;
        s_status.slot = im.off / bytes;
        s_status.seq = im.seq;
        return NVM_SAVE_UNCHANGED;
    }

    // The first slot after the newest image (for an image of this layout simply the next slot; one of another size
    // may span several of ours), so that the newest image survives a power loss during this write.  The sequence
    // number continues from it, whatever its layout, so the new image is the newest.
    uint32_t next = have ? ((im.off + im.hdr + im.size + bytes - 1) / bytes) % n : 0;
    uint32_t next_seq = have ? im.seq + 1 : 1;
    uint32_t base = next * bytes;
    if (!nvm_port_erase(base, bytes)) return NVM_SAVE_FAILED;

    // Program page by page: the header shares the first page with the start of the blob.
    nvm_header_t h = { NVM_MAGIC, (uint32_t)size, next_seq, crc32(blob, size), nvm_layout_id(), 0xFFFFFFFFu };
    const uint8_t *src = blob;
    size_t total = sizeof h + size;
    uint8_t page[NVM_PAGE_BYTES];
    for (size_t off = 0; off < total; off += NVM_PAGE_BYTES) {
        memset(page, 0xFF, sizeof page);
        for (size_t i = 0; i < NVM_PAGE_BYTES && off + i < total; i++) {
            size_t pos = off + i;
            // h is fully initialised; the analyzer loses track of it when it is read byte by byte.
            // NOLINTNEXTLINE(clang-analyzer-core.uninitialized.Assign)
            page[i] = pos < sizeof h ? ((const uint8_t *)&h)[pos] : src[pos - sizeof h];
        }
        if (!nvm_port_program((uint32_t)(base + off), page, sizeof page)) return NVM_SAVE_FAILED;
    }

    // Read back.
    nvm_image_t check;
    uint32_t crc;
    if (!header_at(base, &check, &crc) || !image_matches(&check, size) || check.seq != next_seq ||
        crc32(image_blob(&check), size) != crc || memcmp(image_blob(&check), blob, size) != 0) {
        return NVM_SAVE_FAILED;
    }
    s_status.valid = true;
    s_status.slot = next;
    s_status.seq = next_seq;
    return NVM_SAVE_WRITTEN;
}

nvm_save_result_t nvm_save_logged(const void *blob, size_t size, const char *why) {
    nvm_save_result_t r = nvm_save(blob, size);
    log_printf("NVM: %s (slot %lu, seq %lu)%s%s",
               r == NVM_SAVE_WRITTEN     ? "saved"
               : r == NVM_SAVE_UNCHANGED ? "unchanged"
                                         : "save FAILED",
               (unsigned long)s_status.slot, (unsigned long)s_status.seq, why ? " " : "", why ? why : "");
    return r;
}

bool nvm_erase_all(void) {
    s_status = (nvm_status_t){ 0 };
    return nvm_port_erase(0, NVM_FLASH_BYTES);
}

const nvm_status_t *nvm_status(void) { return &s_status; }

void nvm_mark_dirty(void) { s_dirty = true; }

bool nvm_is_dirty(void) { return s_dirty; }

bool nvm_take_dirty(void) {
    bool d = s_dirty;
    s_dirty = false;
    return d;
}
