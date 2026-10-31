// ISO 7816-3 Answer To Reset: build one from a description, or parse a raw one to learn the transmission parameters it
// announces.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ATR_MAX_LEN 33

// One level of interface bytes TAi / TBi / TCi.
#define ATR_TA 0x1
#define ATR_TB 0x2
#define ATR_TC 0x4

typedef struct {
    uint8_t present;    // ATR_TA | ATR_TB | ATR_TC
    uint8_t ta, tb, tc;
    uint8_t protocol;   // T indicated in TD(i-1) for level i >= 2 (ignored for level 1)
} atr_level_t;

typedef struct {
    bool inverse;                  // TS = 3F instead of 3B
    atr_level_t level[4];          // interface byte levels 1..4
    uint8_t levels;                // 1..4; TD(i) is emitted for i < levels
    uint8_t historical[15];
    uint8_t historical_len;
} atr_desc_t;

// Encode.  Returns the length, 0 if the description is invalid.
size_t atr_build(const atr_desc_t *d, uint8_t *out, size_t cap);

// What an ATR announces (parsed from the bytes, so it is right whether the ATR was built here or pasted from a real
// card).
typedef struct {
    bool inverse;
    uint16_t f;                 // from TA1 (372 if absent, or if TA2 says "implicit values")
    uint8_t d;                  // from TA1 (1 if absent, or if TA2 says "implicit values")
    uint8_t n;                  // extra guard time from TC1 (0 if absent)
    bool specific_mode;         // TA2 present: no PPS, switch to f / d right after the ATR, run specific_protocol
    uint8_t specific_protocol;  // T from TA2 bits 4..1 (only meaningful in specific mode)
    uint8_t first_protocol;     // T offered first (0 if no TD1)
    uint16_t protocols;         // bit t set when T=t is offered
    uint8_t wi;                 // T=0 waiting time integer (TC2, default 10)
    uint8_t ifsc;               // T=1: TA(i>2) (default 32)
    uint8_t cwi, bwi;           // T=1: TB(i>2) (defaults 13 / 4)
    bool crc;                   // T=1: TC(i>2) bit 0 (LRC otherwise)
    bool valid;
} atr_info_t;

bool atr_parse(const uint8_t *atr, size_t len, atr_info_t *info);

// Fi / Di tables (0 = RFU).
uint16_t atr_fi(uint8_t code);
uint8_t atr_di(uint8_t code);
