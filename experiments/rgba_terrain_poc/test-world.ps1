[CmdletBinding()]
param(
    # Zero/null means "reuse the remembered value".
    [int]$Width = 0,
    [int]$Depth = 0,
    [string]$Seed = $null,

    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",

    [string]$OutputDir = "build\terrain-world-test",

    [switch]$Clean,
    [switch]$SkipBuild,
    [switch]$LaunchGame,
    [switch]$ResetSettings
)

$ErrorActionPreference = "Stop"

function Assert-ValidSeed {
    param([Parameter(Mandatory = $true)][string]$Value)

    if ($Value -notmatch '^(0[xX][0-9a-fA-F]+|[0-9]+)$') {
        throw "Seed must be a decimal integer or a 0x-prefixed hexadecimal integer."
    }

    try {
        if ($Value -match '^0[xX]') {
            $numericValue = [Convert]::ToUInt64($Value.Substring(2), 16)
        } else {
            $numericValue = [UInt64]::Parse(
                $Value,
                [Globalization.NumberStyles]::None,
                [Globalization.CultureInfo]::InvariantCulture
            )
        }
    } catch {
        throw "Seed '$Value' is outside the supported unsigned 32-bit range."
    }

    if ($numericValue -gt [UInt32]::MaxValue) {
        throw "Seed '$Value' is outside the supported unsigned 32-bit range (0..4294967295)."
    }
}

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

# Keep local test preferences in the ignored build directory, not in source.
$SettingsPath = Join-Path $RepoRoot "build\terrain-world-test-settings.json"
$SettingsDirectory = Split-Path -Parent $SettingsPath
$Settings = [pscustomobject]@{
    Width = 256
    Depth = 256
    Seed = "0xC0FFEE"
}

if (-not $ResetSettings -and (Test-Path -LiteralPath $SettingsPath -PathType Leaf)) {
    try {
        $SavedSettings = Get-Content -LiteralPath $SettingsPath -Raw |
            ConvertFrom-Json -ErrorAction Stop

        if ($null -eq $SavedSettings -or
            $SavedSettings.schema_version -ne 1 -or
            $null -eq $SavedSettings.width -or
            $null -eq $SavedSettings.depth -or
            [string]::IsNullOrWhiteSpace([string]$SavedSettings.seed)) {
            throw "The settings file is missing required fields or uses an unsupported schema."
        }

        $SavedWidth = [int]$SavedSettings.width
        $SavedDepth = [int]$SavedSettings.depth
        $SavedSeed = [string]$SavedSettings.seed

        if ($SavedWidth -lt 1 -or $SavedWidth -gt 4096 -or
            $SavedDepth -lt 1 -or $SavedDepth -gt 4096) {
            throw "Saved width and depth must each be between 1 and 4096."
        }
        Assert-ValidSeed $SavedSeed

        $Settings.Width = $SavedWidth
        $Settings.Depth = $SavedDepth
        $Settings.Seed = $SavedSeed
    } catch {
        throw "Could not load '$SettingsPath': $($_.Exception.Message) Use -ResetSettings to restore defaults."
    }
}

if ($PSBoundParameters.ContainsKey("Width")) {
    if ($Width -lt 1 -or $Width -gt 4096) {
        throw "Width must be between 1 and 4096."
    }
    $Settings.Width = $Width
}
if ($PSBoundParameters.ContainsKey("Depth")) {
    if ($Depth -lt 1 -or $Depth -gt 4096) {
        throw "Depth must be between 1 and 4096."
    }
    $Settings.Depth = $Depth
}
if ($PSBoundParameters.ContainsKey("Seed")) {
    Assert-ValidSeed $Seed
    $Settings.Seed = $Seed
}

$Width = $Settings.Width
$Depth = $Settings.Depth
$Seed = $Settings.Seed

$PixelCount = [long]$Width * [long]$Depth
$MaxPixels = [long](64 * 1024 * 1024 / 64)
if ($PixelCount -gt $MaxPixels) {
    throw "The POC limit is $MaxPixels map pixels (64 million voxels at 64 blocks high)."
}
if ($LaunchGame -and ($Width -ne 256 -or $Depth -ne 256)) {
    throw "The current voxel_game importer requires exactly 256x256. Set -Width 256 -Depth 256, or use -ResetSettings to restore the default size."
}

# Persist only after all selected values have passed validation.
New-Item -ItemType Directory -Force -Path $SettingsDirectory | Out-Null
$SettingsJson = [ordered]@{
    schema_version = 1
    width = $Width
    depth = $Depth
    seed = $Seed
} | ConvertTo-Json
$TemporarySettingsPath = "$SettingsPath.tmp"
Set-Content -LiteralPath $TemporarySettingsPath -Value $SettingsJson -Encoding UTF8
Move-Item -LiteralPath $TemporarySettingsPath -Destination $SettingsPath -Force

Write-Host ("World settings: {0}x{1}, seed {2}" -f $Width, $Depth, $Seed) -ForegroundColor Cyan
Write-Host "Remembered in:  $SettingsPath" -ForegroundColor DarkGray

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

$BuildDir = Join-Path $RepoRoot "build\debug"
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
    throw "Could not find $ExeName under $BuildDir. Run without -SkipBuild or ensure experiments-debug preset was configured."
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
