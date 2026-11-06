#include "iso7816_pico/apdu.h"
#include "iso7816_pico/card.h"
#include "iso7816_pico/config.h"
#include "iso7816_pico/sec.h"
#include "fake_nvm.h"
#include "hal/nvm.h"
#include "test.h"

#define BLOB 600   // header + 600 bytes -> one 4 KB slot

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
    CHECK_EQ(fake_nvm_program_count(), 3);   // 616 bytes = 3 pages, from the first save only

    // A changed image goes to the next slot; the previous slot keeps the old image.
    fill(blob, 2);
    CHECK_EQ(nvm_save(blob, BLOB), NVM_SAVE_WRITTEN);
    CHECK_EQ(nvm_status()->slot, 1);
    CHECK_EQ(nvm_status()->seq, 2);
    CHECK(nvm_load(back, BLOB));
    CHECK_EQ(back[0], blob[0]);

    // Corrupt the newest slot (power loss in the middle of a write): the older image is used again.
    fake_nvm_raw()[4096 + 20] ^= 0xFF;
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
    CHECK_EQ(nvm_status()->slot, 1);                  // (1 + slots) % slots
    CHECK_EQ(nvm_status()->seq, 2 + slots);
    CHECK(nvm_load(back, BLOB));
    CHECK_EQ(back[0], (uint8_t)(10 + slots - 1));

    // A blob of another size does not match the stored image; an oversized one is refused.
    CHECK(!nvm_load(back, BLOB - 1));
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
    memcpy(hdr + 8, &wrap, 4);                       // seq field of the header in slot 0
    // The CRC covers the blob only, so the slot stays valid with seq = 0xFFFFFFFF.
    fill(blob, 2);
    CHECK_EQ(nvm_save(blob, BLOB), NVM_SAVE_WRITTEN);
    CHECK_EQ(nvm_status()->seq, 0);                  // wrapped
    CHECK(nvm_load(back, BLOB));
    CHECK_EQ(back[0], blob[0]);                      // the wrapped one is the newest

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
    static uint8_t one_slot[40000], one_back[40000];   // 40016 bytes -> 40 KB slot, one in 64 KB
    CHECK(!NVM_BLOB_FITS(sizeof one_slot));
    CHECK(NVM_BLOB_FITS(NVM_FLASH_BYTES / 2 - 16));      // exactly two slots is fine
    CHECK(!nvm_fits(sizeof one_slot));
    fake_nvm_reset();
    CHECK_EQ(nvm_save(one_slot, sizeof one_slot), NVM_SAVE_FAILED);
    CHECK_EQ(fake_nvm_erase_count(), 0);
    CHECK(!nvm_load(one_back, sizeof one_back));
    CHECK_EQ(nvm_status()->slots, 0);
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
    CHECK_EQ(fake_nvm_erase_count(), 1);                // saved synchronously, inside the command
    CHECK_EQ(stored_pin01().retries, retries - 1);
    CHECK(!nvm_is_dirty());                             // nothing left over for the deferred save

    // A correct PIN restores the counter in RAM (and leaves the deferred save to write it).
    CHECK_EQ(run(sec_cmd_verify, "00 20 00 01 04 31 32 33 34"), SW_OK);
    CHECK_EQ(pin->retries, retries);
    CHECK(nvm_is_dirty());
    CHECK_EQ(stored_pin01().retries, retries - 2);      // the decrement of this try reached the flash, too
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
