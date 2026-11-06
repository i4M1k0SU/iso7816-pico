# libFuzzer harnesses for the externally fed parsers of the hardware-independent core: the framework core and the demo
# card compiled for the build machine against the fake PHY and the RAM-backed NVM of the host tests (see docs/tools.md).
# Needs clang (libFuzzer is part of its compiler-rt).
#
#   CC=clang cmake -S tests/fuzz -B build-fuzz && cmake --build build-fuzz
#   build-fuzz/fuzz_t1 -max_total_time=60 tests/fuzz/corpus/t1
cmake_minimum_required(VERSION 3.16)
project(iso7816-pico-fuzz C)

if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "The fuzzers need clang (libFuzzer); configure with CC=clang")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(ISO7816_PICO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
include(${ISO7816_PICO_ROOT}/cmake/core_sources.cmake)

set(FUZZ_SANITIZERS "address,undefined" CACHE STRING "Sanitizers added to -fsanitize=fuzzer")

# Recent clang (23 here) warns about write-only globals, of which the core and the fake PHY have one each (t1.c's error
# counter, the fake's idle hook); that is not what the fuzzers are about, so the warning stays off where it exists.
include(CheckCCompilerFlag)
check_c_compiler_flag(-Wunused-but-set-global FUZZ_HAVE_WUNUSED_BUT_SET_GLOBAL)
set(FUZZ_EXTRA_WARNINGS)
if(FUZZ_HAVE_WUNUSED_BUT_SET_GLOBAL)
    set(FUZZ_EXTRA_WARNINGS -Wno-unused-but-set-global)
endif()

# Everything except the harness itself: the core, the demo card and the fakes.  fuzz_card is instrumented for libFuzzer
# (coverage, comparisons) and the sanitizers but has no main(); fuzz_card_plain is the same without the libFuzzer
# instrumentation, for the corpus generator.
set(FUZZ_CARD_SOURCES
        fuzz_common.c
        ${ISO7816_PICO_ROOT}/tests/fake_phy.c
        ${ISO7816_PICO_ROOT}/tests/fake_nvm.c
        ${ISO7816_PICO_CORE_SOURCES}
        ${ISO7816_PICO_ROOT}/examples/demo/card_profile.c
        ${ISO7816_PICO_ROOT}/examples/demo/applets/demo_applet.c
)
foreach(lib fuzz_card fuzz_card_plain)
    add_library(${lib} STATIC ${FUZZ_CARD_SOURCES})
    target_include_directories(${lib} PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${ISO7816_PICO_ROOT}/tests
            ${ISO7816_PICO_ROOT}/tests/stubs
            ${ISO7816_PICO_ROOT}/include
            ${ISO7816_PICO_ROOT}/src/core
    )
    target_compile_options(${lib} PUBLIC -Wall -Wextra -Werror -g -fno-omit-frame-pointer -fno-sanitize-recover=all
            ${FUZZ_EXTRA_WARNINGS})
endforeach()
target_compile_options(fuzz_card PUBLIC -fsanitize=fuzzer-no-link,${FUZZ_SANITIZERS})
target_compile_options(fuzz_card_plain PUBLIC -fsanitize=${FUZZ_SANITIZERS})

foreach(h apdu atr pps t0 t1 nvm)
    add_executable(fuzz_${h} fuzz_${h}.c)
    target_link_libraries(fuzz_${h} PRIVATE fuzz_card)
    target_link_options(fuzz_${h} PRIVATE -fsanitize=fuzzer,${FUZZ_SANITIZERS})
endforeach()

# Seed corpus generator (not a fuzzer): replays command sequences through the host tests' reader models and writes
# what they send (fake_phy_push() is wrapped to record it).
add_executable(fuzz_gen_corpus gen_corpus.c
        ${ISO7816_PICO_ROOT}/tests/reader_t0.c
        ${ISO7816_PICO_ROOT}/tests/reader_t1.c
)
target_link_libraries(fuzz_gen_corpus PRIVATE fuzz_card_plain)
target_link_options(fuzz_gen_corpus PRIVATE -fsanitize=${FUZZ_SANITIZERS} -Wl,--wrap=fake_phy_push)
