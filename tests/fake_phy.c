#include "fake_phy.h"

#include "test.h"

#include <stdlib.h>

#define QUEUE_SIZE 65536
#define LOG_SIZE   65536

static uint8_t s_queue[QUEUE_SIZE];
static size_t s_q_head, s_q_tail;
static uint8_t s_log[LOG_SIZE];
static size_t s_log_len;
static void (*s_hook)(uint8_t);
static bool s_reset;
static bool s_in_reset = true;
static phy_params_t s_params = PHY_PARAMS_DEFAULT;
static phy_stats_t s_stats;
static uint32_t s_apply_count, s_rx_timeouts;
static void (*s_idle_hook)(void);

// ---- test-side API -----------------------------------------------------------------------------------------------

void fake_phy_push(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        s_queue[s_q_tail] = data[i];
        s_q_tail = (s_q_tail + 1) % QUEUE_SIZE;
        if (s_q_tail == s_q_head) {
            fprintf(stderr, "fake_phy: queue overflow\n");
            abort();
        }
    }
}

void fake_phy_push_hex(const char *hex) {
    uint8_t buf[4200];
    size_t n = hex_parse(hex, buf, sizeof buf);
    fake_phy_push(buf, n);
}

size_t fake_phy_pending(void) { return (s_q_tail - s_q_head + QUEUE_SIZE) % QUEUE_SIZE; }

void fake_phy_clear(void) {
    s_q_head = s_q_tail = 0;
    s_log_len = 0;
    s_reset = false;
}

const uint8_t *fake_phy_tx_log(size_t *len) {
    *len = s_log_len;
    return s_log;
}

void fake_phy_tx_clear(void) { s_log_len = 0; }

void fake_phy_set_tx_hook(void (*hook)(uint8_t)) { s_hook = hook; }

void fake_phy_set_reset(bool asserted) { s_reset = asserted; }

uint32_t fake_phy_apply_count(void) { return s_apply_count; }
uint32_t fake_phy_rx_timeouts(void) { return s_rx_timeouts; }

// ---- phy.h implementation ------------------------------------------------------------------------------------------

void phy_init(void) {}
void phy_set_idle_hook(void (*hook)(void)) { s_idle_hook = hook; }
bool phy_reset_pending(void) { return s_reset; }
bool phy_rst_high(void) { return !s_reset; }
bool phy_in_reset(void) { return s_in_reset; }

void phy_enter_reset(void) {
    s_in_reset = true;
    s_reset = false;
}

void phy_release_io(void) {}

bool phy_params_supported(uint16_t f, uint8_t d) {
    if (!f || !d || f % d) return false;
    return f / d >= 8;
}

const phy_params_t *phy_params(void) { return &s_params; }

bool phy_start_session(const phy_params_t *p) {
    if (!phy_params_supported(p->f, p->d) || p->extra_guard > 20) return false;
    s_params = *p;
    s_in_reset = false;
    s_stats.sessions++;
    s_stats.atr_attempts++;
    s_stats.clk_hz = 4000000;
    return !s_reset;
}

bool phy_apply_params(const phy_params_t *p) {
    if (!phy_params_supported(p->f, p->d) || p->extra_guard > 20) return false;
    s_params = *p;
    s_apply_count++;
    return true;
}

uint32_t phy_etu_us(void) { return 93; }

static bool pop(uint8_t *byte) {
    if (s_q_head == s_q_tail) return false;
    *byte = s_queue[s_q_head];
    s_q_head = (s_q_head + 1) % QUEUE_SIZE;
    return true;
}

bool phy_rx(uint8_t *byte, bool idle) {
    (void)idle;
    if (s_reset) return false;
    if (pop(byte)) {
        s_stats.rx_chars++;
        return true;
    }
    // Nothing queued: the reader model has nothing more to say, which for the card looks like a reset.
    return false;
}

int phy_rx_timeout(uint8_t *byte, uint32_t timeout_us) {
    (void)timeout_us;
    if (s_reset) return -1;
    if (pop(byte)) {
        s_stats.rx_chars++;
        return 1;
    }
    s_rx_timeouts++;
    return 0;
}

bool phy_tx(const uint8_t *data, size_t len) {
    if (s_reset) return false;
    for (size_t i = 0; i < len; i++) {
        if (s_log_len < LOG_SIZE) s_log[s_log_len++] = data[i];
        if (s_hook) s_hook(data[i]);
    }
    s_stats.tx_chars += (uint32_t)len;
    return true;
}

const phy_stats_t *phy_stats(void) { return &s_stats; }

const phy_trace_t *phy_trace(size_t *count) {
    *count = 0;
    return NULL;
}

void phy_sample_pins(phy_pins_t *p) { memset(p, 0, sizeof *p); }

void phy_selftest(char *msg, size_t cap) {
    if (cap) msg[0] = 0;
}
