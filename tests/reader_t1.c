#include "reader.h"

#include "fake_phy.h"
#include "proto/t1.h"

#include <string.h>

#define MAX_BLOCK 260

static uint8_t s_cur[MAX_BLOCK];       // block being received from the card
static size_t s_cur_len;
static uint8_t s_last[MAX_BLOCK];      // last complete block from the card
static size_t s_last_len;
static uint32_t s_blocks, s_wtx;
static uint8_t s_ifsc = 32;            // card's limit (reader -> card chunks)
static uint8_t s_ns;                   // our send sequence number

static uint8_t lrc(const uint8_t *p, size_t n) {
    uint8_t x = 0;
    for (size_t i = 0; i < n; i++) x ^= p[i];
    return x;
}

static void push_block(uint8_t nad, uint8_t pcb, const uint8_t *inf, size_t len) {
    uint8_t blk[MAX_BLOCK];
    blk[0] = nad;
    blk[1] = pcb;
    blk[2] = (uint8_t)len;
    if (len) memcpy(&blk[3], inf, len);
    blk[3 + len] = lrc(blk, 3 + len);
    fake_phy_push(blk, 4 + len);
}

static void hook(uint8_t b) {
    if (s_cur_len < MAX_BLOCK) s_cur[s_cur_len] = b;
    s_cur_len++;
    if (s_cur_len < 3) return;
    size_t total = 3 + (size_t)s_cur[2] + 1;
    if (s_cur_len < total) return;
    memcpy(s_last, s_cur, total);
    s_last_len = total;
    s_cur_len = 0;
    s_blocks++;
    // S(WTX request): the card is still working, answer right away so that its rx_block() finds the response.
    if (s_last[1] == 0xC3) {
        s_wtx++;
        push_block(0x00, 0xE3, &s_last[3], s_last[2]);
    }
}

void t1r_begin(uint8_t ifsc) {
    fake_phy_clear();
    fake_phy_set_tx_hook(hook);
    s_cur_len = s_last_len = 0;
    s_blocks = s_wtx = 0;
    s_ifsc = ifsc;
    s_ns = 0;
    t1_config_t cfg = { ifsc, false, 13 };
    t1_session_begin(&cfg);
}

bool t1r_exchange(const uint8_t *block, size_t len) {
    uint8_t blk[MAX_BLOCK];
    memcpy(blk, block, len);
    blk[len] = lrc(blk, len);
    fake_phy_push(blk, len + 1);
    s_last_len = 0;
    return t1_handle_block(-1);
}

const uint8_t *t1r_last_block(size_t *len) {
    *len = s_last_len;
    return s_last;
}

uint32_t t1r_block_count(void) { return s_blocks; }
uint32_t t1r_wtx_requests(void) { return s_wtx; }

void t1r_set_ifsd(uint8_t ifsd) {
    uint8_t raw[4] = { 0x00, 0xC1, 0x01, ifsd };
    t1r_exchange(raw, 4);
}

void t1r_resynch(void) {
    uint8_t raw[3] = { 0x00, 0xC0, 0x00 };
    t1r_exchange(raw, 3);
    s_ns = 0;
}

static bool is_i(uint8_t pcb) { return !(pcb & 0x80); }
static bool is_r(uint8_t pcb) { return (pcb & 0xC0) == 0x80; }

bool t1r_apdu(const uint8_t *apdu, size_t len, reader_result_t *out) {
    out->len = 0;
    out->sw = 0;
    size_t off = 0;
    // Send the command, chained if needed.
    for (;;) {
        size_t n = len - off < s_ifsc ? len - off : s_ifsc;
        bool more = off + n < len;
        uint8_t pcb = (uint8_t)((s_ns ? 0x40 : 0) | (more ? 0x20 : 0));
        push_block(0x00, pcb, apdu + off, n);
        s_ns ^= 1;
        s_last_len = 0;
        if (!t1_handle_block(-1)) return false;
        off += n;
        if (!more) break;
        // Expect R(N(R) = our next sequence number).
        if (s_last_len < 4 || !is_r(s_last[1]) || ((s_last[1] >> 4) & 1) != s_ns) return false;
    }
    // Receive the response, acknowledging chained I-blocks.
    for (int hop = 0; hop < 200; hop++) {
        if (s_last_len < 4 || !is_i(s_last[1])) return false;
        size_t inf = s_last[2];
        memcpy(out->data + out->len, &s_last[3], inf);
        out->len += inf;
        if (!(s_last[1] & 0x20)) break;
        uint8_t card_ns = (s_last[1] >> 6) & 1;
        uint8_t r = (uint8_t)(0x80 | ((card_ns ^ 1) << 4));
        push_block(0x00, r, NULL, 0);
        s_last_len = 0;
        if (!t1_handle_block(-1)) return false;
    }
    if (out->len < 2) return false;
    out->len -= 2;
    out->sw = (uint16_t)((out->data[out->len] << 8) | out->data[out->len + 1]);
    return true;
}
