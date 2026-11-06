#include "reader.h"

#include "fake_phy.h"
#include "proto/t0.h"

#include <string.h>

typedef enum { ST_PROC, ST_DATA, ST_SW2, ST_DONE, ST_ERROR } state_t;

static state_t s_state;
static uint8_t s_ins;
static const uint8_t *s_out_data; // data still to send after the ACK
static size_t s_out_len;
static size_t s_expect_in; // bytes the card will send after the ACK (case 2)
static reader_result_t *s_res;
static uint32_t s_nulls;

static void hook(uint8_t b) {
    switch (s_state) {
    case ST_DATA:
        if (s_res->len < READER_MAX_DATA) s_res->data[s_res->len++] = b;
        if (--s_expect_in == 0) s_state = ST_PROC;
        break;
    case ST_SW2:
        s_res->sw |= b;
        s_state = ST_DONE;
        break;
    case ST_PROC:
        if (b == 0x60) {
            s_nulls++;
        } else if (b == s_ins) {
            if (s_out_len) {
                fake_phy_push(s_out_data, s_out_len);
                s_out_len = 0;
            } else if (s_expect_in) {
                s_state = ST_DATA;
            } else {
                s_state = ST_ERROR; // ACK for a command without data in either direction
            }
        } else if ((b & 0xF0) == 0x60 || (b & 0xF0) == 0x90) {
            s_res->sw = (uint16_t)(b << 8);
            s_state = ST_SW2;
        } else {
            s_state = ST_ERROR;
        }
        break;
    default:
        s_state = ST_ERROR;
        break;
    }
}

void t0r_begin(void) {
    fake_phy_clear();
    fake_phy_set_tx_hook(hook);
    s_nulls = 0;
}

uint32_t t0r_null_bytes(void) { return s_nulls; }

bool t0r_tpdu(const uint8_t *apdu, size_t len, reader_result_t *out) {
    if (len < 5) return false;
    uint8_t p3 = apdu[4];
    size_t data_len = len - 5;
    if (data_len && data_len != p3 && data_len != (size_t)p3 + 1) return false; // Lc mismatch
    if (data_len) data_len = p3;

    out->len = 0;
    out->sw = 0;
    s_res = out;
    s_ins = apdu[1];
    s_out_data = apdu + 5;
    s_out_len = data_len;
    s_expect_in = data_len ? 0 : (p3 ? p3 : 256);
    s_state = ST_PROC;

    fake_phy_push(apdu, 5);
    bool ok = t0_handle_command(-1);
    return ok && s_state == ST_DONE;
}

bool t0r_apdu(const uint8_t *apdu, size_t len, reader_result_t *out) {
    uint8_t cmd[READER_MAX_DATA];
    if (len > sizeof cmd) return false;
    memcpy(cmd, apdu, len);
    size_t n = len;
    out->len = 0;
    reader_result_t part;
    for (int hop = 0; hop < 40; hop++) {
        if (!t0r_tpdu(cmd, n, &part)) return false;
        memcpy(out->data + out->len, part.data, part.len);
        out->len += part.len;
        uint8_t sw1 = (uint8_t)(part.sw >> 8), sw2 = (uint8_t)part.sw;
        if (sw1 == 0x6C) {
            cmd[4] = sw2;
            n = 5;
            continue;
        }
        if (sw1 == 0x61) {
            uint8_t gr[5] = { 0x00, 0xC0, 0x00, 0x00, sw2 };
            memcpy(cmd, gr, 5);
            n = 5;
            continue;
        }
        out->sw = part.sw;
        return true;
    }
    return false;
}
