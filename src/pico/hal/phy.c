#include "hal/phy.h"

#include "iso7816_pico/config.h"

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"

#include "iso7816.pio.h"

#include <stdio.h>

// Parity-error signal as an ordinary frame: release 1 ETU, drive low 2 ETU, release for the rest.  Started right after
// the parity bit was sampled (9.5 ETU) this drives the line low from ~10.5 to ~12.5 ETU, inside the window ISO 7816-3
// allows.
#define ERROR_SIGNAL_FRAME       0x6u // bits (LSB first): 0,1,1,0,0,...
#define ERROR_SIGNAL_LOW_END_ETU 3    // the pulse has ended after this many ETU

// A character popped from the RX FIFO later than this after its push (9.5 ETU) gets no error signal: the pulse would
// start after the reader's check at 11 +- 0.2 ETU and run into its next character.  In 1/10 ETU.
#define ERROR_SIGNAL_MAX_LATE_TENTHS 3

// Character repetition, card -> reader (ISO 7816-3 7.3, T=0): the reader answers a character it received with bad
// parity by driving I/O low from 10.5 ETU for 1..2 ETU; the card checks at 11 ETU and sends the character again at
// least 2 ETU later.  After this many repetitions of one character the card gives up.
#define TX_REPEAT_MAX 4

// Generous upper bound for "the clock stopped / reader went away".
#define TX_CHAR_TIMEOUT_US 200000

// Smallest ETU (in clock cycles) the PIO loops can follow; the system clock side of the same limit is
// PHY_MIN_SYSCLK_PER_CLK in phy.h.
#define PHY_MIN_ETU_CLKS    8
#define PHY_MAX_EXTRA_GUARD 20 // frame must fit in one 32-bit FIFO word

// ---- Pin checks ---------------------------------------------------------------------------------------------------
// The card contacts and the self-test pins are compile-time constants, so a bad configuration fails the build instead
// of misbehaving at run time.  Only GP0..GP29 are supported: the `wait gpio` patch, the 32-bit pin masks and the
// console's `gpio` command all assume a single 30-pin bank (the RP2350B's GP30..GP47 would need the PIO GPIO base).
#define PHY_PIN_VALID(p) ((p) >= 0 && (p) < 30)
#if !PHY_PIN_VALID(ISO7816_PIN_IO) || !PHY_PIN_VALID(ISO7816_PIN_CLK) || !PHY_PIN_VALID(ISO7816_PIN_RST)
#error "ISO7816_PIN_IO / ISO7816_PIN_CLK / ISO7816_PIN_RST must be GPIO numbers 0..29"
#endif
#if ISO7816_PIN_IO == ISO7816_PIN_CLK || ISO7816_PIN_IO == ISO7816_PIN_RST || ISO7816_PIN_CLK == ISO7816_PIN_RST
#error "ISO7816_PIN_IO, ISO7816_PIN_CLK and ISO7816_PIN_RST must be three different GPIOs"
#endif
#if ISO7816_SELFTEST_ENABLED
#if !PHY_PIN_VALID(ISO7816_SELFTEST_PIN_CLK) || !PHY_PIN_VALID(ISO7816_SELFTEST_PIN_IO)
#error "ISO7816_SELFTEST_PIN_CLK / ISO7816_SELFTEST_PIN_IO must be GPIO numbers 0..29 (or -1 to disable the self-test)"
#endif
#if ISO7816_SELFTEST_PIN_CLK == ISO7816_SELFTEST_PIN_IO
#error "ISO7816_SELFTEST_PIN_CLK and ISO7816_SELFTEST_PIN_IO must be different GPIOs"
#endif
#if ISO7816_SELFTEST_PIN_CLK == ISO7816_PIN_IO || ISO7816_SELFTEST_PIN_CLK == ISO7816_PIN_CLK ||                       \
    ISO7816_SELFTEST_PIN_CLK == ISO7816_PIN_RST || ISO7816_SELFTEST_PIN_IO == ISO7816_PIN_IO ||                        \
    ISO7816_SELFTEST_PIN_IO == ISO7816_PIN_CLK || ISO7816_SELFTEST_PIN_IO == ISO7816_PIN_RST
#error "the self-test pins must not be card contacts (ISO7816_PIN_*): the test drives a clock on them"
#endif
#endif

// The card state machines.  Fixed numbers so that the register accesses below stay constants; phy_init() claims both,
// so application code calling pio_claim_unused_sm(pio0, ...) gets SM2 / SM3 instead of reprogramming the receiver.
static PIO s_pio = pio0;
static const uint s_sm_rx = 0;
static const uint s_sm_tx = 1;
static uint s_off_rx, s_off_tx;
static pio_sm_config s_cfg_rx, s_cfg_tx;

static phy_params_t s_params = PHY_PARAMS_DEFAULT;
static uint32_t s_etu_clks = 372;
static uint s_frame_bits = 12;

static volatile bool s_rst_fell;
static bool s_in_reset = true;
static bool s_ignore_reset;               // self-test: RST is not driven by anyone
static absolute_time_t s_first_echo_time; // when phy_tx() saw its first echo
static bool s_first_echo_valid;
static absolute_time_t s_echo_times[2]; // first two echoes of the last phy_tx()
static absolute_time_t s_tx_start;
static absolute_time_t s_session_t0;

// Leading edge (approx.) of the last character received from the reader. ISO 7816-3 requires a minimum distance between
// the leading edges of two consecutive characters sent in opposite directions (16 ETU for T=0, the 22 ETU block guard
// time for T=1); the receiver pushes a character 9.5 ETU after its leading edge, so our reply waits for the remainder
// (+1 ETU margin).
static absolute_time_t s_last_rx_time;
static bool s_last_rx_valid;

// Last time rx_pop() (or phy_tx(), which drains its echoes) saw the RX FIFO empty: a word found afterwards was pushed
// at most that long ago, which bounds how late an error signal for it would be.
static absolute_time_t s_rx_empty_seen;
static bool s_rx_empty_seen_valid;

// End of the last frame we sent (start + 12 + N ETU, estimated from its echo); phy_apply_params() waits for it.
static absolute_time_t s_tx_end;
static bool s_tx_end_valid;

static uint32_t s_tx_index; // characters sent in this session (diagnostic parity injection)
static uint32_t s_bad_parity_index = UINT32_MAX;
static uint32_t s_bad_parity_count;

static phy_trace_t s_trace[PHY_TRACE_LEN];
static size_t s_trace_len;
static absolute_time_t s_trace_t0;

static phy_stats_t s_stats;
static void (*s_idle_hook)(void);
static uint s_io_gpio = ISO7816_PIN_IO; // the I/O line (a spare pin during the self-test)
static bool s_dbg_error_low_seen;       // the last error signal really drove the line low

static void trace_reset(void) {
    s_trace_len = 0;
    s_trace_t0 = get_absolute_time();
}

static void trace_add(char kind, uint16_t value) {
    if (s_trace_len < PHY_TRACE_LEN) {
        s_trace[s_trace_len].t_us = (uint32_t)absolute_time_diff_us(s_trace_t0, get_absolute_time());
        s_trace[s_trace_len].kind = (uint8_t)kind;
        s_trace[s_trace_len].value = value;
        s_trace_len++;
    }
}

const phy_trace_t *phy_trace(size_t *count) {
    *count = s_trace_len;
    return s_trace;
}

// Reader clock counter (`clkcount`), resident in pio1 because the card programs fill pio0.  `wait pin` reads the pad
// input, so any CLK GPIO works and the pin function stays with pio0.
static const PIO s_pio_cnt = pio1;
static uint s_sm_cnt, s_off_cnt;
static pio_sm_config s_cfg_cnt;

static void clk_counter_init(void) {
    s_off_cnt = pio_add_program(s_pio_cnt, &clkcount_program);
    s_sm_cnt = (uint)pio_claim_unused_sm(s_pio_cnt, true);
    s_cfg_cnt = clkcount_program_get_default_config(s_off_cnt);
    sm_config_set_in_pins(&s_cfg_cnt, ISO7816_PIN_CLK);
    sm_config_set_clkdiv_int_frac(&s_cfg_cnt, 1, 0);
}

// Count CLK rising edges for a short window: X starts at ~0 and the program decrements it per edge.
static uint32_t measure_clk_hz(uint32_t window_us) {
    pio_sm_init(s_pio_cnt, s_sm_cnt, s_off_cnt, &s_cfg_cnt); // disabled, FIFOs cleared, PC at `top`
    pio_sm_exec(s_pio_cnt, s_sm_cnt, pio_encode_mov_not(pio_x, pio_null));
    pio_sm_set_enabled(s_pio_cnt, s_sm_cnt, true);
    busy_wait_us(window_us);
    pio_sm_set_enabled(s_pio_cnt, s_sm_cnt, false);
    pio_sm_exec(s_pio_cnt, s_sm_cnt, pio_encode_mov(pio_isr, pio_x));
    pio_sm_exec(s_pio_cnt, s_sm_cnt, pio_encode_push(false, false));
    if (pio_sm_is_rx_fifo_empty(s_pio_cnt, s_sm_cnt)) return 0;
    uint32_t edges = ~pio_sm_get(s_pio_cnt, s_sm_cnt);
    return (uint32_t)((uint64_t)edges * 1000000ull / window_us);
}

// Rewrite every `wait gpio ISO7816_CLK_GPIO` of a loaded program to use another GPIO.  Instruction memory is
// write-only, so the original words come from the program's instruction array (WAIT has no address to relocate).
// WAIT encoding: 001 ddddd p ss iiiii  (ss = 00 -> GPIO source).
static void patch_wait_gpio(const pio_program_t *prog, uint offset, uint to_gpio) {
    for (uint i = 0; i < prog->length; i++) {
        uint16_t ins = prog->instructions[i];
        if ((ins & 0xE000u) == 0x2000u && ((ins >> 5) & 3u) == 0 && (ins & 0x1Fu) == ISO7816_CLK_GPIO) {
            s_pio->instr_mem[offset + i] = (ins & ~0x1Fu) | (to_gpio & 0x1Fu);
        }
    }
}

static void patch_clk_gpio(uint to_gpio) {
    patch_wait_gpio(&iso7816_rx_program, s_off_rx, to_gpio);
    patch_wait_gpio(&iso7816_tx_program, s_off_tx, to_gpio);
}

uint32_t phy_etu_us(void) {
    uint32_t hz = s_stats.clk_hz ? s_stats.clk_hz : 4000000u;
    uint32_t us = (uint32_t)((uint64_t)s_etu_clks * 1000000ull / hz);
    return us ? us : 1;
}

// Duration of `tenths` / 10 ETU in microseconds, without the rounding of phy_etu_us() (19.4 us at 372/4, 4.8 MHz).
static uint64_t etu_tenths_us(uint32_t tenths) {
    uint32_t hz = s_stats.clk_hz ? s_stats.clk_hz : 4000000u;
    return ((uint64_t)s_etu_clks * tenths * 100000ull + hz - 1) / hz;
}

// The same backwards: microseconds -> 1/10 ETU.
static uint32_t us_to_etu_tenths(int64_t us) {
    if (us <= 0) return 0;
    uint32_t hz = s_stats.clk_hz ? s_stats.clk_hz : 4000000u;
    uint64_t t = (uint64_t)us * hz / ((uint64_t)s_etu_clks * 100000ull);
    return t > 0xFFFF ? 0xFFFF : (uint32_t)t;
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

// Raw shared handler on IO_IRQ_BANK0: it only looks at (and acknowledges) the RST events, so other code can hook its
// own GPIOs with gpio_add_raw_irq_handler() or gpio_set_irq_callback() next to it.
static void rst_irq(void) {
    uint32_t events = gpio_get_irq_event_mask(ISO7816_PIN_RST) & (GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE);
    if (!events) return;
    gpio_acknowledge_irq(ISO7816_PIN_RST, events);
    if (events & GPIO_IRQ_EDGE_FALL) {
        s_rst_fell = true;
        s_stats.rst_falls++;
    }
    if (events & GPIO_IRQ_EDGE_RISE) {
        s_stats.rst_rises++;
    }
}

void phy_sample_pins(phy_pins_t *p) {
    p->rst = gpio_get(ISO7816_PIN_RST);
    p->io = gpio_get(ISO7816_PIN_IO);
    bool last = gpio_get(ISO7816_PIN_CLK);
    p->clk = last;
    p->clk_toggles = 0;
    absolute_time_t end = make_timeout_time_us(2000);
    while (!time_reached(end)) {
        bool now = gpio_get(ISO7816_PIN_CLK);
        if (now != last) {
            p->clk_toggles++;
            last = now;
        }
    }
}

// Load a 30-bit constant into the ISR of a (disabled) state machine using only exec'd instructions.  Requires IN shift
// direction = right.
static void sm_load_isr(PIO pio, uint sm, uint32_t value) {
    pio_sm_exec(pio, sm, pio_encode_mov(pio_isr, pio_null));
    for (int i = 0; i < 6; i++) {
        pio_sm_exec(pio, sm, pio_encode_set(pio_x, (value >> (5 * i)) & 0x1f));
        pio_sm_exec(pio, sm, pio_encode_in(pio_x, 5));
    }
    pio_sm_exec(pio, sm, pio_encode_in(pio_null, 2));
}

static void tx_set_frame_bits(uint bits) {
    hw_write_masked(&s_pio->sm[s_sm_tx].shiftctrl, (bits & 31u) << PIO_SM0_SHIFTCTRL_PULL_THRESH_LSB,
                    PIO_SM0_SHIFTCTRL_PULL_THRESH_BITS);
}

static inline bool even_parity(uint8_t b) {
    b ^= b >> 4;
    b ^= b >> 2;
    b ^= b >> 1;
    return b & 1;
}

// Build the open-drain frame for one character (bit0 first): start (drive low), 8 data bits, parity, guard bits
// released.  Frame bit 1 = drive low.
//   direct : logic 1 = line high, LSB first      -> data bits = ~b
//   inverse: logic 1 = line low,  MSB first      -> data bits = reverse(b)
static inline uint32_t make_frame(uint8_t b) {
    uint32_t frame = 1u;     // start bit
    bool p = even_parity(b); // parity bit (logic value)
    if (!s_params.inverse) {
        frame |= (uint32_t)(~b & 0xFF) << 1;
        frame |= (uint32_t)(p ? 0 : 1) << 9;
    } else {
        for (int i = 0; i < 8; i++) {
            frame |= (uint32_t)((b >> (7 - i)) & 1) << (1 + i);
        }
        frame |= (uint32_t)(p ? 1 : 0) << 9;
    }
    return frame; // remaining bits = 0 (released)
}

// Decode a raw 9-bit RX word (bit k = line state of the k-th bit received).
static inline uint8_t decode_byte(uint32_t w) {
    if (!s_params.inverse) return (uint8_t)(w & 0xFF);
    uint8_t b = 0;
    for (int i = 0; i < 8; i++) {
        if (!((w >> i) & 1)) b |= (uint8_t)(1u << (7 - i));
    }
    return b;
}

// The receiver ends every character with `push block`: with the 8-deep FIFO full it stalls there and does not watch
// the line, so a character the reader sends meanwhile is lost or mis-framed and shows up only later as a parity / EDC
// error or a short block.  FDEBUG.RXSTALL latches such a stall (write 1 to clear); count it.
#define RX_STALL_BIT (1u << (PIO_FDEBUG_RXSTALL_LSB + s_sm_rx))

static inline void rx_check_overrun(void) {
    if (s_pio->fdebug & RX_STALL_BIT) {
        s_pio->fdebug = RX_STALL_BIT;
        s_stats.rx_overruns++;
    }
}

static inline bool parity_ok(uint32_t w, uint8_t b) {
    bool p_line = (w >> 8) & 1;
    bool p_logic = s_params.inverse ? !p_line : p_line;
    return p_logic == even_parity(b);
}

static void rx_restart(void) {
    pio_sm_set_enabled(s_pio, s_sm_rx, false);
    pio_sm_clear_fifos(s_pio, s_sm_rx);
    pio_sm_restart(s_pio, s_sm_rx);
    pio_sm_exec(s_pio, s_sm_rx, pio_encode_jmp(s_off_rx));
    pio_sm_set_enabled(s_pio, s_sm_rx, true);
    s_rx_empty_seen = get_absolute_time();
    s_rx_empty_seen_valid = true;
}

// True while the receiver waits for a start bit, i.e. it has not seen a falling edge since its last push (the first
// two instructions of iso7816_rx: `wait 1 pin`, `wait 0 pin`).
static inline bool rx_idle(void) {
    uint pc = pio_sm_get_pc(s_pio, s_sm_rx);
    return pc == s_off_rx || pc == s_off_rx + 1;
}

// Reader sent a character with bad parity: emit the error signal so that it repeats the character.  RX is paused
// meanwhile so the pulse is not seen as a start bit, and restarted before the repetition can begin.
static void send_error_signal(void) {
    pio_sm_set_enabled(s_pio, s_sm_rx, false);
    pio_sm_put(s_pio, s_sm_tx, ERROR_SIGNAL_FRAME);
    // The pulse is over after ERROR_SIGNAL_LOW_END_ETU; watch the line meanwhile so that the self-test can verify
    // that the low level really appeared.
    absolute_time_t until = make_timeout_time_us(phy_etu_us() * ERROR_SIGNAL_LOW_END_ETU + phy_etu_us() / 4);
    bool low = false;
    while (!time_reached(until)) {
        if (!gpio_get(s_io_gpio)) low = true;
    }
    s_dbg_error_low_seen = low;
    rx_restart();
}

// Bring both state machines up with the current parameters.  The TX OSR is primed with a full frame of zeros
// ("release") so that the state machine idles for exactly one frame length (>= 12 ETU) before pulling the first real
// frame from the FIFO.  (Marking the OSR empty with a forced `out null, 32` instead leaves that instruction stalled on
// autopull and it swallows the first word pushed later.)
static void sm_configure(void) {
    s_etu_clks = (uint32_t)s_params.f / s_params.d;
    s_frame_bits = 12 + s_params.extra_guard;

    pio_sm_init(s_pio, s_sm_rx, s_off_rx, &s_cfg_rx);
    sm_load_isr(s_pio, s_sm_rx, s_etu_clks - 1);
    pio_sm_exec(s_pio, s_sm_rx, pio_encode_mov(pio_osr, pio_isr));
    pio_sm_exec(s_pio, s_sm_rx, pio_encode_mov(pio_isr, pio_null));

    pio_sm_init(s_pio, s_sm_tx, s_off_tx, &s_cfg_tx);
    sm_load_isr(s_pio, s_sm_tx, s_etu_clks - 1);
    pio_sm_exec(s_pio, s_sm_tx, pio_encode_mov(pio_y, pio_isr));
    pio_sm_exec(s_pio, s_sm_tx, pio_encode_mov(pio_isr, pio_null));
    tx_set_frame_bits(s_frame_bits);
    pio_sm_exec(s_pio, s_sm_tx, pio_encode_mov(pio_osr, pio_null));

    s_pio->fdebug = RX_STALL_BIT; // a stall from before this configuration is not an overrun of this session
    pio_sm_set_enabled(s_pio, s_sm_tx, true);
    pio_sm_set_enabled(s_pio, s_sm_rx, true);
    s_rx_empty_seen = get_absolute_time();
    s_rx_empty_seen_valid = true;
    s_tx_end_valid = false;
}

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------

void phy_init(void) {
    // Reserve the card state machines before anything else can claim them (panics if the application already did).
    pio_sm_claim(s_pio, s_sm_rx);
    pio_sm_claim(s_pio, s_sm_tx);

    // RST: plain input, pulled down so that "no reader" reads as reset.
    gpio_init(ISO7816_PIN_RST);
    gpio_set_dir(ISO7816_PIN_RST, GPIO_IN);
    gpio_pull_down(ISO7816_PIN_RST);
    // Edge latch / counters through a raw shared handler: the SDK's single GPIO callback stays free for user code.
    gpio_add_raw_irq_handler(ISO7816_PIN_RST, rst_irq);
    gpio_set_irq_enabled(ISO7816_PIN_RST, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE, true);
    irq_set_enabled(IO_IRQ_BANK0, true);

    // I/O: PIO-owned, open drain (output value 0, direction toggled), pulled up.
    pio_sm_set_pins_with_mask(s_pio, s_sm_tx, 0, 1u << ISO7816_PIN_IO);
    pio_sm_set_consecutive_pindirs(s_pio, s_sm_tx, ISO7816_PIN_IO, 1, false);
    pio_gpio_init(s_pio, ISO7816_PIN_IO);
    gpio_pull_up(ISO7816_PIN_IO);

    // CLK: PIO-owned input, only ever observed with `wait gpio`.
    pio_sm_set_consecutive_pindirs(s_pio, s_sm_tx, ISO7816_PIN_CLK, 1, false);
    pio_gpio_init(s_pio, ISO7816_PIN_CLK);
    gpio_pull_down(ISO7816_PIN_CLK);

    s_off_rx = pio_add_program(s_pio, &iso7816_rx_program);
    s_off_tx = pio_add_program(s_pio, &iso7816_tx_program);
    patch_clk_gpio(ISO7816_PIN_CLK);
    clk_counter_init();
    s_stats.sys_clk_hz = clock_get_hz(clk_sys); // the state machines run undivided

    s_cfg_rx = iso7816_rx_program_get_default_config(s_off_rx);
    sm_config_set_in_pins(&s_cfg_rx, ISO7816_PIN_IO);
    sm_config_set_in_shift(&s_cfg_rx, true, false, 32);   // right, no autopush
    sm_config_set_fifo_join(&s_cfg_rx, PIO_FIFO_JOIN_RX); // 8-deep RX FIFO
    sm_config_set_clkdiv_int_frac(&s_cfg_rx, 1, 0);

    s_cfg_tx = iso7816_tx_program_get_default_config(s_off_tx);
    sm_config_set_out_pins(&s_cfg_tx, ISO7816_PIN_IO, 1);
    sm_config_set_out_shift(&s_cfg_tx, true, true, 12);   // right, autopull (threshold set later)
    sm_config_set_in_shift(&s_cfg_tx, true, false, 32);   // only for sm_load_isr()
    sm_config_set_fifo_join(&s_cfg_tx, PIO_FIFO_JOIN_TX); // 8-deep TX FIFO
    sm_config_set_clkdiv_int_frac(&s_cfg_tx, 1, 0);

    phy_enter_reset();
}

void phy_set_idle_hook(void (*hook)(void)) { s_idle_hook = hook; }

bool phy_rst_high(void) { return gpio_get(ISO7816_PIN_RST); }

bool phy_reset_pending(void) {
    if (s_ignore_reset) return false;
    return s_rst_fell || !gpio_get(ISO7816_PIN_RST);
}

bool phy_in_reset(void) { return s_in_reset; }

void phy_enter_reset(void) {
    pio_sm_set_enabled(s_pio, s_sm_rx, false);
    pio_sm_set_enabled(s_pio, s_sm_tx, false);
    pio_sm_set_consecutive_pindirs(s_pio, s_sm_tx, ISO7816_PIN_IO, 1, false); // release I/O
    pio_sm_clear_fifos(s_pio, s_sm_rx);
    pio_sm_clear_fifos(s_pio, s_sm_tx);
    s_rst_fell = false;
    s_in_reset = true;
}

void phy_release_io(void) {
    pio_sm_set_enabled(s_pio, s_sm_tx, false);
    pio_sm_set_consecutive_pindirs(s_pio, s_sm_tx, ISO7816_PIN_IO, 1, false);
}

bool phy_params_supported(uint16_t f, uint8_t d) {
    if (!f || !d || f % d) return false;
    return f / d >= PHY_MIN_ETU_CLKS;
}

static bool params_valid(const phy_params_t *p) {
    return phy_params_supported(p->f, p->d) && p->extra_guard <= PHY_MAX_EXTRA_GUARD;
}

const phy_params_t *phy_params(void) { return &s_params; }

bool phy_start_session(const phy_params_t *p) {
    if (!params_valid(p)) return false;
    s_params = *p;
    s_rst_fell = false;
    s_stats.atr_attempts++;
    s_last_rx_valid = false;
    s_first_echo_valid = false;
    trace_reset();
    s_tx_index = 0;
    if (!s_ignore_reset) {
        // Diagnostic: hardware count of the reader's clock (0.5 ms window, well inside the 400..40000 clock budget
        // before the ATR has to start).
        s_stats.dbg_clk_counted_hz = measure_clk_hz(500);
        if (s_stats.dbg_clk_counted_hz > 100000) s_stats.clk_hz = s_stats.dbg_clk_counted_hz;
    }

    // ISO 7816-3: the ATR must start between 400 and 40 000 clock cycles after RST rises; the primed frame of >= 12 ETU
    // = 4464 cycles of released line takes care of that, and the ATR characters follow directly behind.
    s_session_t0 = get_absolute_time();
    sm_configure();
    s_in_reset = false;
    s_stats.sessions++;
    s_stats.last_error = NULL;
    return !phy_reset_pending();
}

// Stays armed until it has fired: in the current session if that character is still to come, else in the next one
// (the reader may well reset the card before the session it is meant for).
void phy_debug_bad_parity(uint32_t index, uint32_t count) {
    s_bad_parity_index = count ? index : UINT32_MAX;
    s_bad_parity_count = count;
}

// Wait until the last frame we sent is over.  phy_tx() returns once the echo of its last character is in (9.5 ETU into
// the frame; with error_signal after the check at 12 ETU), and stopping the TX state machine earlier freezes the line
// as it is: a parity bit of 0 would stay driven low until the reconfigured machine releases it.
static void tx_wait_frame_end(void) {
    if (!s_tx_end_valid) return;
    while (!time_reached(s_tx_end)) {}
    s_tx_end_valid = false;
}

bool phy_apply_params(const phy_params_t *p) {
    if (!params_valid(p)) return false;
    tx_wait_frame_end();
    if ((uint32_t)p->f / p->d == s_etu_clks && 12u + p->extra_guard == s_frame_bits) {
        // Same timing: protocol, convention and error signal are only used by the CPU, the state machines run on.
        s_params = *p;
        return true;
    }
    pio_sm_set_enabled(s_pio, s_sm_rx, false);
    pio_sm_set_enabled(s_pio, s_sm_tx, false);
    pio_sm_clear_fifos(s_pio, s_sm_rx);
    pio_sm_clear_fifos(s_pio, s_sm_tx);
    s_params = *p;
    s_last_rx_valid = false;
    sm_configure();
    return true;
}

// Pop one raw RX word and validate parity. Returns:
//   1  valid byte
//   0  nothing available
//  -1  parity error (error signal already sent, unless the word was picked up too late for it)
//
// The error signal is timed from the pop, so it is only on time if the word is picked up right after its push.  The
// push is somewhere between the last time the FIFO was seen empty and now; if that window (or a second word behind
// this one) says the word may have waited longer than ERROR_SIGNAL_MAX_LATE_TENTHS, the error signal is skipped: the
// character is dropped either way and a late pulse would hit the reader's next character instead.
static int rx_pop(uint8_t *byte) {
    rx_check_overrun();
    uint level = pio_sm_get_rx_fifo_level(s_pio, s_sm_rx);
    absolute_time_t now = get_absolute_time();
    if (!level) {
        s_rx_empty_seen = now;
        s_rx_empty_seen_valid = true;
        return 0;
    }
    uint32_t w = pio_sm_get(s_pio, s_sm_rx);
    uint8_t b = decode_byte(w);
    int64_t waited = s_rx_empty_seen_valid ? absolute_time_diff_us(s_rx_empty_seen, now) : INT32_MAX;
    if (level > 1) waited = INT32_MAX; // the one behind arrived >= 12 ETU after this one
    if (waited < INT32_MAX && (uint64_t)waited > s_stats.rx_max_latency_us) {
        s_stats.rx_max_latency_us = (uint32_t)waited;
    }
    bool late = (uint64_t)waited > etu_tenths_us(ERROR_SIGNAL_MAX_LATE_TENTHS);
    if (late) s_stats.rx_late++;
    trace_add('R', (uint16_t)(w & 0x1FF));
    s_last_rx_time = now;
    s_last_rx_valid = true;
    if (!parity_ok(w, b)) {
        s_stats.rx_parity_errors++;
        if (s_params.error_signal && !late) send_error_signal();
        return -1;
    }
    *byte = b;
    return 1;
}

bool phy_rx(uint8_t *byte, bool idle) {
    for (;;) {
        watchdog_update(); // waiting for the reader is legitimate, not a hang
        int r = rx_pop(byte);
        if (r == 1) {
            s_stats.rx_chars++;
            return true;
        }
        if (phy_reset_pending()) return false;
        if (r == 0 && idle && s_idle_hook) s_idle_hook();
    }
}

int phy_rx_timeout(uint8_t *byte, uint32_t timeout_us) {
    absolute_time_t deadline = make_timeout_time_us(timeout_us);
    for (;;) {
        watchdog_update();
        int r = rx_pop(byte);
        if (r == 1) {
            s_stats.rx_chars++;
            return 1;
        }
        if (phy_reset_pending()) return -1;
        if (time_reached(deadline)) return 0;
    }
}

// Frame for the next character, with the diagnostic parity flip applied (phy_debug_bad_parity()).
static uint32_t next_frame(uint8_t b) {
    uint32_t frame = make_frame(b);
    if (s_tx_index == s_bad_parity_index && s_bad_parity_count) {
        s_bad_parity_count--;
        frame ^= 1u << 9;
        trace_add('P', b);
    } else {
        trace_add('T', b);
    }
    return frame;
}

// Our own characters come back through the receiver; use them as completion signal and as a check that nobody fought
// us for the line.  `frame` is what was sent (a deliberately wrong parity bit is not an echo error).
static void tx_echo(uint32_t w, size_t index, uint8_t b, uint32_t frame, bool *mismatch_logged) {
    absolute_time_t now = get_absolute_time();
    trace_add('R', (uint16_t)(w & 0x1FF));
    if (!s_first_echo_valid) {
        s_first_echo_time = now;
        s_first_echo_valid = true;
        // First ATR character: echoed (frame + 9.5) ETU after the session start, a decent estimate of the clock if the
        // clock counter could not be used.
        int64_t dt = absolute_time_diff_us(s_session_t0, s_first_echo_time);
        s_stats.dbg_delay_us = (uint32_t)dt;
        if (dt > 0 && !s_stats.dbg_clk_counted_hz) {
            s_stats.clk_hz = (uint32_t)(((2ull * s_frame_bits + 19) * s_etu_clks / 2) * 1000000ull / (uint64_t)dt);
        }
    }
    if (index < 2) s_echo_times[index] = now;
    // The echo pushes 9.5 ETU into the frame; the frame ends 12 + N ETU after its start.
    s_tx_end = delayed_by_us(now, etu_tenths_us(s_frame_bits * 10 - 95));
    s_tx_end_valid = true;
    if ((w & 0x1FF) != (~frame >> 1 & 0x1FF)) { // frame bit 1 = line low
        s_stats.tx_echo_errors++;
        if (!*mismatch_logged) {
            *mismatch_logged = true;
            s_stats.dbg_echo_index = index;
            s_stats.dbg_echo_expected = b;
            s_stats.dbg_echo_got = w & 0x1FF;
        }
    }
}

static bool tx_abort(const char *why) {
    s_stats.last_error = why;
    s_bad_parity_count = 0; // a diagnostic injection does not outlive the transfer it broke
    return false;
}

// T=1 (no error signal): stream the characters through the TX FIFO, back to back.
static bool tx_stream(const uint8_t *data, size_t len) {
    size_t pushed = 0, echoed = 0;
    bool mismatch_logged = false;
    uint32_t frames[8]; // the TX FIFO is 8 deep, so at most 8 frames are in flight
    absolute_time_t deadline = make_timeout_time_us(TX_CHAR_TIMEOUT_US);

    while (echoed < len) {
        watchdog_update();
        bool progressed = false;
        while (pushed < len && !pio_sm_is_tx_fifo_full(s_pio, s_sm_tx) && pushed - echoed < 8) {
            frames[pushed % 8] = next_frame(data[pushed]);
            pio_sm_put(s_pio, s_sm_tx, frames[pushed % 8]);
            s_tx_index++;
            pushed++;
            progressed = true;
        }
        while (echoed < pushed && !pio_sm_is_rx_fifo_empty(s_pio, s_sm_rx)) {
            tx_echo(pio_sm_get(s_pio, s_sm_rx), echoed, data[echoed], frames[echoed % 8], &mismatch_logged);
            echoed++;
            progressed = true;
        }
        if (progressed) {
            deadline = make_timeout_time_us(TX_CHAR_TIMEOUT_US);
        } else if (time_reached(deadline)) {
            s_stats.tx_timeouts++;
            return tx_abort("tx: no echo (clock stopped?)");
        }
        if (phy_reset_pending()) return tx_abort("tx: reset asserted");
    }
    if (mismatch_logged) {
        // Somebody else drove the line: a stray word may still follow and would pass for the reader's next byte.
        tx_wait_frame_end();
        rx_restart();
    }
    return true;
}

// T=0: one character at a time, each checked for the reader's error signal and repeated if it came.
//
// After the echo (9.5 ETU) the line is watched until 12 ETU: the error signal starts at 10.5 +- 0.2 ETU and lasts at
// least 1 ETU.  The CPU's polling gives its timing for the trace, the receiver makes the check independent of the
// polling: it waits for a start bit after the echo, and a falling edge in the guard time moves it out of that wait
// for the next 9.5 ETU (until it would push a bogus word), so a check of its program counter anywhere in that time
// catches the signal even if an interrupt delayed the CPU.  The receiver is then restarted, which drops the half-read
// bogus character, and the character goes out again at least 2 ETU after the line came back high.
static bool tx_repeating(const uint8_t *data, size_t len) {
    bool mismatch_logged = false;
    for (size_t i = 0; i < len; i++) {
        for (unsigned attempt = 0;; attempt++) {
            uint32_t frame = next_frame(data[i]);
            pio_sm_put(s_pio, s_sm_tx, frame); // the TX FIFO is empty: every frame waits for its echo

            absolute_time_t deadline = make_timeout_time_us(TX_CHAR_TIMEOUT_US);
            while (pio_sm_is_rx_fifo_empty(s_pio, s_sm_rx)) {
                watchdog_update();
                if (phy_reset_pending()) return tx_abort("tx: reset asserted");
                if (time_reached(deadline)) {
                    s_stats.tx_timeouts++;
                    return tx_abort("tx: no echo (clock stopped?)");
                }
            }
            tx_echo(pio_sm_get(s_pio, s_sm_rx), i, data[i], frame, &mismatch_logged);
            absolute_time_t echo = get_absolute_time();

            // Watch 10.1 .. 12 ETU (our own parity bit may hold the line low until 10).
            absolute_time_t from = delayed_by_us(echo, etu_tenths_us(6));
            absolute_time_t until = delayed_by_us(echo, etu_tenths_us(25));
            absolute_time_t low_at = nil_time, high_at = nil_time;
            while (!time_reached(until)) {
                bool high = gpio_get(s_io_gpio);
                if (is_nil_time(low_at)) {
                    if (!high && time_reached(from)) low_at = get_absolute_time();
                } else if (high && is_nil_time(high_at)) {
                    high_at = get_absolute_time();
                }
            }
            if (rx_idle() && pio_sm_is_rx_fifo_empty(s_pio, s_sm_rx)) break; // accepted

            // Error signal (or something else pulled the line low after our character).
            if (is_nil_time(high_at)) {
                absolute_time_t stuck = delayed_by_us(get_absolute_time(), etu_tenths_us(100));
                while (!gpio_get(s_io_gpio)) {
                    if (phy_reset_pending()) return tx_abort("tx: reset asserted");
                    if (time_reached(stuck)) return tx_abort("tx: I/O held low after our character");
                }
                high_at = get_absolute_time();
            }
            trace_add('E',
                      is_nil_time(low_at) ? 0 : (uint16_t)(95 + us_to_etu_tenths(absolute_time_diff_us(echo, low_at))));
            trace_add('e', (uint16_t)(95 + us_to_etu_tenths(absolute_time_diff_us(echo, high_at))));
            // Repeat at least 2 ETU after the line came back high (and so after the detection).
            busy_wait_until(delayed_by_us(high_at, etu_tenths_us(20)));
            rx_restart();
            if (attempt >= TX_REPEAT_MAX) return tx_abort("tx: character rejected by the reader (error signal)");
            s_stats.tx_repeats++;
        }
        s_tx_index++;
    }
    return true;
}

bool phy_tx(const uint8_t *data, size_t len) {
    // Character turnaround: do not start before `turnaround_etu` (+1 margin) after the leading edge of the last
    // character the reader sent (its push came at 9.5 ETU).
    if (s_last_rx_valid) {
        uint32_t wait_tenths = (uint32_t)s_params.turnaround_etu * 10 + 10;
        wait_tenths = wait_tenths > 95 ? wait_tenths - 95 : 0;
        absolute_time_t earliest = delayed_by_us(s_last_rx_time, etu_tenths_us(wait_tenths));
        while (!time_reached(earliest)) {
            if (phy_reset_pending()) return tx_abort("tx: reset asserted");
        }
    }

    s_tx_start = get_absolute_time();
    bool ok = s_params.error_signal ? tx_repeating(data, len) : tx_stream(data, len);
    if (!ok) return false;
    s_stats.tx_chars += len;
    s_last_rx_valid = false; // next turnaround is reader -> card, no wait needed
    // All echoes are in, so the FIFO is empty now: the next rx_pop() measures its latency from here.
    s_rx_empty_seen = get_absolute_time();
    s_rx_empty_seen_valid = true;
    return true;
}

const phy_stats_t *phy_stats(void) { return &s_stats; }

// ---------------------------------------------------------------------------
// Loopback self-test: the whole ATR path (constants, frame delay, TX frames, RX echo) driven by a Pico-generated clock
// on a spare pin (ISO7816_SELFTEST_PIN_CLK), with I/O looped on another spare pin (ISO7816_SELFTEST_PIN_IO).  Nothing
// on the card contacts is touched; both spare pins are left unassigned (GPIO_FUNC_NULL) afterwards.
// ---------------------------------------------------------------------------
#define SELFTEST_CLK_PIN ISO7816_SELFTEST_PIN_CLK
#define SELFTEST_IO_PIN  ISO7816_SELFTEST_PIN_IO
#define SELFTEST_CLK_DIV 32 // clkgen: system clock cycles per test clock period

#if !ISO7816_SELFTEST_ENABLED
void phy_selftest(char *msg, size_t cap) {
    snprintf(msg, cap, "selftest: disabled (ISO7816_SELFTEST_PIN_CLK / _IO = -1)");
}
#else
void phy_selftest(char *msg, size_t cap) {
    static const uint8_t atr[] = { 0x3B, 0x08, 'P', 'i', 'c', 'o', '7', '8', '1', '6' };
    const uint32_t selftest_clk_hz = clock_get_hz(clk_sys) / SELFTEST_CLK_DIV; // 3.9 MHz at 125 MHz
    PIO pio_clk = pio1;
    const uint sm_clk = (uint)pio_claim_unused_sm(pio_clk, true); // the clock counter holds another one

    // Test clock generator on the other PIO block.
    uint off_clk = pio_add_program(pio_clk, &clkgen_program);
    pio_sm_config cc = clkgen_program_get_default_config(off_clk);
    sm_config_set_set_pins(&cc, SELFTEST_CLK_PIN, 1);
    pio_sm_set_consecutive_pindirs(pio_clk, sm_clk, SELFTEST_CLK_PIN, 1, true);
    pio_gpio_init(pio_clk, SELFTEST_CLK_PIN);
    pio_sm_init(pio_clk, sm_clk, off_clk, &cc);
    pio_sm_set_enabled(pio_clk, sm_clk, true);

    // Loopback I/O pin: open drain exactly like the real one.
    pio_sm_set_pins_with_mask(s_pio, s_sm_tx, 0, 1u << SELFTEST_IO_PIN);
    pio_sm_set_consecutive_pindirs(s_pio, s_sm_tx, SELFTEST_IO_PIN, 1, false);
    pio_gpio_init(s_pio, SELFTEST_IO_PIN);
    gpio_pull_up(SELFTEST_IO_PIN);
    s_io_gpio = SELFTEST_IO_PIN;

    // Point both state machines at the test pins.
    pio_sm_config saved_rx = s_cfg_rx, saved_tx = s_cfg_tx;
    sm_config_set_in_pins(&s_cfg_rx, SELFTEST_IO_PIN);
    sm_config_set_out_pins(&s_cfg_tx, SELFTEST_IO_PIN, 1);
    patch_clk_gpio(SELFTEST_CLK_PIN);

    phy_stats_t saved_stats = s_stats;
    s_stats = (phy_stats_t){ .sys_clk_hz = saved_stats.sys_clk_hz };
    s_ignore_reset = true;

    phy_params_t p = PHY_PARAMS_DEFAULT;
    bool ok = phy_start_session(&p) && phy_tx(atr, sizeof atr);
    uint32_t clk_from_echo = s_stats.clk_hz;

    // Pure timing probe: two more characters streamed (T=1 style, no error signal check) after the ATR, whose
    // character-by-character T=0 transmission returns once the last frame is over.  push -> 1st echo = 9.5 ETU, 1st ->
    // 2nd echo = 12 ETU: both measure the ETU.
    static const uint8_t probe_bytes[2] = { 0x55, 0xAA };
    int64_t rx_etu = -1, tx_etu = -1;
    phy_params_t stream = p;
    stream.error_signal = false;
    if (ok && phy_apply_params(&stream) && phy_tx(probe_bytes, 2)) {
        rx_etu = absolute_time_diff_us(s_tx_start, s_echo_times[0]) * 2 / 19;
        tx_etu = absolute_time_diff_us(s_echo_times[0], s_echo_times[1]) / 12;
    }

    // Odd ETU (F=372, D=4 -> 93 clocks) and inverse convention at the same time: exercises the edge-counting receiver
    // and the bit mapping.
    bool fast_ok = false;
    int64_t fast_etu = -1;
    uint32_t fast_errors = 0;
    if (ok) {
        phy_params_t q = PHY_PARAMS_DEFAULT;
        q.d = 4;
        q.inverse = true;
        s_stats.tx_echo_errors = 0;
        static const uint8_t probe2[4] = { 0x3F, 0x5A, 0x01, 0xFE };
        fast_ok = phy_apply_params(&q) && phy_tx(probe2, 4);
        fast_errors = s_stats.tx_echo_errors;
        if (fast_ok) fast_etu = absolute_time_diff_us(s_echo_times[0], s_echo_times[1]) / 12;
    }
    // Parity error path: a frame with a wrong parity bit must be rejected (rx_pop() returns -1), answered with the
    // error signal (line driven low around 10.5..12.5 ETU) and dropped.
    bool perr_ok = false, perr_low = false;
    uint32_t perr_count = s_stats.rx_parity_errors;
    if (ok) {
        phy_params_t q = PHY_PARAMS_DEFAULT;
        phy_apply_params(&q);
        pio_sm_put(s_pio, s_sm_tx, make_frame(0x5A) ^ (1u << 9)); // flipped parity bit
        uint8_t b;
        int r = 0;
        absolute_time_t deadline = make_timeout_time_ms(50);
        while ((r = rx_pop(&b)) == 0 && !time_reached(deadline)) {}
        perr_ok = r == -1 && s_stats.rx_parity_errors == perr_count + 1;
        perr_low = s_dbg_error_low_seen;
        // Nothing may be left over for the next session.
        perr_ok = perr_ok && phy_rx_timeout(&b, 2000) == 0;
    }
    s_ignore_reset = false;

    snprintf(msg, cap,
             "selftest(loopback %s): first echo after %lu us (expect %lu), CLK %lu Hz (expect %lu), "
             "echo err %lu, mismatch idx %lu exp %02lX got 0x%03lX, err=%s | ETU rx %lld us tx %lld us "
             "(expect %lu) | F/D=372/4 inverse: %s, ETU %lld us (expect %lu), echo err %lu | "
             "parity error: %s, line low: %s",
             ok ? "OK" : "FAILED", (unsigned long)s_stats.dbg_delay_us,
             (unsigned long)((12ull * 2 + 19) * 372 / 2 * 1000000ull / selftest_clk_hz), (unsigned long)clk_from_echo,
             (unsigned long)selftest_clk_hz, (unsigned long)s_stats.tx_echo_errors,
             (unsigned long)s_stats.dbg_echo_index, (unsigned long)s_stats.dbg_echo_expected,
             (unsigned long)s_stats.dbg_echo_got, s_stats.last_error ? s_stats.last_error : "-", (long long)rx_etu,
             (long long)tx_etu, (unsigned long)(372ull * 1000000ull / selftest_clk_hz), fast_ok ? "OK" : "FAILED",
             (long long)fast_etu, (unsigned long)(93ull * 1000000ull / selftest_clk_hz), (unsigned long)fast_errors,
             perr_ok ? "detected" : "MISSED", perr_low ? "yes" : "NO");

    // Put everything back.
    phy_enter_reset();
    s_stats = saved_stats;
    s_cfg_rx = saved_rx;
    s_cfg_tx = saved_tx;
    s_params = (phy_params_t)PHY_PARAMS_DEFAULT;
    s_io_gpio = ISO7816_PIN_IO;
    patch_clk_gpio(ISO7816_PIN_CLK);
    pio_sm_set_enabled(pio_clk, sm_clk, false);
    pio_remove_program(pio_clk, &clkgen_program, off_clk);
    pio_sm_unclaim(pio_clk, sm_clk);
    gpio_set_function(SELFTEST_CLK_PIN, GPIO_FUNC_NULL);
    gpio_set_function(SELFTEST_IO_PIN, GPIO_FUNC_NULL);
}
#endif
