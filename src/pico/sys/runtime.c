// The Pico runtime: boot sequence and main loop.
//
// Main loop:
//   RST low  -> reset state, I/O released; NVM written back if dirty.
//   RST high -> one session (ATR, PPS, T=0 / T=1 commands) until the next reset.
//
// Ways to reach BOOTSEL without touching the board:
//   - `picotool load -f ...` / `picotool reboot -f -u` (USB reset interface)
//   - open the USB serial port at 1200 baud
//   - type `bootsel` on the USB serial console
//   - an APDU whose handler calls card_request_bootsel() (80 E0 00 00 00 in the demo)
//   - hold the BOOTSEL button for ~0.3 s while running
#include "iso7816_pico/runtime.h"

#include "console.h"
#include "hal/bootsel_button.h"
#include "hal/nvm.h"
#include "hal/phy.h"
#include "iso7816_pico/config.h"
#include "iso7816_pico/fs.h"
#include "iso7816_pico/log.h"
#include "iso7816_pico/sec.h"
#include "proto/session.h"

#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/stdlib.h"

static void (*s_user_idle)(void);

static void enter_bootsel(const char *why) {
    log_printf("BOOTSEL: %s", why);
    log_drain();
    sleep_ms(50);
    reset_usb_boot(0, 0);
}

// Runs whenever the card has nothing to do: console, logs, BOOTSEL button, the application's hook.
static void idle_service(void) {
    static absolute_time_t next_button_poll;
    static int held;

    if (WATCHDOG_TIMEOUT_MS) watchdog_update();
    console_poll();
    log_drain();

    if (time_reached(next_button_poll)) {
        next_button_poll = make_timeout_time_ms(100);
        held = bootsel_button_pressed() ? held + 1 : 0;
        if (held >= 3) enter_bootsel("button held");
    }
    if (s_user_idle) s_user_idle();
}

static void led(bool on) {
#ifdef PICO_DEFAULT_LED_PIN
    gpio_put(PICO_DEFAULT_LED_PIN, on);
#else
    (void)on;
#endif
}

void card_runtime_set_idle_hook(void (*hook)(void)) { s_user_idle = hook; }

void card_runtime_init(const card_profile_t *profile) {
    card_profile = profile;
    stdio_init_all();
    console_init();

#ifdef PICO_DEFAULT_LED_PIN
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
#endif

    phy_init();
    phy_set_idle_hook(idle_service);
    {
        char msg[400];
        phy_selftest(msg, sizeof msg);
        log_printf("%s", msg);
    }

    // Card data: persistent image, file system, PINs, ATR.
    if (profile->nvm_blob && profile->nvm_size) {
        bool loaded = nvm_load(profile->nvm_blob, profile->nvm_size);
        const nvm_status_t *st = nvm_status();
        log_printf("NVM: %s (%u bytes, slot %lu/%lu, seq %lu)", loaded ? "loaded from flash" : "defaults",
                   (unsigned)profile->nvm_size, (unsigned long)st->slot, (unsigned long)st->slots,
                   (unsigned long)st->seq);
    }
    fs_init(profile->files, profile->n_files);
    sec_init(profile->pins, profile->n_pins);
    session_init();

    // Only a watchdog *timeout* is a recovery; picotool and `reboot` also reboot through the watchdog but report
    // differently.
    if (watchdog_enable_caused_reboot()) {
        log_printf("(recovered by watchdog timeout)");
    }
    if (WATCHDOG_TIMEOUT_MS) watchdog_enable(WATCHDOG_TIMEOUT_MS, true);
}

void card_runtime_run(void) {
    // After a session that did not end with a reset (failed ATR, clock gone) wait for a fresh falling edge on RST
    // instead of retrying at once.
    bool need_fresh_reset = false;

    for (;;) {
        idle_service();

        if (!phy_rst_high()) {
            need_fresh_reset = false;
            if (nvm_take_dirty() && card_profile->nvm_blob) {
                nvm_save_result_t r = nvm_save(card_profile->nvm_blob, card_profile->nvm_size);
                const nvm_status_t *st = nvm_status();
                log_printf("NVM: %s (slot %lu, seq %lu)",
                           r == NVM_SAVE_WRITTEN ? "saved" : r == NVM_SAVE_UNCHANGED ? "unchanged" : "save FAILED",
                           (unsigned long)st->slot, (unsigned long)st->seq);
            }
            continue;
        }
        if (need_fresh_reset && !phy_reset_pending()) continue;

        led(true);
        session_run();
        led(false);
        bool ended_by_reset = phy_reset_pending();

        if (card_bootsel_requested()) enter_bootsel("requested by APDU");

        phy_enter_reset();
        if (card_profile->hooks.on_deactivate) card_profile->hooks.on_deactivate();
        need_fresh_reset = !ended_by_reset;
    }
}
