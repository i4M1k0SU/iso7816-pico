# The hardware-independent part of the framework (ISO 7816-3 protocols, ISO 7816-4 APDU layer, NVM journal).
# Shared by the Pico build (root CMakeLists.txt) and the host tests (tests/CMakeLists.txt).
# Expects ISO7816_PICO_ROOT to point at the repository root.
set(ISO7816_PICO_CORE_SOURCES
        ${ISO7816_PICO_ROOT}/src/core/proto/atr.c
        ${ISO7816_PICO_ROOT}/src/core/proto/pps.c
        ${ISO7816_PICO_ROOT}/src/core/proto/t0.c
        ${ISO7816_PICO_ROOT}/src/core/proto/t1.c
        ${ISO7816_PICO_ROOT}/src/core/proto/session.c
        ${ISO7816_PICO_ROOT}/src/core/apdu/apdu.c
        ${ISO7816_PICO_ROOT}/src/core/apdu/dispatch.c
        ${ISO7816_PICO_ROOT}/src/core/apdu/fs.c
        ${ISO7816_PICO_ROOT}/src/core/apdu/sec.c
        ${ISO7816_PICO_ROOT}/src/core/apdu/iso_cmds.c
        ${ISO7816_PICO_ROOT}/src/core/hal/nvm.c
)
