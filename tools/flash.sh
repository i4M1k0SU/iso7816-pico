#!/usr/bin/env bash
# Build iso7816-pico and flash it to the attached Pico without touching any button.
# Linux / macOS counterpart of flash.ps1 (also works from Git Bash on Windows).
#
# Uses the toolchain installed by the Raspberry Pi Pico VS Code extension (~/.pico-sdk, or $PICO_SDK_ROOT) unless
# PICO_SDK_PATH / PICO_TOOLCHAIN_PATH are already set and cmake / ninja / picotool are on PATH.  The SDK, toolchain and
# picotool versions are the ones pinned in the VS Code block of CMakeLists.txt (sdkVersion / toolchainVersion /
# picotoolVersion), or the newest installed ones if those are missing; cmake and ninja are the newest installed.
# `picotool load -f` asks the running firmware to reboot into BOOTSEL over USB (pico_stdio_usb's reset interface),
# writes the UF2 and starts it again.  Works just as well if the Pico is already in BOOTSEL mode.  On Linux, picotool
# needs permission to open the USB device: run it with sudo or install picotool's udev rules (udev/99-picotool.rules).
#
#   tools/flash.sh                  build/ for the Pico (configure if needed, ninja), then flash
#   tools/flash.sh --board pico2    the same in build-pico2/ with -DPICO_BOARD=pico2
#   tools/flash.sh --no-build       flash the existing UF2 only
#   tools/flash.sh --no-flash       build only
set -euo pipefail

usage() {
    sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
}

no_build=0
no_flash=0
board=pico
while [ $# -gt 0 ]; do
    case "$1" in
        --no-build|-n) no_build=1 ;;
        --no-flash) no_flash=1 ;;
        --board|-b) [ $# -ge 2 ] || { echo "$1 needs a board name" >&2; exit 2; }; board=$2; shift ;;
        --board=*) board=${1#--board=} ;;
        -h|--help) usage ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done
if [ "$no_build" = 1 ] && [ "$no_flash" = 1 ]; then
    echo "--no-build and --no-flash together leave nothing to do" >&2
    exit 2
fi

root=$(cd "$(dirname "$0")/.." && pwd)
sdk_root=${PICO_SDK_ROOT:-$HOME/.pico-sdk}
if [ "$board" = pico ]; then build=$root/build; else build=$root/build-$board; fi

if [ -d "$sdk_root" ]; then
    # Newest installed version of a component (the extension keeps one directory per version).
    newest() { ls -d "$1"/*/ 2>/dev/null | sort -V | tail -n 1 | sed 's#/$##'; }
    # The version CMakeLists.txt pins for a component, if installed; otherwise the newest one.
    pinned() {
        local v
        v=$(sed -n "s/^[[:space:]]*set($2[[:space:]]\{1,\}\([^[:space:])]*\)).*/\1/p" "$root/CMakeLists.txt" |
            head -n 1)
        if [ -n "$v" ] && [ -d "$sdk_root/$1/$v" ]; then
            echo "$sdk_root/$1/$v"
        else
            [ -n "$v" ] && echo "warning: $1 $v (CMakeLists.txt $2) is not installed; using the newest one" >&2
            newest "$sdk_root/$1"
        fi
    }
    sdk_dir=$(pinned sdk sdkVersion)
    tc_dir=$(pinned toolchain toolchainVersion)
    pt_dir=$(pinned picotool picotoolVersion)
    export PICO_SDK_PATH=${PICO_SDK_PATH:-$sdk_dir}
    export PICO_TOOLCHAIN_PATH=${PICO_TOOLCHAIN_PATH:-$tc_dir}
    for d in "$PICO_TOOLCHAIN_PATH/bin" "$pt_dir/picotool" \
             "$(newest "$sdk_root/cmake")/bin" "$(newest "$sdk_root/ninja")"; do
        [ -d "$d" ] && PATH="$d:$PATH"
    done
fi
export PATH

tools="cmake ninja"
[ "$no_build" = 1 ] && tools=""
[ "$no_flash" = 0 ] && tools="$tools picotool"
for tool in $tools; do
    command -v "$tool" >/dev/null || { echo "$tool not found on PATH (set PICO_SDK_ROOT or install it)" >&2; exit 1; }
done
echo "SDK ${PICO_SDK_PATH:-?}, board $board, build directory $build"

if [ "$no_build" = 0 ]; then
    if [ ! -f "$build/build.ninja" ]; then
        cmake -S "$root" -B "$build" -G Ninja -DPICO_BOARD="$board"
    fi
    ninja -C "$build"
fi

uf2="$build/examples/demo/iso7816-pico-demo.uf2"
if [ "$no_flash" = 1 ]; then
    echo "built $uf2"
    exit 0
fi
[ -f "$uf2" ] || { echo "$uf2 not found (build it first)" >&2; exit 1; }
picotool load -f -x "$uf2"
echo "flashed $uf2"
