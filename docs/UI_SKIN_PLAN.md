# UI Task — the Skinnable-Library Plan (tokens, painters, motion, text, icons)

Status: **in progress**. The premise: `ws::ui` is a LIBRARY, and an app
must be able to restyle it — deeply, not just recolor it — without
forking. Every improvement in this plan lands as exactly one of two
things:

- a **THEME TOKEN** — a value in `ChromeTheme` (or the data around it)
  the app overrides, or
- a **PAINTER** — a function pointer the app replaces.

Never a hardcoded look inside a widget. The stock appearance is nothing
special: the default tokens plus the default painters. If a stock widget
cannot express a change through those two seams, the design is wrong and
comes back here.

Hard constraints, all of them standing for every phase:

- The six existing apps (playground, ui_gallery, circsim_editor,
  shell_example, netchat, wavplayer) compile UNCHANGED and behave the
  same. The default look may improve; behavior and API never break.
- No third-party header under any `libs/*/include/`; no exceptions across
  lib boundaries; no per-frame heap allocation in any draw path.
- Animation state lives in the GuiContext side tables (bounded, swept
  like every other persistent state), never in widget locals or
  statics.
- `test_ui_gui` stays green; new tests land in NEW files under
  `libs/ui/tests/` (the widget-file size rule applies to tests too).
- One commit per phase, each gated on the full suite plus check_deps.
- Ground truth re-verified at c015926 before this plan landed:
  - The behavior/draw split already covers button, checkbox, radio,
    toggle_switch, collapsing_header, selectable, slider, drag_float,
    spinner, progress_indeterminate, tab, combo, text_input, the three
    scroll halves, `panel_draw`, and popup placement. NOT split (their
    look is inline): the menu item renderer (widgets_menus.cpp ~617-712),
    the menu title row (~866), the table header/row look
    (widgets_table.cpp ~93-202), the toast chrome + severity band
    (widgets_toasts.cpp ~192-201), and the overlay frame
    (`panel_chrome` + the veil quad, widgets_detail.hpp /
    widgets_overlays.cpp).
  - `ChromeTheme` is a constexpr pure-value struct; one `corner_radius`,
    one `font`, `scaled()` multiplies px metrics; persistence is
    theme_json.cpp / ThemeData (theme_data.cpp) / contrast in
    theme_color.cpp. `ThemeData` already carries FontRole x4, icon_set
    (an unresolved string), and ShadowTokens x2 as inert data.
  - r2d draws rounded quads through an analytic SDF in batch.frag; the
    radius rides the per-vertex tex_index channel as `-radius/w`. The
    vertex layout (color + uv + flat float) does not change in this plan
    unless the layered fallbacks below are PROVEN insufficient.
  - The font bake (font.cpp) uses stbtt_MakeCodepointBitmap at one
    integer size: no oversampling, no coverage/gamma correction, one
    weight; app code hardcodes segoeui/Roboto/dejavu paths; glyph quads
    snap to whole pixels on the axis-aligned 2D camera.
  - There is no animation anywhere ("No animation in v1" at
    widgets_buttons.cpp ~293); borders are 1px straight draw_quad lines;
    there is no rounded outline, no shadow primitive, no icon set, no
    text_secondary, no button variants.

## Phase 0 — the audit and the finished splits

- Verify every widget family's split state against the list above
  (done at plan time; re-verify at land time).
- Finish the missing splits, NO behavior change: lift the menu item
  renderer's look into `menu_item_draw` (+ its behavior half if needed
  to carry the highlight state it already tracks), the table row/header
  look into `table_row_draw`/`table_header_draw`, the toast look into
  `toast_draw`, and the overlay frame into `popup_frame_draw` (wrapping
  panel_chrome at overlay bounds). Each new draw half takes the same
  shape as the existing ones: rect + result/state + theme (+ the row's
  own data), reads no context, recomputes its own label fits.
- Write the findings + the exact painter signature list into
  `libs/ui/src/gui/CLAUDE.md` (short — the table itself lives in the
  painters header once P3 lands).
- Gate: the suite byte-identical behavior (the stub-recording tests are
  the proof; only their recording of the SAME draws from the new
  call sites may differ in nothing).

## Phase 1 — the font bake's quality pass

- font.cpp (ui-private): bake through
  `stbtt_MakeCodepointBitmapSubpixelPrefilter` with 2-3x HORIZONTAL
  oversampling (the axis the renderer filters on), both bake paths —
  the primary `bake_file` and the lazy `bake_face_glyph`. Keep the Font
  public contract exact: same advance fields, same
  measure_text == draw_text return identity, same offset convention.
- Apply a coverage contrast/gamma LUT at bake time, parameterized
  through a `load_font` options argument (a trailing struct, defaulted
  to today's identity-plus-the-new-default: a gamma tuned for
  light-on-dark text; the knob exists so apps flip it for dark-on-light
  without a second bake function).
- Bundle a default face: Inter (OFL) with its license file, embedded
  (a generated byte array in src/text/, never under include/) or loaded
  from a documented assets path — `load_default_font(px)` answers a
  baked Inter so the stock look is identical on Windows and Android.
  Existing `load_font(path, px)` keeps working for app-supplied faces.
- Support the strong weight: the SECOND weight is a second font the app
  bakes (`load_font` on Inter-SemiBold) — no mixed-weight Font; P2's
  `font_strong` token is the pointer the theme carries to it.
- Keep the whole-pixel glyph snap (axis-aligned zoom-1 camera) exactly.
- Tests: bake measure==draw (already pinned; keep), the LUT's default
  changes coverage bytes but no metrics, oversampling changes no
  advance, the bundled font bakes and measures ASCII exactly, the OFL
  text ships in the repo.

## Phase 2 — the token pass

New ChromeTheme fields (app-overridable values, appended so every
existing designated-initializer theme still compiles — order-preserved
insertions only):

- Colors: `text_secondary` (quiet labels, placeholders, hints — derived
  as text-at-lower-alpha or a stepped gray on both stock themes),
  `text_on_accent` (the label color on accent faces — primary buttons,
  checked fills; near-white on both stocks), `accent_hover` (the
  accent's hover rung; apply_accent derives it), `border_strong`
  (dividers, table rules, active edges), `shadow` (the color shadows
  multiply toward; black at low alpha on dark, a warm gray on light).
- Radii, replacing nothing: `radius_control`, `radius_panel`,
  `radius_popup`, `radius_pill` — `corner_radius` STAYS as the default
  every one of them initializes from (kDefaultChrome leaves all four
  unset = corner_radius; an app overriding only corner_radius still
  restyles everything, the back-compat rule).
- Metrics: `border_width` (default 1 — today's hardcoded 1px lines),
  `control_height` default moves to 28 (the 26→28 bump is the one
  deliberate stock-look change; scaled() carries it), shadow
  `shadow_blur` / `shadow_offset_y` (default 0 — stock draws none until
  P5 adopts them; a theme that sets them early just waits for the
  render), `focus_ring_width` (default 1).
- Motion: `anim_duration` (seconds, default 0.10) and `anim_enabled`
  (bool, default true; false = INSTANT — every animated value jumps,
  byte-identical to today's output).
- Fonts: `font_strong` (const r2d::Font*, null = fall back to `font`).
  `font_small`/`font_mono` stay in ThemeData's FontRole data (no third
  baked pointer in the chrome yet — no widget consumes them today).
- `scaled()` multiplies every new px metric, carries timings and
  enabled flags as-is.
- theme_json.cpp: read/write the new fields (canonical order, unknown
  members preserved as today); an old file (no new fields) loads with
  every new token at its default — the migration is "absent = default,"
  no version bump needed, but a written-then-reloaded file is
  byte-stable (round-trip test).
- theme_color.cpp: `contrast_failures()` gains the new checked pairs —
  text_secondary against both backgrounds (AA 4.5 — it is real text),
  text_on_accent against control_on and control_pressed (AA 4.5, the
  primary-button label). apply_accent derives accent_hover and walks
  text_on_accent the same way it walks control_on. The stock themes
  pass with the new defaults (pick the default values to clear, not to
  exempt).
- Tests: every new token round-trips JSON; absent-field load = default;
  the stock themes still pass contrast (both chromes); designated-
  initializer themes from apps still compile (the apps ARE this test).

## Phase 3 — the painters seam

- `struct WidgetPainters` in a new public header
  (include/ws/ui/gui/painters.hpp): pure function pointers, one per
  widget draw half, exact same signatures as the public *_draw
  functions — button, checkbox, radio, toggle_switch,
  collapsing_header, selectable, slider, drag_float, spinner,
  progress_indeterminate, tab, combo, text_input, scrollbar, panel,
  popup_frame, menu_item, table_row, table_header, toast. Plus the
  focus ring and the statics? NO — the statics (text, separator,
  progress, status_bar) stay plain functions: they are chrome, not
  citizens, and a skin recomposes them from the primitives it gets.
- `ChromeTheme` gains `const WidgetPainters* painters = nullptr` —
  null = stock (each widget calls its own file's static draw). The
  composed widgets route through one `theme.painters ? painters->x :
  x_draw` inline fold; the behavior halves never look at painters.
- Stock painters stay in their widget files (declared in widgets.hpp
  as today); a `kStockPainters` constant table (in painters.hpp, the
  one place that names them all) lets an app start from stock and
  override one row.
- Contract, written as the header's comment: a painter receives the
  behavior's answer (state/focus/hit), the rect, the theme and the
  row's data; it draws ONLY — no context, no state change, no
  allocation; it may read any token (including painters of its own
  sub-rows — a skin's button may call the stock checkbox draw if it
  wants, composition not inheritance).
- Benchmark the seam: the pointer load + indirect call per draw, stock
  vs a table — report the number in the commit message (the r2d-stub
  harness timing N thousand button frames; a scripts/ bench, not a
  test, so timing never gates CI).
- Tests: a recording painter (a test-local table whose entries append
  to a vector) proves the routing — every widget family's composed call
  reaches the table entry when painters != null, and the stock path
  when null; the behavior halves are painter-blind (the existing
  behavior tests unchanged).

## Phase 4 — the motion pass

- `float animate_toward(GuiContext&, WidgetId, float target, float
  duration)` — the ONE helper: persistent float under the id, frame
  clock (dt_seconds from the frame input, the injected-clock test
  seam), exponential settle (`t += (target - t) * (1 - exp(-dt *
  k))` with k chosen so `duration` is the 95% time; instant when
  duration <= 0 OR the theme's anim_enabled is off — the caller passes
  theme.anim_duration and the helper reads nothing global). Returns
  the current value; when it moved, it flags the context's redraw want.
- `gui.wants_redraw()` on GuiContext: any animation moved this frame OR
  any animator is unsettled (|value - target| > epsilon) sets it; the
  app loop (App/shell) renders-on-demand otherwise. The existing apps
  render every frame regardless — wants_redraw is advisory, v1, and
  nothing breaks when it is ignored (the stock loop keeps rendering).
- Wire the travelers, all through the helper, all gated on
  anim_enabled (off = EXACTLY today's output — the test pins it):
  - control-face lerp: button/checkbox/radio/tab/combo/selectable rows
    animate control_bg -> control_hovered (and back) on the hover
    edge; pressed stays instant (latency beats smoothness).
  - toggle_switch knob travel: the knob's x animates between its ends
    (the v1 "no travel" note's retirement).
  - tab accent: the selected tab's accent underline fades/slides in.
  - overlay fade-in: popup/tooltip/toast open with a short alpha ramp
    (the frame's first ~80ms), applied INSIDE the draw halves through
    the animate_toward value under the overlay's id; the veil stays
    instant (blocking is the point).
- State: one float per animated id in the persistent side table —
  swept by the existing sweep_persistent_state, bounded by the table
  cap, no new machinery.
- Tests: determinism with injected dt (fixed steps land on the
  settled value within the documented duration), disabled = instant
  (byte-identical draw recordings vs a no-anim build of the same
  frames), wants_redraw set/unset exactly at the settle boundary.

## Phase 5 — outline + shadow (the r2d primitives)

- `draw_rounded_outline(x, y, w, h, radius, thickness, color)` and
  `draw_shadow(x, y, w, h, radius, blur, color)` in the PUBLIC ws::ui
  r2d API (plain types only, no third-party).
- Preferred implementation, NO vertex-layout or shader change:
  - outline = an outer rounded quad in the border color + the caller's
    content (an inner rounded quad inset by thickness with
    radius - thickness) — the painter composes; nothing new in the
    batch.
  - shadow = 3-5 stacked rounded quads, alpha stepping down, radius/
    spread stepping out — measure the quad count against the look and
    write the tradeoff into the commit. If the layered look is
    demonstrably not good enough (banding on soft blurs), THEN extend
    batch.frag's negative-branch: a ring SDF (`abs(d + w/2) - w/2`)
    for outlines and a smoothstep falloff for shadows, both keyed off
    NEW negative encodings in the same tex_index channel (the layout
    still changes zero bytes), both tiers (chunked + bindless) from the
    one source, spirv.h regenerated by the existing script — glslang
    pinned 16.6.0, flags only in the script.
- Stock adoption, all through tokens: panel/popup frames draw the
  border as a rounded outline (border_width, radius_panel/popup), the
  focus ring becomes a rounded outline at focus_ring_width (replacing
  the solid-quad-masks-body trick, same visual weight), popups and
  toasts draw a shadow when the theme's shadow tokens are nonzero
  (stock default: a modest popup/toast shadow, NO panel shadow —
  flat chrome stays flat), radius_pill lands on the toggle track and
  scrollbar thumb.
- Tests: the stub counts quads (an outline = exactly 2, a shadow =
  the documented N), the painter paths draw the same geometry for the
  same tokens, contrast/behavior tests untouched.

## Phase 6 — icons, variants, pixel snapping

- The minimal built-in icon set: chevron-down, chevron-right, check,
  close, search, plus, minus — baked at font-bake time into the font's
  own pages (the atlas machinery already there; codepoints in a
  private-use range the Font serves) or a tiny dedicated atlas; either
  way, one `enum class Icon` + `r2d::Sprite icon_sprite(Icon)` +
  `draw_icon(x, y, size, Icon, color)` and a replaceable mapping (the
  theme's painters table grows an `icon_of(Icon) -> Sprite` row an app
  overrides to re-point at its own atlas). Combo's marker, the
  collapsing header's marker, the text_input clear affordance and
  menu-parent arrows consume them through the painters.
- `ButtonStyle { secondary, primary, ghost, danger }` as a trailing
  optional parameter on button/button_behavior/button_draw (default
  secondary = today's look; primary = accent face + text_on_accent
  label; ghost = transparent idle + hover face; danger = the error
  family). Resolved through tokens — apply_accent derives the danger
  faces alongside the accent's; no widget hardcodes a color.
- Pixel snapping in the painters: widget rects and 1px strokes snap
  to device pixels (the 0.5px offset for odd strokes at any
  ui_scale) — a small `snap_rect`/`snap_x` helper in widgets_detail,
  applied at the stock painters' draw sites, no hit-test changes
  (the behavior rects stay fractional; only the PAINT snaps).
- Tests: the icon sprites bake (sizes, nonzero coverage), the button
  variants' face resolution per style (token-driven table test), the
  snap helper's rounding table, and the composed widgets' variant
  defaults still byte-match the pre-variant recordings (secondary is
  the default — the suite proves no regression).

## Phase 7 — the proof: ui_gallery's Skins page + docs

- A new gallery page ("Skins") rendering the same widget board under
  three looks, side by side: stock dark, stock light, and one CUSTOM
  skin — a flat/square look (radius 0, border_strong dividers, no
  shadows, anim off) built ENTIRELY from a token override + a painters
  table that replaces 2-3 rows. The page IS the acceptance: the
  library restyled without forking, live.
- Update the docs: this plan's status, the gui CLAUDE.md (painters
  contract already there from P0; add the token table), the
  theme_json section of libs/ui/CLAUDE.md for the new fields, and
  apps/CLAUDE.md for the gallery page.
- The phase gates: everything green, check_deps clean, and the Skins
  page renders under Xvfb (the visual check script's pattern) with
  all three looks distinguishable in the screenshot.

## What this plan deliberately does NOT do (and why)

- No shader change before P5 proves the layered quads insufficient —
  the vertex layout and the two-tier shader sync are the repo's most
  fragile invariants; the layered path risks nothing.
- No virtual-widget/painter interfaces, no std::function, no
  allocation in the draw path — the fn-pointer table is the whole
  seam; a painter that needs state keeps it in the persistent side
  tables like any widget.
- No mixed-weight fonts, no kerning, no runtime font scaling — one
  font one size stays the contract; the strong weight is a second
  pointer.
- No localization of dialog/button labels, no RTL, no multi-select
  tables — adjacent to look, out of scope.
- No retained-mode tree, no layout system (flex/grid) — the cursor
  stays; skins change pixels, not flow.
- Icon set stays minimal (7 glyphs) — apps with real icon needs bring
  an atlas and re-point the painters' icon row; the library ships a
  floor, not a ceiling.

## The commit trail (one per phase)

P0 audit+splits → P1 font bake → P2 tokens → P3 painters → P4 motion
→ P5 outline/shadow → P6 icons/variants/snap → P7 gallery+docs. Each
commit message answers the three questions: what changed, what was
tested, and what was deliberately NOT done.
