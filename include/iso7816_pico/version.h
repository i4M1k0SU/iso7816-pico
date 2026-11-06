// Version of the iso7816-pico framework.  The top CMakeLists.txt reads it from here (ISO7816_PICO_VERSION), and the
// console banner shows it together with the commit the firmware was built from.
#pragma once

#define ISO7816_PICO_VERSION_MAJOR  1
#define ISO7816_PICO_VERSION_MINOR  0
#define ISO7816_PICO_VERSION_STRING "1.0"

// The framework version as a string ("1.0"), e.g. for an applet's GET VERSION response.
static inline const char *card_framework_version(void) { return ISO7816_PICO_VERSION_STRING; }
