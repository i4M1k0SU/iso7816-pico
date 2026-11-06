// ISO 7816-3 physical / character layer on top of the RP2040 / RP2350 PIO programs.
//
//   - open-drain I/O, ETU timing derived from the reader's clock
//   - direct and inverse convention
//   - any F/D the PIO can count (ETU = F/D clock cycles, >= 8)
//   - extra guard time (ATR TC1 / N)
//   - parity checking with the T=0 error signal in both directions: a bad character from the reader is answered with
//     the error signal, a character of ours that the reader rejects with the error signal is repeated
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

// The PIO programs run at the system clock and follow the reader's CLK with `wait gpio`: one CLK period costs up to 4
// instructions, plus 5 more at every bit boundary, so the system clock must be at least this many times the CLK
// frequency for the ETU count to stay exact.  125 MHz follows readers up to ~15 MHz, 48 MHz up to ~6 MHz.
#define PHY_MIN_SYSCLK_PER_CLK 8

typedef struct {
    uint32_t sessions;         // ATRs sent
    uint32_t rx_chars;
    uint32_t tx_chars;
    uint32_t rx_parity_errors; // bad parity from reader -> error signal sent
    uint32_t tx_echo_errors;   // our own character read back differently
    uint32_t tx_timeouts;
    uint32_t rx_overruns;      // receiver stalled on a full FIFO: characters from the reader may have been lost
    uint32_t rx_late;          // characters picked up too late for an error signal (one is skipped if parity was bad)
    uint32_t rx_max_latency_us; // longest time a character may have waited in the RX FIFO (upper bound)
    uint32_t tx_repeats;       // characters repeated because the reader sent the error signal (T=0)
    uint32_t clk_hz;           // measured during the last ATR
    uint32_t sys_clk_hz;       // clock the PIO runs at (0 when unknown, e.g. in the host tests)
    volatile uint32_t rst_rises; // RST rising edges seen (written by the RST interrupt)
    volatile uint32_t rst_falls; // RST falling edges seen (written by the RST interrupt)
    uint32_t atr_attempts;     // phy_start_session() calls
    const char *last_error;    // why the last session start / transfer failed
    // Diagnostics for the last ATR attempt
    uint32_t dbg_delay_us;     // time from ATR start to the first echoed character
    uint32_t dbg_echo_index;   // index of first echo mismatch (or count if none)
    uint32_t dbg_echo_expected;
    uint32_t dbg_echo_got;     // raw 9-bit word
    uint32_t dbg_clk_counted_hz; // CLK frequency counted by the PIO clock counter at RST rise
} phy_stats_t;

// Timestamped trace of the last session start (TX pushes and RX words).
typedef struct {
    uint32_t t_us;     // relative to session start
    uint8_t kind;      // 'T' = frame pushed (value = byte), 'P' = same with a deliberately wrong parity bit,
                       // 'R' = word received (9 bits, line states), 'E' = reader's error signal after our character
                       // (value = its start in 1/10 ETU after our leading edge, 0 = not seen by the CPU's polling),
                       // 'e' = end of that error signal (1/10 ETU after our leading edge)
    uint16_t value;
} phy_trace_t;

#define PHY_TRACE_LEN 128
const phy_trace_t *phy_trace(size_t *count);

// Diagnostic: transmit character number `index` of a session (0 = TS of the ATR, counted over everything the card
// sends, repetitions not counted) `count` times with a wrong parity bit, to see the reader's error signal and the
// character repetition.  Fires in the current session if it has not sent that character yet, else in the next one.
void phy_debug_bad_parity(uint32_t index, uint32_t count);

// Snapshot of the contact lines for diagnostics.
typedef struct {
    bool rst;
    bool io;
    bool clk;
    uint32_t clk_toggles;      // CLK level changes seen in a short sampling window
} phy_pins_t;

void phy_sample_pins(phy_pins_t *p);

// Loopback self-test on spare pins (Pico port: ISO7816_SELFTEST_PIN_CLK / _IO, GP16 / GP17 by default; only reports
// "disabled" when they are -1); only in reset.
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
// characters; waits until the last character sent has run out (parity bit, guard time) before the state machines are
// touched, and leaves them alone when the ETU and the guard time stay the same.
bool phy_apply_params(const phy_params_t *p);
const phy_params_t *phy_params(void);

// Microseconds per ETU with the current parameters and measured clock.
uint32_t phy_etu_us(void);

// Receive one character.  Returns false if a reset became pending (the byte is then invalid).  With idle=true the idle
// hook runs while waiting.
bool phy_rx(uint8_t *byte, bool idle);

// Same with a timeout (microseconds).  Returns 1 = byte, 0 = timeout, -1 = reset pending.
int phy_rx_timeout(uint8_t *byte, uint32_t timeout_us);

// Transmit characters.  Returns false on reset, timeout or a character the reader rejected too often.  With
// error_signal (T=0) every character is checked for the reader's error signal and repeated, so the characters go out
// one at a time; without it (T=1) they are streamed.
bool phy_tx(const uint8_t *data, size_t len);

// Release the I/O line driver (used when the card decides to stay mute).
void phy_release_io(void);

const phy_stats_t *phy_stats(void);
