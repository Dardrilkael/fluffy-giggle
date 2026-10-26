# ws::app - CLAUDE.md

The headless app-framework lib (nextplan stage 13): the command
registry, shortcut resolution with user rebinding and the persisted
keymap file, and the undo stack - the services an app shell
composes. Riding ws::core (Result/Status) and ws::settings (the
keymap file's FileSystem + json seams - the platform precedent). The
UI views over these - menu/toolbar builders, the command palette, the
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
  `install` replaces every context's live table in one
  all-or-nothing batch (the keymap load's edge: the bind-time rules
  are the batch's, the defaults seed is untouched, an armed step is
  dropped), `table_of` copies one context's table out for the
  serializer and the editor's list view, and `binding_for`/
  `command_for` feed the menu hints and the keymap editor.
  `kPrimaryModifier` is the Ctrl-vs-Cmd convention: control
  on GLFW/Windows today, a mac bridge remaps chords to `super_key`.
  `Resolution` tells the caller what to dispatch AND whether the
  chord was consumed (a match, or an armed first step) so a focused
  field knows not to type it.
- `keymap.hpp` - the persisted keymap: the resolver's whole live
  table (every context's bindings and sequences, no provenance - the
  2a decision) saved and loaded through the ws::settings json +
  FileSystem seams, the recent-files precedent. The document is
  versioned shape 1 with all three contexts always present, so
  save -> load -> save re-emits byte-identical text (table order
  survives install and the json object keeps insertion order). A
  MISSING file is the first run: `load` succeeds with the resolver
  untouched - never wipe a seed because the file is not there yet
  (the app's flow is install_defaults() then load()). The refusal
  split: json grammar trouble answers parse_failed, a well-formed
  document of the wrong shape corrupt_file (absent "version" reads
  as shape 1; any other value is refused, never downgraded), an fs
  read failure io_failed - and semantic duplicates surface as the
  resolver's own conflict from install(), every refusal leaving the
  resolver exactly as it was. Unknown keys - a top-level field, an
  entry field, a context this version does not know - are forward
  data from a newer file: ignored, never a refusal. `save` returns
  the seam's write Status AS-IS (category "settings"), riding the
  seam's atomic sibling-tmp + rename, so a failed save leaves the
  previous file byte-identical. `export_text`/`import_text` move
  the same document as text for the editor's copy field and the
  file-dialog paths. No .bak: the keymap is re-editable convenience
  data over a re-seedable defaults table (the recent-files policy).
- `undo.hpp` - the bounded undo/redo history over host-defined
  commands (the command pattern: `UndoCommand` with apply/undo/name,
  the `merge_with` coalesce hook and the `memory_estimate` gauge).
  push IS the edit: the stack runs apply() and is the sole executor
  - the host never pre-applies - then records, dropping the redo
  branch (no redo past a fork), offering the new command to the top
  step's last command for coalescing (the TOP absorbs: its undo()
  reverts both, its estimate grows; the offer never crosses a step
  boundary), else appending a step named by the command.
  Transactions group many pushes into ONE named step (empty
  transaction: no step, the redo branch untouched); undo runs a
  step's commands in reverse, redo replays them forward. The save
  point drives `is_dirty` as a POSITION: undoing back onto it reads
  clean, redoing past it dirty, and it goes UNREACHABLE (dirty until
  `mark_saved`) when a push drops a branch it sits inside, an
  eviction eats it, or clear() erases it - one AT a dropped boundary
  survives: it is the current state. History is bounded by step
  count AND the estimates' sum, oldest out first, the newest step
  always kept (a lone over-limit step survives). Listeners fire
  after every state change - including a push inside an open
  transaction, never a begin or an empty end - in registration
  order, with removal mid-notify honored and addition mid-notify
  deferred (the settings observers' rule). undo/redo/`mark_saved`/
  `clear`/`set_limits` all refuse while a transaction is open.

## Allowed dependencies

- `ws::core` PUBLIC (Result/Status/errors - the public API returns
  them).
- `ws::settings` PUBLIC (the keymap file): the FileSystem + json
  seams - the recent-files/theme-file precedent, never a duplicated
  writer. PUBLIC like ws::core because the seam's reference lives in
  a public header (`Keymap` takes a `FileSystem&`), the ws::platform
  precedent.

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
  are lost) and `reset_to_defaults` restores the last seed. The
  keymap file (keymap.hpp) saves the whole live table for exactly
  this reason - no override tracking is needed.
- A pending sequence step holds its rung across a focus change: it
  completes there until it times out or another chord cancels it -
  the timeout bounds a stale pending; there is no focus-change rule.
- `describe_chord`'s key names are a FINITE GLFW table (printables
  plus the named keys); any other code prints "<key N>". They are
  for conflict messages, never for persistence.
- One sequence per first chord per context: two sequences sharing a
  first chord would need second-chord disambiguation, which nobody
  asked for yet.
- The undo command's apply() runs on push AND on every redo, so a
  command must re-capture its inverse (the prior value) inside
  apply() - never once at construction: at redo time the document
  sits exactly where it sat at push, and the captured inverse must
  be the one valid THERE.
- The merge hook runs top-absorbs-new: the surviving object's
  undo() reverts both edits and its memory_estimate grows with the
  absorbed payload; the newer object is destroyed, already applied.
  The default estimate is the BASE size (sizeof(UndoCommand)): the
  stack cannot see the derived object, so a host carrying payload
  overrides it.
- The save point is a position, not a step: it survives undo/redo
  freely, dies only where its position becomes unreachable (a
  dropped branch it sits inside, an eviction, clear()) - and one AT
  a dropped redo boundary survives, being the current state. clear()
  also drops it: a cleared document reads dirty until mark_saved
  (the switch flow: clear(), load, mark_saved()).
- `mark_saved`/`clear`/`set_limits` return Status, not void, purely
  so the open-transaction refusal is reportable - the same
  invalid_argument undo()/redo() answer.

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
naming both commands). The keymap: install's whole-or-nothing batch
(table_of mirroring every rung, the in-batch clash / keyless chord /
plain-plus-prefix / zero-timeout refusals each leaving the live
table standing, an installed sequence arming and completing), the
file round-trip (a user rebind surviving the trip, a seeded reader
replaced not merged, the byte-identical save -> load -> save), the
missing-file first run, the refusal matrix (grammar app/4 vs shape
app/5 - a non-object root, wrong-typed or unsupported version,
contexts or a context of the wrong kind, an entryless binding, key
0, mods 16, timeout 0 - each leaving the seed standing) plus the
forward-data tolerance doc, the in-file clash answered as the
resolver's own conflict, the failed save propagating the seam's
settings/8 with the previous file byte-identical, import/export as
text, and a real StdFileSystem smoke in the OS temp. The undo
stack: push-as-apply with the undo/redo replay and the
empty-branch not_found refusals plus the default-estimate floor,
the coalescing hook (a same-kind run collapsing to one step whose
single undo reverts everything and whose estimate grows with the
absorbed payloads; a different kind never coalesces), a push past
a fork dropping the redo branch, the transaction group (one named
step, the reverse-order undo proven by a per-command log, the
forward replay, the mid-transaction refusals for undo/redo/
mark_saved/clear/set_limits/null-push/begin-while-open, the empty
transaction recording no step, and the merge hook never crossing
the transaction's boundary), the save point across undo/redo in
both directions - a push at a clean position keeps it (it sits AT
the boundary, not inside the dropped branch) while one inside the
dropped branch goes unreachable - the count bound (the evicted
step as the permanent undo floor, and a save point at the base
position going unreachable under an eviction), the memory bound
over the estimates' sum (oldest out first, a lone over-limit step
surviving, set_limits trimming immediately), the listeners (a
fire per change including an in-transaction push but never a
begin or an empty end, registration order, removal mid-notify
honored, addition mid-notify deferred, the empty-callback
invalid_argument, removal by id answering true/false), and clear()
dropping the history and the save point with the document keeping
its text.
