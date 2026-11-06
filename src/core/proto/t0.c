#include "t0.h"

#include "iso7816_pico/apdu.h"
#include "apdu/dispatch.h"
#include "hal/phy.h"
#include "iso7816_pico/log.h"

#include <string.h>

// Response data waiting for GET RESPONSE (after 61 xx) or for the re-issued command after 6C xx.
static const uint8_t *s_pending;
static uint32_t s_pending_len, s_pending_off;
static uint16_t s_pending_sw;        // SW to send with the last chunk
static uint8_t s_pending_hdr[4];     // header that produced a 6C xx answer
static bool s_pending_6c;

// ENVELOPE reassembly (extended-length APDU over T=0).
static uint32_t s_env_len;

// Waiting time integer (TC2 of the ATR) for the WWT used inside a TPDU.
#define T0_DEFAULT_WI 10
static uint8_t s_wi = T0_DEFAULT_WI;

void t0_session_begin(void) {
    s_pending = NULL;
    s_pending_len = s_pending_off = 0;
    s_pending_6c = false;
    s_env_len = 0;
    s_wi = T0_DEFAULT_WI;
}

void t0_set_wi(uint8_t wi) { s_wi = wi ? wi : T0_DEFAULT_WI; }   // TC2 = 00 is reserved

// WWT = 960 x D x WI ETU (ISO 7816-3:2006 10.2), recomputed per use because a PPS may have changed D.  The ETU is
// rounded up so that the card never gives up before the reader would.
static uint32_t wwt_us(void) {
    uint64_t us = 960ull * s_wi * phy_params()->d * (phy_etu_us() + 1);
    return us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
}

static bool send_sw(uint16_t sw) {
    uint8_t b[2] = { (uint8_t)(sw >> 8), (uint8_t)sw };
    return phy_tx(b, 2);
}

// Procedure byte ACK = INS: "send/receive all remaining data bytes".
static bool send_ack(uint8_t ins) { return phy_tx(&ins, 1); }

bool t0_keepalive(void) {
    uint8_t null_byte = 0x60;
    return phy_tx(&null_byte, 1);
}

// Receive n bytes that the reader sends back to back (rest of a header, data field after the ACK).  Each byte must
// come within WWT of the previous one: 1 = all received, 0 = the reader went silent, -1 = reset.
static int rx_bytes(uint8_t *buf, size_t n) {
    uint32_t timeout = wwt_us();
    for (size_t i = 0; i < n; i++) {
        int r = phy_rx_timeout(&buf[i], timeout);
        if (r <= 0) return r;
    }
    return 1;
}

static uint16_t sw_more_data(uint32_t remaining) {
    return SW_BYTES_REMAINING(remaining >= 256 ? 0 : remaining);
}

// Data may only accompany a success or warning status.
static bool sw_allows_data(uint16_t sw) {
    uint8_t sw1 = (uint8_t)(sw >> 8);
    return sw1 == 0x90 || sw1 == 0x61 || sw1 == 0x62 || sw1 == 0x63;
}

// Send one chunk of the pending response: ACK, up to `le` bytes, then 61 xx or the final SW.
static bool send_pending_chunk(uint8_t ins, uint32_t le) {
    uint32_t remaining = s_pending_len - s_pending_off;
    uint32_t n = le < remaining ? le : remaining;
    if (!send_ack(ins) || !phy_tx(&s_pending[s_pending_off], n)) return false;
    s_pending_off += n;
    remaining -= n;
    if (remaining) return send_sw(sw_more_data(remaining));
    uint16_t sw = s_pending_sw;
    s_pending = NULL;
    s_pending_len = s_pending_off = 0;
    return send_sw(sw);
}

// xx C0 00 00 Le  -> next chunk of the pending response
static bool handle_get_response(const uint8_t hdr[5]) {
    uint32_t remaining = s_pending ? s_pending_len - s_pending_off : 0;
    uint32_t le = hdr[4] ? hdr[4] : 256;
    if (remaining == 0) return send_sw(SW_CONDITIONS_NOT_SAT);
    if (le > remaining) return send_sw(SW_WRONG_LE(remaining));
    return send_pending_chunk(hdr[1], le);
}

// Answer a command whose response data is in *resp.
static bool send_response(const uint8_t hdr[5], apdu_case_t kind, uint32_t le,
                          const apdu_resp_t *resp, uint16_t sw) {
    if (resp->len == 0 || !sw_allows_data(sw)) return send_sw(sw);

    s_pending = resp->data;
    s_pending_len = resp->len;
    s_pending_off = 0;
    s_pending_sw = sw;

    if (kind == APDU_CASE2 && resp->len <= 256) {
        if (resp->len == le) return send_pending_chunk(hdr[1], le);
        // Wrong Le: tell the host the right one.  It re-issues the command with that Le and gets the response kept
        // here, so the command is not executed twice.
        memcpy(s_pending_hdr, hdr, 4);
        s_pending_6c = true;
        return send_sw(SW_WRONG_LE(resp->len));
    }
    // Case 4, long case 2 and everything inside an ENVELOPE: the host fetches the data with GET RESPONSE.
    return send_sw(sw_more_data(resp->len));
}

static bool run_command(const uint8_t hdr[5], apdu_t *cmd, apdu_case_t kind) {
    apdu_resp_t resp = { dispatch_resp_buf(), APDU_BUF_SIZE, 0 };
    uint16_t sw = dispatch_process(cmd, &resp);
    bool ok = send_response(hdr, kind, cmd->le, &resp, sw);
    log_apdu(dispatch_name(cmd->cla, cmd->ins), cmd->cla, cmd->ins, cmd->p1, cmd->p2, cmd->data,
             cmd->lc, resp.len ? resp.data : NULL, resp.len, sw);
    return ok;
}

// xx C2 00 00 Lc <part of an extended APDU>; Lc = 255 continues the chain, a shorter Lc (0 included) ends it.
static bool handle_envelope(const uint8_t hdr[5]) {
    uint8_t *buf = dispatch_cmd_buf();
    uint32_t lc = hdr[4];
    if (s_env_len + lc > APDU_BUF_SIZE) {
        s_env_len = 0;
        return send_sw(SW_WRONG_LENGTH);
    }
    if (lc) {
        if (!send_ack(hdr[1])) return false;
        int r = rx_bytes(&buf[s_env_len], lc);
        if (r <= 0) {
            s_env_len = 0;              // the chain is broken either way
            if (r < 0) return false;
            log_printf("T=0: ENVELOPE data stopped short (%lu bytes expected), dropped", (unsigned long)lc);
            return true;
        }
        s_env_len += lc;
    }
    // ISO 7816-4 does not say how an ENVELOPE chain ends, so this is our convention (docs/internals.md): an ENVELOPE
    // with Lc = 255 means more is coming (9000); any shorter one, including Lc = 0 (P3 = 00), is the last piece and
    // the accumulated bytes must decode as one APDU (6A80 otherwise).  A host whose total length is a multiple of 255
    // therefore ends the chain with an empty ENVELOPE.
    if (lc == 255) return send_sw(SW_OK);
    apdu_t cmd;
    bool complete = apdu_decode(buf, s_env_len, &cmd);
    s_env_len = 0;
    if (!complete) return send_sw(SW_WRONG_DATA);
    // The ENVELOPE TPDU itself is case 3: the reader has sent all its bytes and expects only SW1 SW2 now.  So any
    // response data of the inner APDU (case 2 included, whatever its Le) is announced with 61 xx and fetched with
    // GET RESPONSE, never sent after a second ACK or bounced with 6C xx.  Without data the plain SW goes out.
    return run_command(hdr, &cmd, APDU_CASE4);
}

bool t0_handle_command(int first_byte) {
    uint8_t hdr[5];
    if (first_byte >= 0) hdr[0] = (uint8_t)first_byte;
    else if (!phy_rx(&hdr[0], true)) return false;
    // The rest of the header follows back to back.  A lone character (noise, a stray echo) would otherwise become CLA
    // and shift every later header by one byte; drop it and wait for a new header.
    int r = rx_bytes(&hdr[1], 4);
    if (r < 0) return false;
    if (r == 0) {
        log_printf("T=0: partial header (CLA %02X), dropped", hdr[0]);
        return true;
    }

    uint8_t cla = hdr[0], ins = hdr[1];
    if (cla == 0xFF) return send_sw(SW_CLA_NOT_SUPPORTED);   // PPS after the first command

    if (ins == 0xC0) {
        bool ok = handle_get_response(hdr);
        log_apdu("GET RESPONSE", cla, ins, hdr[2], hdr[3], NULL, 0, NULL, 0, 0);
        return ok;
    }
    // A re-issued command after 6C xx: serve the kept response.
    if (s_pending_6c && memcmp(hdr, s_pending_hdr, 4) == 0 && s_pending) {
        s_pending_6c = false;
        uint32_t le = hdr[4] ? hdr[4] : 256;
        if (le == s_pending_len) return send_pending_chunk(ins, le);
    }
    s_pending_6c = false;
    s_pending = NULL;                         // any other command discards leftovers
    s_pending_len = s_pending_off = 0;

    if (ins == 0xC2) return handle_envelope(hdr);
    s_env_len = 0;

    apdu_case_t kind = dispatch_classify(cla, ins, hdr[2], hdr[3]);
    apdu_t cmd = {
        .cla = cla, .ins = ins, .p1 = hdr[2], .p2 = hdr[3],
        .data = NULL, .lc = 0, .le = 0, .le_present = false, .extended = false,
    };
    uint8_t *data_in = dispatch_cmd_buf();

    switch (kind) {
    case APDU_CASE2:
        cmd.le = hdr[4] ? hdr[4] : 256;
        cmd.le_present = true;
        break;
    case APDU_CASE3:
    case APDU_CASE4:
        cmd.lc = hdr[4];
        if (cmd.lc) {
            cmd.data = data_in;
            if (!send_ack(ins)) return false;
            r = rx_bytes(data_in, cmd.lc);
            if (r < 0) return false;
            if (r == 0) {
                log_printf("T=0: %02X %02X data stopped short (%lu bytes expected), dropped", cla, ins,
                           (unsigned long)cmd.lc);
                return true;
            }
        }
        if (kind == APDU_CASE4 || cmd.lc == 0) {
            // P3 = 0 with a data-in command: no data, treat as "Le = 256".
            cmd.le = 256;
            cmd.le_present = true;
        }
        break;
    default:   // case 1 / unknown: P3 is ignored, nothing in, nothing out
        break;
    }
    return run_command(hdr, &cmd, kind);
}
