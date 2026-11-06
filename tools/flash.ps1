<#
.SYNOPSIS
  Build iso7816-pico and flash it to the attached Pico without touching any button.

.DESCRIPTION
  Uses the toolchain installed by the Raspberry Pi Pico VS Code extension
  (~/.pico-sdk).  `picotool load -f` asks the running firmware to reboot into
  BOOTSEL over USB (the firmware includes pico_stdio_usb's reset interface),
  writes the UF2 and starts it again.  Works just as well if the Pico is
  already in BOOTSEL mode.

.PARAMETER NoBuild
  Skip cmake/ninja and only flash build/examples/demo/iso7816-pico-demo.uf2.
#>
param(
    [switch]$NoBuild
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$sdk  = Join-Path $env:USERPROFILE '.pico-sdk'

$env:PICO_SDK_PATH       = Join-Path $sdk 'sdk\2.3.1'
$env:PICO_TOOLCHAIN_PATH = Join-Path $sdk 'toolchain\15_2_Rel1'
$env:Path = @(
    (Join-Path $sdk 'toolchain\15_2_Rel1\bin'),
    (Join-Path $sdk 'picotool\2.3.1\picotool'),
    (Join-Path $sdk 'cmake\v4.3.4\bin'),
    (Join-Path $sdk 'ninja\v1.13.2'),
    $env:Path
) -join ';'

if (-not $NoBuild) {
    if (-not (Test-Path (Join-Path $root 'build\build.ninja'))) {
        cmake -S $root -B (Join-Path $root 'build') -G Ninja
        if ($LASTEXITCODE) { throw "cmake configure failed" }
    }
    ninja -C (Join-Path $root 'build')
    if ($LASTEXITCODE) { throw "build failed" }
}

$uf2 = Join-Path $root 'build\examples\demo\iso7816-pico-demo.uf2'
picotool load -f -x $uf2
if ($LASTEXITCODE) { throw "picotool load failed (is the Pico connected over USB?)" }
Write-Host "flashed $uf2"
