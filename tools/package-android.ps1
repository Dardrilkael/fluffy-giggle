# Package an app into a signed, installable APK - no Gradle.
# aapt2 links the manifest (+ assets/ when the app has any), the module
# .so is zipped in as lib/arm64-v8a/, then zipalign + apksigner finish it.
# The shared ws::ui Activity base in libs/ui/android/*.java is always
# compiled into classes.dex. App-specific shims in apps/<App>/android/*.java
# are compiled alongside it; their Activity subclasses should extend
# com.ws.ui.WsUiActivity. Manifests therefore use hasCode="true".
#
# Usage:
#   tools/package-android.ps1                          playground (default)
#   tools/package-android.ps1 -App chatter             ... apps/chatter
#   tools/package-android.ps1 -App chatter -Install    ... then adb install -r
#   tools/package-android.ps1 -BuildType Release       ... optimized native module
#   tools/package-android.ps1 -Keystore C:\path\to\release.keystore `
#                           -KeyAlias release -StorePass $secret -KeyPass $secret
#                                                       ... sign with a real key
#
# Needs cmake + ninja on PATH and the public `android` preset (it lives in
# CMakePresets.json - ANDROID_NDK_HOME must point at an NDK); the SDK lives
# under %LOCALAPPDATA%\Android\Sdk or $env:ANDROID_HOME.
# -BuildToolsVersion / -PlatformApi (or the ANDROID_BUILD_TOOLS_VERSION /
# ANDROID_PLATFORM_API environment variables) pin the build-tools and the
# android.jar that are used; without them the newest installed ones win.
#
# Signing: defaults to the standard debug keystore (~\.android\debug.keystore,
# auto-created if missing) so a fresh clone produces an installable APK with
# no extra setup. Pass -Keystore / -KeyAlias / -StorePass / -KeyPass to sign
# with a release key (the debug-key fallback is then skipped). A release
# keystore is the developer's responsibility - never commit one.

param(
    [string]$App = 'playground',
    [switch]$Install,
    [ValidateSet('Debug', 'Release')][string]$BuildType = 'Debug',
    [string]$BuildToolsVersion = $env:ANDROID_BUILD_TOOLS_VERSION,
    [string]$PlatformApi = $env:ANDROID_PLATFORM_API,
    [string]$Keystore = "",
    [string]$KeyAlias = "",
    [string]$StorePass = "",
    [string]$KeyPass = ""
)

$ErrorActionPreference = 'Stop'

# Repo root = parent of tools\.
$repo = Split-Path -Parent $PSScriptRoot
$manifest = Join-Path $repo ("apps\$App\android\AndroidManifest.xml")
if (-not (Test-Path $manifest)) {
    throw "unknown app: apps\$App (no android\AndroidManifest.xml)"
}
$assets = Join-Path $repo ("apps\$App\assets")
$sdk = if ($env:ANDROID_HOME) { $env:ANDROID_HOME }
       elseif ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT }
       else { Join-Path $env:LOCALAPPDATA 'Android\Sdk' }
if (-not (Test-Path $sdk)) {
    throw "Android SDK not found at $sdk (set ANDROID_HOME to override)."
}
if (-not $env:ANDROID_NDK_HOME) {
    throw "ANDROID_NDK_HOME is not set - the public 'android' preset needs it (point it at an NDK)."
}

# build-tools (aapt2/zipalign/apksigner) + platform android.jar. A pinned
# version wins (CI pins both); otherwise the newest installed one is used.
# Directory names are reduced to their numeric prefix for sorting, so preview
# or extension dirs ("36.0.0-rc1", "android-35-ext14", "android-Baklava")
# cannot break the [version] parse.
function ConvertTo-SdkVersion([string]$Name) {
    $n = $Name -replace '^android-', ''
    $n = $n -replace '[^0-9.].*$', ''
    if (-not $n) { return [version]'0.0' }
    if ($n -notmatch '\.') { $n += '.0' }   # [version] wants major.minor
    return [version]$n
}
$buildToolsRoot = Join-Path $sdk 'build-tools'
$platformsRoot  = Join-Path $sdk 'platforms'
if (-not (Test-Path $buildToolsRoot)) { throw "no build-tools directory in $sdk" }
if (-not (Test-Path $platformsRoot))  { throw "no platforms directory in $sdk" }
if ($BuildToolsVersion) {
    $buildTools = Get-Item (Join-Path $buildToolsRoot $BuildToolsVersion) -ErrorAction SilentlyContinue
} else {
    $buildTools = Get-ChildItem $buildToolsRoot -Directory |
        Where-Object { $_.Name -match '^[0-9.]+$' } |   # skip -rc / preview dirs
        Sort-Object { ConvertTo-SdkVersion $_.Name } -Descending | Select-Object -First 1
}
if (-not $buildTools) { throw "Android build-tools not found in $buildToolsRoot (wanted: '$BuildToolsVersion')" }
if ($PlatformApi) {
    $platform = Get-Item (Join-Path $platformsRoot "android-$PlatformApi") -ErrorAction SilentlyContinue
} else {
    $platform = Get-ChildItem $platformsRoot -Directory -Filter 'android-*' |
        Where-Object { $_.Name -match '^android-[0-9]+$' } |   # skip -extN / preview dirs
        Sort-Object { ConvertTo-SdkVersion $_.Name } -Descending | Select-Object -First 1
}
if (-not $platform) { throw "Android platform not found in $platformsRoot (wanted: 'android-$PlatformApi')" }
$aapt2      = Join-Path $buildTools.FullName 'aapt2.exe'
$zipalign   = Join-Path $buildTools.FullName 'zipalign.exe'
$apksigner  = Join-Path $buildTools.FullName 'apksigner.bat'
$androidJar = Join-Path $platform.FullName 'android.jar'
$adb        = Join-Path $sdk 'platform-tools\adb.exe'
foreach ($tool in @($aapt2, $zipalign, $apksigner, $androidJar)) {
    if (-not (Test-Path $tool)) { throw "missing SDK component: $tool" }
}

# javac/d8/apksigner all need a JVM. JAVA_HOME wins; the Android Studio JBR
# is a fallback (decided once, used by steps 2.5 and 5). keytool is found
# the same way.
if (-not $env:JAVA_HOME) {
    $jbr = 'C:\Program Files\Android\Android Studio\jbr'
    if (Test-Path $jbr) {
        $env:JAVA_HOME = $jbr
    } else {
        throw "JAVA_HOME is not set and the Android Studio JBR was not found at $jbr. Set JAVA_HOME to a JDK (8+)."
    }
}
$javaHome = $env:JAVA_HOME
$javac  = Join-Path $javaHome 'bin\javac.exe'
$keytool = Join-Path $javaHome 'bin\keytool.exe'

# --- 1. build the module (the public `android` preset; ANDROID_NDK_HOME pins the NDK) ---
Push-Location $repo
try {
    cmake --preset android "-DCMAKE_BUILD_TYPE=$BuildType"
    if ($LASTEXITCODE -ne 0) { throw "configure failed (cmake --preset android, $BuildType)" }
    cmake --build --preset android
    if ($LASTEXITCODE -ne 0) { throw 'build failed (cmake --build --preset android)' }
} finally { Pop-Location }

# The android preset uses build/android-arm64/ as its binaryDir.
$so = Join-Path $repo ("build\android-arm64\bin\lib$App.so")
if (-not (Test-Path $so)) { throw "module not found: $so" }

# --- 2. aapt2 link: manifest (+ assets/ if the app has any) -> unsigned apk --
# Not every app ships assets (chatter carries none) - -A goes in only
# when the dir exists.
$outDir = Join-Path $repo 'build\android-arm64\apk'
New-Item -ItemType Directory -Force $outDir | Out-Null
$unsigned = Join-Path $outDir 'unsigned.apk'
Remove-Item $unsigned -ErrorAction SilentlyContinue
$linkArgs = @('link', '--manifest', $manifest, '-I', $androidJar, '-o', $unsigned)
if (Test-Path $assets) { $linkArgs += @('-A', $assets) }
& $aapt2 @linkArgs
if ($LASTEXITCODE -ne 0) { throw 'aapt2 link failed' }

# --- 2.5. shared ws::ui + optional app Java -> classes.dex ------------------
# Every Android app gets the shared Activity base (IME bridge). App-specific
# subclasses/shims compile in the same javac invocation, so they can extend
# com.ws.ui.WsUiActivity without duplicating keyboard code. The manifest must
# set hasCode="true" and name either the shared base or an app subclass.
# javac comes from the same JVM apksigner uses (JAVA_HOME), d8 from build-tools.
# -target 8 keeps the class file inside d8's comfort zone; --min-api tracks
# the app's minSdkVersion (28).
$sharedJavaFiles = @(Get-ChildItem (Join-Path $repo 'libs\ui\android') -Filter *.java -ErrorAction SilentlyContinue)
if ($sharedJavaFiles.Count -eq 0) { throw 'shared ws::ui Android Activity source not found in libs/ui/android' }
$appJavaFiles = @(Get-ChildItem (Join-Path $repo ("apps\$App\android")) -Filter *.java -ErrorAction SilentlyContinue)
$javaFiles = @($sharedJavaFiles) + @($appJavaFiles)
$dexPath = $null
if ($javaFiles.Count -gt 0) {
    if (-not (Test-Path $javac)) { throw "javac not found in $javaHome (JAVA_HOME must point at a JDK, not a JRE)" }
    $d8 = Join-Path $buildTools.FullName 'd8.bat'
    $classDir = Join-Path $repo 'build\android-arm64\java'
    New-Item -ItemType Directory -Force $classDir | Out-Null
    Remove-Item (Join-Path $classDir '*') -Recurse -Force -ErrorAction SilentlyContinue
    & $javac -source 8 -target 8 -Xlint:-options -bootclasspath $androidJar -d $classDir @($javaFiles.FullName)
    if ($LASTEXITCODE -ne 0) { throw 'javac failed' }
    & $d8 --min-api 28 --lib $androidJar --output $classDir @((Get-ChildItem $classDir -Recurse -Filter *.class).FullName)
    if ($LASTEXITCODE -ne 0) { throw 'd8 failed' }
    $dexPath = Join-Path $classDir 'classes.dex'
    if (-not (Test-Path $dexPath)) { throw 'd8 produced no classes.dex' }
}

# --- 3. lib/arm64-v8a/lib$App.so (+ classes.dex when step 2.5 ran) ------------
# aapt2 has no native-lib flag (AGP does that in Gradle-land), so the entry
# is zipped in directly. extractNativeLibs="true" in the manifest has the
# installer unpack it, so any compression is fine.
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::Open($unsigned, 'Update')
try {
    if ($dexPath) {
        $dexEntry = $zip.CreateEntry('classes.dex')
        $dst = $dexEntry.Open()
        $src = [System.IO.File]::OpenRead($dexPath)
        try { $src.CopyTo($dst) } finally { $src.Dispose(); $dst.Dispose() }
    }
    $entry = $zip.CreateEntry(("lib/arm64-v8a/lib$App.so"))
    $dst = $entry.Open()
    $src = [System.IO.File]::OpenRead($so)
    try { $src.CopyTo($dst) } finally { $src.Dispose(); $dst.Dispose() }
} finally { $zip.Dispose() }

# --- 4. zipalign (must precede signing) -------------------------------------
$aligned = Join-Path $outDir 'aligned.apk'
Remove-Item $aligned -ErrorAction SilentlyContinue
& $zipalign -f 4 $unsigned $aligned
if ($LASTEXITCODE -ne 0) { throw 'zipalign failed' }

# --- 5. sign -----------------------------------------------------------------
# A release keystore (all four params passed) wins; otherwise the standard
# debug keystore is used (auto-created if missing) so a fresh clone still
# produces an installable APK. The debug-signed build is NOT for production -
# the key is not one you control (a fresh CI machine generates a new one every
# run, so two such APKs cannot update each other).
$apk = Join-Path $repo ("build\android-arm64\$App.apk")
if ($Keystore -ne "") {
    if (-not (Test-Path $Keystore)) {
        throw "keystore not found: $Keystore"
    }
    if (-not $KeyAlias -or -not $StorePass -or -not $KeyPass) {
        throw "-Keystore requires -KeyAlias, -StorePass and -KeyPass (release signing needs all four)."
    }
    # Passwords travel through the environment, not the command line, so they
    # do not show up in process listings.
    $env:APKSIGNER_STORE_PASS = $StorePass
    $env:APKSIGNER_KEY_PASS = $KeyPass
    try {
        & $apksigner sign --ks $Keystore --ks-pass env:APKSIGNER_STORE_PASS `
            --key-pass env:APKSIGNER_KEY_PASS --ks-key-alias $KeyAlias --out $apk $aligned
        $signExit = $LASTEXITCODE
    } finally {
        Remove-Item Env:APKSIGNER_STORE_PASS, Env:APKSIGNER_KEY_PASS -ErrorAction SilentlyContinue
    }
    if ($signExit -ne 0) { throw 'apksigner (release) failed' }
} else {
    $debugKeystore = Join-Path $env:USERPROFILE '.android\debug.keystore'
    if (-not (Test-Path $debugKeystore)) {
        if (-not (Test-Path $keytool)) { throw "keytool not found in $javaHome (JAVA_HOME must point at a JDK)" }
        New-Item -ItemType Directory -Force (Split-Path -Parent $debugKeystore) | Out-Null
        & $keytool -genkeypair -keystore $debugKeystore -storepass android -keypass android `
            -alias androiddebugkey -dname 'CN=Android Debug,O=Android,C=US' `
            -keyalg RSA -keysize 2048 -validity 10000
        if ($LASTEXITCODE -ne 0) { throw 'keytool failed' }
    }
    Write-Warning "Signing with the DEBUG keystore ($debugKeystore). Not for production - pass -Keystore for a release build."
    & $apksigner sign --ks $debugKeystore --ks-pass 'pass:android' `
        --key-pass 'pass:android' --ks-key-alias androiddebugkey --out $apk $aligned
    if ($LASTEXITCODE -ne 0) { throw 'apksigner (debug) failed' }
}

& $apksigner verify $apk
if ($LASTEXITCODE -ne 0) { throw 'apksigner verify failed' }

# --- 6. optional install -----------------------------------------------------
if ($Install) {
    & $adb install -r $apk
    if ($LASTEXITCODE -ne 0) { throw 'adb install failed' }
    Write-Host "installed: $apk"
} else {
    Write-Host "apk: $apk"
    Write-Host ("install later with: tools\package-android.ps1 -App {0} -Install" -f $App)
}
