# ws::ui::shell (src/shell/ + include/ws/ui/shell/)

The stage-15 layer: the dock host, the panel framework, the document
layer and AppShell - the reusable skeleton a host app composes. All of
it joins the `ws_ui_gui` pure archive (draws through r2d + the widget
set only; the platform services arrive through the config's injected
interfaces, never a make_* factory).

## Files
- dock.cpp: the split tree - add/remove/activate, resolve() (leaves +
  splitter bands), apply_drag (kMinWeight clamps in NORMALIZED share
  space - a pair holding most of a split still leaves its sibling the
  floor), the tolerant json round-trip (an accept filter drops panels
  the host never opened; empty leaves pruned, starving splits
  collapsed).
- panels.cpp: PanelRegistry validation + PanelHost's instance
  bookkeeping over the tree ("type" single-instance, "type:N"
  multi-instance with a never-reused counter), focus with on_focus
  firing only on the CHANGE, cycling.
- document.cpp: DocumentManager's open set - the close flow
  (plan_close -> the prompt -> finish_close, a failed save keeps the
  document open), session save/restore (missing files land in the
  failures list, never a failed restore), the per-document Autosave
  pairs (untitled docs key "untitled-N").
- shell.cpp: AppShell - startup (settings layer, layout, recents,
  session, recovery probes; tolerant: corrupt files are status notes,
  never failed boots), the frame (menu bar + the automatic Window
  menu, toolbar, the dock view - tab strips, close buttons, focus
  follows press, splitter drags through gui.hit_test, panel bodies in
  scroll scopes - status-job rows, the status bar, the overlays), and
  the ordered shutdown logged phase by phase.

## Rules that keep biting
- **The shell never touches ws::ui::App** - it is a plain class the
  host's App subclass owns and drives from on_update(); headless
  tests drive draw() through the r2d stub exactly like the gui suite.
- **The dirty flag IS the undo save point**: Document::undo().
  is_dirty() is the title's star, the close prompt's trigger, and the
  autosave tick's gate. After any successful save() the CALLER runs
  mark_saved() (the manager does it in open_path/new_document/
  finish_close; the shell does it in save_active/save-as).
- **Shutdown order is pinned by a test**: cancel_jobs ->
  close_panels -> autosaves -> save_session -> save_layout ->
  save_recents -> flush_settings. A panel's on_close must never be
  the first thing shutdown does - jobs feeding panels die first.
- **The Window menu is rebuilt per frame** from borrowed strings: the
  labels live in a deque (c_str()s never move, the builder's own
  hints_ precedent), the recents in the frame's menu_items() vector
  (alive for the menu_bar call).
- **Named layouts need no listdir**: the FileSystem seam has none, so
  layouts.json is the names' index and layouts/<name>.json the trees.
- **The settings panel is the host's schema rendered** - the shell
  registers its own "settings" PanelDesc over property_grid unless
  the host registered one first (the add refusal is swallowed).
- **key_context() reports, never resolves**: the host's chord loop
  asks for the rung (panel while a scoped_shortcuts panel is active);
  the resolver itself is untouched, ws::app's contract.
- window_title()/the enabled predicates read the undo stack, a
  non-const surface - they take the shell/documents mutably, the same
  rule plan_close learned.

## Tests
```
ctest --preset debug -R ui_shell    # or: test_ui_shell directly
```
Fifteen cases: dock geometry/serialization/clamps/pruning, panel
validation/focus, document close-dirty/session/autosave, shell
first-run/corrupt-config/layout-restore/dirty-title/palette/drops/
theme/shutdown-order. The suite links ws::ui_gui + the r2d stub +
ws::platform's headless halves - no Vulkan, GLFW or OS TU anywhere
(the acceptance the stage set out to prove).
