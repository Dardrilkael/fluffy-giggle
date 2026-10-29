# Windows smoke test for the spice CLI (apps/spice + libs/spice, M1+M2).
#
# Builds the spice subset (unless -SkipBuild / -Spice), then runs a battery
# of analytic decks through the real CLI and asserts closed-form values,
# headers, row counts and exit codes. Every case names itself, so a
# regression (e.g. "the .op output was all zeros") fails loudly with the
# case name, the expected value and the offending CSV dumped to console.
#
#   .\tools\smoke-spice.ps1                          # build spice-debug, then smoke
#   .\tools\smoke-spice.ps1 -Test                    # also run ctest before the smoke
#   .\tools\smoke-spice.ps1 -Preset spice-release    # smoke the release build
#   .\tools\smoke-spice.ps1 -SkipBuild               # smoke build\<preset>\bin\spice.exe as-is
#   .\tools\smoke-spice.ps1 -Spice C:\path\spice.exe # smoke one specific exe (no build)
#   .\tools\smoke-spice.ps1 -KeepWork                # keep the temp deck dir for debugging
#
# Exit code 0 = every assertion passed; 1 = at least one failed (a summary
# lists each one). The deck battery mirrors tools/smoke_spice.py - the
# Python port runs on Linux and in CI; this PowerShell script is retained
# for Windows-only shops. Keep the two in sync when adding cases (the
# Python file is the canonical CI driver, registered as a ctest entry by
# apps/spice/CMakeLists.txt when WS_BUILD_TESTS is on).
#
# NOTE: keep this file pure ASCII. PowerShell 5.1 reads BOM-less files as
# ANSI, so UTF-8 punctuation (em-dashes, smart quotes) breaks parsing.

param(
    [string]$Preset = "spice-debug",
    [string]$Spice = "",
    [switch]$SkipBuild,
    [switch]$KeepWork,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Rest
)

$ErrorActionPreference = "Continue"

# --- locate the exe -------------------------------------------------------------

$repoRoot = Split-Path $PSScriptRoot -Parent

if ($Spice -ne "") {
    $SkipBuild = $true
    if (-not (Test-Path $Spice)) {
        Write-Host "error: -Spice path not found: $Spice" -ForegroundColor Red
        exit 1
    }
} else {
    if (-not $SkipBuild) {
        & (Join-Path $PSScriptRoot "build-spice.ps1") -Preset $Preset @Rest
        if ($LASTEXITCODE -ne 0) {
            Write-Host "error: build failed (preset $Preset)" -ForegroundColor Red
            exit 1
        }
    }
    $Spice = Join-Path $repoRoot ("build\" + $Preset + "\bin\spice.exe")
    if (-not (Test-Path $Spice)) {
        Write-Host "error: spice.exe not found at $Spice" -ForegroundColor Red
        Write-Host "       build first (drop -SkipBuild) or pass -Spice <path>"
        exit 1
    }
}

Write-Host "smoking: $Spice" -ForegroundColor Cyan

# --- work dir --------------------------------------------------------------------

$workName = "spice-smoke-" + [System.Guid]::NewGuid().ToString("N").Substring(0, 8)
$Work = Join-Path ([System.IO.Path]::GetTempPath()) $workName
New-Item -ItemType Directory -Path $Work | Out-Null

# --- helpers ---------------------------------------------------------------------

$script:Pass = 0
$script:Fail = 0
$script:Failures = New-Object System.Collections.Generic.List[string]
$script:LastOut = @()
$script:LastErr = ""
$script:RunId = 0

function Write-Deck {
    # Mode: "lf", "crlf" (default) or "bom" (CRLF + UTF-8 BOM - the Notepad
    # authoring scenario; the parser must survive both).
    param([string]$Name, [string[]]$Lines, [string]$Mode = "crlf")
    $nl = "`r`n"
    if ($Mode -eq "lf") { $nl = "`n" }
    $text = ($Lines -join $nl) + $nl
    $path = Join-Path $Work ($Name + ".cir")
    $enc = New-Object System.Text.UTF8Encoding($Mode -eq "bom")
    [System.IO.File]::WriteAllText($path, $text, $enc)
    return $path
}

function Invoke-Spice {
    param([string[]]$SpiceArgs)
    $script:RunId++
    $errPath = Join-Path $Work ("stderr-" + $script:RunId + ".txt")
    $out = & $Spice @SpiceArgs 2>$errPath
    $code = $LASTEXITCODE
    $err = ""
    if (Test-Path $errPath) { $err = [System.IO.File]::ReadAllText($errPath) }
    $script:LastOut = @($out)
    $script:LastErr = $err
    return @{ Exit = $code; Out = @($out); Err = $err }
}

function ConvertTo-Table {
    # Parses the first CSV section of a single-analysis run: header line +
    # rows, stopping at the blank section separator.
    param([string[]]$Lines)
    $start = 0
    while ($start -lt $Lines.Count -and $Lines[$start].Trim() -eq "") { $start++ }
    $headers = @()
    $rows = New-Object System.Collections.Generic.List[string[]]
    if ($start -lt $Lines.Count) {
        $headers = $Lines[$start].Trim().Split(',')
        $i = $start + 1
        while ($i -lt $Lines.Count -and $Lines[$i].Trim() -ne "") {
            $rows.Add($Lines[$i].Trim().Split(','))
            $i++
        }
    }
    return @{ Headers = $headers; Rows = $rows }
}

function DNum {
    # Invariant-culture double parse: the CSV is always '.'-decimal, even
    # when the host locale uses ',' (e.g. de-DE). Empty/garbage -> NaN so
    # the comparison records a clean FAIL instead of throwing.
    param([string]$Text)
    if ([string]::IsNullOrWhiteSpace($Text)) { return [double]::NaN }
    $v = 0.0
    if (-not [double]::TryParse($Text, [System.Globalization.NumberStyles]::Float,
            [System.Globalization.CultureInfo]::InvariantCulture, [ref]$v)) {
        return [double]::NaN
    }
    return $v
}

function ColOf {
    param($Table, [string]$Name)
    for ($i = 0; $i -lt $Table.Headers.Count; $i++) {
        if ($Table.Headers[$i] -eq $Name) { return $i }
    }
    return -1
}

function Find-RowNear {
    # The row whose sweep column is closest to $Target (mirrors row_near in
    # libs/spice/tests/test_run.cpp).
    param($Rows, [double]$Target)
    $best = $null
    $bestDiff = [double]::MaxValue
    foreach ($row in $Rows) {
        $d = [math]::Abs((DNum $row[0]) - $Target)
        if ($d -lt $bestDiff) { $bestDiff = $d; $best = $row }
    }
    return ,$best
}

function Expect-True {
    param([bool]$Cond, [string]$Case, [string]$What)
    if ($Cond) { $script:Pass++ } else {
        $script:Fail++
        $script:Failures.Add("${Case}: ${What}")
    }
}

function Expect-Approx {
    # |got - want| <= rtol * max(1, |want|) - relative with an absolute floor.
    param([double]$Got, [double]$Want, [double]$Rtol, [string]$Case, [string]$What)
    $tol = $Rtol * [math]::Max(1.0, [math]::Abs($Want))
    if ([math]::Abs($Got - $Want) -le $tol) { $script:Pass++ } else {
        $script:Fail++
        $script:Failures.Add("${Case}: ${What} - got ${Got}, want ${Want} (rtol ${Rtol})")
    }
}

function Expect-Header {
    param($Table, [string]$Want, [string]$Case)
    $got = ($Table.Headers -join ",")
    if ($got -eq $Want) { $script:Pass++ } else {
        $script:Fail++
        $script:Failures.Add("${Case}: header is '${got}', want '${Want}'")
    }
}

function Run-Case {
    param([string]$Name, [scriptblock]$Body)
    $before = $script:Failures.Count
    & $Body
    if ($script:Failures.Count -gt $before) {
        Write-Host "  [FAIL] $Name" -ForegroundColor Red
        if ($script:LastErr -ne "") {
            Write-Host "    stderr: $($script:LastErr.Trim())" -ForegroundColor DarkGray
        }
        if ($script:LastOut.Count -gt 0) {
            $n = [math]::Min(4, $script:LastOut.Count)
            Write-Host "    stdout (first $n lines):" -ForegroundColor DarkGray
            for ($i = 0; $i -lt $n; $i++) {
                Write-Host "      $($script:LastOut[$i])" -ForegroundColor DarkGray
            }
        }
    } else {
        Write-Host "  [ ok ] $Name" -ForegroundColor Green
    }
}

# --- the battery -----------------------------------------------------------------

Write-Host "== analytic decks =="

# 1. .op resistor divider, LF endings (the baseline).
Run-Case "op-divider-lf" {
    $deck = Write-Deck "op1" @(
        "divider", "V1 in 0 DC 5", "R1 in mid 2k", "R2 mid 0 1k",
        ".op", ".end") -Mode "lf"
    $r = Invoke-Spice @("run", $deck)
    Expect-True ($r.Exit -eq 0) "op-divider-lf" "exit code $($r.Exit), want 0"
    $t = ConvertTo-Table $r.Out
    Expect-Header $t "v(in),v(mid),i(V1)" "op-divider-lf"
    Expect-True ($t.Rows.Count -eq 1) "op-divider-lf" "row count $($t.Rows.Count), want 1"
    $row = $t.Rows[0]
    Expect-Approx (DNum $row[0]) 5.0 1e-9 "op-divider-lf" "v(in)"
    Expect-Approx (DNum $row[1]) (5.0/3.0) 1e-9 "op-divider-lf" "v(mid)"
    Expect-Approx (DNum $row[2]) (-5.0/3000.0) 1e-9 "op-divider-lf" "i(V1)"
}

# 2. The same deck authored in Notepad: CRLF line endings + UTF-8 BOM.
#    This is the reported "all zeros on Windows" scenario - the parser must
#    produce byte-identical numbers to case 1.
Run-Case "op-divider-crlf-bom" {
    $deck = Write-Deck "op2" @(
        "divider", "V1 in 0 DC 5", "R1 in mid 2k", "R2 mid 0 1k",
        ".op", ".end") -Mode "bom"
    $r = Invoke-Spice @("run", $deck)
    Expect-True ($r.Exit -eq 0) "op-divider-crlf-bom" "exit code $($r.Exit), want 0"
    $t = ConvertTo-Table $r.Out
    Expect-Header $t "v(in),v(mid),i(V1)" "op-divider-crlf-bom"
    $row = $t.Rows[0]
    Expect-Approx (DNum $row[0]) 5.0 1e-9 "op-divider-crlf-bom" "v(in)"
    Expect-Approx (DNum $row[1]) (5.0/3.0) 1e-9 "op-divider-crlf-bom" "v(mid)"
    Expect-Approx (DNum $row[2]) (-5.0/3000.0) 1e-9 "op-divider-crlf-bom" "i(V1)"
}

# 3. .op with all four controlled sources (E F G H) against the closed form.
Run-Case "op-controlled-sources" {
    $deck = Write-Deck "op3" @(
        "controlled sources",
        "V1 a 0 1", "R1 a b 1k", "Rload b 0 2k",
        "E1 c 0 b 0 3", "R2 c d 500", "G1 d 0 a 0 0.5",
        "H1 e 0 V1 100", "R3 e 0 1k",
        "F1 0 f V1 2", "R4 f 0 100",
        ".op", ".end")
    $r = Invoke-Spice @("run", $deck)
    Expect-True ($r.Exit -eq 0) "op-ctrl" "exit code $($r.Exit), want 0"
    $t = ConvertTo-Table $r.Out
    $row = $t.Rows[0]
    $iv1 = -1.0/3000.0
    if ($t.Rows.Count -gt 0) {
        Expect-Approx (DNum $row[(ColOf $t "v(a)")]) 1.0 1e-9 "op-ctrl" "v(a)"
        Expect-Approx (DNum $row[(ColOf $t "v(b)")]) (2.0/3.0) 1e-9 "op-ctrl" "v(b)"
        Expect-Approx (DNum $row[(ColOf $t "v(c)")]) 2.0 1e-9 "op-ctrl" "v(c)"
        Expect-Approx (DNum $row[(ColOf $t "v(d)")]) -248.0 1e-9 "op-ctrl" "v(d)"
        Expect-Approx (DNum $row[(ColOf $t "v(e)")]) (100.0*$iv1) 1e-9 "op-ctrl" "v(e)"
        Expect-Approx (DNum $row[(ColOf $t "v(f)")]) (2.0*$iv1*100.0) 1e-9 "op-ctrl" "v(f)"
        Expect-Approx (DNum $row[(ColOf $t "i(V1)")]) $iv1 1e-9 "op-ctrl" "i(V1)"
        Expect-Approx (DNum $row[(ColOf $t "i(E1)")]) -0.5 1e-9 "op-ctrl" "i(E1)"
        Expect-Approx (DNum $row[(ColOf $t "i(H1)")]) (-100.0*$iv1/1000.0) 1e-9 "op-ctrl" "i(H1)"
    }
}

# 4. .op subckt flattening with scoped node names and param overrides.
Run-Case "op-subckt-params" {
    $deck = Write-Deck "op4" @(
        "amp stage",
        ".param rgain=2k",
        ".subckt divider n1 n2 n3 params: rtop=1k rbot=1k",
        "Rt n1 nm {rtop}",
        "Rb nm n2 {rbot}",
        ".ends",
        "X1 in out 0 divider params: rtop=3k rbot=1k",
        "V1 in 0 DC 4",
        "R2 out 0 5k",
        ".op", ".end")
    $r = Invoke-Spice @("run", $deck)
    Expect-True ($r.Exit -eq 0) "op-subckt" "exit code $($r.Exit), want 0"
    $t = ConvertTo-Table $r.Out
    Expect-Header $t "v(in),v(X1:nm),v(out),i(V1)" "op-subckt"
    if ($t.Rows.Count -gt 0) {
        $row = $t.Rows[0]
        Expect-Approx (DNum $row[0]) 4.0 1e-9 "op-subckt" "v(in)"
        Expect-Approx (DNum $row[1]) (8.0/3.0) 1e-9 "op-subckt" "v(X1:nm)"
        Expect-Approx (DNum $row[2]) (20.0/9.0) 1e-9 "op-subckt" "v(out)"
        Expect-Approx (DNum $row[3]) (-4.0/9000.0) 1e-9 "op-subckt" "i(V1)"
    }
}

# 5. .dc linear sweep: exact point grid, closed-form divider at both ends.
Run-Case "dc-linear" {
    $deck = Write-Deck "dc1" @(
        "dc sweep", "V1 in 0 1", "R1 in mid 1k", "R2 mid 0 2k",
        ".dc V1 0 1.2 0.3", ".end")
    $r = Invoke-Spice @("run", $deck)
    Expect-True ($r.Exit -eq 0) "dc-linear" "exit code $($r.Exit), want 0"
    $t = ConvertTo-Table $r.Out
    Expect-Header $t "V1,v(in),v(mid),i(V1)" "dc-linear"
    Expect-True ($t.Rows.Count -eq 5) "dc-linear" "row count $($t.Rows.Count), want 5"
    if ($t.Rows.Count -eq 5) {
        Expect-Approx (DNum $t.Rows[0][0]) 0.0 1e-12 "dc-linear" "row 0 sweep value"
        $last = $t.Rows[4]
        Expect-Approx (DNum $last[0]) 1.2 1e-9 "dc-linear" "row 4 sweep value"
        Expect-Approx (DNum $last[2]) 0.8 1e-9 "dc-linear" "row 4 v(mid)"
        Expect-Approx (DNum $last[3]) -0.0004 1e-9 "dc-linear" "row 4 i(V1)"
    }
}

# 6. .dc two nested sweeps: inner source cycles fastest, 3x2 grid.
Run-Case "dc-nested" {
    $deck = Write-Deck "dc2" @(
        "nested",
        "V1 in 0 DC 5", "R1 in mid 1k", "R2 mid 0 2k", "I1 mid 0 DC 0",
        ".dc V1 0 4 2 I1 0 1m 1m", ".end")
    $r = Invoke-Spice @("run", $deck)
    Expect-True ($r.Exit -eq 0) "dc-nested" "exit code $($r.Exit), want 0"
    $t = ConvertTo-Table $r.Out
    Expect-Header $t "V1,I1,v(in),v(mid),i(V1)" "dc-nested"
    Expect-True ($t.Rows.Count -eq 6) "dc-nested" "row count $($t.Rows.Count), want 6"
    if ($t.Rows.Count -eq 6) {
        $last = $t.Rows[5]
        Expect-Approx (DNum $last[0]) 4.0 1e-9 "dc-nested" "row 5 V1"
        Expect-Approx (DNum $last[1]) 0.001 1e-9 "dc-nested" "row 5 I1"
        Expect-Approx (DNum $last[(ColOf $t "v(mid)")]) 2.0 1e-9 "dc-nested" "row 5 v(mid)"
    }
}

# 7. .tran RC step against 1 - e^{-t/RC} at the default tolerances.
Run-Case "tran-rc" {
    $deck = Write-Deck "tr1" @(
        "rc step",
        "V1 in 0 PULSE(0 1 1u 1n 1u 10m 10m)",
        "R1 in out 1k", "C1 out 0 1u",
        ".tran 10u 5m", ".end")
    $r = Invoke-Spice @("run", $deck)
    Expect-True ($r.Exit -eq 0) "tran-rc" "exit code $($r.Exit), want 0"
    $t = ConvertTo-Table $r.Out
    Expect-Header $t "time,v(in),v(out),i(V1)" "tran-rc"
    Expect-True ($t.Rows.Count -gt 10) "tran-rc" "row count $($t.Rows.Count), want > 10"
    $vout = (ColOf $t "v(out)")
    if ($t.Rows.Count -gt 0) {
        $r1 = Find-RowNear $t.Rows 0.001001
        Expect-Approx (DNum $r1[$vout]) 0.632120558828 1e-3 "tran-rc" "v(out) at ~1.001ms (1 tau)"
        $r3 = Find-RowNear $t.Rows 0.003001
        Expect-Approx (DNum $r3[$vout]) 0.950212931632 1e-3 "tran-rc" "v(out) at ~3.001ms (3 tau)"
    }
}

# 8. .ac RC low-pass: re()/im() columns against 1/(1 + jwRC) on every row.
Run-Case "ac-rc-lp" {
    $deck = Write-Deck "ac1" @(
        "lp",
        "V1 in 0 DC 0 AC 1",
        "R1 in out 1k", "C1 out 0 1u",
        ".ac dec 4 1 1e6", ".end")
    $r = Invoke-Spice @("run", $deck)
    Expect-True ($r.Exit -eq 0) "ac-rc" "exit code $($r.Exit), want 0"
    $t = ConvertTo-Table $r.Out
    Expect-Header $t "frequency,re(v(in)),im(v(in)),re(v(out)),im(v(out)),re(i(V1)),im(i(V1))" "ac-rc"
    Expect-True ($t.Rows.Count -gt 10) "ac-rc" "row count $($t.Rows.Count), want > 10"
    $cre = (ColOf $t "re(v(out))")
    $cim = (ColOf $t "im(v(out))")
    foreach ($row in $t.Rows) {
        $w = 2.0 * [math]::PI * (DNum $row[0])
        $x = $w * 1e-3
        $den = 1.0 + $x * $x
        Expect-Approx (DNum $row[$cre]) (1.0/$den) 1e-6 "ac-rc" ("re(v(out)) at f=" + $row[0])
        Expect-Approx (DNum $row[$cim]) (-$x/$den) 1e-6 "ac-rc" ("im(v(out)) at f=" + $row[0])
    }
}

Write-Host "== output plumbing =="

# 9. -o writes the same CSV to a file.
Run-Case "csv-file-output" {
    $deck = Write-Deck "op1b" @(
        "divider", "V1 in 0 DC 5", "R1 in mid 2k", "R2 mid 0 1k",
        ".op", ".end")
    $outPath = Join-Path $Work "file-out.csv"
    $r = Invoke-Spice @("run", $deck, "-o", $outPath)
    Expect-True ($r.Exit -eq 0) "csv-file" "exit code $($r.Exit), want 0"
    $haveFile = (Test-Path $outPath)
    Expect-True $haveFile "csv-file" "-o file was not created"
    if ($haveFile) {
        $lines = @([System.IO.File]::ReadAllLines($outPath))
        $t = ConvertTo-Table $lines
        if ($t.Rows.Count -gt 0) {
            Expect-Approx (DNum $t.Rows[0][1]) (5.0/3.0) 1e-9 "csv-file" "v(mid)"
        }
    }
}

# 10. .include resolves relative to the including deck's directory.
Run-Case "include-resolution" {
    Write-Deck "sub" @("Rsub out 0 1k") | Out-Null
    $deck = Write-Deck "inc" @(
        "include main",
        "V1 in 0 5", "R1 in out 1k",
        ".include sub.cir",
        ".op", ".end")
    $r = Invoke-Spice @("run", $deck)
    Expect-True ($r.Exit -eq 0) "include" "exit code $($r.Exit), want 0"
    $t = ConvertTo-Table $r.Out
    if ($t.Rows.Count -gt 0) {
        Expect-Approx (DNum $t.Rows[0][(ColOf $t "v(out)")]) 2.5 1e-9 "include" "v(out)"
    }
}

# 11. Unknown .options keys warn on stderr and never fail the run.
Run-Case "options-warn-never-fails" {
    $deck = Write-Deck "opt" @(
        "options warn",
        "V1 in 0 5", "R1 in 0 1k",
        ".op", ".options bogus=1", ".end")
    $r = Invoke-Spice @("run", $deck)
    Expect-True ($r.Exit -eq 0) "opt-warn" "exit code $($r.Exit), want 0"
    Expect-True ($r.Err.Trim() -ne "") "opt-warn" "expected a warning on stderr, got none"
    $t = ConvertTo-Table $r.Out
    if ($t.Rows.Count -gt 0) {
        Expect-Approx (DNum $t.Rows[0][(ColOf $t "v(in)")]) 5.0 1e-9 "opt-warn" "v(in)"
    }
}

Write-Host "== exit codes =="

# 12. DESIGN.md 9.5: 0 ok / 1 deck or simulation error / 2 usage or file I/O.
Run-Case "exit-codes" {
    $missing = Invoke-Spice @("run", (Join-Path $Work "does-not-exist.cir"))
    Expect-True ($missing.Exit -eq 2) "exit-missing" "exit code $($missing.Exit), want 2"

    $deck = Write-Deck "op1c" @(
        "divider", "V1 in 0 DC 5", "R1 in mid 2k", "R2 mid 0 1k",
        ".op", ".end")
    $badopt = Invoke-Spice @("run", $deck, "-x")
    Expect-True ($badopt.Exit -eq 2) "exit-badopt" "exit code $($badopt.Exit), want 2"

    $bad = Write-Deck "badcard" @("bad deck", "R1 a b", ".op", ".end")
    $badrun = Invoke-Spice @("run", $bad)
    Expect-True ($badrun.Exit -eq 1) "exit-baddeck" "exit code $($badrun.Exit), want 1"

    $dirDeck = Write-Deck "direction" @(
        "direction", "V1 in 0 5", "R1 in 0 1k",
        ".dc V1 0 1 -0.5", ".end")
    $dirRun = Invoke-Spice @("run", $dirDeck)
    Expect-True ($dirRun.Exit -eq 1) "exit-dcdir" "exit code $($dirRun.Exit), want 1"
}

# --- summary ---------------------------------------------------------------------

Write-Host ""
Write-Host ("smoke: {0} passed, {1} failed  ({2})" -f $script:Pass, $script:Fail, $Spice)
if ($script:Fail -gt 0) {
    Write-Host ""
    Write-Host "failures:" -ForegroundColor Red
    foreach ($f in $script:Failures) { Write-Host "  - $f" -ForegroundColor Red }
    if ($KeepWork) {
        Write-Host ""
        Write-Host "decks kept at: $Work" -ForegroundColor Yellow
    }
    exit 1
}
if (-not $KeepWork) { Remove-Item -Recurse -Force $Work -ErrorAction SilentlyContinue }
else { Write-Host "decks kept at: $Work" -ForegroundColor Yellow }
exit 0
