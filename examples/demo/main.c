// Demo firmware: the demo card profile on the Pico runtime.  Your own application looks the same, with your profile.
#include "iso7816_pico/runtime.h"

extern const card_profile_t demo_card_profile; // card_profile.c

int main(void) {
    card_runtime_init(&demo_card_profile);
    card_runtime_run();
}
