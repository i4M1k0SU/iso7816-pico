#include "iso7816_pico/config.h"
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
}
