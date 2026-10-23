# ws::settings - CLAUDE.md

The headless settings store (nextplan stage 9): a typed schema
(defaults + constraints), layered sources with provenance, change
observers, and (later phases) JSON persistence through a minimal
FileSystem seam. Pure std + ws::core - no third-party anywhere, no
platform headers, no locks.

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
  validation, find, introspection: descriptors/categories/in_category)
  and `coerce()` - the one validation path both set() and the later
  file-load share.
- `settings.hpp` - `Layer` (defaults < system < user < project <
  command_line; the rank IS the precedence) + `SettingsStore`: get<T>
  with layer provenance, set into one layer, layer_of/has_value/clear,
  observers keyed by exact key or dot-prefix.

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
- declare() validation: empty key, duplicate key, default type mismatch,
  constraint applicability (range on bool/text, enum entries of the
  wrong type, regex on non-text), empty enum, min > max, unparsable
  regex - all answer bad_schema, and a refused declare stores nothing.

## Known tradeoffs

- `Schema::find` is a linear scan (schemas run to hundreds of keys; the
  index arrives when a profile asks).
- Regex constraints compile per check - set() is rare, get() is the hot
  path and touches none of this.
- `SettingsStore` is non-copyable (schema + layers + observer ids are
  one owner's state).
- `clear()` allocates one string for the erase (no heterogeneous erase
  until C++23).

## Tests

`test_settings.cpp` - the phase-1 suite: declare validation (every
bad_schema flavor), defaults with provenance, layer precedence with
clear() falling back down rank by rank, the defaults-layer refusal,
unknown keys, strict type matching both directions, range reject + clamp,
enum, regex, round-trips of all four types (int64 edges included),
observer patterns/order/payload/equal-value sets, removal and
registration mid-notification, re-entrant set, introspection, and the
SettingValue unit behavior.

The later phases grow this suite: json (phase 2), layer files +
migrations (phase 3), the FileSystem seam / atomic saves / corrupt
recovery / debounced writes (phase 4).
