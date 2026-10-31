# Build all Android ABIs (arm64-v8a, armeabi-v7a, x86, x86_64) in one pass.
# Each ABI gets its own build directory (build/android-<abi>) and its own
# .so set under libs/<abi>/. The package-android.ps1 script can then pick
# up the multi-ABI libs for a fat APK.
#
# Usage:
#   tools/build-android-all.ps1                   Debug, all 4 ABIs
#   tools/build-android-all.ps1 -Release          Release
#   tools/build-android-all.ps1 -Abis arm64-v8a   just one ABI
#   tools/build-android-all.ps1 -Abis arm64-v8a,x86_64  subset
#
# Needs:
#   - cmake + ninja on PATH
#   - ANDROID_NDK_HOME env var (or pass -Ndk <path>)
#   - The public `android` preset (CMakePresets.json)

param(
    [switch]$Release,
    [string[]]$Abis = @('arm64-v8a', 'armeabi-v7a', 'x86', 'x86_64'),
    [string]$Ndk = $env:ANDROID_NDK_HOME,
    [string]$LibsOut = 'libs'  # collects .so files per ABI
)

$ErrorActionPreference = 'Stop'

if (-not $Ndk) {
    throw "ANDROID_NDK_HOME not set. Pass -Ndk <path> or set the env var."
}
if (-not (Test-Path "$Ndk/build/cmake/android.toolchain.cmake")) {
    throw "NDK toolchain not found at $Ndk/build/cmake/android.toolchain.cmake"
}

# Make sure cmake + ninja are on PATH.
$cmake = Get-Command cmake -ErrorAction SilentlyContinue
$ninja = Get-Command ninja -ErrorAction SilentlyContinue
if (-not $cmake) { throw "cmake not on PATH" }
if (-not $ninja) { throw "ninja not on PATH" }

$buildType = if ($Release) { 'Release' } else { 'Debug' }
$presetBase = if ($Release) { 'android' } else { 'android' }

# Collect the .so output directory for each ABI.
if (Test-Path $LibsOut) { Remove-Item -Recurse -Force $LibsOut }
New-Item -ItemType Directory -Path $LibsOut | Out-Null

$failed = @()
foreach ($abi in $Abis) {
    Write-Host "`n=== Building $abi ($buildType) ===" -ForegroundColor Cyan

    # Map ABI to preset name.
    $preset = switch ($abi) {
        'arm64-v8a'    { 'android' }
        'armeabi-v7a'  { 'android-armv7' }
        'x86'          { 'android-x86' }
        'x86_64'       { 'android-x8664' }
        default { throw "Unknown ABI: $abi" }
    }

    # Override the build type if Release.
    $configureArgs = @('--preset', $preset)
    if ($Release) {
        # The android preset inherits from _base (Debug). For Release,
        # override CMAKE_BUILD_TYPE via -D.
        $configureArgs = @('--preset', $preset, '-DCMAKE_BUILD_TYPE=Release')
    }

    # Configure.
    Write-Host "  cmake $($configureArgs -join ' ')"
    & cmake @configureArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  CONFIGURE FAILED for $abi" -ForegroundColor Red
        $failed += $abi
        continue
    }

    # Build.
    $buildPreset = switch ($abi) {
        'arm64-v8a'    { 'android' }
        'armeabi-v7a'  { 'android-armv7' }
        'x86'          { 'android-x86' }
        'x86_64'       { 'android-x8664' }
    }
    Write-Host "  cmake --build --preset $buildPreset"
    & cmake --build --preset $buildPreset
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  BUILD FAILED for $abi" -ForegroundColor Red
        $failed += $abi
        continue
    }

    # Collect .so files.
    $buildDir = "build/android-$abi"
    $soDir = "$buildDir/libs/$abi"
    if (Test-Path $soDir) {
        $outDir = "$LibsOut/$abi"
        New-Item -ItemType Directory -Path $outDir -Force | Out-Null
        Copy-Item "$soDir/*.so" $outDir -Force
        $count = (Get-ChildItem "$outDir/*.so").Count
        Write-Host "  Collected $count .so files → $outDir" -ForegroundColor Green
    } else {
        # Fallback: search the build tree for .so files.
        $soFiles = Get-ChildItem -Recurse -Filter "*.so" $buildDir -ErrorAction SilentlyContinue
        if ($soFiles) {
            $outDir = "$LibsOut/$abi"
            New-Item -ItemType Directory -Path $outDir -Force | Out-Null
            $soFiles | Copy-Item -Destination $outDir -Force
            Write-Host "  Collected $($soFiles.Count) .so files → $outDir" -ForegroundColor Green
        } else {
            Write-Host "  No .so files found in $buildDir" -ForegroundColor Yellow
        }
    }
}

Write-Host ""
if ($failed.Count -gt 0) {
    Write-Host "FAILED ABIs: $($failed -join ', ')" -ForegroundColor Red
    exit 1
} else {
    Write-Host "All ABIs built successfully." -ForegroundColor Green
    Write-Host "Native libraries collected in $LibsOut/"
    Get-ChildItem -Recurse $LibsOut -Filter "*.so" | ForEach-Object {
        Write-Host "  $($_.DirectoryName | Split-Leaf)/$($_.Name)"
    }
}
