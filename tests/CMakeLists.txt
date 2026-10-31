# Host-side tests: the protocol and APDU layers compiled for the build machine against a fake PHY, driven by
# reader-side models of T=0 and T=1.  Nothing here needs the Pico SDK.
#
#   cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests --output-on-failure
cmake_minimum_required(VERSION 3.16)
project(iso7816-pico-tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/../src)

add_executable(iso7816_tests
        main.c
        fake_phy.c
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
        test_card_suite.c
        # code under test
        ${SRC}/proto/atr.c
        ${SRC}/proto/pps.c
        ${SRC}/proto/t0.c
        ${SRC}/proto/t1.c
        ${SRC}/proto/session.c
        ${SRC}/apdu/apdu.c
        ${SRC}/apdu/dispatch.c
        ${SRC}/apdu/fs.c
        ${SRC}/apdu/sec.c
        ${SRC}/apdu/iso_cmds.c
        ${SRC}/card/card_profile.c
        ${SRC}/card/applets/demo_applet.c
)

target_include_directories(iso7816_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/stubs
        ${SRC}
)

target_compile_options(iso7816_tests PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_test(NAME iso7816_tests COMMAND iso7816_tests)
