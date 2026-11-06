# Host-side tests: the framework core compiled for the build machine against a fake PHY and a RAM-backed NVM,
# driven by reader-side models of T=0 and T=1.  Nothing here needs the Pico SDK.
#
#   cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests --output-on-failure
cmake_minimum_required(VERSION 3.16)
project(iso7816-pico-tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

set(ISO7816_PICO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
include(${ISO7816_PICO_ROOT}/cmake/core_sources.cmake)

add_executable(iso7816_tests
        main.c
        fake_phy.c
        fake_nvm.c
        fakes.c
        reader_t0.c
        reader_t1.c
        test_atr.c
        test_apdu.c
        test_pps.c
        test_session.c
        test_t0.c
        test_t1.c
        test_dispatch.c
        test_fs.c
        test_fs_sec.c
        test_nvm.c
        test_card_suite.c
        # code under test
        ${ISO7816_PICO_CORE_SOURCES}
        # the demo card is the test subject
        ${ISO7816_PICO_ROOT}/examples/demo/card_profile.c
        ${ISO7816_PICO_ROOT}/examples/demo/applets/demo_applet.c
)

target_include_directories(iso7816_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/stubs
        ${ISO7816_PICO_ROOT}/include
        ${ISO7816_PICO_ROOT}/src/core
)

target_compile_options(iso7816_tests PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_test(NAME iso7816_tests COMMAND iso7816_tests)
