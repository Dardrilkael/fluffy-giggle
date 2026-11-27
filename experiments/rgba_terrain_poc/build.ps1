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
    "-DWS_BUILD_LIBS=image"
)

Write-Host "Building terrain generator and image/terrain tests..." -ForegroundColor Cyan
Invoke-NativeStep "cmake" @(
    "--build", $BuildDir,
    "--config", $Configuration,
    "--target", "rgba_terrain_poc", "rgba_terrain_tests", "test_image",
    "--parallel"
)

Write-Host "Running image and terrain tests..." -ForegroundColor Cyan
Invoke-NativeStep "ctest" @(
    "--test-dir", $BuildDir,
    "-C", $Configuration,
    "--output-on-failure",
    "-R", "^(image($|[.])|rgba_terrain_tests$)"
)

$Executable = Join-Path $BuildDir "bin"
if ($IsWindows -or $env:OS -eq "Windows_NT") {
    $Executable = Join-Path $Executable "rgba_terrain_poc.exe"
} else {
    $Executable = Join-Path $Executable "rgba_terrain_poc"
}

Write-Host ""
Write-Host "Build and tests completed." -ForegroundColor Green
Write-Host "Executable: $Executable"
Write-Host "Example:"
Write-Host "  & $Executable input.png input.rgba-terrain output.voxcols"
