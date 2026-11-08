# ws::ui::shell (src/shell/ + include/ws/ui/shell/)

The stage-15 layer: the dock host, the panel framework, the document
layer and AppShell - the reusable skeleton a host app composes. All of
it joins the `ws_ui_gui` pure archive (draws through r2d + the widget
set only; the platform services arrive through the config's injected
interfaces, never a make_* factory).

## Files
- dock.cpp: the split tree - add/move/move_tab (the full placement
  vocabulary: enter, re-dock, reorder/retab), remove/activate,
  resolve() (leaves + splitter bands; the focused-pane zoom
  projection while a panel is maximized), apply_drag (kMinWeight
  clamps in NORMALIZED share space - a pair holding most of a split
  still leaves its sibling the floor), distribute (the splitter
  double-click's even split), dock_hit (the five-zone drop walk over
  a resolved layout - the drag's target picker, pure data), and the
  tolerant json round-trip (an accept filter drops panels the host
  never opened; empty leaves pruned, starving splits collapsed).
  FUSION is the placement machinery's one idea: a side placement
  whose target sits in a split of the same axis lands as one more
  sibling of that split, not a fresh split nested inside the leaf
  (same pixels - the new leaf takes its share of the target's span
  either way; the tree stays flat, the json keeps its N-children
  shape). The maximized panel is transient state the serializers
  never write: a restored layout starts unzoomed.
- panels.cpp: PanelRegistry validation + PanelHost's instance
  bookkeeping over the tree ("type" single-instance, "type:N"
  multi-instance with a never-reused counter), focus with on_focus
  firing only on the CHANGE, cycling, redock/retab (a re-dock and a
  retab activate the moved tab - opening and docking are one act,
  and so are re-docking and focusing).
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
  the ordered shutdown logged phase by phase. The dock view's three
  gestures: the TAB DRAG (armed off tab_bar's held-claim out-param,
  a 6-px threshold, a drop-preview veil over dock_hit's zone, the
  release drops through PanelHost::redock - outside every leaf
  cancels, and a hold that never crossed the threshold stays the
  strip's click), the ZOOM BUTTON (the strip's right end, a round
  ghost glyph button that maximizes the pane's front tab and
  restores it - resolve()'s projection does the rest), and the
  SPLITTER DOUBLE-CLICK (two fresh presses on one band inside
  0.35 s of the frame clock distribute that split; the same hold
  never also drags the boundary it just evened).

## Rules that keep biting
- **The shell never touches ws::ui::App** - it is a plain class the
  host's App subclass owns and drives from on_update(); headless
  tests drive draw() through the r2d stub exactly like the gui suite.
- **The tree answers WHERE, the view owns the gesture**: every dock
  mutation goes through DockTree's seam (add/move/move_tab over a
  resolved layout's leaf indexes), and the shell only arms gestures
  off widget claims (tab_bar's held_tab, the splitter hit_test) and
  drops through PanelHost (redock/retab) so focus follows. Nothing
  in the view walks the tree's nodes - the resolve walk is the only
  contract between them, which is what keeps both halves testable.
- **A drag is a claim, and one pointer holds one claim**: the tab
  drag rides the tab's capture (hit.pressed stays true wherever the
  pointer carries it), so no other widget can steal the hold - and
  the drop logic reads mouse_down directly, exactly like the
  splitter drag before it.
- **Move ordering is pointer-safety, not style**: a re-dock inserts
  the new leaf BEFORE erasing the old tab when the source and the
  target differ (the erase can empty the source and the prune that
  follows re-shapes the tree around node pointers), and erases first
  only when source IS the target (the wrap below moves the leaf's
  remaining tabs as one side). dock.cpp's comments pin the why.
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
Twenty-six cases: dock geometry/serialization/clamps/pruning, the
fusion shapes (flat splits, at_root strips) and their pixel-exact
shares, move/move_tab (retab, reorder, prune-behind), the
maximize/restore projection, distribute, dock_hit's five zones,
panel validation/focus, document close-dirty/session/autosave, shell
first-run/corrupt-config/layout-restore/dirty-title/palette/drops/
theme/shutdown-order, and the three gestures through scripted
three-frame pointer runs (the tab drag's retab/split/cancel/click,
the zoom button, the splitter double-click). The suite links
ws::ui_gui + the r2d stub + ws::platform's headless halves - no
Vulkan, GLFW or OS TU anywhere (the acceptance the stage set out to
prove).
