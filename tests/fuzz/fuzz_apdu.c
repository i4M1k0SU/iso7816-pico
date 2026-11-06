// apdu_decode() on arbitrary bytes: no read outside the input (ASan sees the exact-size buffer libFuzzer hands over),
// and a decoded APDU whose length fields add up to exactly the input length.
#include "fuzz_common.h"
#include "iso7816_pico/apdu.h"

#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    apdu_t a;
    if (!apdu_decode(data, size, &a)) return 0;

    // Header, then Lc (1 or 3 bytes in front of the data, the 3-byte form starting with 00), data, Le (1 / 2 / 3).
    size_t expect = 4;
    if (a.extended) {
        expect += 1; // the 00 that marks extended length
        if (a.lc) expect += 2 + a.lc;
        if (a.le_present) expect += 2;
        if (a.lc > 65535 || a.le > 65536) FUZZ_FAIL();
    } else {
        if (a.lc) expect += 1 + a.lc;
        if (a.le_present) expect += 1;
        if (a.lc > 255 || a.le > 256) FUZZ_FAIL();
    }
    if (expect != size) FUZZ_FAIL();
    if (a.le_present != (a.le != 0)) FUZZ_FAIL();
    if ((a.lc != 0) != (a.data != NULL)) FUZZ_FAIL();
    if (a.data && (a.data < data || a.data + a.lc > data + size)) FUZZ_FAIL();
    if (a.cla != data[0] || a.ins != data[1] || a.p1 != data[2] || a.p2 != data[3]) FUZZ_FAIL();
    return 0;
}
