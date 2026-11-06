#include "fake_nvm.h"
#include "fakes.h"
#include "hal/nvm.h"
#include "iso7816_pico/apdu.h"
#include "iso7816_pico/card.h"
#include "iso7816_pico/config.h"
#include "iso7816_pico/sec.h"
#include "test.h"

#define BLOB 600 // header + 600 bytes -> one 4 KB slot

static void fill(uint8_t *b, uint8_t seed) {
    for (int i = 0; i < BLOB; i++) b[i] = (uint8_t)(seed + i * 7);
}

void test_nvm(void) {
    uint8_t blob[BLOB], back[BLOB];
    const uint32_t slots = NVM_FLASH_BYTES / 4096;

    // Empty flash: nothing to load, the first save goes to slot 0 with seq 1.
    fake_nvm_reset();
    CHECK(!nvm_load(back, BLOB));
    fill(blob, 1);
    CHECK_EQ(nvm_save(blob, BLOB), NVM_SAVE_WRITTEN);
    CHECK_EQ(nvm_status()->slot, 0);
    CHECK_EQ(nvm_status()->seq, 1);
    CHECK_EQ(nvm_status()->slots, slots);
    CHECK_EQ(fake_nvm_erase_count(), 1);
    memset(back, 0, BLOB);
    CHECK(nvm_load(back, BLOB));
    CHECK(memcmp(back, blob, BLOB) == 0);

    // Identical image: nothing is written.
    CHECK_EQ(nvm_save(blob, BLOB), NVM_SAVE_UNCHANGED);
    CHECK_EQ(fake_nvm_erase_count(), 1);
    CHECK_EQ(fake_nvm_program_count(), 3); // 624 bytes = 3 pages, from the first save only

    // A changed image goes to the next slot; the previous slot keeps the old image.
    fill(blob, 2);
    CHECK_EQ(nvm_save(blob, BLOB), NVM_SAVE_WRITTEN);
    CHECK_EQ(nvm_status()->slot, 1);
    CHECK_EQ(nvm_status()->seq, 2);
    CHECK(nvm_load(back, BLOB));
    CHECK_EQ(back[0], blob[0]);

    // Corrupt the newest slot (power loss in the middle of a write): the older image is used again.
    fake_nvm_raw()[4096 + 40] ^= 0xFF; // a byte of the blob
    CHECK(nvm_load(back, BLOB));
    CHECK_EQ(nvm_status()->slot, 0);
    CHECK_EQ(nvm_status()->seq, 1);
    fill(blob, 1);
    CHECK(memcmp(back, blob, BLOB) == 0);

    // The next save continues after the slot that holds the valid image.
    fill(blob, 3);
    CHECK_EQ(nvm_save(blob, BLOB), NVM_SAVE_WRITTEN);
    CHECK_EQ(nvm_status()->slot, 1);
    CHECK_EQ(nvm_status()->seq, 2);

    // Rotate through every slot and wrap around; each save erases exactly one slot.
    uint32_t erases = fake_nvm_erase_count();
    for (uint32_t i = 0; i < slots; i++) {
        fill(blob, (uint8_t)(10 + i));
        CHECK_EQ(nvm_save(blob, BLOB), NVM_SAVE_WRITTEN);
    }
    CHECK_EQ(fake_nvm_erase_count(), erases + slots);
    CHECK_EQ(nvm_status()->slot, 1); // (1 + slots) % slots
    CHECK_EQ(nvm_status()->seq, 2 + slots);
    CHECK(nvm_load(back, BLOB));
    CHECK_EQ(back[0], (uint8_t)(10 + slots - 1));

    // A blob of another size does not match the stored image; an oversized one is refused.
    CHECK(!nvm_load(back, BLOB - 1));
    CHECK(nvm_status()->layout_changed);
    CHECK_EQ(nvm_status()->stored_size, BLOB);
    static uint8_t huge[NVM_FLASH_BYTES];
    CHECK_EQ(nvm_save(huge, sizeof huge), NVM_SAVE_FAILED);

    // Blobs larger than a sector use several sectors per slot.
    static uint8_t big[5000], big_back[5000];
    for (int i = 0; i < 5000; i++) big[i] = (uint8_t)i;
    fake_nvm_reset();
    CHECK_EQ(nvm_save(big, sizeof big), NVM_SAVE_WRITTEN);
    CHECK_EQ(nvm_status()->slots, NVM_FLASH_BYTES / 8192);
    CHECK(nvm_load(big_back, sizeof big_back));
    CHECK(memcmp(big, big_back, sizeof big) == 0);

    // Sequence numbers wrap around safely.
    fake_nvm_reset();
    fill(blob, 1);
    CHECK_EQ(nvm_save(blob, BLOB), NVM_SAVE_WRITTEN);
    uint8_t *hdr = fake_nvm_raw();
    uint32_t wrap = 0xFFFFFFFFu;
    // Rewrite slot 0's sequence number to the maximum (bits only clear, so erase and program the slot again).
    nvm_erase_all();
    CHECK_EQ(nvm_save(blob, BLOB), NVM_SAVE_WRITTEN);
    memcpy(hdr + 8, &wrap, 4); // seq field of the header in slot 0
    // The CRC covers the blob only, so the slot stays valid with seq = 0xFFFFFFFF.
    fill(blob, 2);
    CHECK_EQ(nvm_save(blob, BLOB), NVM_SAVE_WRITTEN);
    CHECK_EQ(nvm_status()->seq, 0); // wrapped
    CHECK(nvm_load(back, BLOB));
    CHECK_EQ(back[0], blob[0]); // the wrapped one is the newest

    // Factory reset.
    CHECK(nvm_erase_all());
    CHECK(!nvm_load(back, BLOB));

    // Dirty flag (earlier groups may have left it set).
    nvm_take_dirty();
    CHECK(!nvm_take_dirty());
    nvm_mark_dirty();
    CHECK(nvm_take_dirty());
    CHECK(!nvm_take_dirty());

    // A blob for which the region offers a single slot is refused: a save would erase the only image first.
    static uint8_t one_slot[40000], one_back[40000]; // 40024 bytes -> 40 KB slot, one in 64 KB
    CHECK(!NVM_BLOB_FITS(sizeof one_slot));
    CHECK(NVM_BLOB_FITS(NVM_FLASH_BYTES / 2 - 24)); // exactly two slots is fine
    CHECK(!NVM_BLOB_FITS(NVM_FLASH_BYTES / 2 - 23));
    CHECK(!nvm_fits(sizeof one_slot));
    fake_nvm_reset();
    CHECK_EQ(nvm_save(one_slot, sizeof one_slot), NVM_SAVE_FAILED);
    CHECK_EQ(fake_nvm_erase_count(), 0);
    CHECK(!nvm_load(one_back, sizeof one_back));
    CHECK_EQ(nvm_status()->slots, 0);
}

// ---- layout ids ----------------------------------------------------------------------------------------------------

static uint8_t s_lay_blob[BLOB];
static uint32_t s_mig_layout, s_mig_size, s_mig_calls;
static uint8_t s_mig_first;
static bool s_mig_accept;

// Keeps the first old byte (+1) when it accepts; leaves the blob alone when it refuses.
static bool lay_migrate(uint32_t old_layout, const void *old_blob, size_t old_size) {
    s_mig_calls++;
    s_mig_layout = old_layout;
    s_mig_size = (uint32_t)old_size;
    s_mig_first = ((const uint8_t *)old_blob)[0];
    if (!s_mig_accept) return false;
    s_lay_blob[0] = (uint8_t)(s_mig_first + 1);
    return true;
}

static uint32_t test_crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static void defaults(void) { memset(s_lay_blob, 0xD0, sizeof s_lay_blob); }

void test_nvm_layout(void) {
    const card_profile_t *saved = card_profile;
    card_profile_t p = *saved;
    p.nvm_blob = s_lay_blob;
    p.nvm_size = sizeof s_lay_blob;
    p.nvm_version = 1;
    p.hooks.nvm_migrate = NULL;
    card_profile = &p;
    CHECK_EQ(nvm_layout_id(), (NVM_LAYOUT_FRAMEWORK << 16) | 1);
    CHECK_EQ(NVM_LAYOUT_FRAMEWORK, 1);

    // Same layout: loads.
    fake_nvm_reset();
    nvm_take_dirty();
    fill(s_lay_blob, 1);
    CHECK_EQ(nvm_save(s_lay_blob, BLOB), NVM_SAVE_WRITTEN);
    defaults();
    CHECK(nvm_load(s_lay_blob, BLOB));
    CHECK_EQ(s_lay_blob[0], 1);
    CHECK(!nvm_status()->layout_changed);
    CHECK_EQ(nvm_status()->stored_layout, 0x00010001);
    CHECK(!nvm_is_dirty());

    // Another application version, no hook: defaults, logged with both ids, dirty so that the defaults supersede it.
    p.nvm_version = 2;
    defaults();
    unsigned lines = g_log_lines;
    CHECK(!nvm_load(s_lay_blob, BLOB));
    CHECK_EQ(s_lay_blob[0], 0xD0);
    CHECK_EQ(g_log_lines, lines + 1);
    CHECK(strstr(g_log_last, "layout 00010001") != NULL);
    CHECK(strstr(g_log_last, "expects 00010002") != NULL);
    CHECK(strstr(g_log_last, "no nvm_migrate hook") != NULL);
    CHECK(nvm_status()->layout_changed);
    CHECK(!nvm_status()->valid);
    CHECK(nvm_is_dirty());
    // ... the save that follows (first boot) makes the next boot load normally.
    nvm_take_dirty();
    CHECK_EQ(nvm_save(s_lay_blob, BLOB), NVM_SAVE_WRITTEN);
    CHECK_EQ(nvm_status()->seq, 2);
    s_lay_blob[0] = 0;
    CHECK(nvm_load(s_lay_blob, BLOB));
    CHECK_EQ(s_lay_blob[0], 0xD0);
    CHECK(!nvm_status()->layout_changed);
    CHECK(!nvm_is_dirty());

    // Back to version 1: the version 1 image in slot 0 is older than the version 2 one and must not come back.
    p.nvm_version = 1;
    defaults();
    CHECK(!nvm_load(s_lay_blob, BLOB));
    CHECK_EQ(s_lay_blob[0], 0xD0);
    CHECK_EQ(nvm_status()->stored_layout, 0x00010002);
    nvm_take_dirty();
    CHECK_EQ(nvm_save(s_lay_blob, BLOB), NVM_SAVE_WRITTEN);
    CHECK_EQ(nvm_status()->seq, 3); // continues after the newest image, whatever its layout
    CHECK_EQ(nvm_status()->slot, 2);

    // With a hook that accepts: the migrated image is used and saved soon after.
    fake_nvm_reset();
    fill(s_lay_blob, 5);
    CHECK_EQ(nvm_save(s_lay_blob, BLOB), NVM_SAVE_WRITTEN); // version 1
    p.nvm_version = 3;
    p.hooks.nvm_migrate = lay_migrate;
    s_mig_accept = true;
    s_mig_calls = 0;
    defaults();
    CHECK(nvm_load(s_lay_blob, BLOB));
    CHECK_EQ(s_mig_calls, 1);
    CHECK_EQ(s_mig_layout, 0x00010001);
    CHECK_EQ(s_mig_size, BLOB);
    CHECK_EQ(s_mig_first, 5);
    CHECK_EQ(s_lay_blob[0], 6);
    CHECK_EQ(s_lay_blob[1], 0xD0); // what the hook did not set keeps its default
    CHECK(nvm_status()->migrated);
    CHECK(strstr(g_log_last, "migrated to layout 00010003") != NULL);
    CHECK(nvm_is_dirty());
    nvm_take_dirty();
    CHECK_EQ(nvm_save(s_lay_blob, BLOB), NVM_SAVE_WRITTEN);
    s_mig_calls = 0;
    CHECK(nvm_load(s_lay_blob, BLOB));
    CHECK_EQ(s_mig_calls, 0); // now it is this layout
    CHECK(!nvm_status()->migrated);

    // A size change with the same version also goes through the hook (old_size tells).
    p.nvm_size = BLOB - 8;
    s_mig_calls = 0;
    CHECK(nvm_load(s_lay_blob, BLOB - 8));
    CHECK_EQ(s_mig_calls, 1);
    CHECK_EQ(s_mig_layout, 0x00010003);
    CHECK_EQ(s_mig_size, BLOB);
    p.nvm_size = BLOB;
    nvm_take_dirty();

    // A hook that refuses: defaults.
    p.nvm_version = 4;
    s_mig_accept = false;
    s_mig_calls = 0;
    defaults();
    CHECK(!nvm_load(s_lay_blob, BLOB));
    CHECK_EQ(s_mig_calls, 1);
    CHECK_EQ(s_lay_blob[0], 0xD0);
    CHECK(!nvm_status()->migrated);
    CHECK(strstr(g_log_last, "migration refused") != NULL);
    CHECK(nvm_is_dirty());
    nvm_take_dirty();

    // An image in the first header format (16 bytes, no layout id) counts as layout 0.
    fake_nvm_reset();
    uint8_t *f = fake_nvm_raw();
    fill(s_lay_blob, 9);
    uint32_t v1[4] = { 0x4D564E37u, BLOB, 41, test_crc32(s_lay_blob, BLOB) };
    memcpy(f, v1, sizeof v1);
    memcpy(f + 16, s_lay_blob, BLOB);
    p.nvm_version = 0;
    p.hooks.nvm_migrate = NULL;
    defaults();
    CHECK(!nvm_load(s_lay_blob, BLOB));
    CHECK_EQ(s_lay_blob[0], 0xD0);
    CHECK(nvm_status()->layout_changed);
    CHECK_EQ(nvm_status()->stored_layout, 0);
    CHECK_EQ(nvm_status()->seq, 41);
    CHECK(strstr(g_log_last, "layout 00000000 (600 bytes), firmware expects 00010000") != NULL);
    nvm_take_dirty();
    CHECK_EQ(nvm_save(s_lay_blob, BLOB), NVM_SAVE_WRITTEN);
    CHECK_EQ(nvm_status()->seq, 42);
    CHECK_EQ(nvm_status()->slot, 1); // slot 0 (the old image) is kept until the new one is written
    CHECK(nvm_load(s_lay_blob, BLOB));
    // ... and a hook can read it.
    fake_nvm_reset();
    fill(s_lay_blob, 9);
    memcpy(f, v1, sizeof v1);
    memcpy(f + 16, s_lay_blob, BLOB);
    p.hooks.nvm_migrate = lay_migrate;
    s_mig_accept = true;
    s_mig_calls = 0;
    defaults();
    CHECK(nvm_load(s_lay_blob, BLOB));
    CHECK_EQ(s_mig_calls, 1);
    CHECK_EQ(s_mig_layout, 0);
    CHECK_EQ(s_mig_first, 9);
    CHECK_EQ(s_lay_blob[0], 10);

    // Leave the flash and the dirty flag as the other groups expect them.
    card_profile = saved;
    nvm_take_dirty();
    fake_nvm_reset();
}

static uint16_t run(uint16_t (*fn)(const apdu_t *, apdu_resp_t *), const char *hex) {
    static uint8_t buf[300], out[300];
    size_t n = hex_parse(hex, buf, sizeof buf);
    apdu_t a;
    if (!apdu_decode(buf, n, &a)) return 0xFFFF;
    apdu_resp_t r = APDU_RESP_INIT(out, sizeof out);
    return fn(&a, &r);
}

// The PIN 01 entry of the image the journal holds in (fake) flash right now.
static sec_pin_t stored_pin01(void) {
    static uint8_t img[4096];
    sec_pin_t p;
    memset(&p, 0, sizeof p);
    size_t size = card_profile->nvm_size;
    if (size > sizeof img || !nvm_load(img, size)) return p;
    const sec_pin_t *ram = sec_find(0x01);
    size_t off = (size_t)((const uint8_t *)ram - (const uint8_t *)card_profile->nvm_blob);
    if (off + sizeof p > size) return p;
    memcpy(&p, img + off, sizeof p);
    return p;
}

// PIN and PUK tries reach the flash before the comparison: cutting the power after a wrong try keeps the decrement.
void test_nvm_pin_durability(void) {
    sec_pin_t *pin = sec_find(0x01);
    CHECK(pin != NULL);
    if (!pin) return;
    const uint8_t *blob = card_profile->nvm_blob;
    CHECK((const uint8_t *)pin >= blob && (const uint8_t *)(pin + 1) <= blob + card_profile->nvm_size);
    uint8_t retries = pin->retries, puk_retries = pin->puk_retries;
    CHECK(retries >= 2 && puk_retries >= 2);

    fake_nvm_reset();
    nvm_take_dirty();
    CHECK_EQ(run(sec_cmd_verify, "00 20 00 01 04 31 31 31 31"), SW_WARN_COUNTER(retries - 1));
    CHECK_EQ(pin->retries, retries - 1);
    CHECK_EQ(fake_nvm_erase_count(), 1); // saved synchronously, inside the command
    CHECK_EQ(stored_pin01().retries, retries - 1);
    CHECK(!nvm_is_dirty()); // nothing left over for the deferred save

    // A correct PIN restores the counter in RAM (and leaves the deferred save to write it).
    CHECK_EQ(run(sec_cmd_verify, "00 20 00 01 04 31 32 33 34"), SW_OK);
    CHECK_EQ(pin->retries, retries);
    CHECK(nvm_is_dirty());
    CHECK_EQ(stored_pin01().retries, retries - 2); // the decrement of this try reached the flash, too
    CHECK_EQ(nvm_save(card_profile->nvm_blob, card_profile->nvm_size), NVM_SAVE_WRITTEN);
    nvm_take_dirty();
    CHECK_EQ(stored_pin01().retries, retries);

    // RESET RETRY COUNTER with a wrong PUK.
    CHECK_EQ(run(sec_cmd_reset_retry_counter, "00 2C 01 01 08 39 39 39 39 39 39 39 39"),
             SW_WARN_COUNTER(puk_retries - 1));
    CHECK_EQ(pin->puk_retries, puk_retries - 1);
    CHECK_EQ(stored_pin01().puk_retries, puk_retries - 1);
    CHECK(!nvm_is_dirty());
    CHECK_EQ(run(sec_cmd_reset_retry_counter, "00 2C 01 01 08 31 32 33 34 35 36 37 38"), SW_OK);
    CHECK_EQ(pin->puk_retries, puk_retries);
    CHECK(nvm_is_dirty());

    // Leave the flash and the dirty flag as the other groups expect them.
    nvm_take_dirty();
    sec_reset();
    fake_nvm_reset();
}
