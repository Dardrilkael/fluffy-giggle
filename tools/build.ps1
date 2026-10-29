# Build driver for the ws workspace on Windows. One script, every preset.
# Loads the MSVC x64 environment (vcvars64) inline when cl.exe is not already
# on PATH - so you can run this straight from Explorer or a plain PowerShell
# prompt without dot-sourcing dev-shell.ps1 first. Then it runs the CMake
# configure/build/test preset triplet you ask for.
#
# Usage:
#   .\tools\build.ps1                                 # spice-debug, build only
#   .\tools\build.ps1 -Preset release                 # the whole workspace
#   .\tools\build.ps1 -Preset spice-release           # spice subset, optimized
#   .\tools\build.ps1 -Test                           # also run ctest
#   .\tools\build.ps1 -Target spice                   # build one target
#   .\tools\build.ps1 -Clean                          # wipe build/<preset> first
#   .\tools\build.ps1 -Preset vs-debug -NoVcvars      # VS generator: no cl needed
#   .\tools\build.ps1 -Preset spice-debug -Test -Verbose
#
# Presets live in CMakePresets.json. The spice-* presets build only
# ws::core + ws::math + ws::spice (WS_BUILD_LIBS=core;math;spice) - no Vulkan,
# no OpenAL, no libuv, no Box2D, no Jolt. That is what the spice arc needs.
# The plain debug/release presets build the whole workspace and pull every
# third-party backend (glfw, Vulkan-Headers, volk, VMA, ImGui, ImPlot, stb,
# dr_wav, OpenAL Soft, libuv, Box2D, Jolt) - heavy, but the workspace default.
#
# NOTE: keep this file pure ASCII. PowerShell 5.1 reads BOM-less files as
# ANSI, so UTF-8 punctuation (em-dashes, smart quotes) breaks parsing.

param(
    [string]$Preset = "spice-debug",
    [string]$Target = "",
    [switch]$Test,
    [switch]$Clean,
    [switch]$NoVcvars,
    [switch]$Verbose
)

$ErrorActionPreference = "Stop"
if ($Verbose) { $VerbosePreference = "Continue" }

$repo = Split-Path -Parent $PSScriptRoot
$presetFile = Join-Path $repo "CMakePresets.json"
if (-not (Test-Path $presetFile)) {
    throw "CMakePresets.json not found at $presetFile - run this script from the repo root or its tools/ dir."
}

# Locate cmake. CMake 3.28+ ships with Visual Studio (Common7\IDE\CommonExtensions\
# Microsoft\CMake\CMake\bin), so it usually is on PATH after vcvars; but allow
# a standalone install too.
function Find-Cmake {
    $cmake = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmake) { return $cmake.Source }
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsPath = & $vswhere -latest -property installationPath
        if ($vsPath) {
            $candidate = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
            if (Test-Path $candidate) { return $candidate }
        }
    }
    throw "cmake not found on PATH or under Visual Studio. Install CMake 3.28+ (https://cmake.org/download/) or open a Developer PowerShell."
}

# Locate and load vcvars64.bat into THIS process's environment, mirroring
# tools/dev-shell.ps1 - but inline, since dot-sourcing this script from the
# dev shell is opt-in and we want a one-command build from a plain prompt.
# Skipped when -NoVcvars is set (the Visual Studio generator does not need
# cl.exe on PATH - it shells out to MSBuild itself) or when cl.exe is already
# on PATH (the user already dot-sourced dev-shell.ps1).
function Load-Vcvars {
    if ($NoVcvars) { return }
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) { return }

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        throw "vswhere not found at $vswhere - is Visual Studio with the C++ workload installed?"
    }
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsPath) {
        throw "No Visual Studio installation with the C++ workload (Microsoft.VisualStudio.Component.VC.Tools.x86.x64) found."
    }
    $vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
    if (-not (Test-Path $vcvars)) {
        throw "vcvars64.bat not found under $vsPath"
    }
    Write-Host "Loading MSVC environment from $vsPath ..."
    cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') {
            Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2]
        }
    }
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw "vcvars64.bat ran but cl.exe is still not on PATH. Try dot-sourcing .\tools\dev-shell.ps1 first to diagnose."
    }
}

Load-Vcvars
$cmake = Find-Cmake
Write-Host "cmake: $cmake"

# Validate the preset exists. CMakePresets.json v6 schema: configurePresets is
# a flat array of { name: ... } objects.
$presetJson = Get-Content -Raw $presetFile | ConvertFrom-Json
$validNames = $presetJson.configurePresets | ForEach-Object { $_.name }
if ($Preset -notin $validNames) {
    Write-Host "Unknown preset: '$Preset'. Available presets:"
    $validNames | ForEach-Object { Write-Host "  - $_" }
    throw "Preset '$Preset' not found in CMakePresets.json"
}

$buildDir = Join-Path $repo "build\$Preset"
if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "Cleaning $buildDir ..."
    Remove-Item -Recurse -Force $buildDir
}

Write-Host "Configure: cmake --preset $Preset"
& $cmake --preset $Preset
if ($LASTEXITCODE -ne 0) { throw "Configure failed (exit $LASTEXITCODE)" }

Write-Host "Build: cmake --build --preset $Preset"
$buildArgs = @("--build", "--preset", $Preset)
if ($Target -ne "") {
    $buildArgs += @("--target", $Target)
    Write-Host "  target: $Target"
}
& $cmake @buildArgs
if ($LASTEXITCODE -ne 0) { throw "Build failed (exit $LASTEXITCODE)" }

if ($Test) {
    Write-Host "Test: ctest --preset $Preset"
    $ctest = Join-Path (Split-Path -Parent $cmake) "ctest.exe"
    if (-not (Test-Path $ctest)) {
        $ctest = Get-Command ctest -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
    }
    if (-not $ctest -or -not (Test-Path $ctest)) {
        throw "ctest not found next to cmake ($cmake) or on PATH."
    }
    & $ctest --preset $Preset
    if ($LASTEXITCODE -ne 0) { throw "ctest failed (exit $LASTEXITCODE)" }
}

$bin = Join-Path $buildDir "bin"
Write-Host ""
Write-Host "OK. Build dir: $buildDir"
if (Test-Path $bin) {
    Write-Host "Binaries in:  $bin"
    Get-ChildItem $bin -Filter *.exe | ForEach-Object { Write-Host "  $($_.Name)" }
}
