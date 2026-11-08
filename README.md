# ws — C++20 Cross-Platform Framework

A C++20 monorepo for building native applications: a self-contained GUI framework, a SPICE circuit simulator, a physics engine abstraction, networking, audio, and more — all behind clean `ws::` namespace boundaries with zero third-party leakage into app code.

## Features

- **Self-contained GUI framework** (`ws::ui`) — widget system, canvas layer, docking shell, theme engine with skins, immediate-mode GUI over a retained renderer, Vulkan-backed 2D renderer
- **SPICE circuit simulator** (`ws::spice`) — netlist parser, MNA solver, `.op`/`.dc`/`.tran`/`.ac` analyses, diode/BJT/MOSFET device models
- **Schematic editor** (`ws::circsim`) — `.csim` format, graph-based net building, ERC, symbol libraries, headless SVG render
- **Physics engine** (`ws::physics`) — unified 2D/3D API over Box2D and Jolt (both private backends)
- **Networking** (`ws::net`) — event loop, framed TCP, UDP, LAN discovery
- **Cross-platform** — Windows (desktop), Linux (headless subset), Android (NDK, arm64/armeabi-v7a/x86/x86_64)
- **Vulkan graphics layer** (`ws::gfx`) — device/queue selection, VMA allocator, swapchain, shader module helpers

## Architecture

```
libs/
  core      - logging, asserts, Result<T>/Status error types
  math      - Matrix<T>, LU decomposition, sparse solver, vecmath
  input     - GLFW-free event types + InputFrame (touch/mouse unified)
  gfx       - Vulkan device layer (volk/VMA, all private)
  ui        - app shell + widgets + renderer + canvas + GUI + docking shell
  audio     - WAV decode + OpenAL Soft playback
  net       - event loop, framed TCP, UDP, LAN discovery
  physics   - 2D + 3D physics (Box2D/Jolt, private)
  jobs      - thread pool with priorities, cancellation, main-thread completion
  settings  - typed schema, layered sources, JSON persistence
  platform  - locations, file dialogs, clipboard, autosave, single instance
  app       - command registry, shortcut resolver, undo stack
  spice     - SPICE simulator (parser, MNA, analyses, devices)
  circsim   - schematic data model, .csim format, ERC, symbol library
apps/
  circsim_editor  - schematic capture + simulation (desktop + Android)
  ui_gallery      - widget showcase (desktop + Android)
  spice           - SPICE CLI
  circsim_cli     - circsim CLI (run/erc/netlist/symbols)
  bounce          - physics + UI + audio demo (desktop + Android)
  chatter         - LAN chat (desktop + Android)
  ...             - playground, shell_example, netchat, netdemo, wavplayer, bench_physics
```

### The one workspace rule: dependencies never leak into apps

- Apps include only `ws/...` headers and link only `ws::` targets
- Every third-party target is linked PRIVATE by the `ws_*` library that wraps it
- No third-party header appears under any `libs/*/include/`
- Enforced by `tools/check_deps.ps1`, registered as a ctest

## Quick Start

### Prerequisites

- **CMake** 3.28+
- **Ninja** build system
- **C++20 compiler**: MSVC (Windows), GCC 12+ (Linux), or NDK r25+ (Android)
- **PowerShell** (for `tools/` scripts and `check_deps` test)

### Windows (MSVC + Ninja)

```bash
# Open a shell with the MSVC environment loaded
powershell -File tools/dev-shell.ps1

# Configure and build
cmake --preset debug
cmake --build --preset debug

# Run tests
ctest --preset debug
```

### Linux (GCC, headless subset)

```bash
cmake --preset linux
cmake --build --preset linux
ctest --preset linux
```

### Linux with sanitizers (ASan + UBSan)

```bash
cmake --preset linux-asan
cmake --build --preset linux-asan
ctest --preset linux-asan
```

### Android (NDK)

```bash
# Set ANDROID_NDK_HOME, then:
cmake --preset android          # arm64-v8a
cmake --build --preset android

# Package an APK
powershell -File tools/package-android.ps1 -App circsim_editor
```

### Build a subset of libs

```bash
# Only build core + math + spice
cmake --preset spice-debug --fresh -DWS_BUILD_LIBS="core;math;spice"
cmake --build --preset spice-debug
```

## Applications

| App | Description | Platforms |
|-----|-------------|-----------|
| `circsim_editor` | Schematic capture + SPICE simulation with plot results | Desktop, Android |
| `ui_gallery` | Full widget set showcase (themes, skins, commands, platform) | Desktop, Android |
| `bounce` | Physics + UI + audio composition demo | Desktop, Android |
| `chatter` | LAN chat over ws::net with tap-phrase palette | Desktop, Android |
| `spice` | SPICE CLI (`run`, `check`, `--format raw`, `--keep-going`) | Desktop |
| `circsim_cli` | Circsim CLI (`run`, `erc`, `netlist`, `symbols`) | Desktop |
| `shell_example` | AppShell acceptance app (text + plot scratchpad) | Desktop |
| `playground` | Shell smoke-test app | Desktop |

## Error Handling

All public APIs return `ws::core::Result<T>` or `Status` — no exceptions cross library boundaries. Each library defines its own error enum and converts to `ws::core::Error` at its public boundary.

## License

MIT — see [LICENSE](LICENSE).

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for coding conventions, build/test instructions, and PR guidelines.

## Project Structure

Each `libs/<name>/` directory has its own `CLAUDE.md` with detailed rules, API surface, and test documentation. The root `CLAUDE.md` provides the workspace-wide conventions. Design documents live in `docs/` and per-library design docs (e.g., `libs/spice/DESIGN.md`).
