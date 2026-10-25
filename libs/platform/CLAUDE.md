# ws::platform - CLAUDE.md

The headless platform-services lib (nextplan stage 12): the OS-facing
services an app needs, behind Result-only APIs - standard locations now,
dialogs, recent files, autosave and file watching later, riding
ws::core (Result/Status) and the ws::settings seams (FileSystem + json)
wherever a file is involved. Pure std + ws::core; the OS halves live in
per-platform TUs and no platform header ever crosses into include/.

## Purpose, per header

- `errors.hpp` - the lib's error codes (the settings shape: append-only
  int32 enum, describe(), fail() -> unexpected<Error>, category
  "platform"). canceled/parse_failed/corrupt_file/invalid_argument are
  named for the arc's later phases (dialogs, recent files, autosave
  recovery); the table is complete the day the lib lands.
- `paths.hpp` - the lexical helpers. Canonical separators are '/'
  everywhere; normalize collapses duplicate separators, drops "." and
  trailing separators, resolves ".." never past a root (a relative
  climb with nothing to pop stays ".."); join folds with an absolute
  right side winning outright; is_absolute wants a root or a drive root
  ("C:x" is drive-relative, not absolute); name_of/parent_of take the
  last segment, a root keeping its shape ("C:/a" -> "C:/").
- `locations.hpp` - `Location` (config/data/cache app-scoped,
  documents/temp user-wide), `LocationRoots` (fully supplied, answered
  as given - the installer owns scoping; an empty member answers
  `unavailable`) and `Locations`, the caller-owned class (the
  SettingsStore shape: no globals, one owner thread): built with an app
  name it answers the OS truth, built with roots it answers those.
  `os_location` is the raw truth (Windows: the KnownFolders, cache under
  data/cache; Android: `unavailable` - the activity owns the package
  dirs, the app TU installs a LocationRoots at startup).
  `ensure_directory` is mkdir -p over utf-8 narrow paths (the
  StdFileSystem char8_t routing), `io_failed` on any blocker.
- `watch.hpp` - the "changed on disk" detector: `WatchSource` (the time
  half the FileSystem seam leaves out - modified_ms + exists; a fake in
  tests, `StdWatchSource` in production), `FileEvent`
  (modified/removed) and `FileWatch`, the LayerFile debouncer's shape:
  an injected `std::function<int64_t()>` ms clock (default
  steady_clock), owner-thread state, `poll()` on the frame cadence
  firing at most one event per call. The event fires `quiet_ms` after
  the LAST observed change - a writer still writing slides the
  deadline; a removal and a fast reappear merge into one modified; a
  file missing at construction baselines as missing, so its first
  appearance fires modified; `arm()` re-baselines and swallows the
  pending event (a load followed by arm() misses the load's own
  write). An exists-but-unstatable sample reads as ms=0 and the next
  poll re-reads - no event is invented from a failed stat.

## Allowed dependencies

- `ws::core` PUBLIC (Result/Status/errors - the public API returns
  them).
- ws::settings arrives with the file-riding phases (recent files,
  autosave) as PUBLIC - the FileSystem seam and the json facility are
  the workspace's standard, never duplicated here.
- System link libraries (shell32/ole32 for the KnownFolders) stay
  PRIVATE on ws_platform, advapi32's precedent in ws::ui.

## Rules

- Owner-thread state, no locks (the jobs/settings rule): every call
  runs on one thread; nothing here is callable from render or audio
  callbacks.
- Every path crossing the API is an utf-8 narrow string, '/'-canonical
  when a helper touched it. Platform TUs route through char8_t for
  std::filesystem and convert wide -> utf-8 at the OS edge, once.
- The OS halves never leak: windows.h/shlobj stay inside src/*.cpp, no
  Windows/NDK type in a public header, no platform include under
  include/.
- Result/Status only, no exceptions across the boundary (root
  CLAUDE.md).

## Known tradeoffs (phase 1)

- `Locations` with an empty member AND no app name answers
  `unavailable` rather than inventing a path.
- Drive letters are ASCII a..z/A..Z, case preserved, never folded.
- `os_location` on Android is a refusal, not a guess - the glue knows
  the package dirs; the app-TU install lands with the first android
  consumer.
- `locations_win.cpp` is the desktop half (the workspace's desktop is
  Windows-only today, the ws::ui windowing precedent).
- `normalize_path` is purely lexical: it never touches the disk, so
  symlinked segments resolve by name, not by target.
- The watch sees the file's stamp move, not same-stamp rewrites: two
  writes inside one stamp tick (100ns on NTFS, coarser elsewhere) land
  as one. `StdWatchSource::exists` answering false for an
  undeterminable path reads as "gone" - the seam's contract, inherited.

## Tests

`test_platform.cpp` - the path table (join's folding and the
absolute-right-wins rule, normalize's separators/dots/root-clamp and
relative-".." cases, drive and unc roots, "C:" drive-only, is_absolute,
name_of/parent_of edges), the Locations seam (supplied roots answered
as given and normalized, roots stored verbatim, the empty member's
unavailable with category and code asserted), ensure's mkdir -p chain
and idempotence, ensure_directory's file-blocker io_failed, and the
os_location smoke on the real KnownFolders (the app suffix,
cache == data/cache, documents/temp absolute, the empty-name refusal,
the app-name constructor matching os_location). Phase 2 adds the watch
suite: a settled change firing once after quiet, a still-moving writer
sliding the deadline, the removal firing and the fast reappear merging
into one modified, arm() swallowing a pending event, a missing file's
first appearance, and a StdWatchSource case whose stamps are set
explicitly through std::filesystem::last_write_time (quiet 0, no
races against the clock).
