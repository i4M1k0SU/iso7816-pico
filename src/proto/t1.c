#include "t1.h"

#include "apdu/apdu.h"
#include "apdu/dispatch.h"
#include "hal/phy.h"
#include "sys/log.h"

#include <string.h>

#define T1_MAX_INF   254
#define T1_MAX_BLOCK (3 + T1_MAX_INF + 2)
#define T1_CHAR_TIMEOUT_US 500000   // generous CWT: the reader stopped mid-block

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

static t1_config_t s_cfg;
static uint8_t s_ns;              // sequence number of our next I-block
static uint8_t s_nr;              // sequence number we expect from the reader
static uint8_t s_ifsd = 32;       // reader's maximum INF (S(IFS) changes it)
static uint8_t s_nad_resp;        // NAD with the address nibbles swapped

// Command being assembled from chained I-blocks.
static uint32_t s_cmd_len;

// Response being sent, possibly in several I-blocks: data + SW1 SW2, in place in the dispatcher's response buffer.
// Nothing else writes to that buffer until the next command is executed, and a new command abandons the chain anyway.
static const uint8_t *s_resp;
static uint32_t s_resp_len, s_resp_off;
static bool s_tx_more;            // we are in the middle of a chained response

// Last block we sent, for retransmission.
static uint8_t s_last[T1_MAX_BLOCK];
static size_t s_last_len;
static bool s_last_valid;

static uint32_t s_errors;

void t1_session_begin(const t1_config_t *cfg) {
    s_cfg = *cfg;
    if (!s_cfg.ifsc || s_cfg.ifsc > T1_MAX_INF) s_cfg.ifsc = s_cfg.ifsc ? T1_MAX_INF : 32;
    s_ns = s_nr = 0;
    s_ifsd = 32;
    s_cmd_len = 0;
    s_resp = NULL;
    s_resp_len = s_resp_off = 0;
    s_tx_more = false;
    s_last_valid = false;
    s_errors = 0;
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

static bool send_block(uint8_t pcb, const uint8_t *inf, size_t len, bool remember) {
    uint8_t blk[T1_MAX_BLOCK];
    blk[0] = s_nad_resp;
    blk[1] = pcb;
    blk[2] = (uint8_t)len;
    if (len) memcpy(&blk[3], inf, len);
    edc_append(blk, 3 + len);
    size_t n = 3 + len + edc_len();
    if (remember) {
        memcpy(s_last, blk, n);
        s_last_len = n;
        s_last_valid = true;
    }
    return phy_tx(blk, n);
}

static bool resend_last(void) {
    if (!s_last_valid) return true;
    return phy_tx(s_last, s_last_len);
}

static bool send_r(uint8_t nr, uint8_t err) {
    return send_block((uint8_t)(PCB_R | (nr ? PCB_R_NR : 0) | err), NULL, 0, true);
}

// Receive one block.  Returns 1 = block in blk/len (EDC not yet checked), 0 = timeout / oversize (garbage), -1 = reset.
static int rx_block(int first_byte, uint8_t *blk, size_t *len) {
    size_t n = 0;
    if (first_byte >= 0) {
        blk[n++] = (uint8_t)first_byte;
    } else {
        if (!phy_rx(&blk[n++], true)) return -1;
    }
    for (int i = 1; i < 3; i++) {
        int r = phy_rx_timeout(&blk[n++], T1_CHAR_TIMEOUT_US);
        if (r <= 0) return r;
    }
    size_t inf_len = blk[2];
    if (inf_len > T1_MAX_INF) return 0;      // 255 is RFU
    size_t total = 3 + inf_len + edc_len();
    while (n < total) {
        int r = phy_rx_timeout(&blk[n++], T1_CHAR_TIMEOUT_US);
        if (r <= 0) return r;
    }
    *len = total;
    return 1;
}

// ---------------------------------------------------------------------------
// response sending (chained)
// ---------------------------------------------------------------------------

static bool send_next_i_block(void) {
    uint32_t remaining = s_resp_len - s_resp_off;
    uint32_t n = remaining < s_ifsd ? remaining : s_ifsd;
    bool more = n < remaining;
    uint8_t pcb = (uint8_t)((s_ns ? PCB_I_NS : 0) | (more ? PCB_I_M : 0));
    bool ok = send_block(pcb, &s_resp[s_resp_off], n, true);
    s_resp_off += n;
    s_ns ^= 1;
    s_tx_more = more;
    return ok;
}

// Execute the assembled command and start sending the response.
static bool execute_command(void) {
    uint8_t *buf = dispatch_cmd_buf();
    apdu_t cmd;
    apdu_resp_t resp = { dispatch_resp_buf(), APDU_BUF_SIZE, 0 };
    uint16_t sw;
    if (!apdu_decode(buf, s_cmd_len, &cmd)) {
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

bool t1_handle_block(int first_byte) {
    uint8_t blk[T1_MAX_BLOCK];
    size_t len;
    int r = rx_block(first_byte, blk, &len);
    if (r < 0) return false;
    if (r == 0) {
        s_errors++;
        return send_r(s_nr, 2);                     // other error
    }
    if (!edc_check(blk, len)) {
        s_errors++;
        return send_r(s_nr, 1);                     // EDC error
    }
    if (blk[0] == 0xFF) return true;                // invalid NAD, ignore
    s_nad_resp = (uint8_t)((blk[0] << 4) | (blk[0] >> 4));

    uint8_t pcb = blk[1];
    size_t inf_len = blk[2];
    const uint8_t *inf = &blk[3];

    // ---- S-block ------------------------------------------------------------
    if ((pcb & PCB_S) == PCB_S) {
        if (pcb & PCB_S_RESP) return true;          // stray response: ignore
        switch (pcb & 0x1F) {
        case S_RESYNCH:
            s_ns = s_nr = 0;
            s_cmd_len = 0;
            s_tx_more = false;
            s_last_valid = false;
            return send_block(PCB_S | PCB_S_RESP | S_RESYNCH, NULL, 0, false);
        case S_IFS:
            if (inf_len != 1 || inf[0] < 0x10 || inf[0] == 0xFF) return send_r(s_nr, 2);
            s_ifsd = inf[0];
            return send_block(PCB_S | PCB_S_RESP | S_IFS, inf, 1, false);
        case S_ABORT:
            s_cmd_len = 0;
            s_tx_more = false;
            return send_block(PCB_S | PCB_S_RESP | S_ABORT, NULL, 0, false);
        default:                                    // WTX is ours to request
            return send_r(s_nr, 2);
        }
    }

    // ---- R-block ------------------------------------------------------------
    if ((pcb & PCB_R) == PCB_R) {
        uint8_t nr = (pcb & PCB_R_NR) ? 1 : 0;
        if (s_tx_more && nr == s_ns) return send_next_i_block();   // chain ack
        if (s_last_valid && nr != s_ns) return resend_last();      // please repeat
        if (s_tx_more) return resend_last();
        return send_r(s_nr, 0);
    }

    // ---- I-block ------------------------------------------------------------
    uint8_t ns = (pcb & PCB_I_NS) ? 1 : 0;
    bool more = pcb & PCB_I_M;
    if (ns != s_nr) {
        // The reader repeats its previous block: it missed our answer.
        return resend_last();
    }
    if (s_cmd_len + inf_len > APDU_BUF_SIZE) {
        s_cmd_len = 0;
        return send_block(PCB_S | S_ABORT, NULL, 0, false);
    }
    memcpy(dispatch_cmd_buf() + s_cmd_len, inf, inf_len);
    s_cmd_len += inf_len;
    s_nr ^= 1;
    s_tx_more = false;
    if (more) return send_r(s_nr, 0);               // acknowledge the chain
    return execute_command();
}

bool t1_keepalive(void) {
    uint8_t mult = 1;
    if (!send_block(PCB_S | S_WTX, &mult, 1, false)) return false;
    uint8_t blk[T1_MAX_BLOCK];
    size_t len;
    int r = rx_block(-1, blk, &len);
    if (r < 0) return false;
    // Anything but S(WTX response) is a protocol slip; carry on regardless.
    return true;
}
