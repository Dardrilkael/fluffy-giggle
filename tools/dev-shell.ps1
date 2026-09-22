# Loads the MSVC x64 build environment (vcvars64) into the CURRENT PowerShell
# session. Usage:
#
#   . .\tools\dev-shell.ps1
#
# (dot-source it - running it as a normal script would lose the env vars when
# the script exits). After this, `cmake --preset debug` etc. work because
# cl.exe is on PATH.
#
# NOTE: keep this file pure ASCII. PowerShell 5.1 reads BOM-less files as
# ANSI, so UTF-8 punctuation (em-dashes, smart quotes) breaks parsing.

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    Write-Error "vswhere not found at $vswhere - is Visual Studio installed?"
    return
}

$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) {
    Write-Error "No Visual Studio installation with the C++ workload found."
    return
}

$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) {
    Write-Error "vcvars64.bat not found under $vsPath"
    return
}

Write-Host "Loading MSVC environment from $vsPath ..."
cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') {
        Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2]
    }
}
Write-Host "MSVC x64 environment loaded. cl.exe: $((Get-Command cl.exe).Source)"
