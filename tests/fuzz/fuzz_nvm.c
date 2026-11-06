// The NVM journal (hal/nvm.c) on a random flash image in the RAM "flash" of tests/fake_nvm.c: nvm_load() must never
// read outside the region, hand the migration hook more than the stored image, or load anything but a valid image;
// and a save on top of whatever was found must be read back by the next load.
//
// Byte 0: bit 0 = fix the CRC of every plausible header (so that the fuzzer gets past the CRC-32), bit 1 = a
// migration hook is installed, bit 2 = it accepts, bit 3 = nvm_save() + nvm_load() round trip afterwards, bits 6..4 =
// blob size (see s_sizes).  Byte 1: the profile's nvm_version.  Then chunks [sector] [n] [n bytes], each written at
// the start of sector (sector % 16) of the otherwise erased region.
#include "fake_nvm.h"
#include "fuzz_common.h"
#include "hal/nvm.h"
#include "iso7816_pico/config.h"

#include <stdlib.h>
#include <string.h>

#define MAGIC    0x324E5637u
#define MAGIC_V1 0x4D564E37u

// Blob sizes around the slot boundaries: 4072 + 24 is one sector, 32744 + 24 is half the region (the largest blob
// that still gets two slots), 32745 does not fit.
static const size_t s_sizes[8] = { 0, 1, 4072, 4073, 8000, 32744, 32745, 600 };
static uint8_t s_blob[32768];
static uint8_t s_check[32768];
static bool s_accept;

static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static uint32_t rd32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool migrate(uint32_t old_layout, const void *old_blob, size_t old_size) {
    (void)old_layout;
    // Copy through an exact-size heap buffer, so that ASan notices if old_size reaches past the stored image's region.
    uint8_t *tmp = malloc(old_size ? old_size : 1);
    if (!tmp) FUZZ_FAIL();
    memcpy(tmp, old_blob, old_size);
    size_t n = old_size < card_profile->nvm_size ? old_size : card_profile->nvm_size;
    if (s_accept) memcpy(card_profile->nvm_blob, tmp, n);
    free(tmp);
    return s_accept;
}

// Real images have consecutive sequence numbers, while random ones may lie more than 2^31 apart, where the wrapping
// "newer" comparison has no newest image (a is newer than b, b than c, c than a) and a save cannot become the newest
// either; on flash that would take 2^31 saves.  So every header's seq is folded into -32768..32767, which keeps the
// wrap from FFFFFFFF to 0 in reach.  The CRC covers the blob only, but a blob may span later headers: this runs before
// fix_crcs().
static void fold_seqs(uint8_t *flash) {
    for (size_t off = 0; off < NVM_FLASH_BYTES; off += NVM_SECTOR_BYTES) {
        uint8_t *p = flash + off;
        if (rd32(p) != MAGIC && rd32(p) != MAGIC_V1) continue;
        p[10] = p[11] = (p[9] & 0x80) ? 0xFF : 0x00;
    }
}

static void fix_crcs(uint8_t *flash) {
    // Back to front: an image's blob may run over the headers of later sectors, whose CRCs are fixed first.
    for (int s = NVM_FLASH_BYTES / NVM_SECTOR_BYTES - 1; s >= 0; s--) {
        uint8_t *p = flash + (size_t)s * NVM_SECTOR_BYTES;
        uint32_t magic = rd32(p), size = rd32(p + 4);
        uint32_t hdr = magic == MAGIC ? 24 : magic == MAGIC_V1 ? 16 : 0;
        if (!hdr || size == 0 || size > NVM_FLASH_BYTES - (uint32_t)s * NVM_SECTOR_BYTES - hdr) continue;
        uint32_t c = crc32(p + hdr, size);
        for (int i = 0; i < 4; i++) p[12 + i] = (uint8_t)(c >> (8 * i));
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 2 || size > FUZZ_MAX_INPUT) return 0;
    fuzz_card_reset(0, false, 32);
    uint8_t cfg = data[0];
    size_t blob_size = s_sizes[(cfg >> 4) & 7];
    if (!blob_size) blob_size = card_profile->nvm_size;
    s_accept = cfg & 4;
    g_fuzz_profile.nvm_version = data[1];
    g_fuzz_profile.nvm_blob = s_blob;
    g_fuzz_profile.nvm_size = blob_size;
    g_fuzz_profile.hooks.nvm_migrate = (cfg & 2) ? migrate : NULL;
    for (size_t i = 0; i < blob_size; i++) s_blob[i] = (uint8_t)(i * 7 + 1); // the "defaults"

    uint8_t *flash = fake_nvm_raw();
    size_t pos = 2;
    while (pos + 2 <= size) {
        size_t off = (size_t)(data[pos] % (NVM_FLASH_BYTES / NVM_SECTOR_BYTES)) * NVM_SECTOR_BYTES;
        size_t n = data[pos + 1];
        pos += 2;
        if (n > size - pos) n = size - pos;
        memcpy(flash + off, data + pos, n);
        pos += n;
    }
    fold_seqs(flash);
    if (cfg & 1) fix_crcs(flash);

    bool fits = nvm_fits(blob_size);
    bool loaded = nvm_load(s_blob, blob_size);
    const nvm_status_t *st = nvm_status();
    if (loaded && !fits) FUZZ_FAIL();
    if (loaded && !st->migrated && (!st->valid || st->layout_changed)) FUZZ_FAIL();
    if (loaded && st->migrated && !(cfg & 2 && s_accept)) FUZZ_FAIL();

    if ((cfg & 8) && fits) {
        nvm_save_result_t r = nvm_save(s_blob, blob_size);
        if (r == NVM_SAVE_FAILED) FUZZ_FAIL();
        memset(s_check, 0, blob_size);
        if (!nvm_load(s_check, blob_size) || memcmp(s_check, s_blob, blob_size) != 0) FUZZ_FAIL();
        if (nvm_save(s_blob, blob_size) != NVM_SAVE_UNCHANGED) FUZZ_FAIL();
    }
    return 0;
}
