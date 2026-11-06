# Validation of the configuration cache variables (see the root CMakeLists.txt and include/iso7816_pico/config.h).
# The values become compile definitions, so a typo would otherwise surface as a confusing compiler error -- or not at
# all.  The variables are cached: a value given once (-D on the command line, or a set(... CACHE ...) in the parent
# project) stays in CMakeCache.txt until it is changed there or set again with FORCE.

# iso7816_pico_check_int(<var> <min> <max>): <var> must be a decimal integer in <min>..<max>.
function(iso7816_pico_check_int var min max)
    set(value "${${var}}")
    if(NOT value MATCHES "^-?[0-9]+$")
        message(FATAL_ERROR "iso7816-pico: ${var} must be an integer, got \"${value}\"")
    endif()
    if(value LESS min OR value GREATER max)
        message(FATAL_ERROR "iso7816-pico: ${var} = ${value} is out of range (${min}..${max})")
    endif()
endfunction()

# Card contacts: GP0..GP29 (the GPIOs every RP2040 / RP2350 package has), three different pins.
foreach(var ISO7816_PIN_IO ISO7816_PIN_CLK ISO7816_PIN_RST)
    iso7816_pico_check_int(${var} 0 29)
endforeach()
if(ISO7816_PIN_IO EQUAL ISO7816_PIN_CLK OR ISO7816_PIN_IO EQUAL ISO7816_PIN_RST
        OR ISO7816_PIN_CLK EQUAL ISO7816_PIN_RST)
    message(FATAL_ERROR "iso7816-pico: ISO7816_PIN_IO (${ISO7816_PIN_IO}), ISO7816_PIN_CLK (${ISO7816_PIN_CLK}) and "
            "ISO7816_PIN_RST (${ISO7816_PIN_RST}) must be three different GPIOs")
endif()

# Self-test pins: -1 (no self-test) or two different spare GPIOs that are not card contacts.
foreach(var ISO7816_SELFTEST_PIN_CLK ISO7816_SELFTEST_PIN_IO)
    iso7816_pico_check_int(${var} -1 29)
endforeach()
if(ISO7816_SELFTEST_PIN_CLK GREATER_EQUAL 0 AND ISO7816_SELFTEST_PIN_IO GREATER_EQUAL 0)
    if(ISO7816_SELFTEST_PIN_CLK EQUAL ISO7816_SELFTEST_PIN_IO)
        message(FATAL_ERROR "iso7816-pico: ISO7816_SELFTEST_PIN_CLK and ISO7816_SELFTEST_PIN_IO must be different "
                "GPIOs (both are ${ISO7816_SELFTEST_PIN_IO}); set either to -1 to skip the self-test")
    endif()
    foreach(st ISO7816_SELFTEST_PIN_CLK ISO7816_SELFTEST_PIN_IO)
        foreach(contact ISO7816_PIN_IO ISO7816_PIN_CLK ISO7816_PIN_RST)
            if(${st} EQUAL ${contact})
                message(FATAL_ERROR "iso7816-pico: ${st} = ${${st}} is also ${contact}: the self-test drives a clock "
                        "on its pins, so they must be unconnected (or set either to -1 to skip the self-test)")
            endif()
        endforeach()
    endforeach()
endif()

# Protocol limits.  APDU_BUF_SIZE: at least a short APDU (4-byte header + Lc + 255 data + Le = 261), at most the
# longest extended-length APDU (4 + 3 + 65535 + 2).
iso7816_pico_check_int(APDU_BUF_SIZE 261 65544)
iso7816_pico_check_int(CARD_MAX_CHANNELS 1 20)
iso7816_pico_check_int(SEC_PIN_MAX_LEN 1 255)

# NVM: whole 4 KB flash sectors, and at least the two slots the journal needs.
iso7816_pico_check_int(NVM_FLASH_BYTES 8192 16777216)
math(EXPR _iso7816_pico_rem "${NVM_FLASH_BYTES} % 4096")
if(NOT _iso7816_pico_rem EQUAL 0)
    message(FATAL_ERROR "iso7816-pico: NVM_FLASH_BYTES = ${NVM_FLASH_BYTES} must be a multiple of 4096 (flash sector)")
endif()
iso7816_pico_check_int(NVM_IDLE_SAVE_MS 0 60000)

iso7816_pico_check_int(LOG_RING_SIZE 64 1048576)
# The watchdog counter limits the timeout: 8388 ms on the RP2040, 16777 ms on the RP2350.
if(DEFINED PICO_PLATFORM AND NOT PICO_PLATFORM MATCHES "^rp2040")
    iso7816_pico_check_int(WATCHDOG_TIMEOUT_MS 0 16777)
else()
    iso7816_pico_check_int(WATCHDOG_TIMEOUT_MS 0 8388)
endif()
