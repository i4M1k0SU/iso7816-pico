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
# Cache variables: override them from your project before add_subdirectory() (set(ISO7816_PIN_IO 4 CACHE STRING ""))
# or on the command line (-DISO7816_PIN_IO=4).  Once in CMakeCache.txt a value sticks: change it there, with -D, or
# with set(... CACHE STRING "" FORCE).  cmake/config_checks.cmake rejects out-of-range values.
set(ISO7816_PIN_IO 4 CACHE STRING "GPIO of the card I/O contact (C7)")
set(ISO7816_PIN_CLK 1 CACHE STRING "GPIO of the card CLK contact (C3)")
set(ISO7816_PIN_RST 2 CACHE STRING "GPIO of the card RST contact (C2)")
set(ISO7816_SELFTEST_PIN_CLK 16 CACHE STRING "Unconnected GPIO for the boot self-test clock (-1 = no self-test)")
set(ISO7816_SELFTEST_PIN_IO 17 CACHE STRING "Unconnected GPIO for the boot self-test I/O loop (-1 = no self-test)")
set(APDU_BUF_SIZE 4096 CACHE STRING "Largest command / response data field in bytes")
set(CARD_MAX_CHANNELS 4 CACHE STRING "Logical channels (1..20; 1..4 with the basic CLA coding)")
set(SEC_PIN_MAX_LEN 16 CACHE STRING "Longest stored PIN / PUK")
set(NVM_FLASH_BYTES 65536 CACHE STRING "Flash bytes reserved for the NVM journal (multiple of 4096)")
set(LOG_RING_SIZE 4096 CACHE STRING "Console log ring buffer in bytes")
set(WATCHDOG_TIMEOUT_MS 3000 CACHE STRING "Watchdog timeout in ms (0 = disabled)")
set(NVM_IDLE_SAVE_MS 200 CACHE STRING "Quiet time in ms after which a dirty NVM blob is saved")
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/config_checks.cmake)

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

# ---- Version and build commit ------------------------------------------------------------------------------------------
# The version lives in include/iso7816_pico/version.h; ISO7816_PICO_VERSION is the same value for the application.
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/include/iso7816_pico/version.h _ver REGEX "^#define ISO7816_PICO_VERSION_")
string(REGEX REPLACE ".*VERSION_MAJOR +([0-9]+).*" "\\1" _ver_major "${_ver}")
string(REGEX REPLACE ".*VERSION_MINOR +([0-9]+).*" "\\1" _ver_minor "${_ver}")
string(REGEX REPLACE ".*VERSION_STRING +\"([^\"]*)\".*" "\\1" _ver_string "${_ver}")
if(NOT _ver_string STREQUAL "${_ver_major}.${_ver_minor}")
    message(FATAL_ERROR "version.h: ISO7816_PICO_VERSION_STRING +\"${_ver_string}\" is not MAJOR.MINOR")
endif()
set(ISO7816_PICO_VERSION ${_ver_string})
if(NOT ISO7816_PICO_STANDALONE)
    set(ISO7816_PICO_VERSION ${_ver_string} PARENT_SCOPE)
endif()

# iso7816_pico_build.h (ISO7816_PICO_COMMIT / ISO7816_PICO_APP_COMMIT, see cmake/build_info.cmake) is regenerated at
# every build, not only at configure time, so the console banner never shows a stale commit.  The script rewrites the
# file only when the content changes, so an unchanged commit rebuilds nothing.  Only console.c includes it.
find_package(Git QUIET)
set(ISO7816_PICO_BUILD_H ${CMAKE_CURRENT_BINARY_DIR}/generated/iso7816_pico_build.h)
add_custom_target(iso7816_pico_build_info
        COMMAND ${CMAKE_COMMAND} -DGIT_EXECUTABLE=${GIT_EXECUTABLE} -DFRAMEWORK_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                -DAPP_DIR=${CMAKE_SOURCE_DIR} -DOUTPUT=${ISO7816_PICO_BUILD_H}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_info.cmake
        BYPRODUCTS ${ISO7816_PICO_BUILD_H}
        COMMENT "iso7816-pico: build commit"
        VERBATIM
)

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
                ${CMAKE_CURRENT_BINARY_DIR}/generated
)
add_dependencies(iso7816_pico_pico iso7816_pico_build_info)
target_compile_definitions(iso7816_pico_pico PUBLIC
        ISO7816_PIN_IO=${ISO7816_PIN_IO}
        ISO7816_PIN_CLK=${ISO7816_PIN_CLK}
        ISO7816_PIN_RST=${ISO7816_PIN_RST}
        ISO7816_SELFTEST_PIN_CLK=${ISO7816_SELFTEST_PIN_CLK}
        ISO7816_SELFTEST_PIN_IO=${ISO7816_SELFTEST_PIN_IO}
        LOG_RING_SIZE=${LOG_RING_SIZE}
        WATCHDOG_TIMEOUT_MS=${WATCHDOG_TIMEOUT_MS}
        NVM_IDLE_SAVE_MS=${NVM_IDLE_SAVE_MS}
)
target_compile_options(iso7816_pico_pico PRIVATE -Wall -Wextra -Werror)
# SDK dependencies.  The SDK libraries are INTERFACE libraries whose sources are compiled into whatever links them, so
# linking them PRIVATE / PUBLIC here would compile ~90 SDK sources into this archive -- with the -Werror above, and
# again into the application's executable, whose copy is the one actually linked.  Instead they are INTERFACE
# requirements (the application compiles and links them once, with its own flags) and this library only takes their
# compile requirements: include directories, definitions and options, read through $<TARGET_PROPERTY:lib,INTERFACE_*>,
# which CMake evaluates over the transitive dependencies, so the framework's sources are compiled with exactly the
# configuration a normal link would give them.
#
# pico_stdio_usb is a public requirement: the console, the picotool reset interface and the 1200-baud BOOTSEL entry
# all live in it.  The framework writes only to the USB driver, so UART stdio is not needed: disable it in the
# application with pico_enable_stdio_uart(<target> 0), otherwise it claims GP0 / GP1 (GP1 is the default CLK contact).
set(ISO7816_PICO_SDK_LIBS
        pico_stdlib pico_stdio_usb
        pico_bootrom pico_flash pico_rand hardware_flash hardware_pio hardware_watchdog
)
target_link_libraries(iso7816_pico_pico PUBLIC iso7816_pico_core INTERFACE ${ISO7816_PICO_SDK_LIBS})
foreach(lib IN LISTS ISO7816_PICO_SDK_LIBS)
    target_include_directories(iso7816_pico_pico SYSTEM PRIVATE
            $<TARGET_PROPERTY:${lib},INTERFACE_SYSTEM_INCLUDE_DIRECTORIES>)
    target_include_directories(iso7816_pico_pico PRIVATE $<TARGET_PROPERTY:${lib},INTERFACE_INCLUDE_DIRECTORIES>)
    target_compile_definitions(iso7816_pico_pico PRIVATE $<TARGET_PROPERTY:${lib},INTERFACE_COMPILE_DEFINITIONS>)
    target_compile_options(iso7816_pico_pico PRIVATE $<TARGET_PROPERTY:${lib},INTERFACE_COMPILE_OPTIONS>)
endforeach()

if(ISO7816_PICO_BUILD_EXAMPLES)
    add_subdirectory(examples/demo)
endif()
