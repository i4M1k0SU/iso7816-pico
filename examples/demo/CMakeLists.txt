# The demo card: built only when this repository is the top-level project (see ../../CMakeLists.txt).
set(DEMO_SOURCES
        main.c
        card_profile.c
        applets/demo_applet.c
)
add_executable(iso7816-pico-demo ${DEMO_SOURCES})

target_link_libraries(iso7816-pico-demo PRIVATE iso7816_pico::pico)

# Which of the ATRs in card_profile.c the demo sends.  Empty (the default) leaves the choice to the ATR_IN_USE #define
# there; t0 / t1 / negotiable override it (tools/hw_regression.ps1 builds all three, each in its own build directory).
set(DEMO_ATR "" CACHE STRING "Demo card ATR: t0, t1 or negotiable (empty: the #define in card_profile.c)")
set_property(CACHE DEMO_ATR PROPERTY STRINGS "" t0 t1 negotiable)
if(DEMO_ATR)
    if(NOT DEMO_ATR MATCHES "^(t0|t1|negotiable)$")
        message(FATAL_ERROR "DEMO_ATR must be t0, t1 or negotiable (got '${DEMO_ATR}')")
    endif()
    target_compile_definitions(iso7816-pico-demo PRIVATE DEMO_ATR_IN_USE=s_atr_${DEMO_ATR})
endif()
# Warnings as errors for the card's own sources only: the executable also compiles the Pico SDK's sources (they come
# with the SDK's INTERFACE libraries), and those should not break the build on a newer compiler or SDK.
set_source_files_properties(${DEMO_SOURCES} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror")

pico_set_program_name(iso7816-pico-demo "iso7816-pico-demo")
pico_set_program_version(iso7816-pico-demo "1.0")
pico_set_program_description(iso7816-pico-demo "ISO 7816 smart card framework demo (PIO, T=0/T=1)")

# USB serial console.  pico_stdio_usb also provides the picotool reset interface and the 1200-baud BOOTSEL trick.
pico_enable_stdio_uart(iso7816-pico-demo 0)
pico_enable_stdio_usb(iso7816-pico-demo 1)

pico_add_extra_outputs(iso7816-pico-demo)
