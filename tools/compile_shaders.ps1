# Regenerates libs/ui/src/shaders/spirv.h from the .vert/.frag files next to
# it. MANUAL by design: the committed SPIR-V arrays are the build input - the
# build itself needs no Vulkan SDK and no shader compiler. Run this only when
# a shader changes.
#
# A compiler is located in this order:
#   1. -Compiler <path>            (glslc, glslang or glslangValidator)
#   2. $env:WS_GLSLANG
#   3. glslang.exe / glslangValidator.exe / glslc.exe on PATH
#   4. $env:VULKAN_SDK\Bin, else the newest C:\VulkanSDK
#   5. the known %TEMP%\glslang-16.6.0 drop (re-create it by unzipping
#      "glslang-16.6.0-windows-x86_64-release.zip" from the KhronosGroup
#      glslang GitHub release 16.6.0 into %TEMP%\glslang-16.6.0)
#
# THE FLAGS AND THE HEADER FORMAT LIVE HERE ONLY - the CMake check/regen
# targets and the dev hot-reload provider call this script (or resolve the
# compiler the same way) and must never duplicate them.
#
# Usage:
#   .\tools\compile_shaders.ps1                          # regenerate in place
#   .\tools\compile_shaders.ps1 -OutFile C:\tmp\spirv.h  # write elsewhere
#   .\tools\compile_shaders.ps1 -Check                   # compare, don't write
#   .\tools\compile_shaders.ps1 -Compiler C:\path\to\glslang.exe
#
# -Check exits 0 when the tree's spirv.h matches what the flags would
# regenerate, 1 when it differs (with the first differing line), 2 when
# compilation itself failed.
#
# NOTE: keep this file pure ASCII. PowerShell 5.1 reads BOM-less files as
# ANSI, so UTF-8 punctuation (em-dashes, smart quotes) breaks parsing.

param(
    [string]$Compiler = "",
    [string]$OutFile = "",
    [switch]$Check,
    [switch]$AllowMissingCompiler
)

$ErrorActionPreference = "Stop"

# The glslang version this script's output is pinned to. Byte-identical
# output depends on it (the SPIR-V carries the generator version), so the
# staleness checks warn - never fail - when a different version is found.
$pinnedGlslang = "16.6.0"

$repo = Split-Path -Parent $PSScriptRoot
$shaderDir = Join-Path $repo "libs\ui\src\shaders"
$repoHeader = Join-Path $shaderDir "spirv.h"
if ($OutFile -eq "") { $OutFile = $repoHeader }
$tmp = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "ws-shaders") -Force

function Find-Compiler([string]$Explicit) {
    if ($Explicit -ne "") {
        if (-not (Test-Path $Explicit)) {
            if ($AllowMissingCompiler) {
                Write-Host "SKIPPED: compiler not found at the explicit path: $Explicit"
                exit 0
            }
            throw "Compiler not found: $Explicit"
        }
        return $Explicit
    }
    if ($env:WS_GLSLANG) {
        if (Test-Path $env:WS_GLSLANG) { return $env:WS_GLSLANG }
        Write-Warning "WS_GLSLANG is set but does not exist: $env:WS_GLSLANG"
    }
    foreach ($name in @("glslang.exe", "glslangValidator.exe", "glslc.exe")) {
        $onPath = Get-Command $name -ErrorAction SilentlyContinue
        if ($onPath) { return $onPath.Source }
    }
    if ($env:VULKAN_SDK) {
        $candidate = Join-Path $env:VULKAN_SDK "Bin\glslc.exe"
        if (Test-Path $candidate) { return $candidate }
        $candidate = Join-Path $env:VULKAN_SDK "Bin\glslang.exe"
        if (Test-Path $candidate) { return $candidate }
    }
    if (Test-Path "C:\VulkanSDK") {
        $candidate = Get-ChildItem "C:\VulkanSDK" -Directory |
            Sort-Object Name -Descending |
            ForEach-Object { Join-Path $_.FullName "Bin\glslc.exe" } |
            Where-Object { Test-Path $_ } |
            Select-Object -First 1
        if ($candidate) { return $candidate }
    }
    $temp = Join-Path $env:TEMP "glslang-16.6.0\bin\glslang.exe"
    if (Test-Path $temp) { return $temp }
    $looked = @(
        "-Compiler argument",
        "WS_GLSLANG env var ($env:WS_GLSLANG)",
        "glslang.exe / glslangValidator.exe / glslc.exe on PATH",
        "VULKAN_SDK\Bin ($env:VULKAN_SDK)",
        "C:\VulkanSDK",
        "$env:TEMP\glslang-16.6.0\bin\glslang.exe"
    ) -join "; "
    if ($AllowMissingCompiler) {
        Write-Host "SKIPPED: no shader compiler found. Looked for: $looked"
        exit 0
    }
    throw "No shader compiler found. Looked for: $looked. Install the Vulkan SDK, put glslang on PATH, or pass -Compiler <path>."
}

function Compile-Spv([string]$Compiler, [string]$Source, [string]$Out,
                     [string]$TargetEnv = "vulkan1.0", [string[]]$Defines = @()) {
    # glslc is Vulkan-only already; the glslang tools need -V for Vulkan
    # semantics (both old and new binary names). All take --target-env, -O, -o.
    # $Defines: batch.frag's bindless variant passes WS_BINDLESS - glslang
    # spells it -DNAME, glslc --define=NAME.
    $flags = @("--target-env", $TargetEnv, "-O")
    if ((Split-Path -Leaf $Compiler) -like "glslang*") {
        $flags = @("-V") + $flags
        foreach ($d in $Defines) { $flags += "-D$d" }
    } else {
        foreach ($d in $Defines) { $flags += "--define=$d" }
    }
    & $Compiler @flags $Source -o $Out
    if ($LASTEXITCODE -ne 0) {
        throw "$Compiler failed on $Source (exit $LASTEXITCODE)"
    }
}

function Emit-SpvArray([string]$SpvPath) {
    # .spv bytes are little-endian 32-bit words on this platform.
    $bytes = [System.IO.File]::ReadAllBytes($SpvPath)
    if ($bytes.Length -eq 0 -or ($bytes.Length % 4) -ne 0) {
        throw "SPIR-V is empty or not word-aligned: $SpvPath"
    }
    $words = New-Object System.Collections.Generic.List[string]
    for ($i = 0; $i -lt $bytes.Length; $i += 4) {
        $words.Add("0x{0:x8}" -f [System.BitConverter]::ToUInt32($bytes, $i))
    }
    $lines = New-Object System.Collections.Generic.List[string]
    for ($i = 0; $i -lt $words.Count; $i += 8) {
        $end = [Math]::Min($i + 7, $words.Count - 1)
        # Trailing comma on every line is legal C++ initializer syntax and
        # keeps the generator branch-free.
        $lines.Add("    " + ($words[$i..$end] -join ", ") + ",")
    }
    return $lines
}

function Get-CompilerVersion([string]$Compiler) {
    try {
        $out = (& $Compiler --version 2>$null) -join " "
        if ($out -match "Version:\s*(\d+:\s*)?([\d.]+)") {
            return $Matches[2]
        }
    } catch { }
    return ""
}

$compiler = Find-Compiler $Compiler

# Byte-identical output depends on the pinned glslang version (the SPIR-V
# generator version is embedded in the binary), so a mismatch warns - the
# check target must not fail a build over it.
$version = Get-CompilerVersion $compiler
if ($version -ne "" -and $version -ne $pinnedGlslang) {
    Write-Warning ("glslang $version found, but the committed spirv.h is " +
        "pinned to glslang $pinnedGlslang - a diff may be the version, not " +
        "the shaders.")
}
Write-Host "Compiler: $compiler"
if ($version -ne "") { Write-Host "Version: $version (pinned: $pinnedGlslang)" }

$vertSpv = Join-Path $tmp "batch.vert.spv"
$fragSpv = Join-Path $tmp "batch.frag.spv"
$fragBindlessSpv = Join-Path $tmp "batch.frag-bindless.spv"
Compile-Spv $compiler (Join-Path $shaderDir "batch.vert") $vertSpv
# The chunked variant keeps the original flags - the committed words are
# byte-pinned, so a shader edit must not silently recompile it differently.
Compile-Spv $compiler (Join-Path $shaderDir "batch.frag") $fragSpv
# The bindless variant: -DWS_BINDLESS, and vulkan1.1 - nonuniformEXT emits
# SPIR-V 1.3 decorations the vulkan1.0 environment does not accept.
Compile-Spv $compiler (Join-Path $shaderDir "batch.frag") $fragBindlessSpv `
    -TargetEnv "vulkan1.1" -Defines @("WS_BINDLESS")

# (Emit-SpvArray's List unrolls to object[] on return - cast back to
# [string[]] for AddRange.)
$vertLines = [string[]](Emit-SpvArray $vertSpv)
$fragLines = [string[]](Emit-SpvArray $fragSpv)
$fragBindlessLines = [string[]](Emit-SpvArray $fragBindlessSpv)

$header = New-Object System.Collections.Generic.List[string]
$header.Add("// Generated by tools/compile_shaders.ps1 from batch.vert / batch.frag -")
$header.Add("// DO NOT EDIT. Change the shaders and re-run the script instead. These")
$header.Add("// committed words are the build input; no shader compiler runs in the")
$header.Add("// build (the same pattern as the ImGui Vulkan backend's embedded SPIR-V).")
$header.Add("")
$header.Add("#pragma once")
$header.Add("")
$header.Add("#include <cstdint>")
$header.Add("")
$header.Add("namespace ws::ui::r2d::spirv {")
$header.Add("")
$header.Add("inline constexpr std::uint32_t kBatchVertSpv[] = {")
$header.AddRange($vertLines)
$header.Add("};")
$header.Add("")
$header.Add("inline constexpr std::uint32_t kBatchFragSpv[] = {")
$header.AddRange($fragLines)
$header.Add("};")
$header.Add("")
$header.Add("inline constexpr std::uint32_t kBatchFragBindlessSpv[] = {")
$header.AddRange($fragBindlessLines)
$header.Add("};")
$header.Add("")
$header.Add("}  // namespace ws::ui::r2d::spirv")

if ($Check) {
    $fresh = ($header -join "`n") + "`n"
    if (-not (Test-Path $repoHeader)) {
        Write-Host "STALE: $repoHeader does not exist."
        exit 1
    }
    # Git hands out CRLF or LF depending on autocrlf, so compare the
    # content, not the raw bytes - the generator always emits LF.
    $committed = [System.IO.File]::ReadAllText($repoHeader)
    $committedLf = $committed.Replace("`r`n", "`n")
    if ($committedLf -ceq $fresh) {
        Write-Host "IN SYNC: $repoHeader matches the current shaders."
        exit 0
    }
    Write-Host "STALE: $repoHeader differs from what the current shaders compile to."
    $c = ($committedLf -split "`n") | ForEach-Object { $_.TrimEnd("`r") }
    $f = $fresh -split "`n"
    $shown = 0
    for ($i = 0; $i -lt [Math]::Min($c.Count, $f.Count); $i++) {
        if ($c[$i] -cne $f[$i]) {
            Write-Host "  line $($i+1):"
            Write-Host "    committed: $($c[$i])"
            Write-Host "    expected:  $($f[$i])"
            $shown++
            if ($shown -ge 3) { break }
        }
    }
    if ($c.Count -ne $f.Count) {
        Write-Host "  (line counts differ: committed $($c.Count), expected $($f.Count))"
    }
    exit 1
}

# Write-then-move: a compile failure above throws before anything lands, but
# the move also protects against a partial write and keeps $OutFile atomic
# for the -OutFile callers (the CMake check target reads it after exit).
$tmpHeader = Join-Path $tmp "spirv.h.new"
[System.IO.File]::WriteAllText($tmpHeader, ($header -join "`n") + "`n")
if ($OutFile -ne $repoHeader) {
    New-Item -ItemType Directory -Path (Split-Path -Parent $OutFile) -Force | Out-Null
}
Move-Item -Force $tmpHeader $OutFile
Write-Host "Wrote $OutFile"
