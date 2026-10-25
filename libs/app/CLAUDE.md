# ws::app - CLAUDE.md

The headless app-framework lib (nextplan stage 13): the command
registry, shortcut resolution with user rebinding, and the undo stack -
the services an app shell composes. Riding ws::core (Result/Status);
ws::settings joins when keymap persistence lands (its FileSystem + json
seams are the workspace's standard, the platform precedent). The UI
views over these - menu/toolbar builders, the command palette, the
keymap editor - live in ws::ui, never here. Pure std; owner-thread
state, no locks.

## Purpose, per header

- `errors.hpp` - the lib's error codes (the settings shape: append-only
  int32 enum, describe(), fail() -> unexpected<Error>, category "app").
  not_found/conflict/parse_failed/corrupt_file/io_failed are named for
  the arc's later phases (dispatching an unknown id now, then the
  shortcut resolver's registration-time conflict and the keymap file's
  parse/corrupt/io stories); the table is complete the day the lib
  lands.
- `command.hpp` - the registry. `Mod`/`Shortcut` are the chord as DATA:
  key codes ride as plain ints in GLFW numbering (ws::input's rule) and
  the modifier bits mirror GLFW_MOD_* (shift 1, control 2, alt 4, super
  8), so ws::ui's bridge is a pass-through with no mapping table.
  `Command` carries the stable string id (menus and keymap persistence
  name commands by it - renaming one orphans user bindings), label,
  category, an icon id the ui maps, the declared default chord, the
  needs_argument flag the palette prompts on, enabled/checked
  predicates the views read per frame (empty = always enabled / never
  checked), and the execute callback taking the argument payload (""
  = none). `CommandRegistry` is caller-owned: `add` refuses the empty
  id/label/callback and the duplicate id (all invalid_argument -
  registration bugs, not runtime failures), `execute` dispatches by id
  answering not_found for the unknown, `find`/`commands()` feed the
  builders (nullptr skips an unresolvable menu entry; registration
  order is the stable walk).

## Allowed dependencies

- `ws::core` PUBLIC (Result/Status/errors - the public API returns
  them).
- `ws::settings` (planned, with keymap persistence): the FileSystem +
  json seams for the keymap file - the recent-files/theme-file
  precedent, never a duplicated writer.

## Rules

- Owner-thread state, no locks (the jobs/settings/platform rule):
  registration at setup, queries and dispatch on the frame; nothing
  here is callable from render or audio callbacks.
- Result/Status only, no exceptions across the boundary (root
  CLAUDE.md).
- Command ids are the persistence key: stable across refactors, or the
  user's bindings orphan (the price is documented, not prevented).
- The ui views stay in ws::ui; this lib never includes a window, gui
  or input header.

## Known tradeoffs

- `add`/`find` are linear scans: registries hold dozens of commands,
  not thousands - a map would cost more than it answers.
- `execute` does NOT consult enabled(): the views gate their chrome on
  it, but a direct dispatch is the caller's decision - programmatic
  invokes are how buttons and palette picks run.
- A command body carries no error channel: the callback is void, and a
  failing action surfaces its own toast/error through app machinery -
  the registry's Result covers its own lookup only.
- The declared chord is inert data: live bindings, conflict detection
  and rebinding are the shortcut resolver's table (a later phase of
  this arc).
- `Mod::super_key` is spelled out because `super` reads as a keyword
  to unfamiliar eyes (it is not one in C++, but the spelling costs
  nothing and avoids the flinch).

## Tests

`test_app.cpp` - the Mod helpers' composition and tests, the
registration round trip (every field survives the move), the three
empty-field refusals and the duplicate id (category/code asserted,
nothing stored on refusal), the dispatch round trip with the argument
payload (the default empty and an explicit one), the not-found
refusal, the direct-dispatch-ignores-enabled contract, the predicates
read live per call (empty = always/never), and the registration-order
walk. The resolver and the undo stack grow their own sections as the
arc's later phases land.
