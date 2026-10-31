# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.1)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(iso7816-pico C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

add_executable(iso7816-pico
        # framework: hardware
        src/hal/phy.c
        src/hal/nvm.c
        src/hal/bootsel_button.c
        # framework: ISO 7816-3
        src/proto/atr.c
        src/proto/pps.c
        src/proto/t0.c
        src/proto/t1.c
        src/proto/session.c
        # framework: ISO 7816-4
        src/apdu/apdu.c
        src/apdu/dispatch.c
        src/apdu/fs.c
        src/apdu/sec.c
        src/apdu/iso_cmds.c
        # framework: system
        src/sys/main.c
        src/sys/console.c
        src/sys/services.c
        # your card
        src/card/card_profile.c
        src/card/applets/demo_applet.c
)

pico_set_program_name(iso7816-pico "iso7816-pico")
pico_set_program_version(iso7816-pico "1.0")
pico_set_program_description(iso7816-pico "ISO 7816 smart card framework (PIO, T=0/T=1)")

# Generate PIO header
pico_generate_pio_header(iso7816-pico ${CMAKE_CURRENT_LIST_DIR}/src/hal/iso7816.pio)

# USB serial console. pico_stdio_usb also provides the picotool reset
# interface and the 1200-baud BOOTSEL trick (both enabled by default).
pico_enable_stdio_uart(iso7816-pico 0)
pico_enable_stdio_usb(iso7816-pico 1)

target_compile_options(iso7816-pico PRIVATE -Wall -Wextra -Werror)

target_include_directories(iso7816-pico PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src
)

target_link_libraries(iso7816-pico
        pico_stdlib
        pico_bootrom
        pico_flash
        pico_rand
        hardware_flash
        hardware_pio
        hardware_watchdog
)

pico_add_extra_outputs(iso7816-pico)
