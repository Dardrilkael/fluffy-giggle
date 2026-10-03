# check_deps.ps1 - the workspace rule as a ctest test (root CMakeLists
# registers it whenever this file exists):
#
#   1. No third-party header may appear under any libs/*/include/ - public
#      headers are ws-owned (root CLAUDE.md, "the one workspace rule").
#      Allowed: <ws/...>, standard C/C++ headers (no slash), and the ONE
#      sanctioned third-party: <tl/expected.hpp> - named only inside
#      ws/core/result.hpp.
#   2. Apps include only ws/... headers (plus std and the OS platform
#      surface - android/, sys/ - which is platform, not a dependency);
#      every third-party target is linked PRIVATE by the ws_* lib that
#      wraps it.
#
# Pure ASCII on purpose: PowerShell 5.1 reads BOM-less files as ANSI.

param([string]$Root = (Get-Location).Path)

$ErrorActionPreference = 'Stop'

# Known third-party include signatures (regex, matched against the text
# between < and >). Anything matching is a leak in BOTH scopes except the
# tl/expected.hpp carve-out. The list is the enforcement backstop for
# third-party headers without a slash (uv.h, glfw3.h, imgui.h, ...) - the
# structural "no slash = std" test cannot catch those on its own.
$script:thirdParty = @(
  'uv\.h$', 'glfw3', 'vulkan', '^volk', '^vk_',
  '^AL/', '^al\.h$', '^alext', 'dr_wav',
  '^imgui', '^implot', '^imstb',
  '^Eigen/', '^eigen',
  '^stb_',
  '^box2d/', '^Jolt/',
  '^tl/expected\.hpp',
  '^oboe/', '^fmt/', '^spdlog/', '^doctest/', '^glm/', '^assimp/'
)

function Test-ThirdParty([string]$Header) {
  foreach ($p in $script:thirdParty) {
    if ($Header -match $p) { return $true }
  }
  return $false
}

# Scope 1: libs/*/include - ws/ + std + the result.hpp carve-out, nothing else.
function Test-LibInclude([string]$Header, [string]$FilePath) {
  if (Test-ThirdParty $Header) {
    if ($Header -eq 'tl/expected.hpp' -and
        $FilePath.EndsWith('ws\core\result.hpp')) {
      return $true  # the one sanctioned exception, by file
    }
    return $false
  }
  if ($Header.StartsWith('ws/')) { return $true }
  if (-not $Header.Contains('/')) { return $true }  # standard C/C++ header
  return $false
}

# Scope 2: apps - ws/ + std + the OS platform surface.
function Test-AppInclude([string]$Header) {
  if (Test-ThirdParty $Header) { return $false }
  if ($Header.StartsWith('ws/')) { return $true }
  if ($Header.StartsWith('android/')) { return $true }  # NDK platform, not a dep
  if ($Header.StartsWith('sys/')) { return $true }      # CRT/POSIX (sys/stat.h)
  if (-not $Header.Contains('/')) { return $true }     # standard C/C++ header
  return $false
}

$extensions = @('*.h', '*.hpp', '*.ipp', '*.inl', '*.c', '*.cpp')

# Collect libs/*/include files.
$libFiles = @()
if (Test-Path (Join-Path $Root 'libs')) {
  foreach ($libDir in (Get-ChildItem -Path (Join-Path $Root 'libs') -Directory)) {
    $incDir = Join-Path $libDir.FullName 'include'
    if (Test-Path $incDir) {
      $libFiles += @(Get-ChildItem -Path $incDir -Recurse -Include $extensions -File)
    }
  }
}

# Collect app source files (apps have no other C/C++ surface).
$appFiles = @()
$appsDir = Join-Path $Root 'apps'
if (Test-Path $appsDir) {
  $appFiles = @(Get-ChildItem -Path $appsDir -Recurse -Include $extensions -File)
}

$violations = New-Object System.Collections.Generic.List[string]
$scanned = 0

function Scan-File([System.IO.FileInfo]$File, [scriptblock]$Allowed) {
  $script:scanned++
  $lineNo = 0
  foreach ($line in (Get-Content -LiteralPath $File.FullName)) {
    $lineNo++
    if ($line -match '^\s*#\s*include\s*<([^>]+)>') {
      $header = $Matches[1]
      if (-not (& $Allowed $header $File.FullName)) {
        $script:violations.Add(("{0}:{1}: forbidden include <{2}>" -f `
          $File.FullName, $lineNo, $header))
      }
    }
  }
}

foreach ($f in $libFiles) { Scan-File $f ${function:Test-LibInclude} }
foreach ($f in $appFiles) { Scan-File $f ${function:Test-AppInclude} }

if ($violations.Count -gt 0) {
  foreach ($v in $violations) { Write-Output $v }
  Write-Output ("check_deps: FAILED - {0} violation(s), {1} files scanned" -f `
    $violations.Count, $scanned)
  exit 1
}
Write-Output ("check_deps: OK - {0} files scanned, no dependency leaks" -f $scanned)
exit 0
