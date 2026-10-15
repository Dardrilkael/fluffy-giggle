<#
.SYNOPSIS
  Reorganiza src/ do ws::ui em pastas por camada: app, platform, gfx, render, text, gui, widgets.
  (platform e gfx sao os futuros libs ws::platform e ws::gfx; render/text/gui/app ficam no ui.)

.PARAMETER Root
  Raiz do repositorio (onde ficam src/, include/, tests/).

.PARAMETER Phase
  Moves    = so git mv (rode, compile nada, faca commit)
  Includes = so reescreve #include "..." dos headers movidos
  All      = as duas fases em sequencia (padrao)

.PARAMETER DryRun
  Nao altera nada, so mostra o que seria feito.

.EXAMPLE
  .\reorganize-ui-src.ps1 -Root E:\ws-ui -DryRun
  .\reorganize-ui-src.ps1 -Root E:\ws-ui -Phase Moves
  git commit -am "ui: move src files into layer folders"
  .\reorganize-ui-src.ps1 -Root E:\ws-ui -Phase Includes
#>
[CmdletBinding()]
param(
    [string]$Root = ".",
    [ValidateSet("Moves", "Includes", "All")]
    [string]$Phase = "All",
    [string[]]$ScanDirs = @("src", "include", "tests"),
    [switch]$DryRun,
    [switch]$Force
)

Set-Location $Root
$src = "src"

if (-not (Test-Path $src)) { Write-Error "Pasta '$src' nao encontrada em $(Get-Location)"; exit 1 }

# ---------------------------------------------------------------- layout
# pasta (dentro de src/) -> arquivos que vao para ela (hoje na raiz de src/)
#
# Notas:
#  - app/      : so a cola (unico lugar que conhece platform + gfx + ui)
#  - platform/ : janela GLFW/Android (futuro ws::platform; unico lugar com GLFW)
#  - gfx/      : camada de device Vulkan (futuro ws::gfx). Hoje so tem vma_impl.cpp;
#                o resto do device ainda esta dentro de renderer2d.cpp e sera
#                extraido no passo 2 (nao ha o que mover agora).
#  - render/   : renderer 2D construido sobre o gfx (fica no ui)
#  - text/     : font + atlas + stb (fica no ui)
#  - gui/      : modo imediato; src/gui/*.cpp ja estao la
#  - widgets/  : os wrappers ImGui/ImPlot por tras do ws/ui/widgets.hpp
#                publico - a camada ImGui, distinta do gui imediato (que
#                nao tem ImGui nenhum)
#  - input/    : nao tem arquivos em src/ hoje (eventos/InputFrame sao headers publicos
#                em include/ws/ui); a pasta nasce no passo 2/3. gui_input.cpp fica no gui.
$layout = [ordered]@{
    "app"      = @("app.cpp")
    "platform" = @("window.h", "window_glfw.cpp", "window_android.cpp")
    "gfx"      = @("vma_impl.cpp")
    "render"   = @("renderer2d.h", "renderer2d.cpp",
                   "batch_core.h", "batch_core.cpp",
                   "shader_provider.h", "shader_provider.cpp",
                   "rounded_texture.h", "rounded_texture.cpp")
    "text"     = @("font.h", "font.cpp", "atlas_packer.h", "atlas_packer.cpp",
                   "stb_impl.cpp")
    "gui"      = @("gui.h")
    "widgets"  = @("widgets.cpp")
}

# nome do header -> caminho novo relativo a src/ (para reescrever includes)
$headerMap = @{}
foreach ($dir in $layout.Keys) {
    foreach ($f in $layout[$dir]) {
        if ($f -match '\.(h|hpp)$') { $headerMap[$f] = "$dir/$f" }
    }
}
$headerMap["spirv.h"] = "render/shaders/spirv.h"

# ---------------------------------------------------------------- helpers
function Test-Tracked([string]$path) {
    git ls-files --error-unmatch -- $path *> $null
    return ($LASTEXITCODE -eq 0)
}

function Invoke-Move([string]$from, [string]$to) {
    if (-not (Test-Path $from)) { Write-Warning "ausente, ignorado: $from"; return }
    if (Test-Path $to)          { Write-Warning "destino ja existe, ignorado: $to"; return }
    if ($DryRun) { Write-Host "[dry] $from -> $to"; return }

    New-Item -ItemType Directory -Force -Path (Split-Path $to -Parent) | Out-Null
    if (Test-Tracked $from) { git mv -- $from $to }
    else                    { Move-Item -LiteralPath $from -Destination $to }
    Write-Host "moved $from -> $to"
}

# ---------------------------------------------------------------- fase 1: moves
if ($Phase -in @("Moves", "All")) {
    if (-not $Force -and -not $DryRun) {
        $dirty = git status --porcelain
        if ($dirty) {
            Write-Error "Working tree com alteracoes nao commitadas. Faca commit/stash ou use -Force."
            exit 1
        }
    }

    foreach ($dir in $layout.Keys) {
        foreach ($f in $layout[$dir]) {
            Invoke-Move "$src/$f" "$src/$dir/$f"
        }
    }
    # shaders (pasta inteira)
    Invoke-Move "$src/shaders" "$src/render/shaders"
}

# ---------------------------------------------------------------- fase 2: includes
if ($Phase -in @("Includes", "All")) {
    $names   = ($headerMap.Keys | ForEach-Object { [regex]::Escape($_) }) -join "|"
    # #include "[qualquer/prefixo/]nome.h"  ->  #include "pasta/nome.h"
    $pattern = '(#\s*include\s*")(?:[^"]*/)?(' + $names + ')(")'
    $utf8    = New-Object System.Text.UTF8Encoding($false)
    $changed = 0

    $files = foreach ($d in $ScanDirs) {
        if (Test-Path $d) {
            Get-ChildItem -Path $d -Recurse -File -Include *.h, *.hpp, *.hh, *.inl, *.c, *.cc, *.cpp
        }
    }

    foreach ($file in $files) {
        $text = [System.IO.File]::ReadAllText($file.FullName)
        $new  = [regex]::Replace($text, $pattern, {
            param($m)
            $m.Groups[1].Value + $headerMap[$m.Groups[2].Value] + $m.Groups[3].Value
        })
        if ($new -ne $text) {
            $changed++
            if ($DryRun) { Write-Host "[dry] includes em $($file.FullName)" }
            else {
                [System.IO.File]::WriteAllText($file.FullName, $new, $utf8)
                Write-Host "includes atualizados: $($file.FullName)"
            }
        }
    }
    Write-Host "`n$changed arquivo(s) com includes reescritos."
}

# ---------------------------------------------------------------- relatorio de CMake
Write-Host "`n--- Referencias a caminhos antigos em CMake (ajuste manualmente) ---"
$allNames = @($headerMap.Keys) + ($layout.Values | ForEach-Object { $_ }) + @("shaders")
$cmakeFiles = Get-ChildItem -Recurse -File -Include CMakeLists.txt, *.cmake -ErrorAction SilentlyContinue |
              Where-Object { $_.FullName -notmatch '[\\/](build|out|\.git)[\\/]' }
foreach ($cf in $cmakeFiles) {
    $hits = Select-String -Path $cf.FullName -SimpleMatch -Pattern ($allNames | Sort-Object -Unique) -ErrorAction SilentlyContinue
    foreach ($h in $hits) { Write-Host ("{0}:{1}: {2}" -f $cf.Name, $h.LineNumber, $h.Line.Trim()) }
}

Write-Host "`nLembrete: com os includes novos (\"render/renderer2d.h\" etc.), o CMake precisa de"
Write-Host "  target_include_directories(<alvo> PRIVATE `${CMAKE_CURRENT_SOURCE_DIR}/src)"
Write-Host "e qualquer caminho para src/shaders deve apontar para src/render/shaders."
