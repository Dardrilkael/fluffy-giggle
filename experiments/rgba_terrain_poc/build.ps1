[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

function Invoke-NativeStep {
    param(
        [Parameter(Mandatory = $true)][string]$Program,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Program failed with exit code $LASTEXITCODE"
    }
}

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$BuildDir = Join-Path $RepoRoot "build\rgba-terrain-poc"

if ($Clean -and (Test-Path $BuildDir)) {
    Remove-Item -Recurse -Force $BuildDir
}

Write-Host "Configuring the RGBA terrain POC..." -ForegroundColor Cyan
Invoke-NativeStep "cmake" @(
    "-S", $RepoRoot,
    "-B", $BuildDir,
    "-DWS_BUILD_APPS=OFF",
    "-DWS_BUILD_EXPERIMENTS=ON",
    "-DWS_BUILD_TESTS=ON",
    "-DWS_BUILD_LIBS=rgba_terrain_core"
)

Write-Host "Building terrain generator and image/terrain tests..." -ForegroundColor Cyan
Invoke-NativeStep "cmake" @(
    "--build", $BuildDir,
    "--config", $Configuration,
    "--target", "rgba_terrain_poc", "test_image", "test_rgba_terrain_core",
    "--parallel"
)

Write-Host "Running shared image and terrain-core tests..." -ForegroundColor Cyan
Invoke-NativeStep "ctest" @(
    "--test-dir", $BuildDir,
    "-C", $Configuration,
    "--output-on-failure",
    "-R", "^(image($|\.)|rgba_terrain_core($|\.))"
)

$ExecutableName = "rgba_terrain_poc"
if ($IsWindows -or $env:OS -eq "Windows_NT") {
    $ExecutableName += ".exe"
}
$ExecutableCandidates = @(
    (Join-Path $BuildDir "bin\$Configuration\$ExecutableName"),
    (Join-Path $BuildDir "bin\$ExecutableName")
)
$Executable = $ExecutableCandidates |
    Where-Object { Test-Path $_ } |
    Select-Object -First 1
if (-not $Executable) {
    $Executable = $ExecutableCandidates[0]
}

Write-Host ""
Write-Host "Build and tests completed." -ForegroundColor Green
Write-Host "Executable: $Executable"
Write-Host "Example:"
Write-Host "  & $Executable generate 256 256 0xC0FFEE terrain.png terrain.rgba-terrain terrain.rgba-voxels"
Write-Host "  & $Executable inspect terrain.png terrain.rgba-terrain"
Write-Host "  & $Executable validate terrain.png terrain.rgba-terrain terrain.rgba-voxels"
