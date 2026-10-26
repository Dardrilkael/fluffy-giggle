# ws::app - CLAUDE.md

The headless app-framework lib (nextplan stage 13): the command
registry, shortcut resolution with user rebinding (the resolution
engine landed; keymap persistence through the settings seams is the
arc's next phase), and the undo stack - the services an app shell
composes. Riding ws::core (Result/Status); ws::settings joins when
keymap persistence lands (its FileSystem + json seams are the
workspace's standard, the platform precedent). The UI views over
these - menu/toolbar builders, the command palette, the keymap editor
- live in ws::ui, never here. Pure std; owner-thread state, no locks.

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
- `shortcut.hpp` - the resolver: the live binding table over the
  registry's declared defaults, plus user rebinding. `KeyContext` is
  the ladder (global < panel < text_editing): the caller reports the
  highest ACTIVE rung per key press, the walk goes from there down to
  global, and rungs above the active one never fire. THE TEXT RULE:
  while a field holds focus, a lower-rung binding fires only when its
  chord carries a modifier beyond shift (shift alone is typing) - an
  unmodified key is the field's to type. Conflicts are reported at
  bind time (`AppError::conflict`): one chord in one context is either
  a plain binding or a sequence prefix, never both; the same chord in
  another context is the point of contexts and never conflicts; a
  sequence's SECOND never conflicts, because a pending step outranks
  any plain binding on that chord. Two-step sequences arm on the
  first chord, complete on the second within a timeout measured on
  the injected ms clock (the platform FileWatch shape - FakeClock in
  the suite), and a first chord equal to its second is the legal
  double tap. `rebind` moves a command's chord within one context
  (key 0 removes it; a collision with another command or a prefix is
  refused), `install_defaults` seeds a registry's declared chords
  whole-or-not-at-all, `reset_to_defaults` restores the last seed,
  `binding_for`/`command_for` feed the menu hints and the keymap
  editor. `kPrimaryModifier` is the Ctrl-vs-Cmd convention: control
  on GLFW/Windows today, a mac bridge remaps chords to `super_key`.
  `Resolution` tells the caller what to dispatch AND whether the
  chord was consumed (a match, or an armed first step) so a focused
  field knows not to type it.

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
- The resolver keeps NO binding provenance: `install_defaults`
  re-seeds a context's whole live table (rebinds made on the old one
  are lost) and `reset_to_defaults` restores the last seed. Keymap
  persistence (the arc's next phase) saves the whole live table, so
  no override tracking is needed.
- A pending sequence step holds its rung across a focus change: it
  completes there until it times out or another chord cancels it -
  the timeout bounds a stale pending; there is no focus-change rule.
- `describe_chord`'s key names are a FINITE GLFW table (printables
  plus the named keys); any other code prints "<key N>". They are
  for conflict messages, never for persistence.
- One sequence per first chord per context: two sequences sharing a
  first chord would need second-chord disambiguation, which nobody
  asked for yet.

## Tests

`test_app.cpp` - the Mod helpers' composition and tests, the
registration round trip (every field survives the move), the three
empty-field refusals and the duplicate id (category/code asserted,
nothing stored on refusal), the dispatch round trip with the argument
payload (the default empty and an explicit one), the not-found
refusal, the direct-dispatch-ignores-enabled contract, the predicates
read live per call (empty = always/never), and the registration-order
walk. The resolver: the bind/resolve/lookup round trip, the bind
refusals (empty command, keyless chord, same-context collision with
the message naming chord and command, same chord across contexts OK),
the plain-vs-prefix exclusivity in both directions plus the duplicate
prefix and non-positive timeout, the sequence lifecycle on a fake
clock (arm, complete, expiry falling through to the plain binding
underneath, re-arm, wrong-chord cancel, cancel_pending, a pending
step outranking a plain binding on the second chord, and the double
tap), the context ladder (three rungs answering on their own focus,
rungs above the active one inert for plains AND prefixes), the text
rule (plain and shift-only chords left for the field, ctrl-modified
fired, the text rung's own unmodified binding fired and dormant
outside it), the pending step surviving a focus change, the rebind
rules (collision refused naming the owner, move frees the old chord,
no-op onto its own chord, onto-prefix refused, key-0 removal, the
not-found refusal), and install_defaults (declared chords seeded,
chordless commands skipped, reset after a rebind, the re-seed
replacing the live table, the clash seeding nothing with the message
naming both commands). The undo stack grows its own section as the
arc's later phases land.
