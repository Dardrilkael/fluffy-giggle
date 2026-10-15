# Generic 2D GUI Library — Build Plan for Claude Code

## Context

We are building a **generic, dependency-free 2D GUI library** in C++ for the
monorepo (`ws::ui` namespace family), with **no Dear ImGui dependency**.
It sits on top of an existing batched 2D renderer (`ws::ui::r2d::Renderer2D`)
that already does sort-and-merge batching by (material, clip) and will grow
texture-atlas support.

Target consumers: `circsim-ui` (circuit simulator) and `neuralnet-studio`
(neural net comparison tool), both currently on ImGui — they will migrate to
this library once it's solid.

## Architectural decision (do not deviate without asking)

**Build immediate-mode first.** Immediate mode is the right fit: both
consumer apps are data-driven (netlists, run tables, node graphs) that change
shape constantly, and a persisted widget tree would need constant manual
diffing to stay in sync. Immediate mode avoids that by re-describing the UI
from current data every frame.

**However, design for a possible future retained-mode addition.** This means:

- Keep **widget identity + hit-testing + draw emission** (`GuiContext`) as a
  layer that is only called *through* immediate-mode widget functions — never
  let a widget function assume it is the only possible caller.
- Keep the **draw command layer** (`r2d::DrawCommand`, sort/batch) fully
  decoupled from widget code. A future retained-mode tree would build the
  same `DrawCommand`s by walking persisted nodes instead of re-running
  immediate-mode functions — it must not need renderer changes.
- Do not hardcode "no state survives this frame" anywhere. Persistent-but-
  implicit state (scroll offset, text cursor, hot/active ids) already lives
  in side-tables keyed by `WidgetId` inside `GuiContext` — this pattern is
  exactly what a retained layer could also key into later. Keep it that way;
  don't inline state into local statics or one-off globals.
- Do not add this abstraction now. No retained-mode code, no "IWidget"
  interface, no scene graph. Just don't paint into a corner. If in doubt,
  favor the simpler immediate-mode-only solution and flag the tension instead
  of guessing.

## Process rules (read before starting)

1. **One phase per session.** Implement exactly one phase below, then stop
   and report back: what was built, what compiles/runs, and anything that
   deviated from this plan. Wait for confirmation before starting the next
   phase. Do not read ahead and pre-build later phases "while you're at it."
2. **Small diffs.** Prefer adding new files over editing large existing ones.
   Keep each phase's changeset reviewable in one sitting.
3. **Compile-check after every phase.** A phase isn't done until it builds.
4. **If a phase seems to require a decision not covered here**, stop and ask
   rather than assuming — especially anything touching the immediate/retained
   boundary above.
5. Reuse existing code where it already exists in the repo (renderer's
   `DrawCommand`, sort/batch pass, clip stack, texture atlas) — do not
   reimplement what's already there; extend it.

## Phases

### Phase 0 — Skeleton
- Create `ws::ui::gui` namespace/module skeleton: empty `GuiContext`,
  `IdStack`, `InputFrame`, `LayoutCursor` headers with just data members and
  method signatures (no logic yet). Get it compiling and linked into one
  consumer app as a no-op.

### Phase 1 — Input snapshot
- Implement `InputFrame`: mouse position/buttons (down/pressed/released
  edges), scroll delta, keyboard pressed-this-frame, text input codepoint
  queue.
- Wire up the Win32 platform layer to fill one `InputFrame` per frame.
- No widgets yet — just prove the input snapshot is correct (e.g. log
  clicks/keys for a frame or two).

### Phase 2 — Widget identity
- Implement `IdStack`: hash-based `push(label)`, `push(int)`, `pop()`,
  `current(label)`.
- Unit-test (or at least sanity-check) that two widgets with the same label
  under different pushed scopes get different ids, and the same label under
  the same scope is stable across frames.

### Phase 3 — Hit-testing core (`GuiContext`)
- Implement `hot_id_` / `active_id_` tracking and `hit_test(id, rect) ->
  {hovered, pressed, clicked, released}` exactly per the hot/active pattern
  (mouse-down-inside sets active; release-inside-while-active = click;
  release-outside clears active without a click).
- Add the side-table mechanism for persistent-but-implicit per-widget state
  (keyed by `WidgetId`), even if nothing uses it yet — e.g. a
  `template<typename T> T& persistent_state(WidgetId)`.

### Phase 4 — Layout cursor
- Implement `LayoutCursor`: `begin_panel(bounds, padding)`, `next(w, h)`,
  `same_line()`, `end_panel()`, stack-based (no flex solver).

### Phase 5 — First real widget: Button
- Implement `Button(gui, ids, layout, r2d, theme, label) -> bool` end to end,
  using existing `r2d` draw calls (plain rects for now, no rounded corners /
  atlas yet).
- Get one real button working in a throwaway test window, driven by real
  input.

### Phase 6 — Theme
- Implement `ChromeTheme` struct (colors, metrics, corner radius, control
  height) per the earlier design. Wire `Button` to read from it instead of
  hardcoded values.

### Phase 7 — More widgets
- Add `Checkbox`, `Slider` (drag + click-to-set), `Panel` (background +
  clip-rect push/pop via the renderer's existing clip stack).
- Each widget: implement, then manually verify in the test window before
  moving to the next.

### Phase 8 — Rounded rects + icon atlas hookup
- Add `draw_rounded_quad` to `r2d` if not present.
- Hook up `IconSet`/`SpriteHandle` (from the existing/upcoming texture atlas
  work) so `Button` can optionally draw an icon.

### Phase 9 — Tabs + scrollable region
- Implement `Tabs` and a scrollable `Panel` variant (scroll offset stored via
  the Phase 3 persistent-state mechanism, clip rect from the renderer's clip
  stack intersected with the scroll viewport).

### Phase 10 — Integration pass
- Wire the library into **one** real screen of `circsim-ui` or
  `neuralnet-studio` (pick the simpler one) replacing a small, self-contained
  piece of ImGui UI, as a real-world smoke test. Do not attempt full
  migration yet — one screen only.

## Explicitly out of scope for this plan

- Docking/floating windows
- Text editing widgets (text input cursor/selection) — separate future plan
- Retained-mode implementation itself
- DPI scaling
- Any layout beyond stack-based cursor (no flex/grid solver)

Stop and ask before adding any of the above, even if it seems small.
