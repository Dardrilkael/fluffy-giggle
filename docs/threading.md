# The circsim threading contract

One page, written down so nobody has to reverse-engineer it from the
code. Scope: `ws::circsim` (the headless library the editor and the CLI
ride). `ws::spice` follows the same shape internally - its entry points
(`read_deck`, `build_deck_ast`, `run_deck`) share no mutable state.

## The singletons

`builtin_library()` and `builtin_symbols()` are function-local `static
const` objects. C++11 magic statics guarantee thread-safe, exactly-once
initialization; after that first call they are immutable. **Safe to call
from any thread, always.**

A `SymbolLibrary` you construct and `load_text()` yourself is NOT
protected: layer files into it on one thread, then share it. Nothing in
this library ever mutates a `SymbolLibrary` behind your back.

## Schematic: not thread-safe, by design

`Schematic` is a plain data model. Every mutation - `add_part`,
`add_wire`, `erase`-style editor commands, the `*_mut()` seams,
`restore_*`, direct field writes (`analysis`, `directives`, `temp`,
`view`, `grid`) - must happen-before any concurrent read, with the
synchronization done by the caller. There is no internal lock and there
will not be one: the editor's undo stack and the loaders are single
writers, and a lock would only hide races, not fix them.

The `*_mut()` accessors exist for the editor's undo commands (writers
that manage ids themselves); the invariants they must uphold are spelled
out in model.hpp. The loader does not use them - it rides `restore_*`,
which does the id/revision fixup itself.

## The read passes: safe to run concurrently

Given the happens-before above, all of these are **pure reads** of
`Schematic` (+ `SymbolLibrary` where passed) and may run on any number
of threads at once - no shared mutable state, no lazy caches, no
`static` scratch:

- `build_nets(s, symbols)`
- `run_erc(s, build, symbols)` (and the convenience overload)
- `export_deck(s, nets, ...)`
- `analysis_card(spec)`, `parse_analysis_spec(text)`
- `run_schematic(...)` - each call owns its deck, its spice run and its
  diagnostics end to end
- `locate_diag` / `focus_bounds`
- `save_csim(s)`

That is the contract that makes the editor's design sound: run ERC on a
worker thread while the UI keeps drawing, export a deck while another
simulation finishes - as long as the schematic is not being mutated
mid-flight. The editor's existing pattern (snapshot the revision, hand
the same `Schematic` to the worker, discard results if the revision
moved) is exactly right.

## What is NOT covered

- `load_csim` / `read_csim` WRITE a fresh `Schematic` (safe to build in
  parallel; hand the result over with proper synchronization).
- Anything taking a `ws::settings::FileSystem` follows that seam's own
  contract (its write is atomic by design).
- The `ws::ui` layer has its own single-thread frame model; nothing
  there is covered by this page.

## The locale rule (the sibling contract)

Number text that a deck, a netlist, or a `.csim`/`.csym` file will
re-read must come from `fmt_num` / `parse_plain_number`
(include/ws/circsim/text.hpp) - `std::to_chars`/`std::from_chars` in
the "C" locale. `std::snprintf("%g")` and `std::atof` answer to
LC_NUMERIC (a German locale turns `0.1` into `0,1` and silently corrupts
exported decks); `std::tolower` answers to LC_CTYPE, which is why the
shared `lower()` is ASCII-only. If you are writing a number whose text
will be re-parsed, or case-folding an identifier, use the shared
utilities - that is a threading-adjacent rule of the same kind: the
library's output must not depend on the environment it runs in.
