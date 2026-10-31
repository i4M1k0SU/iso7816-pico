#include "phy.h"

#include "config.h"

#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/pwm.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"

#include "iso7816.pio.h"

#include <stdio.h>

// Parity-error signal as an ordinary frame: release 1 ETU, drive low 2 ETU, release for the rest.  Started right after
// the parity bit was sampled (9.5 ETU) this drives the line low from ~10.5 to ~12.5 ETU, inside the window ISO 7816-3
// allows.
#define ERROR_SIGNAL_FRAME  0x6u   // bits (LSB first): 0,1,1,0,0,...
#define ERROR_SIGNAL_LOW_END_ETU 3 // the pulse has ended after this many ETU

// Generous upper bound for "the clock stopped / reader went away".
#define TX_CHAR_TIMEOUT_US  200000

// Smallest ETU the PIO loops can follow comfortably at 125 MHz sysclk.
#define PHY_MIN_ETU_CLKS    8
#define PHY_MAX_EXTRA_GUARD 20     // frame must fit in one 32-bit FIFO word

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
static bool s_ignore_reset;        // self-test: RST is not driven by anyone
static absolute_time_t s_first_echo_time;  // when phy_tx() saw its first echo
static bool s_first_echo_valid;
static absolute_time_t s_echo_times[2];    // first two echoes of the last phy_tx()
static absolute_time_t s_tx_start;
static absolute_time_t s_session_t0;

// Leading edge (approx.) of the last character received from the reader. ISO 7816-3 requires a minimum distance between
// the leading edges of two consecutive characters sent in opposite directions (16 ETU for T=0, the 22 ETU block guard
// time for T=1); the receiver pushes a character 9.5 ETU after its leading edge, so our reply waits for the remainder
// (+1 ETU margin).
static absolute_time_t s_last_rx_time;
static bool s_last_rx_valid;

static phy_trace_t s_trace[PHY_TRACE_LEN];
static size_t s_trace_len;
static absolute_time_t s_trace_t0;

static phy_stats_t s_stats;
static void (*s_idle_hook)(void);

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

// Count CLK rising edges with the PWM slice for a short window.  Only odd GPIOs are B inputs of a PWM slice, so this is
// a no-op for even CLK pins.
static uint32_t measure_clk_pwm_hz(uint32_t window_us) {
    if (!(ISO7816_PIN_CLK & 1)) return 0;
    uint slice = pwm_gpio_to_slice_num(ISO7816_PIN_CLK);
    pwm_config c = pwm_get_default_config();
    pwm_config_set_clkdiv_mode(&c, PWM_DIV_B_RISING);
    pwm_config_set_clkdiv_int(&c, 1);
    pwm_config_set_wrap(&c, 0xFFFF);
    gpio_set_function(ISO7816_PIN_CLK, GPIO_FUNC_PWM);
    pwm_init(slice, &c, false);
    pwm_set_counter(slice, 0);
    pwm_set_enabled(slice, true);
    busy_wait_us(window_us);
    pwm_set_enabled(slice, false);
    uint32_t count = pwm_get_counter(slice);
    gpio_set_function(ISO7816_PIN_CLK, GPIO_FUNC_PIO0);
    return (uint32_t)((uint64_t)count * 1000000ull / window_us);
}

// Rewrite every `wait gpio ISO7816_CLK_GPIO` of a loaded program to use another GPIO.  Instruction memory is
// write-only, so the original words come from the program's instruction array (WAIT has no address to relocate).
// WAIT encoding: 001 ddddd p ss iiiii  (ss = 00 -> GPIO source).
static void patch_wait_gpio(const pio_program_t *prog, uint offset, uint to_gpio) {
    for (uint i = 0; i < prog->length; i++) {
        uint16_t ins = prog->instructions[i];
        if ((ins & 0xE000u) == 0x2000u && ((ins >> 5) & 3u) == 0 &&
            (ins & 0x1Fu) == ISO7816_CLK_GPIO) {
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

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static void rst_irq(uint gpio, uint32_t events) {
    if (gpio != ISO7816_PIN_RST) return;
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
    hw_write_masked(&s_pio->sm[s_sm_tx].shiftctrl,
                    (bits & 31u) << PIO_SM0_SHIFTCTRL_PULL_THRESH_LSB,
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
    uint32_t frame = 1u;                           // start bit
    bool p = even_parity(b);                       // parity bit (logic value)
    if (!s_params.inverse) {
        frame |= (uint32_t)(~b & 0xFF) << 1;
        frame |= (uint32_t)(p ? 0 : 1) << 9;
    } else {
        for (int i = 0; i < 8; i++) {
            frame |= (uint32_t)((b >> (7 - i)) & 1) << (1 + i);
        }
        frame |= (uint32_t)(p ? 1 : 0) << 9;
    }
    return frame;                                  // remaining bits = 0 (released)
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
}

// Reader sent a character with bad parity: emit the error signal so that it repeats the character.  RX is paused
// meanwhile so the pulse is not seen as a start bit, and restarted before the repetition can begin.
static void send_error_signal(void) {
    pio_sm_set_enabled(s_pio, s_sm_rx, false);
    pio_sm_put(s_pio, s_sm_tx, ERROR_SIGNAL_FRAME);
    absolute_time_t deadline = make_timeout_time_us(TX_CHAR_TIMEOUT_US);
    while (!pio_sm_is_tx_fifo_empty(s_pio, s_sm_tx) && !time_reached(deadline)) {
        tight_loop_contents();
    }
    sleep_us(phy_etu_us() * ERROR_SIGNAL_LOW_END_ETU + phy_etu_us() / 4);
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

    pio_sm_set_enabled(s_pio, s_sm_tx, true);
    pio_sm_set_enabled(s_pio, s_sm_rx, true);
}

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------

void phy_init(void) {
    // RST: plain input, pulled down so that "no reader" reads as reset.
    gpio_init(ISO7816_PIN_RST);
    gpio_set_dir(ISO7816_PIN_RST, GPIO_IN);
    gpio_pull_down(ISO7816_PIN_RST);
    gpio_set_irq_enabled_with_callback(ISO7816_PIN_RST, GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE,
                                       true, rst_irq);

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

    s_cfg_rx = iso7816_rx_program_get_default_config(s_off_rx);
    sm_config_set_in_pins(&s_cfg_rx, ISO7816_PIN_IO);
    sm_config_set_in_shift(&s_cfg_rx, true, false, 32);     // right, no autopush
    sm_config_set_fifo_join(&s_cfg_rx, PIO_FIFO_JOIN_RX);    // 8-deep RX FIFO
    sm_config_set_clkdiv_int_frac(&s_cfg_rx, 1, 0);

    s_cfg_tx = iso7816_tx_program_get_default_config(s_off_tx);
    sm_config_set_out_pins(&s_cfg_tx, ISO7816_PIN_IO, 1);
    sm_config_set_out_shift(&s_cfg_tx, true, true, 12);     // right, autopull (threshold set later)
    sm_config_set_in_shift(&s_cfg_tx, true, false, 32);     // only for sm_load_isr()
    sm_config_set_fifo_join(&s_cfg_tx, PIO_FIFO_JOIN_TX);    // 8-deep TX FIFO
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
    if (!s_ignore_reset) {
        // Diagnostic: hardware count of the reader's clock (0.5 ms window, well inside the 400..40000 clock budget
        // before the ATR has to start).
        s_stats.dbg_clk_pwm_hz = measure_clk_pwm_hz(500);
        if (s_stats.dbg_clk_pwm_hz > 100000) s_stats.clk_hz = s_stats.dbg_clk_pwm_hz;
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

bool phy_apply_params(const phy_params_t *p) {
    if (!params_valid(p)) return false;
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
//  -1  parity error (error signal already sent)
static int rx_pop(uint8_t *byte) {
    if (pio_sm_is_rx_fifo_empty(s_pio, s_sm_rx)) return 0;
    uint32_t w = pio_sm_get(s_pio, s_sm_rx);
    uint8_t b = decode_byte(w);
    trace_add('R', (uint16_t)(w & 0x1FF));
    s_last_rx_time = get_absolute_time();
    s_last_rx_valid = true;
    if (!parity_ok(w, b)) {
        s_stats.rx_parity_errors++;
        if (s_params.error_signal) send_error_signal();
        return -1;
    }
    *byte = b;
    return 1;
}

bool phy_rx(uint8_t *byte, bool idle) {
    for (;;) {
        watchdog_update();   // waiting for the reader is legitimate, not a hang
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

bool phy_tx(const uint8_t *data, size_t len) {
    size_t pushed = 0, echoed = 0;
    bool mismatch_logged = false;

    // Character turnaround: do not start before `turnaround_etu` (+1 margin) after the leading edge of the last
    // character the reader sent (its push came at 9.5 ETU).
    if (s_last_rx_valid) {
        uint32_t wait_tenths = (uint32_t)s_params.turnaround_etu * 10 + 10;
        wait_tenths = wait_tenths > 95 ? wait_tenths - 95 : 0;
        absolute_time_t earliest =
            delayed_by_us(s_last_rx_time, (uint64_t)phy_etu_us() * wait_tenths / 10);
        while (!time_reached(earliest)) {
            if (phy_reset_pending()) {
                s_stats.last_error = "tx: reset asserted";
                return false;
            }
        }
    }

    absolute_time_t deadline = make_timeout_time_us(TX_CHAR_TIMEOUT_US);
    s_tx_start = get_absolute_time();

    while (echoed < len) {
        watchdog_update();
        bool progressed = false;
        while (pushed < len && !pio_sm_is_tx_fifo_full(s_pio, s_sm_tx)) {
            pio_sm_put(s_pio, s_sm_tx, make_frame(data[pushed]));
            trace_add('T', data[pushed]);
            pushed++;
            progressed = true;
        }
        // Our own characters come back through the receiver; use them as completion signal and as a check that nobody
        // fought us for the line.
        while (echoed < len && !pio_sm_is_rx_fifo_empty(s_pio, s_sm_rx)) {
            uint32_t w = pio_sm_get(s_pio, s_sm_rx);
            uint8_t b = decode_byte(w);
            trace_add('R', (uint16_t)(w & 0x1FF));
            if (!s_first_echo_valid) {
                s_first_echo_time = get_absolute_time();
                s_first_echo_valid = true;
                // First ATR character: echoed (frame + 9.5) ETU after the session start, a decent estimate of the clock
                // if the PWM counter could not be used.
                int64_t dt = absolute_time_diff_us(s_session_t0, s_first_echo_time);
                s_stats.dbg_delay_us = (uint32_t)dt;
                if (dt > 0 && !s_stats.dbg_clk_pwm_hz) {
                    s_stats.clk_hz = (uint32_t)(((2ull * s_frame_bits + 19) * s_etu_clks / 2) *
                                                1000000ull / (uint64_t)dt);
                }
            }
            if (echoed < 2) s_echo_times[echoed] = get_absolute_time();
            if (b != data[echoed] || !parity_ok(w, b)) {
                s_stats.tx_echo_errors++;
                if (!mismatch_logged) {
                    mismatch_logged = true;
                    s_stats.dbg_echo_index = echoed;
                    s_stats.dbg_echo_expected = data[echoed];
                    s_stats.dbg_echo_got = w & 0x1FF;
                }
            }
            echoed++;
            progressed = true;
        }
        if (progressed) {
            deadline = make_timeout_time_us(TX_CHAR_TIMEOUT_US);
        } else if (time_reached(deadline)) {
            s_stats.tx_timeouts++;
            s_stats.last_error = "tx: no echo (clock stopped?)";
            return false;
        }
        if (phy_reset_pending()) {
            s_stats.last_error = "tx: reset asserted";
            return false;
        }
    }
    s_stats.tx_chars += len;
    s_last_rx_valid = false;   // next turnaround is reader -> card, no wait needed
    return true;
}

const phy_stats_t *phy_stats(void) { return &s_stats; }

// ---------------------------------------------------------------------------
// Loopback self-test: the whole ATR path (constants, frame delay, TX frames, RX echo) driven by a Pico-generated clock
// on a spare pin, with I/O looped on another spare pin.  Nothing on the card contacts is touched.
// ---------------------------------------------------------------------------
#define SELFTEST_CLK_PIN 16
#define SELFTEST_IO_PIN  17
#define SELFTEST_CLK_HZ  (125000000 / 32)

void phy_selftest(char *msg, size_t cap) {
    static const uint8_t atr[] = { 0x3B, 0x08, 'P', 'i', 'c', 'o', '7', '8', '1', '6' };
    PIO pio_clk = pio1;
    const uint sm_clk = 0;

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

    // Point both state machines at the test pins.
    pio_sm_config saved_rx = s_cfg_rx, saved_tx = s_cfg_tx;
    sm_config_set_in_pins(&s_cfg_rx, SELFTEST_IO_PIN);
    sm_config_set_out_pins(&s_cfg_tx, SELFTEST_IO_PIN, 1);
    patch_clk_gpio(SELFTEST_CLK_PIN);

    phy_stats_t saved_stats = s_stats;
    s_stats = (phy_stats_t){ 0 };
    s_ignore_reset = true;

    phy_params_t p = PHY_PARAMS_DEFAULT;
    bool ok = phy_start_session(&p) && phy_tx(atr, sizeof atr);
    uint32_t clk_from_echo = s_stats.clk_hz;

    // Pure timing probe: two more characters pushed while the last ATR frame is still running out its guard bits.  push
    // -> 1st echo = 2.5 + 9.5 ETU, 1st -> 2nd echo = 12 ETU: both measure the ETU.
    static const uint8_t probe_bytes[2] = { 0x55, 0xAA };
    int64_t rx_etu = -1, tx_etu = -1;
    if (ok && phy_tx(probe_bytes, 2)) {
        rx_etu = absolute_time_diff_us(s_tx_start, s_echo_times[0]) / 12;
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
    s_ignore_reset = false;

    snprintf(msg, cap,
             "selftest(loopback %s): first echo after %lu us (expect %lu), CLK %lu Hz (expect %lu), "
             "echo err %lu, mismatch idx %lu exp %02lX got 0x%03lX, err=%s | ETU rx %lld us tx %lld us "
             "(expect %lu) | F/D=372/4 inverse: %s, ETU %lld us (expect %lu), echo err %lu",
             ok ? "OK" : "FAILED", (unsigned long)s_stats.dbg_delay_us,
             (unsigned long)((12ull * 2 + 19) * 372 / 2 * 1000000ull / SELFTEST_CLK_HZ),
             (unsigned long)clk_from_echo, (unsigned long)SELFTEST_CLK_HZ,
             (unsigned long)s_stats.tx_echo_errors, (unsigned long)s_stats.dbg_echo_index,
             (unsigned long)s_stats.dbg_echo_expected, (unsigned long)s_stats.dbg_echo_got,
             s_stats.last_error ? s_stats.last_error : "-", (long long)rx_etu, (long long)tx_etu,
             (unsigned long)(372ull * 1000000ull / SELFTEST_CLK_HZ),
             fast_ok ? "OK" : "FAILED", (long long)fast_etu,
             (unsigned long)(93ull * 1000000ull / SELFTEST_CLK_HZ), (unsigned long)fast_errors);

    // Put everything back.
    phy_enter_reset();
    s_stats = saved_stats;
    s_cfg_rx = saved_rx;
    s_cfg_tx = saved_tx;
    s_params = (phy_params_t)PHY_PARAMS_DEFAULT;
    patch_clk_gpio(ISO7816_PIN_CLK);
    pio_sm_set_enabled(pio_clk, sm_clk, false);
    pio_remove_program(pio_clk, &clkgen_program, off_clk);
    gpio_set_function(SELFTEST_CLK_PIN, GPIO_FUNC_NULL);
    gpio_set_function(SELFTEST_IO_PIN, GPIO_FUNC_NULL);
}
