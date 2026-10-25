# ws::platform - CLAUDE.md

The headless platform-services lib (nextplan stage 12): the OS-facing
services an app needs, behind Result-only APIs - standard locations,
the debounced file watch, the recent-files MRU, the autosave
crash-recovery pair, the file dialogs, the clipboard and the message
boxes now, the window title and single-instance later, riding
ws::core (Result/Status) and the ws::settings seams (FileSystem +
json) wherever a file is involved. Pure std + ws::core +
ws::settings; the OS halves live in per-platform TUs and no
platform header ever crosses into include/.

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
- `recent.hpp` - the capped MRU of recently used files, one list per
  document type ("" is the whole-app default). `touch` promotes and
  dedupes by the normalized path keeping an existing pin; over the cap
  the oldest unpinned TAIL entry falls off - the new head never evicts
  itself, so an all-pinned full list refuses the touch, untouched.
  `pin`/`unpin` mark entries that survive both the cap and the lazy
  missing-file prune (`menu_items` drops unpinned entries whose file
  is gone - the FileSystem's exists, so an undeterminable path reads
  as gone; pinned entries stay even when missing). `menu_items` also
  builds the labels: the first same-named file wears the bare name,
  later siblings grow " (parent)". Persistence is explicit json
  through the settings seams: `load` (a missing file is the first run;
  a corrupt file answers parse_failed with NOTHING preserved - unlike
  the settings .bak, recent files rebuild from use; a load replaces
  the in-memory lists, all-or-nothing; shape 1 only, a newer shape is
  refused never downgraded) and `save` (atomic through the seam's
  temp+rename, so a failure leaves the previous file intact).
- `autosave.hpp` - the crash-recovery helper. `Autosave` keeps a
  content copy plus a marker file in a caller-chosen store dir, both
  named after the document - its name plus a hash of the normalized
  path, so same-named documents in different folders never collide
  and two spellings of one document derive one family. The marker is
  the commit point: `snapshot` writes content first, marker last (a
  crash between the two leaves an orphan nothing claims; a failed
  write never promises a recovery), `finish` removes the marker
  first (the same window can only orphan). `probe` reads the marker
  alone as the crash evidence - a clean exit always clears it; a
  corrupt marker answers corrupt_file while `discard` still cleans
  the pair. `recover` requires only the content file (an orphan is
  recoverable by construction) and holds the bytes before removing
  anything. Two injected seams: the saved_ms clock (default
  wall-clock ms - display information, not a debounce) and the
  store-dir maker (DirEnsurer, default ensure_directory - the
  FileSystem has five content verbs and no mkdir, the WatchSource
  precedent).
- `dialogs.hpp` - the pickers behind one interface, so tests and the
  gallery can script a fake: `FileFilter` (a name + the OS-shaped
  pattern, passed through verbatim - the OS is the matcher),
  `FileDialogOptions` (title, default_path - the folder to start in
  for the open and folder dialogs, a file name for save - filters)
  and `FileDialogs` (open_file/open_files/save_file/select_folder).
  Every call blocks while the user decides: the caller's worker
  thread, never the render loop. A dismissed dialog is nullopt - an
  empty list from open_files - a normal answer, never an error.
  `UnavailableFileDialogs` is the android answer (the Storage Access
  Framework lives behind the activity, which only the app TU knows)
  and a public class so the refusal is testable everywhere.
  `FakeFileDialogs` scripts the answers - a queue per kind, an empty
  queue or a nullopt entry answers a dismissal - and records every
  call's options. `make_file_dialogs()` builds the platform's native
  set: the COM IFileDialog family on windows (a per-call STA
  apartment, an MTA thread answers unavailable), the refusal on
  android. The linux story is documented-only
  (xdg-desktop-portal): the workspace's desktop is Windows-only, the
  ws::ui windowing precedent.
- `clipboard.hpp` - the session clipboard as free functions, no
  interface: there is no fake to script - a round trip IS the real
  thing, and the gallery demos it interactively. Text is utf-8 at
  the edge (the OS channel is NUL-terminated: embedded NULs stop
  at the first), payloads carry arbitrary bytes under a
  caller-chosen type that doubles as the OS format name, images are
  plain RGBA top-down (the DIB shuffle - BGRA, row order, padding -
  is this lib's, never the caller's). Every read answers nullopt
  when the clipboard holds no such content - a normal answer, never
  an error. The suite has no clipboard case on purpose: a test that
  clobbers the user's clipboard would be a bad citizen.
- `messagebox.hpp` - the native ask behind one interface, the
  dialogs pattern: `MessageBoxes::ask(MessageBoxRequest)` answers a
  `MessageBoxChoice` - always, because a box cannot be dismissed
  without one (the X maps to cancel on ok_cancel, to the only
  choice on ok), so there is no nullopt channel.
  `UnavailableMessageBoxes` is the android answer, public so the
  refusal is testable everywhere; `FakeMessageBoxes` scripts a queue
  and records every request, an empty queue answering the request's
  first choice (the click-through). `make_message_boxes()`:
  MessageBoxW on windows, the refusal on android.

## Allowed dependencies

- `ws::core` PUBLIC (Result/Status/errors - the public API returns
  them).
- `ws::settings` PUBLIC, arrived with recent files (the
  file-riding phases): the FileSystem seam and the json facility are
  the workspace's standard, never duplicated here. The reference
  lives in a public header (recent.hpp) - the ws::ui gui-target
  precedent.
- System link libraries (shell32/ole32 for the KnownFolders and the
  COM dialogs, user32 for the clipboard + MessageBoxW) stay PRIVATE
  on ws_platform, advapi32's precedent in ws::ui.

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

## Known tradeoffs

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
- RecentFiles has no .bak: a corrupt store costs the recent list
  (rebuilds from use), unlike the settings store's preserved corrupt
  file. The list is convenience data, not user work.
- The recent-files shape is version 1, append-only from here; a file
  newer than shape 1 is refused, never downgraded.
- The first same-named file wears the bare label; which sibling gets
  the " (parent)" suffix follows MRU order, not the menu's stable
  order - acceptable for a menu rebuilt per open.
- menu_items' prune consults the FileSystem's exists per call: a
  network share that answers undeterminable reads as gone, dropping
  the entry. Pinned entries never prune, so pin what matters.
- One RecentFiles owns one store path (the seam's fixed ".tmp"
  sibling); two instances on one path would race through the same
  temp name - the owner-thread rule already forbids it.
- The autosave marker is the whole crash evidence: a content file
  with no marker (the crash window between the two writes) is an
  orphan probe never claims - it waits for the next snapshot to
  overwrite it; nothing sweeps the store dir.
- The marker shape is version 1, append-only; a corrupt marker costs
  the prompt, not the draft - recover() keys on the content file
  alone, and only discard() drops it.
- saved_ms is wall-clock because it is shown ("autosave from 14:02"),
  never compared for elapsed time: a clock that jumps moves it.
- One Autosave per document, owner thread (the seam's fixed ".tmp"
  sibling already forbids a second writer on one path).
- The dialogs map a dismissal to nullopt, never to the canceled
  code: the Result's failure channel stays for the platform failing,
  and canceled remains for a caller that prefers to re-map (the
  table is append-only).
- The COM apartment is paid per call: a dialog is a human-speed
  event, and the lib owns no threads to hold an apartment. A worker
  already in the MTA apartment answers unavailable - a dialog cannot
  host there.
- The dialogs' title and default_path are hints, never failures: a
  folder the shell cannot parse falls back to the OS's remembered
  one, and a filter with an empty name or pattern is dropped.
- The multi-select skips an item that cannot be named (FORCEFILESYSTEM
  makes it a non-case in practice); the single kinds answer
  io_failed when their one result has no file-system path.
- The clipboard text channel is a NUL-terminated string end to
  end: embedded NULs stop at the first. The payload channel is the
  binary-safe one - its length rides the block's GlobalSize.
- clipboard_image reads only plain 32-bit BI_RGB DIBs: a 24-bit or
  compressed DIB answers io_failed (screenshots and PNG copies
  arrive 32-bit; the first consumer that needs more grows the
  conversion).
- The clipboard is busy-shared with every other window: a refused
  OpenClipboard answers io_failed, never retried in a spin - retry
  is the caller's policy. And the suite never touches it: the
  user's clipboard is not the suite's to clobber.
- A message box always answers a choice: a box cannot be dismissed
  without one, so ask() has no nullopt channel (unlike the
  dialogs, where a dismissal is a normal answer).
- The fake message box's empty queue answers the request's first
  choice: an unscripted ask never invents a choice the buttons did
  not offer (ok for the ok-shaped sets, yes for yes_no).
- MessageBoxW runs parentless: ui's window is not platform's to
  parent on, and the box locks its own modal loop - a worker
  thread's ask, never the render loop's.
- The wide/narrow edge is shared src-local win_utf8.hpp (never
  under include/), one conversion per TU, hoisted once the win TUs
  hit three.

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
races against the clock). Phase 3 adds the recent-files suite over a
FileSystem fake (a file map, a deny-set for "not there", a
write-failure countdown): MRU order + dedupe by two spellings + cap
eviction taking the tail, pins surviving the cap and the re-touch
keeping a pin, the lazy prune (unpinned missing dropped, pinned
missing kept), same-named label disambiguation, the json round trip
across two doc types with the saved shape checked, the corrupt file
refused with in-memory lists intact and a future shape refused, the
missing store as the first run, a failed save leaving the previous
file byte-identical (then recovering), and a real StdFileSystem smoke
in the OS temp: real files survive the prune, the removed one prunes,
the pinned missing file stays. Phase 4 adds the autosave suite over
DiskFs - a fake whose exists is honest (only what was written exists),
with a write-failure countdown and the contract-true remove: the
clean cycle (a fresh instance sees nothing, snapshot, a second
instance sees the crashed pair, finish clears both), the crash +
recover cycle (the content handed back, the pair cleared, the second
recover's invalid_argument), crash + discard, the failed snapshot
leaving the v1 pair intact with the marker still claiming v1, recover
requiring content while an orphan content file is recoverable by
construction, the corrupt marker answering corrupt_file with discard
still cleaning and future/missing-field shapes refused, the derived
paths (same name in different folders never colliding, two spellings
one family), and a real StdFileSystem smoke where snapshot makes the
store dir that does not exist yet. Phase 5 adds the dialogs suite:
the fake playing the scripted answers through the interface ref (an
empty queue answering dismissals, the queues popping in order, a
nullopt entry scripted as a dismissal, the files vector, every
call's options recorded verbatim), the unavailable set refusing
every kind with category and code asserted, and the native factory
constructed and destroyed only - a real call raises a modal dialog
and would hang the suite; the gallery exercises it interactively.
Phase 6 adds the message boxes' suite (the clipboard has none on
purpose): the fake playing scripted answers through the interface
ref - an empty queue answering the request's first choice, the
queues popping in order, every ask's request recorded verbatim
(with the request mutated between asks visible in the records) -
the refusal's platform/2, and the native factory constructed and
destroyed only; a real ask raises a modal box.
