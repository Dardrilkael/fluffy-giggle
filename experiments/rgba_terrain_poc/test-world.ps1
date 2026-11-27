[CmdletBinding()]
param(
    [ValidateRange(1, 4096)]
    [int]$Width = 256,

    [ValidateRange(1, 4096)]
    [int]$Depth = 256,

    [ValidatePattern('^(0[xX][0-9a-fA-F]+|[0-9]+)$')]
    [string]$Seed = "0xC0FFEE",

    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",

    [string]$OutputDir = "build\terrain-world-test",

    [switch]$Clean,
    [switch]$SkipBuild,
    [switch]$LaunchGame
)

$ErrorActionPreference = "Stop"

function Invoke-NativeStep {
    param(
        [Parameter(Mandatory = $true)][string]$Program,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )

    Write-Host ""
    Write-Host "> $Program $($Arguments -join ' ')" -ForegroundColor DarkGray
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Program failed with exit code $LASTEXITCODE"
    }
}

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$PixelCount = [long]$Width * [long]$Depth
$MaxPixels = [long](64 * 1024 * 1024 / 64)
if ($PixelCount -gt $MaxPixels) {
    throw "The POC limit is $MaxPixels map pixels (64 million voxels at 64 blocks high)."
}
if ($LaunchGame -and ($Width -ne 256 -or $Depth -ne 256)) {
    throw "The current voxel_game importer requires exactly 256x256. Generate 256x256 when using -LaunchGame."
}

if ([System.IO.Path]::IsPathRooted($OutputDir)) {
    $ResolvedOutputDir = $OutputDir
} else {
    $ResolvedOutputDir = Join-Path $RepoRoot $OutputDir
}
New-Item -ItemType Directory -Force -Path $ResolvedOutputDir | Out-Null
$ResolvedOutputDir = (Resolve-Path $ResolvedOutputDir).Path

$PngPath = Join-Path $ResolvedOutputDir "terrain.png"
$MetadataPath = Join-Path $ResolvedOutputDir "terrain.rgba-terrain"
$VoxelsPath = Join-Path $ResolvedOutputDir "terrain.rgba-voxels"

if (-not $SkipBuild) {
    Write-Host "=== Building terrain generator and running core tests ===" -ForegroundColor Cyan
    $BuildScript = Join-Path $PSScriptRoot "build.ps1"
    & $BuildScript -Configuration $Configuration -Clean:$Clean
    if ($LASTEXITCODE -ne 0) {
        throw "Terrain POC build script failed with exit code $LASTEXITCODE"
    }
}

$BuildDir = Join-Path $RepoRoot "build\rgba-terrain-poc"
$ExeName = if ($env:OS -eq "Windows_NT") { "rgba_terrain_poc.exe" } else { "rgba_terrain_poc" }
$GeneratorCandidates = @(
    (Join-Path $BuildDir "bin\$Configuration\$ExeName"),
    (Join-Path $BuildDir "bin\$ExeName")
)
$Generator = $GeneratorCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $Generator) {
    $Generator = Get-ChildItem $BuildDir -Recurse -File -Filter $ExeName -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $Generator) {
    throw "Could not find $ExeName under $BuildDir. Run without -SkipBuild."
}

Write-Host ""
Write-Host "=== Generating plan-based world ===" -ForegroundColor Cyan
Invoke-NativeStep $Generator @(
    "generate-world", "$Width", "$Depth", $Seed,
    $PngPath, $MetadataPath, $VoxelsPath
)

Write-Host ""
Write-Host "=== Inspecting generated RGBA map ===" -ForegroundColor Cyan
Invoke-NativeStep $Generator @("inspect", $PngPath, $MetadataPath)

Write-Host ""
Write-Host "=== Validating PNG-to-voxel reconstruction ===" -ForegroundColor Cyan
Invoke-NativeStep $Generator @(
    "validate", $PngPath, $MetadataPath, $VoxelsPath
)

Write-Host ""
Write-Host "Generation and validation completed." -ForegroundColor Green
Write-Host "PNG:      $PngPath"
Write-Host "Metadata: $MetadataPath"
Write-Host "Voxels:   $VoxelsPath"

if ($LaunchGame) {
    Write-Host ""
    Write-Host "=== Building voxel_game and terrain-import tests ===" -ForegroundColor Cyan
    Invoke-NativeStep "cmake" @(
        "--build", "--preset", "debug",
        "--target", "voxel_game", "test_voxel_terrain_import"
    )
    Invoke-NativeStep "ctest" @(
        "--test-dir", (Join-Path $RepoRoot "build\debug"),
        "--output-on-failure",
        "-R", "voxel_terrain_import"
    )

    $GameBuildDir = Join-Path $RepoRoot "build\debug"
    $Game = Get-ChildItem $GameBuildDir -Recurse -File -Filter "voxel_game.exe" |
        Select-Object -First 1 -ExpandProperty FullName
    if (-not $Game) {
        throw "voxel_game.exe was not found under $GameBuildDir."
    }

    Write-Host ""
    Write-Host "=== Launching voxel_game with generated PNG ===" -ForegroundColor Cyan
    $OldPng = $env:WS_VOXEL_TERRAIN_PNG
    $OldMetadata = $env:WS_VOXEL_TERRAIN_META
    $OldVoxels = $env:WS_VOXEL_TERRAIN_VOXELS
    try {
        $env:WS_VOXEL_TERRAIN_PNG = $PngPath
        $env:WS_VOXEL_TERRAIN_META = $MetadataPath
        $env:WS_VOXEL_TERRAIN_VOXELS = $VoxelsPath
        & $Game
        if ($LASTEXITCODE -ne 0) {
            throw "voxel_game exited with code $LASTEXITCODE"
        }
    }
    finally {
        $env:WS_VOXEL_TERRAIN_PNG = $OldPng
        $env:WS_VOXEL_TERRAIN_META = $OldMetadata
        $env:WS_VOXEL_TERRAIN_VOXELS = $OldVoxels
    }
}
