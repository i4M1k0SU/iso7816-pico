// Reader-side models of T=0 and T=1 that drive the card code through the fake PHY.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define READER_MAX_DATA 8192

typedef struct {
    uint8_t data[READER_MAX_DATA];
    size_t len;
    uint16_t sw;
} reader_result_t;

// ---- T=0 ------------------------------------------------------------------------------------------------------------

// Prepare the T=0 model (installs the PHY hook).
void t0r_begin(void);

// One TPDU exchange: apdu = CLA INS P1 P2 P3 [data].  A trailing Le byte after the data is dropped, as PC/SC drivers
// do.  Returns false when the card call failed (reset / nothing queued).
bool t0r_tpdu(const uint8_t *apdu, size_t len, reader_result_t *out);

// APDU exchange following 61 xx / 6C xx like a PC/SC layer.
bool t0r_apdu(const uint8_t *apdu, size_t len, reader_result_t *out);

uint32_t t0r_null_bytes(void); // NULL procedure bytes (60) seen

// ---- T=1 ------------------------------------------------------------------------------------------------------------

// EDC used by the next t1r_begin() for both the card and the model: CRC (ATR TC3 b1 = 1) or LRC (the default).
void t1r_set_crc(bool crc);
bool t1r_crc(void);
size_t t1r_edc_len(void);                                   // 1 (LRC) or 2 (CRC)
size_t t1r_edc(const uint8_t *blk, size_t n, uint8_t *out); // EDC of blk[0..n) as sent on the wire; returns its length
uint16_t t1r_crc16(const uint8_t *p, size_t n);             // the model's CRC register value (high byte goes first)
uint32_t t1r_edc_errors(void); // blocks from the card whose EDC did not match, since t1r_set_crc()

// Prepare the T=1 model: reader's view of IFSC (the card's limit) and our IFSD (what we announce).
void t1r_begin(uint8_t ifsc);

// Send one raw block (NAD PCB LEN INF; the EDC is appended) and run the card once.  The card's reply block(s) are
// available through t1r_last_block().
bool t1r_exchange(const uint8_t *block, size_t len);
const uint8_t *t1r_last_block(size_t *len);
uint32_t t1r_block_count(void);  // blocks received from the card so far
uint32_t t1r_wtx_requests(void); // S(WTX request) blocks answered automatically
void t1r_set_ifsd(uint8_t ifsd); // our limit for card -> reader I-blocks (send S(IFS) first)
void t1r_resynch(void);          // S(RESYNCH request); the model's N(S) restarts at 0 as well

// Full APDU exchange with chaining in both directions.
bool t1r_apdu(const uint8_t *apdu, size_t len, reader_result_t *out);
