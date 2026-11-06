English | [日本語](README.ja.md)

# ISO7816 Pico

[![CI](https://github.com/i4M1k0SU/iso7816-pico/actions/workflows/ci.yml/badge.svg)](https://github.com/i4M1k0SU/iso7816-pico/actions/workflows/ci.yml)

A framework for developing ISO 7816 smart cards on the Raspberry Pi Pico (RP2040) / Pico 2 (RP2350).

PIO handles the bit-level protocol (start bit / parity / guard time / ETU timing), and the framework implements
ISO 7816-3 (ATR / PPS / T=0 / T=1) and ISO 7816-4 (APDU / logical channels / chaining / file system / PIN / standard
commands).

**Add it to the consumer project as a Git submodule and link it with two lines, `add_subdirectory()` and
`target_link_libraries()`. The consumer writes only the card profile, the applets and a `main.c` of a few lines.**

```
  Consumer project (e.g. a submodule in external/iso7816-pico)
              ┌──────────────────────────────────────────────────────┐
  Written by  │ card_profile.c            ATR, files, PINs, hooks    │
  the         │ applets/*.c               applets (AID + commands)   │
  developer   │ main.c                    card_runtime_init / run    │
              └──────────────┬───────────────────────────────────────┘
                             │ include/iso7816_pico/*.h  (public headers)
  ┌──────────────────────────▼───────────────────────────────────────────┐
  │ src/core/apdu/   ISO 7816-4                                          │
  │   apdu.c      APDU parsing (short/extended), TLV append to responses │
  │   dispatch.c  CLA decoding, logical channels, chaining, SM hook,     │
  │               AID selection, routing (applet → standard → hook)      │
  │   fs.c        MF/DF/EF, SELECT, BINARY/RECORD commands               │
  │   sec.c       PIN, VERIFY/CHANGE/RESET RETRY/GET CHALLENGE etc.      │
  │   iso_cmds.c  standard command table (also used for the T=0 case)    │
  ├──────────────────────────────────────────────────────────────────────┤
  │ src/core/proto/  ISO 7816-3                                          │
  │   session.c   ATR → PPS → protocol selection → command loop          │
  │   atr.c       ATR build / parse   pps.c  PPS response and F/D switch │
  │   t0.c        T=0 (procedure bytes, 61xx/6Cxx, GET RESPONSE,         │
  │               ENVELOPE)                                              │
  │   t1.c        T=1 (I/R/S blocks, chaining, IFS, RESYNCH, WTX)        │
  │ src/core/hal/nvm.c  NVM journal (flash operations live in the port)  │
  ├──────────────────────────────────────────────────────────────────────┤
  │ src/pico/   port and runtime for RP2040 / RP2350                     │
  │   hal/phy.c + iso7816.pio  PIO character layer (any F/D,             │
  │                            inverse convention, error signal)         │
  │   hal/nvm_port.c  flash   hal/bootsel_button.c                       │
  │   sys/runtime.c  main loop, watchdog   sys/console.c                 │
  │   sys/services.c  services of card.h (card_nvm_changed() etc.)       │
  └──────────────────────────────────────────────────────────────────────┘
```

`src/core/` does not depend on the Pico SDK and is compiled as is by the host tests (`tests/`).

## Wiring

| Card contact | Pico        | Notes                        |
|--------------|-------------|------------------------------|
| C1 VCC       | VSYS        | through a diode              |
| C2 RST       | **GP2**     | 1 kΩ                         |
| C3 CLK       | **GP1**     | 1 kΩ                         |
| C5 GND       | GND         |                              |
| C7 I/O       | **GP4**     | 1 kΩ, open-drain operation   |

The pin numbers are set with the CMake variables `ISO7816_PIN_IO` / `ISO7816_PIN_CLK` / `ISO7816_PIN_RST` (defaults
in [include/iso7816_pico/config.h](include/iso7816_pico/config.h)). At startup the firmware rewrites the PIO
`wait gpio` instructions in instruction memory to match the CLK pin.

## Running it

```powershell
.\tools\flash.ps1            # cmake + ninja → picotool load -f -x  (no button press needed)
.\tools\sc_test.ps1          # checks the demo card end to end over PC/SC (winscard.dll)
```

On Linux / macOS, `tools/flash.sh` and `tools/sc_test.py` (pyscard) do the same job.
There are also host-side tests that run without a Pico (`tests/`) and a GitHub Actions CI.

With an SCR3310 on Windows 11, the bundled demo card passes every check of `sc_test.ps1` in T=0, in T=1 and with PPS
(F/D=372/4). Details are in [docs/tools.md](docs/tools.md).

## Developing a card

0. Add the framework to the consumer project as a submodule and link `iso7816_pico::pico`
   ([docs/embedding.md](docs/embedding.md)).
1. Copy [examples/demo/card_profile.c](examples/demo/card_profile.c) and decide the ATR (raw bytes or a descriptor).
2. Gather the data that must persist into one struct, and place the EFs and PINs in it.
3. Define the file tree with a `fs_file_t` table and the PINs with a `sec_pin_t` table.
4. Write the applets ([examples/demo/applets/demo_applet.c](examples/demo/applets/demo_applet.c) is the template)
   and fill in the command table (CLA / INS / T=0 case / handler).
5. Implement the hooks you need (GET DATA, authentication, secure messaging, cryptographic commands).

How to write each of these is described in [docs/card-profile.md](docs/card-profile.md), and the types are defined in
[include/iso7816_pico/card.h](include/iso7816_pico/card.h).
The list of features the framework handles, the dispatch order, pitfalls and known limitations are in
[docs/internals.md](docs/internals.md).

## Documents

The detailed documents are in Japanese.

| File | Contents |
|------|----------|
| [docs/embedding.md](docs/embedding.md) | Embedding as a submodule, CMake, main.c, overriding the configuration |
| [docs/card-profile.md](docs/card-profile.md) | Writing the card profile and applets, the API available to handlers |
| [docs/internals.md](docs/internals.md) | How it works, session and dispatch flow, implemented features, pitfalls, known limitations |
| [docs/tools.md](docs/tools.md) | Build and flash, the five ways into BOOTSEL without replugging, PC/SC tests, console commands |

## License

[MIT LICENSE](LICENSE)
