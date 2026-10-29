# One-command build of the spice subset (ws::core + ws::math + ws::spice +
# apps/spice). Thin wrapper around tools/build.ps1 - see that file for the
# real work. Forwards every extra arg, so:
#
#   .\tools\build-spice.ps1                 # debug, build only (the default)
#   .\tools\build-spice.ps1 -Test           # debug, then run ctest
#   .\tools\build-spice.ps1 -Clean          # wipe build/spice-debug first
#   .\tools\build-spice.ps1 -Target spice   # build only the spice exe
#
# For release, use:
#   .\tools\build-spice.ps1 -Preset spice-release
#
# For the full workspace (every lib + every app) use tools\build.ps1 with the
# plain debug or release preset instead - the spice subset is what the
# spice arc needs and is the much lighter build (no Vulkan, no OpenAL, no
# libuv, no physics).
#
# NOTE: keep this file pure ASCII. PowerShell 5.1 reads BOM-less files as
# ANSI, so UTF-8 punctuation (em-dashes, smart quotes) breaks parsing.

param(
    [string]$Preset = "spice-debug",
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Rest
)

$build = Join-Path $PSScriptRoot "build.ps1"
& $build -Preset $Preset @Rest
exit $LASTEXITCODE
