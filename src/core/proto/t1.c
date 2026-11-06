#include "t1.h"

#include "iso7816_pico/apdu.h"
#include "apdu/dispatch.h"
#include "hal/phy.h"
#include "iso7816_pico/log.h"

#include <string.h>

#define T1_MAX_INF   254
#define T1_MAX_BLOCK (3 + T1_MAX_INF + 2)
#define T1_IFS_DEFAULT 32
#define T1_CWT_MARGIN_US 1000       // polling and ETU rounding slack on top of CWT

// PCB coding
#define PCB_I_NS   0x40
#define PCB_I_M    0x20
#define PCB_R      0x80
#define PCB_R_NR   0x10
#define PCB_S      0xC0
#define PCB_S_RESP 0x20
#define S_RESYNCH  0
#define S_IFS      1
#define S_ABORT    2
#define S_WTX      3

// R-block error codes (PCB b2..b1)
#define R_OK         0
#define R_EDC_ERROR  1
#define R_OTHER      2

static t1_config_t s_cfg;
static uint8_t s_ns;              // sequence number of our next I-block
static uint8_t s_nr;              // sequence number we expect from the reader
static uint8_t s_ifsd = T1_IFS_DEFAULT; // reader's maximum INF (S(IFS) changes it)
static uint8_t s_nad_resp;        // NAD with the address nibbles swapped

// Command being assembled from chained I-blocks.  On overflow the rest of the chain is acknowledged and discarded, and
// the command is answered with 6700 when the chain ends (a card-initiated S(ABORT) is not handled by many TPDU-level
// readers, Windows' usbccid among them).
static uint32_t s_cmd_len;
static bool s_cmd_overflow;

// Response being sent, possibly in several I-blocks: data + SW1 SW2, in place in the dispatcher's response buffer.
// Nothing else writes to that buffer until the next command is executed, and a new command abandons the chain anyway.
static const uint8_t *s_resp;
static uint32_t s_resp_len, s_resp_off;
static bool s_tx_more;            // we are in the middle of a chained response

// Block kept for retransmission (ISO/IEC 7816-3 11.6.3): only the last I-block we sent (until the reader's next
// I-block acknowledges it) or the R-block acknowledging the reader's chain.  Error R-blocks and S-blocks are not kept:
// they would replace the I-block that a later retransmission request asks for.
enum { LAST_NONE, LAST_I, LAST_R_ACK };
static uint8_t s_last[T1_MAX_BLOCK];
static size_t s_last_len;
static uint8_t s_last_kind;

static uint32_t s_errors;

static void reset_link_state(void) {
    s_ns = s_nr = 0;
    s_ifsd = T1_IFS_DEFAULT;
    s_cmd_len = 0;
    s_cmd_overflow = false;
    s_resp = NULL;
    s_resp_len = s_resp_off = 0;
    s_tx_more = false;
    s_last_kind = LAST_NONE;
}

void t1_session_begin(const t1_config_t *cfg) {
    s_cfg = *cfg;
    if (!s_cfg.ifsc || s_cfg.ifsc > T1_MAX_INF) s_cfg.ifsc = s_cfg.ifsc ? T1_MAX_INF : T1_IFS_DEFAULT;
    if (s_cfg.cwi > 15) s_cfg.cwi = 13;
    reset_link_state();
    s_nad_resp = 0;               // an R-block before the first valid block goes out with NAD 0
    s_errors = 0;
}

// Character waiting time CWT = (11 + 2^CWI) ETU, plus a margin, in microseconds.
static uint32_t cwt_us(void) {
    uint32_t etu = 11u + (1u << s_cfg.cwi);
    return etu * (phy_etu_us() + 1) + T1_CWT_MARGIN_US;
}

// ---------------------------------------------------------------------------
// error detection code
// ---------------------------------------------------------------------------

static uint16_t crc16(const uint8_t *p, size_t n) {
    // ISO/IEC 13239 (as used by ISO 7816-3 T=1): x^16 + x^12 + x^5 + 1, reflected, initial value FFFF, final XOR FFFF.
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0x8408 : c >> 1;
    }
    return (uint16_t)~c;
}

static size_t edc_len(void) { return s_cfg.crc ? 2 : 1; }

static void edc_append(uint8_t *blk, size_t n) {
    if (s_cfg.crc) {
        uint16_t c = crc16(blk, n);
        blk[n] = (uint8_t)(c >> 8);
        blk[n + 1] = (uint8_t)c;
    } else {
        uint8_t lrc = 0;
        for (size_t i = 0; i < n; i++) lrc ^= blk[i];
        blk[n] = lrc;
    }
}

static bool edc_check(const uint8_t *blk, size_t n_with_edc) {
    size_t n = n_with_edc - edc_len();
    if (s_cfg.crc) {
        uint16_t c = crc16(blk, n);
        return blk[n] == (uint8_t)(c >> 8) && blk[n + 1] == (uint8_t)c;
    }
    uint8_t lrc = 0;
    for (size_t i = 0; i < n_with_edc; i++) lrc ^= blk[i];
    return lrc == 0;
}

// ---------------------------------------------------------------------------
// block I/O
// ---------------------------------------------------------------------------

static bool send_block(uint8_t pcb, const uint8_t *inf, size_t len, uint8_t keep) {
    uint8_t blk[T1_MAX_BLOCK];
    blk[0] = s_nad_resp;
    blk[1] = pcb;
    blk[2] = (uint8_t)len;
    if (len) memcpy(&blk[3], inf, len);
    edc_append(blk, 3 + len);
    size_t n = 3 + len + edc_len();
    if (keep != LAST_NONE) {
        memcpy(s_last, blk, n);
        s_last_len = n;
        s_last_kind = keep;
    }
    return phy_tx(blk, n);
}

static bool send_r(uint8_t nr, uint8_t err) {
    return send_block((uint8_t)(PCB_R | (nr ? PCB_R_NR : 0) | err), NULL, 0, LAST_NONE);
}

// The reader asks for a retransmission (or repeats its own block because it missed our answer): resend the kept
// block.  With none (nothing sent yet, or just after RESYNCH / ABORT) the request is invalid, so answer as for any
// invalid block (rule 7.1): an R-block naming the I-block we expect, with "other error".  Staying silent would leave
// the reader waiting until BWT.
static bool resend_last(void) {
    if (s_last_kind == LAST_NONE) return send_r(s_nr, R_OTHER);
    return phy_tx(s_last, s_last_len);
}

// After a receive error, wait until the reader has been silent for CWT so that the answer does not collide with the
// rest of a block it is still sending.  Returns 0 once the line is quiet, -1 on reset.
static int drain(void) {
    uint8_t junk;
    int r;
    while ((r = phy_rx_timeout(&junk, cwt_us())) == 1) {}
    return r;
}

// rx_block() results
#define RX_RESET  (-1)
#define RX_OTHER  0         // timeout or LEN = FF
#define RX_OK     1
#define RX_EDC    2         // complete block with a bad EDC

// Receive one block into blk/len.  After an invalid block the line has been drained (see drain()).
static int rx_block(int first_byte, uint8_t *blk, size_t *len) {
    size_t n = 0;
    if (first_byte >= 0) {
        blk[n++] = (uint8_t)first_byte;
    } else {
        if (!phy_rx(&blk[n++], true)) return RX_RESET;
    }
    uint32_t cwt = cwt_us();
    for (int i = 1; i < 3; i++) {
        int r = phy_rx_timeout(&blk[n++], cwt);
        if (r <= 0) return r;                    // RX_RESET or RX_OTHER; a timeout is already CWT of silence
    }
    size_t inf_len = blk[2];
    if (inf_len > T1_MAX_INF) return drain();    // 255 is RFU; up to 256 more characters may follow (RX_OTHER)
    size_t total = 3 + inf_len + edc_len();
    while (n < total) {
        int r = phy_rx_timeout(&blk[n++], cwt);
        if (r <= 0) return r;
    }
    // A bad EDC may come from a corrupted LEN, in which case the reader is still sending.
    if (!edc_check(blk, total)) return drain() < 0 ? RX_RESET : RX_EDC;
    *len = total;
    return RX_OK;
}

// ---------------------------------------------------------------------------
// response sending (chained)
// ---------------------------------------------------------------------------

static bool send_next_i_block(void) {
    uint32_t remaining = s_resp_len - s_resp_off;
    uint32_t n = remaining < s_ifsd ? remaining : s_ifsd;
    bool more = n < remaining;
    uint8_t pcb = (uint8_t)((s_ns ? PCB_I_NS : 0) | (more ? PCB_I_M : 0));
    bool ok = send_block(pcb, &s_resp[s_resp_off], n, LAST_I);
    s_resp_off += n;
    s_ns ^= 1;
    s_tx_more = more;
    return ok;
}

// Execute the assembled command and start sending the response.
static bool execute_command(void) {
    uint8_t *buf = dispatch_cmd_buf();
    apdu_t cmd;
    apdu_resp_t resp = APDU_RESP_INIT(dispatch_resp_buf(), APDU_BUF_SIZE);
    uint16_t sw;
    if (s_cmd_overflow) {
        sw = SW_WRONG_LENGTH;
        log_printf("T=1: command chain longer than the APDU buffer (%u bytes)", (unsigned)APDU_BUF_SIZE);
    } else if (!apdu_decode(buf, s_cmd_len, &cmd)) {
        sw = SW_WRONG_LENGTH;
        log_printf("T=1: malformed APDU (%lu bytes)", (unsigned long)s_cmd_len);
    } else {
        sw = dispatch_process(&cmd, &resp);
        uint8_t sw1 = (uint8_t)(sw >> 8);
        bool data_ok = sw1 == 0x90 || sw1 == 0x61 || sw1 == 0x62 || sw1 == 0x63;
        if (!data_ok) resp.len = 0;
        if (cmd.le_present && resp.len > cmd.le) resp.len = cmd.le;
        log_apdu(dispatch_name(cmd.cla, cmd.ins), cmd.cla, cmd.ins, cmd.p1, cmd.p2, cmd.data,
                 cmd.lc, resp.len ? resp.data : NULL, resp.len, sw);
    }
    s_cmd_len = 0;
    s_cmd_overflow = false;
    // resp.len <= APDU_BUF_SIZE: the SW goes into the two spare bytes after the data (see dispatch_resp_buf()).
    resp.data[resp.len] = (uint8_t)(sw >> 8);
    resp.data[resp.len + 1] = (uint8_t)sw;
    s_resp = resp.data;
    s_resp_len = resp.len + 2;
    s_resp_off = 0;
    return send_next_i_block();
}

// ---------------------------------------------------------------------------
// main entry
// ---------------------------------------------------------------------------

// Error handling follows ISO/IEC 7816-3 11.6.3: an invalid block is answered with an R-block naming the I-block we
// expect (rule 7.1), and the same R-block again on each further error (rule 7.2); a retransmission request gets the
// kept block back (see resend_last()).
bool t1_handle_block(int first_byte) {
    uint8_t blk[T1_MAX_BLOCK];
    size_t len;
    int r = rx_block(first_byte, blk, &len);
    if (r == RX_RESET) return false;
    if (r != RX_OK) {
        s_errors++;
        return send_r(s_nr, r == RX_EDC ? R_EDC_ERROR : R_OTHER);
    }
    if (blk[0] == 0xFF) return true;                // invalid NAD, ignore
    s_nad_resp = (uint8_t)((blk[0] << 4) | (blk[0] >> 4));

    uint8_t pcb = blk[1];
    size_t inf_len = blk[2];
    const uint8_t *inf = &blk[3];

    // ---- S-block ------------------------------------------------------------
    if ((pcb & PCB_S) == PCB_S) {
        // We never send an S(... request) that is not answered inside t1_keepalive(), so a response here is
        // unexpected: treat it as an invalid block.
        if (pcb & PCB_S_RESP) return send_r(s_nr, R_OTHER);
        switch (pcb & 0x1F) {
        case S_RESYNCH:
            // Back to the initial state, including IFSD = 32 (11.6.3, RESYNCH resets the communication parameters).
            reset_link_state();
            return send_block(PCB_S | PCB_S_RESP | S_RESYNCH, NULL, 0, LAST_NONE);
        case S_IFS:
            // IFSD 00 and FF are reserved; 01..FE are valid.
            if (inf_len != 1 || inf[0] == 0x00 || inf[0] == 0xFF) return send_r(s_nr, R_OTHER);
            s_ifsd = inf[0];
            return send_block(PCB_S | PCB_S_RESP | S_IFS, inf, 1, LAST_NONE);
        case S_ABORT:
            s_cmd_len = 0;
            s_cmd_overflow = false;
            s_tx_more = false;
            s_last_kind = LAST_NONE;
            return send_block(PCB_S | PCB_S_RESP | S_ABORT, NULL, 0, LAST_NONE);
        default:                                    // WTX is ours to request
            return send_r(s_nr, R_OTHER);
        }
    }

    // ---- R-block ------------------------------------------------------------
    if ((pcb & PCB_R) == PCB_R) {
        uint8_t nr = (pcb & PCB_R_NR) ? 1 : 0;
        if (s_tx_more && nr == s_ns) return send_next_i_block();       // chain ack
        if (s_last_kind == LAST_I && nr != s_ns) return resend_last();  // please repeat our last I-block
        if (s_last_kind == LAST_R_ACK) return resend_last();           // during the reader's chain: repeat the ack
        if (nr != s_ns) return resend_last();                          // nothing to repeat: R(N(R)), other error
        return send_r(s_nr, R_OK);
    }

    // ---- I-block ------------------------------------------------------------
    uint8_t ns = (pcb & PCB_I_NS) ? 1 : 0;
    bool more = pcb & PCB_I_M;
    if (ns != s_nr) {
        // The reader repeats its previous block: it missed our answer.
        return resend_last();
    }
    // A new I-block acknowledges our previous one, which is no longer retransmitted.
    s_last_kind = LAST_NONE;
    if (s_cmd_overflow || s_cmd_len + inf_len > APDU_BUF_SIZE) {
        s_cmd_overflow = true;                      // discard, keep acknowledging, answer 6700 at the end
    } else {
        memcpy(dispatch_cmd_buf() + s_cmd_len, inf, inf_len);
        s_cmd_len += inf_len;
    }
    s_nr ^= 1;
    s_tx_more = false;
    if (more) {                                     // acknowledge the chain
        return send_block((uint8_t)(PCB_R | (s_nr ? PCB_R_NR : 0)), NULL, 0, LAST_R_ACK);
    }
    return execute_command();
}

bool t1_keepalive(void) {
    uint8_t mult = 1;
    if (!send_block(PCB_S | S_WTX, &mult, 1, LAST_NONE)) return false;
    uint8_t blk[T1_MAX_BLOCK];
    size_t len;
    if (rx_block(-1, blk, &len) == RX_RESET) return false;
    // Anything but S(WTX response) is a protocol slip; carry on regardless.
    return true;
}
