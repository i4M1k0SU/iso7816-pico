# Embedding contract test: a minimal application that uses the framework exactly like a project that carries it as a
# git submodule would -- add_subdirectory() + target_link_libraries(), nothing else from the framework's tree.
#
#   cmake -S tests/embed -B build-embed -G Ninja && cmake --build build-embed
cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
# (what the extension puts into every project: lets the build find the SDK, picotool and pioasm it installed)
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

# The framework is two directories up here; in a real project it would be e.g. external/iso7816-pico.
set(FRAMEWORK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../..)

include(${FRAMEWORK_DIR}/pico_sdk_import.cmake)
project(embed-test C CXX ASM)
pico_sdk_init()

# Configuration overrides go before add_subdirectory().
set(ISO7816_PIN_IO 4 CACHE STRING "")
set(APDU_BUF_SIZE 1024 CACHE STRING "")

add_subdirectory(${FRAMEWORK_DIR} iso7816-pico)

add_executable(embed-test main.c)
target_link_libraries(embed-test PRIVATE iso7816_pico::pico)
# Warnings as errors for the application's own sources only (the executable also compiles the SDK's sources).
set_source_files_properties(main.c PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror")
pico_enable_stdio_usb(embed-test 1)
# The SDK enables UART stdio by default: it would claim GP0 / GP1 (GP1 is the default CLK contact) and slow down any
# printf() of the application.  The framework's console uses USB only.
pico_enable_stdio_uart(embed-test 0)
pico_add_extra_outputs(embed-test)
