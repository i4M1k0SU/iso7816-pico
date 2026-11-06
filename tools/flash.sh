#!/usr/bin/env bash
# Build iso7816-pico and flash it to the attached Pico without touching any button.
# Linux / macOS counterpart of flash.ps1 (also works from Git Bash on Windows).
#
# Uses the toolchain installed by the Raspberry Pi Pico VS Code extension (~/.pico-sdk) unless PICO_SDK_PATH /
# PICO_TOOLCHAIN_PATH are already set and cmake / ninja / picotool are on PATH.  `picotool load -f` asks the running
# firmware to reboot into BOOTSEL over USB (the firmware includes pico_stdio_usb's reset interface), writes the UF2 and
# starts it again.  Works just as well if the Pico is already in BOOTSEL mode.
#
# On Linux, picotool needs permission to open the USB device: either run it with sudo or install the udev rules
# shipped with picotool (udev/99-picotool.rules).
#
#   tools/flash.sh              cmake + ninja, then flash
#   tools/flash.sh --no-build   flash build/examples/demo/iso7816-pico-demo.uf2 only
set -euo pipefail

usage() {
    sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
}

no_build=0
for arg in "$@"; do
    case "$arg" in
        --no-build|-n) no_build=1 ;;
        -h|--help) usage ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

root=$(cd "$(dirname "$0")/.." && pwd)
sdk_root=${PICO_SDK_ROOT:-$HOME/.pico-sdk}

if [ -d "$sdk_root" ]; then
    # Newest installed version of each component (the extension keeps one directory per version).
    newest() { ls -d "$1"/*/ 2>/dev/null | sort -V | tail -n 1; }
    sdk_dir=$(newest "$sdk_root/sdk")
    tc_dir=$(newest "$sdk_root/toolchain")
    export PICO_SDK_PATH=${PICO_SDK_PATH:-${sdk_dir%/}}
    export PICO_TOOLCHAIN_PATH=${PICO_TOOLCHAIN_PATH:-${tc_dir%/}}
    for d in "$PICO_TOOLCHAIN_PATH/bin" "$(newest "$sdk_root/picotool")picotool" \
             "$(newest "$sdk_root/cmake")bin" "$(newest "$sdk_root/ninja")"; do
        [ -d "$d" ] && PATH="${d%/}:$PATH"
    done
fi
export PATH

for tool in cmake ninja picotool; do
    command -v "$tool" >/dev/null || { echo "$tool not found on PATH (set PICO_SDK_ROOT or install it)" >&2; exit 1; }
done

if [ "$no_build" = 0 ]; then
    if [ ! -f "$root/build/build.ninja" ]; then
        cmake -S "$root" -B "$root/build" -G Ninja
    fi
    ninja -C "$root/build"
fi

uf2="$root/build/examples/demo/iso7816-pico-demo.uf2"
picotool load -f -x "$uf2"
echo "flashed $uf2"
