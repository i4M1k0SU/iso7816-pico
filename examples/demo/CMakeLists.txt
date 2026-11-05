# The demo card: built only when this repository is the top-level project (see ../../CMakeLists.txt).
add_executable(iso7816-pico-demo
        main.c
        card_profile.c
        applets/demo_applet.c
)

target_link_libraries(iso7816-pico-demo PRIVATE iso7816_pico::pico)
target_compile_options(iso7816-pico-demo PRIVATE -Wall -Wextra -Werror)

pico_set_program_name(iso7816-pico-demo "iso7816-pico-demo")
pico_set_program_version(iso7816-pico-demo "1.0")
pico_set_program_description(iso7816-pico-demo "ISO 7816 smart card framework demo (PIO, T=0/T=1)")

# USB serial console.  pico_stdio_usb also provides the picotool reset interface and the 1200-baud BOOTSEL trick.
pico_enable_stdio_uart(iso7816-pico-demo 0)
pico_enable_stdio_usb(iso7816-pico-demo 1)

pico_add_extra_outputs(iso7816-pico-demo)
