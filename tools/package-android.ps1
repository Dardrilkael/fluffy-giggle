# Package apps/playground into a signed, installable APK - no Java, no
# Gradle. aapt2 links the manifest + assets/, the module .so is zipped in
# as lib/arm64-v8a/, then zipalign + apksigner (the standard debug
# keystore) finish it. NativeActivity + hasCode=false means the APK carries
# zero code besides libplayground.so.
#
# Usage:
#   tools/package-android.ps1             build + package only
#   tools/package-android.ps1 -Install    ... then adb install -r
#
# Needs cmake + ninja on PATH and the android-local preset's NDK
# (CMakeUserPresets.json - gitignored, pins this machine's NDK path); the
# SDK lives under %LOCALAPPDATA%\Android\Sdk or $env:ANDROID_HOME.

param([switch]$Install)

$ErrorActionPreference = 'Stop'

# Repo root = parent of tools\.
$repo = Split-Path -Parent $PSScriptRoot
$sdk = if ($env:ANDROID_HOME) { $env:ANDROID_HOME }
       else { Join-Path $env:LOCALAPPDATA 'Android\Sdk' }
if (-not (Test-Path $sdk)) {
    throw "Android SDK not found at $sdk (set ANDROID_HOME to override)."
}

# Newest build-tools (aapt2/zipalign/apksigner) + newest platform android.jar.
$buildTools = Get-ChildItem (Join-Path $sdk 'build-tools') -Directory |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
$platform = Get-ChildItem (Join-Path $sdk 'platforms') -Directory -Filter 'android-*' |
    # [version] wants major.minor at minimum - android-35 pads to 35.0.
    Sort-Object { [version](($_.Name -replace '^android-', '') + '.0') } -Descending |
    Select-Object -First 1
$aapt2      = Join-Path $buildTools.FullName 'aapt2.exe'
$zipalign   = Join-Path $buildTools.FullName 'zipalign.exe'
$apksigner  = Join-Path $buildTools.FullName 'apksigner.bat'
$androidJar = Join-Path $platform.FullName 'android.jar'
$adb        = Join-Path $sdk 'platform-tools\adb.exe'

# --- 1. build the module (the preset pins the NDK; no MSVC env needed) ------
Push-Location $repo
try {
    cmake --preset android-local
    if ($LASTEXITCODE -ne 0) { throw 'configure failed (cmake --preset android-local)' }
    cmake --build --preset android-local
    if ($LASTEXITCODE -ne 0) { throw 'build failed (cmake --build --preset android-local)' }
} finally { Pop-Location }

$so = Join-Path $repo 'build\android\bin\libplayground.so'
if (-not (Test-Path $so)) { throw "module not found: $so" }
$manifest = Join-Path $repo 'apps\playground\android\AndroidManifest.xml'
$assets   = Join-Path $repo 'apps\playground\assets'

# --- 2. aapt2 link: manifest + assets/ -> unsigned apk ---------------------
$outDir = Join-Path $repo 'build\android\apk'
New-Item -ItemType Directory -Force $outDir | Out-Null
$unsigned = Join-Path $outDir 'unsigned.apk'
Remove-Item $unsigned -ErrorAction SilentlyContinue
& $aapt2 link --manifest $manifest -A $assets -I $androidJar -o $unsigned
if ($LASTEXITCODE -ne 0) { throw 'aapt2 link failed' }

# --- 3. lib/arm64-v8a/libplayground.so --------------------------------------
# aapt2 has no native-lib flag (AGP does that in Gradle-land), so the entry
# is zipped in directly. extractNativeLibs="true" in the manifest has the
# installer unpack it, so any compression is fine.
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::Open($unsigned, 'Update')
try {
    $entry = $zip.CreateEntry('lib/arm64-v8a/libplayground.so')
    $dst = $entry.Open()
    $src = [System.IO.File]::OpenRead($so)
    try { $src.CopyTo($dst) } finally { $src.Dispose(); $dst.Dispose() }
} finally { $zip.Dispose() }

# --- 4. zipalign (must precede signing) -------------------------------------
$aligned = Join-Path $outDir 'aligned.apk'
Remove-Item $aligned -ErrorAction SilentlyContinue
& $zipalign -f 4 $unsigned $aligned
if ($LASTEXITCODE -ne 0) { throw 'zipalign failed' }

# --- 5. sign with the standard debug keystore -------------------------------
$keystore = Join-Path $env:USERPROFILE '.android\debug.keystore'
if (-not (Test-Path $keystore)) {
    $keytool = 'C:\Program Files\Android\Android Studio\jbr\bin\keytool.exe'
    if (-not (Test-Path $keytool)) { $keytool = 'keytool' }
    New-Item -ItemType Directory -Force (Split-Path -Parent $keystore) | Out-Null
    & $keytool -genkeypair -keystore $keystore -storepass android -keypass android -alias androiddebugkey -dname 'CN=Android Debug,O=Android,C=US' -keyalg RSA -keysize 2048 -validity 10000
    if ($LASTEXITCODE -ne 0) { throw 'keytool failed' }
}
# apksigner is a .bat needing a JVM: fall back to the Android Studio JBR
# when JAVA_HOME is unset.
$jbr = 'C:\Program Files\Android\Android Studio\jbr'
if (-not $env:JAVA_HOME -and (Test-Path $jbr)) { $env:JAVA_HOME = $jbr }

$apk = Join-Path $repo 'build\android\playground.apk'
& $apksigner sign --ks $keystore --ks-pass pass:android --key-pass pass:android --ks-key-alias androiddebugkey --out $apk $aligned
if ($LASTEXITCODE -ne 0) { throw 'apksigner failed' }

# --- 6. optional install -----------------------------------------------------
if ($Install) {
    & $adb install -r $apk
    if ($LASTEXITCODE -ne 0) { throw 'adb install failed' }
    Write-Host "installed: $apk"
} else {
    Write-Host "apk: $apk"
    Write-Host 'install later with: tools\package-android.ps1 -Install'
}
