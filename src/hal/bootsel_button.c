// Based on pico-examples/picoboard/button (BSD-3-Clause, Raspberry Pi Ltd).
//
// The BOOTSEL button grounds the flash chip-select line, so reading it means temporarily floating QSPI_SS and looking
// at the pin, with flash (and hence interrupts, whose handlers live in flash) disabled for a few microseconds.
#include "bootsel_button.h"

#include "hardware/gpio.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "pico/platform.h"

bool __no_inline_not_in_flash_func(bootsel_button_pressed)(void) {
    const uint CS_PIN_INDEX = 1;

    uint32_t flags = save_and_disable_interrupts();

    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    // Let the pull-up settle before sampling.
    for (volatile int i = 0; i < 1000; ++i) {
    }

    bool pressed = !(sio_hw->gpio_hi_in & (1u << CS_PIN_INDEX));

    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    restore_interrupts(flags);
    return pressed;
}
