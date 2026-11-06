// The T=0 TPDU layer (t0_handle_command()) over the fake PHY, driving the full dispatcher with the demo profile, so
// that the file system and security commands, GET RESPONSE / 6C xx and ENVELOPE are fuzzed through the real entry
// point.
//
// Byte 0: bit 0 = gap mode.  Without it the rest of the input is the byte stream from the reader.  With it the rest
// is a sequence of chunks [n] [n bytes], and the reader goes silent (a WWT timeout for the card) after each chunk, so
// that partial headers and data fields that stop short are reachable in the middle of a session too.
#include "fake_phy.h"
#include "fuzz_common.h"
#include "proto/t0.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1 || size > FUZZ_MAX_INPUT) return 0;
    fuzz_card_reset(0, false, 32);
    t0_set_wi(10);
    if (data[0] & 1) {
        size_t pos = 1;
        while (pos < size) {
            size_t n = data[pos++];
            if (n > size - pos) n = size - pos;
            fuzz_phy_push(data + pos, n);
            pos += n;
            fake_phy_push_gap();
        }
    } else {
        fuzz_phy_push(data + 1, size - 1);
    }
    // Every call that returns true has consumed at least the CLA, so this ends when the queue is empty (the fake PHY
    // then reports a reset).
    while (t0_handle_command(-1)) {}
    return 0;
}
