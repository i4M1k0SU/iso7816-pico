// ISO 7816-3 physical / character layer on top of the RP2040 PIO programs.
//
//   - open-drain I/O, ETU timing derived from the reader's clock
//   - direct and inverse convention
//   - any F/D the PIO can count (ETU = F/D clock cycles, >= 8)
//   - extra guard time (ATR TC1 / N)
//   - parity checking with the T=0 error signal and character repetition
//   - character turnaround (16 ETU for T=0, 22 ETU block guard time for T=1)
//
// Everything is polled from the main loop; the only interrupt is a falling-edge latch on RST so that even a short reset
// pulse is noticed.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Transmission parameters of a session.  Changed by PPS (proto/pps.c).
typedef struct {
    uint16_t f;             // clock rate conversion integer Fi (372 ...)
    uint8_t d;              // baud rate adjustment integer Di (1 ...)
    bool inverse;           // inverse convention (TS = 3F)
    uint8_t extra_guard;    // N from TC1: extra guard ETUs after each character (0..20)
    bool error_signal;      // send the T=0 parity error signal (false for T=1)
    uint8_t turnaround_etu; // min. leading-edge distance reader char -> our char
} phy_params_t;

#define PHY_PARAMS_DEFAULT { 372, 1, false, 0, true, 16 }

typedef struct {
    uint32_t sessions;         // ATRs sent
    uint32_t rx_chars;
    uint32_t tx_chars;
    uint32_t rx_parity_errors; // bad parity from reader -> error signal sent
    uint32_t tx_echo_errors;   // our own character read back differently
    uint32_t tx_timeouts;
    uint32_t clk_hz;           // measured during the last ATR
    volatile uint32_t rst_rises; // RST rising edges seen (written by the RST interrupt)
    volatile uint32_t rst_falls; // RST falling edges seen (written by the RST interrupt)
    uint32_t atr_attempts;     // phy_start_session() calls
    const char *last_error;    // why the last session start / transfer failed
    // Diagnostics for the last ATR attempt
    uint32_t dbg_delay_us;     // time from ATR start to the first echoed character
    uint32_t dbg_echo_index;   // index of first echo mismatch (or count if none)
    uint32_t dbg_echo_expected;
    uint32_t dbg_echo_got;     // raw 9-bit word
    uint32_t dbg_clk_pwm_hz;   // CLK frequency counted by the PWM slice at RST rise
} phy_stats_t;

// Timestamped trace of the last session start (TX pushes and RX words).
typedef struct {
    uint32_t t_us;     // relative to session start
    uint8_t kind;      // 'T' = frame pushed (value = byte), 'R' = word received
    uint16_t value;
} phy_trace_t;

#define PHY_TRACE_LEN 48
const phy_trace_t *phy_trace(size_t *count);

// Snapshot of the contact lines for diagnostics.
typedef struct {
    bool rst;
    bool io;
    bool clk;
    uint32_t clk_toggles;      // CLK level changes seen in a short sampling window
} phy_pins_t;

void phy_sample_pins(phy_pins_t *p);

// Loopback self-test on spare pins (GP16 clock, GP17 I/O); only in reset.
void phy_selftest(char *msg, size_t cap);

// One-time hardware setup (GPIOs, PIO programs, RST edge latch).
void phy_init(void);

// Function called while waiting for the first byte of a command (idle time).
void phy_set_idle_hook(void (*hook)(void));

// True when the reader holds RST low *or* a falling edge was latched since the last phy_enter_reset().
bool phy_reset_pending(void);
bool phy_rst_high(void);

// Stop both state machines, release I/O, forget any latched reset.
void phy_enter_reset(void);
bool phy_in_reset(void);

// True when the PIO can produce this ETU (F/D integer and >= 8 clocks).
bool phy_params_supported(uint16_t f, uint8_t d);

// Start a card session after RST went high: restart the state machines with the given parameters.  The first character
// sent afterwards (the ATR) starts exactly 12 ETU later.  Returns false if the reset was re-asserted.
bool phy_start_session(const phy_params_t *p);

// Switch parameters in the middle of a session (after a PPS response has been sent).  Must be called between
// characters.
bool phy_apply_params(const phy_params_t *p);
const phy_params_t *phy_params(void);

// Microseconds per ETU with the current parameters and measured clock.
uint32_t phy_etu_us(void);

// Receive one character.  Returns false if a reset became pending (the byte is then invalid).  With idle=true the idle
// hook runs while waiting.
bool phy_rx(uint8_t *byte, bool idle);

// Same with a timeout (microseconds).  Returns 1 = byte, 0 = timeout, -1 = reset pending.
int phy_rx_timeout(uint8_t *byte, uint32_t timeout_us);

// Transmit characters.  Returns false on reset or timeout.
bool phy_tx(const uint8_t *data, size_t len);

// Release the I/O line driver (used when the card decides to stay mute).
void phy_release_io(void);

const phy_stats_t *phy_stats(void);
