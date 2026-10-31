// Read the Pico's BOOTSEL button while the firmware is running.
#pragma once

#include <stdbool.h>

// True while the button is held.  Briefly disables flash access, so call it only from idle code paths.
bool bootsel_button_pressed(void);
