# The hover-only claim — plan (the why-is-this-disabled gap)

Status: **draft** (2026-11-06) — for review; nothing implemented, nothing
committed.

## The gap

A locked widget can never show a "why is this disabled" tooltip outside
the menus. The roadmap has carried this as the hover-only-claim candidate
since the widget-arc tail landed. The mechanism, confirmed against the
code:

- `tooltip_delayed` (src/gui/widgets_overlays.cpp:161) banks its rest
  only while the explained widget is hot: `if (!gui.is_hot(id))` zeroes
  the clock (widgets_overlays.cpp:171). `is_hot` reads the pointer's
  live hot track (gui_context.cpp:1032, mouse-spelled by default,
  gui_context.hpp:568).
- A disabled widget never claims hot: `state()` short-circuits BEFORE
  hit_test on purpose — "claims are exactly what a disabled widget must
  not make - no hover, no hot, no press" (gui_context.cpp:821-833; the
  M10 disabled scopes, eed9bc0). So `is_hot` can never answer true for
  a locked widget, the rest never banks, and the delay never ripens.
- The menus work around it with their own hover engine:
  `menu_hover_step` (widgets.hpp:1282) is a self-contained clock the
  chain drives with its own pointer-in-row test — the row renderer's
  comment names the gap outright: "is_hot cannot fire on a dead row"
  (widgets_menus.cpp:597-618), and `MenuHoverAction::show_tooltip`
  (widgets.hpp:1279) hands the caller `disabled_reason`
  (widgets.hpp:1236, 1246) to draw through `begin_tooltip` by hand.

Everywhere else — a locked `button`, `checkbox`, `slider`, `combo`, the
gallery's locked rows — the app's existing tooltip line
(`tooltip_delayed` / `tooltip_text`) simply goes dead the moment the
widget locks. Enabled hover works (the hot lane), locked hover does not.

## The rule

A second hover lane — the **hover-only lane** — that settles exactly
like the hot lane's `hot_prev` (M7) but never touches hot, press,
active, focus or the census. The hot lane stays the pointer's chrome
owner; the hover-only lane is its fallback, granted only where no live
claim exists:

1. **Registration**: `state()`'s disabled branch registers
   `(id, claimable rect, layer)` into a per-frame table — the same
   intersected rect hit_test would claim through (bounds ∩ the innermost
   hit clip, the 123af64 seam: a locked row scrolled out of a scroll
   panel registers nothing). The modal veil gates it like every claim:
   a strictly-lower stratum under a raised veil does not register (the
   `blocked` expression at gui_context.cpp:892). HitShape is ignored —
   a locked widget's tooltip area is its visual bounds, its shape pads
   an interactivity that does not exist.
2. **Resolution** (end_frame, beside the `hot_prev` write,
   gui_context.cpp:189-191): per pointer, among the registered rects
   containing the pointer — the winner is the highest layer, later
   registration within a layer (mirroring the hot claim's
   `layer >=` unseat, gui_context.cpp:910-915) — **but only when that
   pointer's hot lane is EMPTY this frame** (`track.hot ==
   kInvalidWidgetId` at end_frame is this frame's final hot) and the
   pointer is not cancelled. A live claimant owns the pointer's chrome;
   the fallback exists only where nothing live claimed. The winner and
   its stratum settle into the track (the M7 idiom: end_frame writes
   what next frame reads).
3. **Reporting**: the disabled branch's `WidgetResult.hit.hovered`
   answers the settled fallback — `inside && !cancelled && !blocked &&
   hover_only_prev == id`, the exact shape of the hot lane's settled
   report (gui_context.cpp:902-903), folded per pointer like every
   hit_test answer. `state` stays `WidgetState::Disabled` — no stock
   face changes (control_disabled covers idle/hover/press, the M10 look
   decision); the field is for the caller and the delay clocks.
4. **The delay**: `tooltip_delayed`'s clock banks while
   `is_hot(id) || gui.hover_only_settled(id)` — a new mouse-spelled
   accessor answering the settled fallback for the mouse track, veil-
   gated on the settled winner's own stratum (a veil that rose mid-dwell
   kills the stale settle's banking the same frame, the one leak the
   stored stratum exists to close). An existing tooltip line therefore
   "just works" the frame a widget locks — zero new call-site idiom —
   and re-locks restart the clock honestly (a lane transition zeroes the
   rest for a frame, like any hover transition).

Consequences worth stating:

- One tooltip per pointer from the lane: overlapping locked widgets
  arbitrate (topmost layer, then later) — the tap-defect lesson, no
  double tooltips. A locked widget overlapping a LIVE one never tooltips
  (the live one claimed hot): the click target owns the pointer's
  chrome, and M10's pass-through semantics stay untouched.
- The flicker contract holds: the lane settles one frame late exactly
  like `hot_prev`, so "a flickering hover never banks enough rest to
  fire" (widgets_overlays.cpp:164-166) stays true for locked rows.
- Tooltips stay mouse-spelled: `tooltip_delayed` reads the mouse track;
  the lane folds per pointer but the clock never does. The Android
  bridge's mirror behaves exactly as it does for `is_hot` today.
- Disabled widgets still skip the duplicate census (M10); a duplicate
  id in the lane is harmless (same id, same rect, same answer).

## The change (one commit)

1. `PointerTrack` (include/ws/ui/gui/gui_context.hpp:670, beside
   `hot_prev`) gains `WidgetId hover_only_prev = kInvalidWidgetId;` +
   `std::uint32_t hover_only_layer = 0;` — last frame's fallback winner
   and its stratum (the stratum exists for the accessor's veil gate).
   Fresh and vanished tracks value-init away, the hot_prev mechanics.
2. `GuiContext` (gui_context.hpp:716, beside `kMaxFocusables`) gains the
   per-frame table: `kMaxHoverOnly = 128`, a fixed claim array
   `{id, rect, layer}`, a count, and the warn-once overflow flag (the
   `register_focusable` idiom, gui_context.cpp:1082-1088). Public: one
   accessor beside `is_hot` (gui_context.hpp:568) —
   `bool hover_only_settled(WidgetId id) const;` (mouse-spelled, like
   `is_hot`'s default; veil-gated on the stored stratum).
3. `begin_frame` (gui_context.cpp:~136, beside the claim-table resets):
   `hover_only_count_ = 0;`
4. `state()`'s disabled branch (gui_context.cpp:821-833): register
   (veil-gated, clip-intersected, capacity warn-once) and fold the
   settled report into `result.hit.hovered` / `result.hit.pointer`.
5. `end_frame` (gui_context.cpp:189-191, beside `track.hot_prev =
   track.hot`): the per-track resolution walk — containment, the
   hot-lane-empty gate, `!cancelled`, the `>=` layer arbitration —
   writing `hover_only_prev` + `hover_only_layer`.
6. `tooltip_delayed` (widgets_overlays.cpp:171): the clock condition
   gains the second lane.
7. Docs: widgets.hpp:797-805 (tooltip_delayed's comment gains the
   locked-widget sentence), libs/ui/CLAUDE.md (the disabled bullet: the
   hover-only lane; the hot/hover bullet: the fallback and its
   yield-to-live rule).

The steal machinery, the click floor, the re-claim block, the veil
latch, focus — all untouched; the lane never writes a track field the
claim machinery reads.

## What this deliberately does NOT do

- **No change to the hot lane.** `is_hot`/`hot_id`/enabled `hovered`
  answer exactly as today; the fallback never writes `track.hot`. M10's
  hover-and-click-never-disagree stands.
- **No visual change.** `state` stays `WidgetState::Disabled` on locked
  rows; the stock painters key on state. A custom painter reading
  `hit.hovered` must gate on state first (the stock order) — flagged.
- **Menus unchanged.** `menu_hover_step` keeps self-serving (its
  per-row, multi-level engine predates and outruns the lane); migrating
  the chain onto the lane is a later cleanup if ever wanted. The new
  cases and the menu's disabled-reason cases (test_ui.cpp:10439,
  test_ui_gui.cpp:1926) must stay green side by side.
- **No touch tooltips.** The clock stays mouse-spelled.
- **No public per-pointer lane query** beyond the mouse-spelled
  accessor — raise it if a consumer appears.
- **No opt-in parameter.** Registration rides `state()`'s disabled
  branch automatically; the alternative (a flag on every widget call)
  buys nothing the auto-lane loses and would leave most call sites
  un-updated.

## Tests — cases in `test_ui_gui`

The headless archive suite, beside the menu-hover precedent
(test_ui_gui.cpp:1926); no new target. The frame idiom is
test_ui_tap's (`tap()`), plus `dt_seconds` on the frame for the clocks.

1. **The gap pin**: a locked button under a resting pointer —
   `tooltip_delayed` ripens after `theme.tooltip_delay` seconds of
   frames. (Today: red — the rest never banks.)
2. **The settle**: the disabled `WidgetResult.hit.hovered` is false on
   the first rest frame, true on the second (the M7 one-frame settle).
3. **Yields to a live claimant**: locked A over enabled B (B hit-tests)
   — B hovered + ripens, A neither.
4. **One lane winner**: two overlapping locked widgets — the later
   hovered, the earlier not; one tooltip's worth of answer.
5. **The veil**: a modal up, a locked base widget — no report, no
   banking; and the stale pre-veil settle does not bank either (the
   accessor's stored-stratum gate).
6. **Cancelled**: `cancel_pointer` — no fallback report.
7. **The clip**: a locked row scrolled fully out of a scroll panel (the
   123af64 geometry trick) — no report, no banking.
8. **The lane resets under a live claim**: the locked tooltip ripening,
   then an enabled widget claims hot over the point — the rest zeroes.
9. **Touch-only frame**: no mouse present — `tooltip_delayed` stays
   false (the mouse-spelled clock, unchanged).

Existing pins that must stay green: the menu disabled-reason cases
(test_ui.cpp:10439, test_ui_gui.cpp:1926 — the engine and the lane are
independent), the M10 disabled cases, the M7 settle cases
(test_ui.cpp:9606), the M9 absent-mouse cases (a vanished pointer drops
its track, the stale lane dies with it), test_ui_variants.cpp:161.

## Gates

Desktop debug AND release, /W4 /WX: ws_ui + the full ui suite family
(ui, ui_gui, ui_tap, ui_canvas, ui_grid, ui_shell, ui_gui_splits,
ui_font, ui_theme_tokens, ui_painters, ui_motion, ui_primitives,
ui_variants) + check_deps. Playground 6 s smoke, no new warns. Android
arm64: ws_ui + the app .so modules link (the lane is pure CPU gui-core;
nothing platform-side moves).

One commit: `ui gui: the hover-only lane - locked widgets settle hover
for their tooltips`.
