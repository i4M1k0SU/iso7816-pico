<#
.SYNOPSIS
  Build iso7816-pico and flash it to the attached Pico without touching any button.

.DESCRIPTION
  Uses the toolchain installed by the Raspberry Pi Pico VS Code extension
  (~/.pico-sdk).  The SDK, toolchain and picotool versions are the ones pinned
  in the VS Code block of CMakeLists.txt (sdkVersion / toolchainVersion /
  picotoolVersion); if one of them is not installed, the newest installed
  version is used instead (with a warning).  CMake and Ninja are the newest
  installed versions.

  `picotool load -f` asks the running firmware to reboot into BOOTSEL over USB
  (the firmware includes pico_stdio_usb's reset interface), writes the UF2 and
  starts it again.  Works just as well if the Pico is already in BOOTSEL mode.

.PARAMETER Board
  PICO_BOARD to build for: pico (default, build directory build/) or another
  board such as pico2 (build directory build-<board>/, e.g. build-pico2/).

.PARAMETER BuildDir
  Build directory to use instead of build/ or build-<board>/ (a relative path is relative to the repository root).

.PARAMETER CMakeArgs
  Extra cmake arguments such as -DDEMO_ATR=t1: passed to the first configure and, if the build directory is already
  configured, to a cmake re-run on it before ninja.

.PARAMETER NoBuild
  Skip cmake/ninja and only flash the existing UF2.

.PARAMETER NoFlash
  Only build (configure if needed, then ninja); do not run picotool.

.EXAMPLE
  .\tools\flash.ps1                    # build/ for the Pico, then flash
  .\tools\flash.ps1 -Board pico2       # build-pico2/ for the Pico 2, then flash
  .\tools\flash.ps1 -NoFlash           # build only
  .\tools\flash.ps1 -BuildDir build-atr-t1 -CMakeArgs '-DDEMO_ATR=t1'   # the demo with its T=1 ATR
#>
param(
    [string]$Board = 'pico',
    [string]$BuildDir,
    [string[]]$CMakeArgs = @(),
    [switch]$NoBuild,
    [switch]$NoFlash
)

$ErrorActionPreference = 'Stop'
if ($NoBuild -and $NoFlash) { throw "-NoBuild and -NoFlash together leave nothing to do" }

$root  = Split-Path -Parent $PSScriptRoot
$sdk   = Join-Path $env:USERPROFILE '.pico-sdk'
$build = Join-Path $root $(if ($Board -eq 'pico') { 'build' } else { "build-$Board" })
if ($BuildDir) { $build = if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $root $BuildDir } }

# Sort key for the extension's version directories (2.3.1, 15_2_Rel1, v4.3.4): every number zero-padded.
function Get-VersionKey([string]$name) {
    ([regex]::Matches($name, '\d+') | ForEach-Object { $_.Value.PadLeft(8, '0') }) -join '.'
}

function Get-Newest([string]$dir) {
    if (-not (Test-Path $dir)) { return $null }
    Get-ChildItem -Directory $dir | Sort-Object { Get-VersionKey $_.Name } | Select-Object -Last 1
}

# Version pinned in the VS Code block of CMakeLists.txt, if that is installed; otherwise the newest installed one.
$cmakeLists = Get-Content -Raw (Join-Path $root 'CMakeLists.txt')
function Resolve-Component([string]$component, [string]$variable) {
    $dir = Join-Path $sdk $component
    if ($cmakeLists -match "set\($variable\s+([^\s)]+)\)") {
        $pinned = Join-Path $dir $Matches[1]
        if (Test-Path $pinned) { return $pinned }
        Write-Warning "$component $($Matches[1]) (CMakeLists.txt $variable) is not installed; using the newest one"
    }
    $newest = Get-Newest $dir
    if (-not $newest) { throw "no $component found in $dir (install it with the Pico VS Code extension)" }
    return $newest.FullName
}

$sdkDir       = Resolve-Component 'sdk' 'sdkVersion'
$toolchainDir = Resolve-Component 'toolchain' 'toolchainVersion'
$picotoolDir  = Resolve-Component 'picotool' 'picotoolVersion'
$cmakeDir     = Get-Newest (Join-Path $sdk 'cmake')
$ninjaDir     = Get-Newest (Join-Path $sdk 'ninja')

$env:PICO_SDK_PATH       = $sdkDir
$env:PICO_TOOLCHAIN_PATH = $toolchainDir
$paths = @((Join-Path $toolchainDir 'bin'), (Join-Path $picotoolDir 'picotool'))
if ($cmakeDir) { $paths += Join-Path $cmakeDir.FullName 'bin' }
if ($ninjaDir) { $paths += $ninjaDir.FullName }
$env:Path = ($paths + $env:Path) -join ';'
Write-Host "SDK $sdkDir, toolchain $toolchainDir, board $Board, build directory $build"

if (-not $NoBuild) {
    if (-not (Test-Path (Join-Path $build 'build.ninja'))) {
        cmake -S $root -B $build -G Ninja "-DPICO_BOARD=$Board" @CMakeArgs
        if ($LASTEXITCODE) { throw "cmake configure failed" }
    } elseif ($CMakeArgs.Count) {
        cmake -B $build @CMakeArgs
        if ($LASTEXITCODE) { throw "cmake configure failed" }
    }
    ninja -C $build
    if ($LASTEXITCODE) { throw "build failed" }
}

$uf2 = Join-Path $build 'examples\demo\iso7816-pico-demo.uf2'
if ($NoFlash) {
    Write-Host "built $uf2"
    return
}
if (-not (Test-Path $uf2)) { throw "$uf2 not found (build it first)" }
picotool load -f -x $uf2
if ($LASTEXITCODE) { throw "picotool load failed (is the Pico connected over USB?)" }
Write-Host "flashed $uf2"
