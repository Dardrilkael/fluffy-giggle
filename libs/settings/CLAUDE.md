# ws::settings - CLAUDE.md

The headless settings store (nextplan stage 9): a typed schema
(defaults + constraints), layered sources with provenance, change
observers, an in-house JSON facility, layer files folded in through a
versioned migration chain, and (phase 4) a minimal FileSystem seam on
top. Pure std + ws::core - no third-party anywhere, no platform
headers, no locks.

## Purpose, per header

- `errors.hpp` - the lib's error codes (the net/jobs shape: append-only
  int32 enum, describe(), fail() -> unexpected<Error>, category
  "settings"). parse_failed/migrate_failed/io_failed/corrupt_file are
  named for the later phases; they are part of the table from day one.
- `value.hpp` - `SettingType` (boolean/integer/real/text) +
  `SettingValue`, a variant-backed value that is exactly one of the four
  scalar types. No null state: absence is a missing entry, never a
  value. Typed getters answer Result; `to_text()` renders for messages.
- `schema.hpp` - `Constraint` (range/enum/regex; the reject-or-clamp
  policy lives on the range), `SettingDesc`, `Schema` (declare with full
  validation, find, introspection: descriptors/categories/in_category,
  the shape version via set_version/version) and `coerce()` - the one
  validation path set() and the file-load share. "version" is reserved
  at declare (the layer-file format owns it).
- `settings.hpp` - `Layer` (defaults < system < user < project <
  command_line; the rank IS the precedence) + `SettingsStore`: get<T>
  with layer provenance, set into one layer, layer_of/has_value/clear,
  observers keyed by exact key or dot-prefix, and the layer-file
  bridge: load_layer folds one parsed file into a layer (migrations
  first, unknowns remembered, all-or-nothing), save_layer emits the
  layer back out as one json object.
- `migration.hpp` - `MigrationStep` + `Migrations`: the versioned
  chain a load walks. Steps are keyed by the shape they migrate FROM
  (step n walks n -> n+1, so a gap in the chain is a refusal, never a
  guess); run() re-fetches the root after each step (a step may
  rebuild the document), propagates a step's own Status as-is, and
  stamps the schema's version when the walk lands. A file with no
  "version" field reads as shape 1; a file newer than the schema is
  refused, never downgraded.
- `json.hpp` - the in-house JSON facility (phase 2): `Value` (seven
  kinds, integer/real DISTINCT so a round trip never re-types a
  value), the insertion-order `Object`, `parse()` (strict grammar plus
  tolerances for comments and a leading UTF-8 BOM, every failure
  naming itself and its line/column, nesting capped at kMaxDepth=128)
  and `write()` (4-space indent, one entry per line, escaped strings,
  shortest-round-trip reals that always carry '.' or an exponent).
  Hand-rolled on purpose: a third-party parser (linked PRIVATE) was
  the alternative, but the settings-shaped subset fits one .cpp, and
  std-only keeps it building on every platform the store does.

## Allowed dependencies

- `ws::core` PUBLIC (Result/Status/errors - the public API returns them).
- Nothing else, ever: no third-party, no ws::jobs (observers run on the
  set() thread; marshalling to a gui thread is the caller's composition
  - ws::ui's App drains a JobSystem once per frame, post the set
  through it), no platform headers.

## Invariants and rules

- Owner-thread state, no locks (the jobs wait_* rule): every call runs
  on one thread; observers run on the thread that called set().
- defaults is the schema's layer: set(Layer::defaults, ...) answers
  invalid_argument, has_value(defaults, ...) is always false. Nothing
  ever writes into slot 0.
- get<T> accepts exactly bool / std::int64_t / double / std::string
  (static_assert); the key's declared type must match - a wrong-typed
  read answers type_mismatch, never a guessed default.
- set accepts any integral, floating or string-view-convertible value,
  narrowed through `detail::make_value` - but the narrowed value must
  MATCH the schema type: a 3 written into a real key answers
  type_mismatch. No silent conversions, either direction.
- Constraints run in declaration order; the first reject answers; a
  clamp rewrites the value and later constraints see the clamped one.
  Integer ranges clamp through double then llround (bounds are doubles;
  2^53 is the exactness ceiling, far above any settings value).
- Observers: a pattern matches when equal to the changed key, or when it
  ends in '.' and the key starts with it. Delivery after the value
  landed, registration order, on the set() thread; notify snapshots then
  liveness-checks each entry, so removal during a notification is
  honored and observers registered mid-notification run from the next
  change. set() notifies on EVERY successful write, even when the value
  equals the old one. A re-entrant set from an observer recurses: do
  not build cycles.
- declare() validation: empty key, a key named "version" (reserved for
  the layer-file format), duplicate key, default type mismatch,
  constraint applicability (range on bool/text, enum entries of the
  wrong type, regex on non-text), empty enum, min > max, unparsable
  regex - all answer bad_schema, and a refused declare stores nothing.
- A file IS the layer. load_layer migrates the document to the
  schema's version first, then stages the whole file - a bad key
  anywhere fails the load naming the key, with the layer untouched
  (all-or-nothing). Staged values apply through set_value itself, so
  the one validation path runs, clamps land, and observers fire per
  applied key in the file's order. Unknown keys are remembered
  verbatim (as json) and re-emitted by save_layer; the reserved
  "version" field is consumed, never stored. What the layer held
  before a load is dropped: a key the file dropped reverts silently
  (the clear() contract - clear() notifies nobody; flagged as a gap).
- save_layer writes "version" first, then the layer's own values
  (alphabetical - they live in a map), then the remembered unknowns in
  the order the file carried them. An empty layer saves as just the
  version field. Load failures keep the inner error's code/category/
  location and only re-prefix the message ("load 'key': ...").

## Known tradeoffs

- `Schema::find` is a linear scan (schemas run to hundreds of keys; the
  index arrives when a profile asks).
- Regex constraints compile per check - set() is rare, get() is the hot
  path and touches none of this.
- `SettingsStore` is non-copyable (schema + layers + observer ids are
  one owner's state).
- `clear()` allocates one string for the erase (no heterogeneous erase
  until C++23).
- `json::Object` lookups are a linear scan (settings files run to
  hundreds of keys), and duplicate keys resolve to first position /
  last value - hand-edited files override at the bottom.
- The json writer prefers plain decimals and falls back to scientific
  notation only where `%f` cannot spell the value in 64 chars; a
  non-finite real has no json form and writes as null (schema
  validation keeps those out of settings files).
- `Object`'s members are all defined out of line in `src/json.cpp`:
  the entries hold an incomplete `Value` in the header, so nothing may
  odr-use them there - same for `Value::operator==`, kept out of line
  so the defaulted form is never instantiated against the
  self-referential `Array`.
- A load validates twice: the staged pass (coerce, so a bad key
  anywhere fails before anything lands) and set_value's own pass when
  the staged values apply. The price of the one-validation-path rule;
  loads are startup and file-drop events, not hot.
- save_layer's known keys come out alphabetical, not in the order the
  file carried them - the map's order wins; hand-diffing a re-saved
  file shows moved lines. Only the unknowns keep their file order.
- A reload's reverted keys notify nobody (clear() is silent) - the
  phase-1 contract; revisit if a ui ever needs to react to "the file
  dropped this key".

## Tests

`test_settings.cpp` - the phase-1 suite: declare validation (every
bad_schema flavor), defaults with provenance, layer precedence with
clear() falling back down rank by rank, the defaults-layer refusal,
unknown keys, strict type matching both directions, range reject + clamp,
enum, regex, round-trips of all four types (int64 edges included),
observer patterns/order/payload/equal-value sets, removal and
registration mid-notification, re-entrant set, introspection, and the
SettingValue unit behavior.

Phase 2 adds the json suite: scalars (int64 edges), strings with the
full escape set and surrogate pairs, arrays/objects/nesting, the depth
cap (the document needs an innermost VALUE to reach it - an empty
innermost array never asks for one), comments and a leading BOM, a
30-entry malformed table (every failure checked for message and
line/column), the exact written shape of a nested document,
escape/control-char writing, non-finite reals writing as null,
parse -> write -> parse stability, insertion order + duplicate
resolution + Object::set, and integer/real staying apart across a
round trip.

The later phases grow this suite: the FileSystem seam / atomic saves /
corrupt recovery / debounced writes (phase 4).

Phase 3 adds the layer-file suite: the shape version (default 1, the
reserved "version" key), the migration chain (a two-step walk, the
already-current fast path, a missing step naming its gap, a
newer-than-schema file refused, a non-object root and a non-integer
version answering corrupt_file, a step's own failure propagating
message-intact, a root-rebuilding step caught, add()'s refusals), a
load folding a file (clamping on the way in, unknowns remembered,
unknown keys never becoming settings), all-or-nothing failure naming
its key with the layer untouched, the file-is-the-layer reload
semantics, load-order observer notification, save's version /
alphabetical-knowns / file-order-unknowns shape (order walked by
iterator), a load -> save -> load round-trip that is byte-equal the
second time, and a rename migration end to end (the retired key gone
from the save, the caller's document unmodified).
