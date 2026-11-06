// The T=1 block layer (t1_handle_block()) over the fake PHY, driving the full dispatcher with the demo profile: I-block
// chaining in both directions, R-blocks, S(RESYNCH / IFS / ABORT / WTX), EDC errors, and every command behind them.
//
// Byte 0: bit 0 = CRC instead of LRC, bit 1 = framed mode.  Byte 1: the card's IFSC (0 = default).  Without framed
// mode the rest of the input is the raw byte stream from the reader, EDC included.  In framed mode it is a sequence of
// NAD PCB LEN INF chunks (INF as long as LEN says, or what is left), each sent with a correct EDC appended, so that the
// fuzzer reaches the command layer in CRC mode as easily as in LRC mode.
//
// Whatever the reader sends, every block the card transmits must be well-formed: LEN <= 254 and a correct EDC.
#include "fake_phy.h"
#include "fuzz_common.h"
#include "proto/t1.h"

#include <stdlib.h>
#include <string.h>

#define TX_LOG_SIZE 65536 // tests/fake_phy.c keeps this much of what the card sent

static bool s_crc;

static uint16_t crc16(const uint8_t *p, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0x8408 : c >> 1;
    }
    return c; // CRC-16/MCRF4XX: no final complement, as in t1.c
}

static size_t edc_append(uint8_t *blk, size_t n) {
    if (s_crc) {
        uint16_t c = crc16(blk, n);
        blk[n] = (uint8_t)(c >> 8);
        blk[n + 1] = (uint8_t)c;
        return n + 2;
    }
    uint8_t lrc = 0;
    for (size_t i = 0; i < n; i++) lrc ^= blk[i];
    blk[n] = lrc;
    return n + 1;
}

static void check_card_blocks(void) {
    size_t n;
    const uint8_t *tx = fake_phy_tx_log(&n);
    size_t edc = s_crc ? 2 : 1;
    size_t pos = 0;
    while (pos < n) {
        bool cut = n >= TX_LOG_SIZE; // the log is full: its last block may be incomplete
        if (n - pos < 3 + edc) {
            if (cut) break;
            FUZZ_FAIL();
        }
        size_t len = tx[pos + 2];
        if (len > 254) FUZZ_FAIL();
        if (n - pos < 3 + len + edc) {
            if (cut) break;
            FUZZ_FAIL();
        }
        uint8_t blk[3 + 254 + 2];
        memcpy(blk, &tx[pos], 3 + len);
        edc_append(blk, 3 + len);
        if (memcmp(blk + 3 + len, &tx[pos + 3 + len], edc) != 0) FUZZ_FAIL();
        pos += 3 + len + edc;
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 2 || size > FUZZ_MAX_INPUT) return 0;
    s_crc = data[0] & 1;
    fuzz_card_reset(1, s_crc, data[1]);
    if (data[0] & 2) {
        size_t pos = 2;
        while (pos < size) {
            uint8_t blk[3 + 255 + 2];
            size_t n = 0;
            while (n < 3 && pos < size) blk[n++] = data[pos++];
            if (n == 3) {
                size_t inf = blk[2];
                if (inf > size - pos) inf = size - pos;
                memcpy(&blk[3], &data[pos], inf);
                pos += inf;
                n += inf;
            }
            fuzz_phy_push(blk, edc_append(blk, n));
        }
    } else {
        fuzz_phy_push(data + 2, size - 2);
    }
    // Each call consumes at least one byte, so this ends when the queue is empty (the fake PHY then reports a reset).
    while (t1_handle_block(-1)) {}
    check_card_blocks();
    return 0;
}
