# The same-frame tap double-click — plan (the M7 leftover)

Status: **landed** (2026-11-06) — implemented as approved; this document is
the design record.

## The defect

A same-frame tap — one frame carrying BOTH the press and the release edges
(`pressed && released && !down`) — landing on a point covered by two
overlapping widgets of the same layer clicks BOTH. Recorded as M7's known
defect left (74c651c, 2026-10-08): "a same-frame tap on two overlapping
widgets can click BOTH - the first widget's press claim clears on its
release, then the second claims fresh in the same frame; candidate for a
later milestone."

The mechanism, confirmed against the code:

- `GuiContext::hit_test_pointer` (src/gui/gui_context.cpp:877) claims the
  press the ordinary way when `active == kInvalidWidgetId`
  (gui_context.cpp:928-931).
- The release report (gui_context.cpp:957-961) publishes the click and
  FREES THE CLAIM INLINE — `track.active = kInvalidWidgetId` at :960 — so
  widget B, hit-tested later in the same frame, sees `active` invalid
  again, claims fresh, and publishes its own click. The M10 steal gate
  (:932-940) is never reached: there is nothing left to steal.
- One physical gesture, two `clicked=true` answers, two caller actions —
  a toggle toggles twice, a menu item and the widget under it both fire.

Multi-frame presses are already correct: the M10 rule (eed9bc0) lets the
later same-stratum widget STEAL a fresh press on the press frame, so only
one widget ever clicks. The defect is exclusive to the tap that begins
and ends inside one frame — common on touch (the event pump batches),
rare but real on the mouse.

## The rule

Within one frame, one pointer's click publications form a strictly
increasing layer sequence. Each publication raises that pointer's **click
floor** to the publishing widget's stratum; a fresh press claim at or
below the floor is refused while the contact is over (`!view.down`).

- The defect dies: the second same-stratum claimant is refused — exactly
  one click per stratum per tap. The first hit-tested claimant wins
  (call order decides within a stratum; deterministic per app).
- The M19 cascade survives: a one-frame tap on a combo row may still
  open the dropdown AND pick the item under it in one act — the row
  publishes at base (10), the item claims at popup (20), strictly above
  the floor. That behavior is documented in the M19 clamp test's
  comment; it is preserved as-is, now pinned by a test.
- The M11 reuse survives: an up-then-down frame reads
  `pressed && released && down`; its claims are for the NEW contact, so
  the `!view.down` guard exempts them — the re-claim
  (gui_context.cpp:982) and a later widget's steal of it (M11's "the
  re-claim is FRESH: stealable" contract) are untouched, and the new
  contact's click lands on its own release in a later frame, floor long
  reset.
- Hover/hot are untouched: a one-frame tap has no settled hover either
  way, so no hover/click disagreement can show.

## The change (three sites, one commit)

1. `PointerTrack` (include/ws/ui/gui/gui_context.hpp:666) gains
   `std::uint32_t click_floor = 0;` — the highest stratum that already
   published this pointer's click this frame; 0 = none (strata start at
   base = 10). Plain uint32, same style as `hot_layer`/`active_layer`.
2. `begin_frame`'s track match (src/gui/gui_context.cpp:163-166) resets
   it beside `active_fresh`: `matched[p].click_floor = 0;` — surviving
   tracks carry the field through the copy otherwise (fresh tracks
   value-init to 0).
3. `hit_test_pointer`'s ordinary press claim (gui_context.cpp:928) gains
   the guard: the fresh claim requires
   `view.down || layer > track.click_floor` — a live contact may still
   claim (the reuse's new gesture); a spent tap may not claim at or
   below the floor.

The steal branch (:932-940) needs NO change and the re-claim block
(:982-987) needs NO exemption: in a frame where the floor is set and the
contact is over, every earlier claimant's release report already freed
the claim inline (:960), so `active` is invalid and only the ordinary
path is reachable; when the contact is down (the reuse frame) the guard
exempts everything anyway. Write this reasoning into the comment at the
guard so the next reader does not re-derive it.

Also reconcile the M19 clamp test's comment if it calls the cascade
"the higher-layer fresh-claim steal" — with the floor, the item's claim
is the ordinary path above the floor (the row freed its claim at its own
click). Behavior identical; the comment should say what the code does.

## What this deliberately does NOT do

- **No click deferral / arbitration pass.** Publishing the winner next
  frame (the wheel-claim settle pattern) would add a frame of latency to
  every click in the library and rewrite the two-frame-click contract
  every widget test pins. The floor is the surgical alternative.
- **No change to cross-stratum pass-through.** A toast's click handling
  reads raw input (widgets_toasts.cpp:276-289), never the claim
  machinery, so toasts keep "blocking nothing"; the popup's
  outside-press close still passes the press through (M18). A toast and
  an underlying widget can still both react to one tap — that is
  today's behavior and stays (a separate question if ever wanted).
- **No topmost-wins guarantee for the one-frame tap.** The first
  hit-tested claimant wins. Topmost-wins needs lookahead the
  immediate-mode frame cannot have; settled hover does not exist in a
  tap frame, so no visible hover/click disagreement is possible.
- **No change** to the multi-frame steal (M10), the reuse semantics
  (M11), the scroll arm's raw press reads, the Escape claims, focus, or
  the veil.

## Tests — new suite `test_ui_tap`

Wired like test_ui_variants (libs/ui/CMakeLists.txt:261): the gui
archive + the r2d stub + the real font bake, `LINK ws::ui_gui`,
`DEPENDS stb`, src/ on the include path; `ctest --preset debug -R ui_tap`.

Cases:

1. **The defect pin**: two overlapping same-layer buttons, hit-tested A
   then B, one-frame tap on the overlap → `A.clicked`, `!B.clicked`.
   (Today: both — this is the red-before-green case.)
2. **Call order**: the same scene, B hit-tested first → B clicks, A
   does not.
3. **The M19 cascade**: a base-layer widget publishes, then a
   popup-layer widget publishes in the same frame → both clicked. (Pins
   the exemption the combo's open-and-pick needs.)
4. **The floor is per stratum, not per widget**: after the popup
   publication, a second base-layer widget is refused (10 <= 20).
5. **The M11 reuse**: a reuse triple landing on a different widget B →
   the frame snapshot folds the lift and the landing to the LANDING's
   verdict: A reports released with no click (and raises no floor), B
   claims the new contact (pressed, no click this frame), B clicks on
   the new contact's release next frame.
6. **The reuse steal**: reuse on A, later same-layer B overlapping the
   landing steals the new contact's press (M11's documented contract,
   now explicitly pinned).
7. **Per-pointer**: two fingers tap two overlapping pairs in one frame →
   one click per finger (the floors are per track).
8. **Next frame is clean**: a tap on B the frame AFTER A's tap click
   clicks B (the floor resets at begin_frame).
9. **The shape miss**: a tap inside A's claimable rect but outside A's
   hit shape does NOT raise the floor (no publication happened) — B,
   whose shape contains the point, still clicks.

Existing pins that must stay green (all single-widget one-frame taps —
no second claimant, so the gate is inert): test_ui_gui.cpp:1148 (the
behavior folds), :1426 (slider click-to-set), :1452 (drag_float),
:1555 (tab), :1673 (list row), :1787 (combo row toggles open); the M10
overlap suites; the M11 reuse suites; the bridge tests (the fold's slot
order runs mouse then finger, each on its own track floor; the
widget-facing OR is unchanged). Before starting, sweep the suite for
any pin of the both-click behavior — none is expected; the defect was
recorded as a defect, not a contract. (Swept at landing: no test pins
the both-click behavior, and the combo test's comment already describes
the one-frame tap accurately — no comment reconciliation was needed.)

## Docs

- libs/ui/CLAUDE.md, the "Hot/active per pointer" bullet: one sentence —
  the click floor: same-frame taps click at most one widget per stratum,
  publications rise strictly through the strata, and the reuse frame's
  new contact is exempt.
- libs/ui/src/gui/CLAUDE.md: nothing (no new file, no family split).
- The roadmap memory's open-candidates line drops the tap defect after
  landing.

## Gates

Desktop debug AND release, /W4 /WX: ws_ui + the full ui suite family
(ui, ui_gui, ui_tap, ui_canvas, ui_grid, ui_shell, ui_gui_splits,
ui_font, ui_theme_tokens, ui_painters, ui_motion, ui_primitives,
ui_variants) + check_deps. Playground compile + 6 s smoke, no new
warns. Android arm64: ws_ui + the app .so modules link (the guard is
pure CPU; the gui archive stays platform-free).

One commit: `ui gui: the click floor - a same-frame tap clicks one
widget per stratum (the M7 leftover)`.
