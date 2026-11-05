# iso7816-pico: an ISO 7816 smart card framework for the Raspberry Pi Pico / Pico 2.
#
# Two ways to build:
#   * standalone (this file is the top-level project): the Pico SDK is imported here and the demo in examples/ is
#     built as well -> build/examples/demo/iso7816-pico-demo.uf2
#   * embedded (add_subdirectory() from your project, e.g. as a git submodule): only the libraries are defined; your
#     project imports the SDK, calls pico_sdk_init() and links iso7816_pico::pico.  See docs/embedding.md.
#
# Targets:
#   iso7816_pico::pico   the framework for the RP2040 / RP2350: ISO 7816-3 / -4 logic, PIO character layer, NVM,
#                        console and runtime.  Link this one.
#   iso7816_pico::core   usage requirements of the hardware-independent part (public headers and the size
#                        definitions); the host tests compile its sources (cmake/core_sources.cmake) by themselves.
cmake_minimum_required(VERSION 3.13)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(ISO7816_PICO_STANDALONE ON)
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

    include(pico_sdk_import.cmake)
    project(iso7816-pico C CXX ASM)
    pico_sdk_init()
else()
    set(ISO7816_PICO_STANDALONE OFF)
endif()

option(ISO7816_PICO_BUILD_EXAMPLES "Build the demo firmware in examples/" ${ISO7816_PICO_STANDALONE})

# ---- Configuration (defaults in include/iso7816_pico/config.h) -------------------------------------------------------
# Override from your project before add_subdirectory(): set(ISO7816_PIN_IO 4 CACHE STRING "") etc.
set(ISO7816_PIN_IO 4 CACHE STRING "GPIO of the card I/O contact (C7)")
set(ISO7816_PIN_CLK 1 CACHE STRING "GPIO of the card CLK contact (C3)")
set(ISO7816_PIN_RST 2 CACHE STRING "GPIO of the card RST contact (C2)")
set(APDU_BUF_SIZE 4096 CACHE STRING "Largest command / response data field in bytes")
set(CARD_MAX_CHANNELS 4 CACHE STRING "Logical channels (1..4)")
set(SEC_PIN_MAX_LEN 16 CACHE STRING "Longest stored PIN / PUK")
set(NVM_FLASH_BYTES 65536 CACHE STRING "Flash bytes reserved for the NVM journal (multiple of 4096)")
set(LOG_RING_SIZE 4096 CACHE STRING "Console log ring buffer in bytes")
set(WATCHDOG_TIMEOUT_MS 3000 CACHE STRING "Watchdog timeout in ms (0 = disabled)")

# ---- iso7816_pico::core ------------------------------------------------------------------------------------------------
set(ISO7816_PICO_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
include(cmake/core_sources.cmake)

add_library(iso7816_pico_core INTERFACE)
add_library(iso7816_pico::core ALIAS iso7816_pico_core)
target_include_directories(iso7816_pico_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(iso7816_pico_core INTERFACE
        APDU_BUF_SIZE=${APDU_BUF_SIZE}
        CARD_MAX_CHANNELS=${CARD_MAX_CHANNELS}
        SEC_PIN_MAX_LEN=${SEC_PIN_MAX_LEN}
        NVM_FLASH_BYTES=${NVM_FLASH_BYTES}
)
target_compile_features(iso7816_pico_core INTERFACE c_std_11)

# ---- iso7816_pico::pico ------------------------------------------------------------------------------------------------
# One static library: the core sources depend on the PHY / NVM port implemented here, so splitting them into two
# archives would only create a link-order problem for the application.
add_library(iso7816_pico_pico STATIC
        ${ISO7816_PICO_CORE_SOURCES}
        src/pico/hal/phy.c
        src/pico/hal/nvm_port.c
        src/pico/hal/bootsel_button.c
        src/pico/sys/console.c
        src/pico/sys/services.c
        src/pico/sys/runtime.c
)
add_library(iso7816_pico::pico ALIAS iso7816_pico_pico)
pico_generate_pio_header(iso7816_pico_pico ${CMAKE_CURRENT_SOURCE_DIR}/src/pico/hal/iso7816.pio)
target_include_directories(iso7816_pico_pico
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core ${CMAKE_CURRENT_SOURCE_DIR}/src/pico
)
target_compile_definitions(iso7816_pico_pico PUBLIC
        ISO7816_PIN_IO=${ISO7816_PIN_IO}
        ISO7816_PIN_CLK=${ISO7816_PIN_CLK}
        ISO7816_PIN_RST=${ISO7816_PIN_RST}
        LOG_RING_SIZE=${LOG_RING_SIZE}
        WATCHDOG_TIMEOUT_MS=${WATCHDOG_TIMEOUT_MS}
)
target_compile_options(iso7816_pico_pico PRIVATE -Wall -Wextra -Werror)
target_link_libraries(iso7816_pico_pico
        PUBLIC iso7816_pico_core pico_stdlib pico_stdio_usb
        PRIVATE pico_bootrom pico_flash pico_rand hardware_flash hardware_pio hardware_watchdog
)

if(ISO7816_PICO_BUILD_EXAMPLES)
    add_subdirectory(examples/demo)
endif()
