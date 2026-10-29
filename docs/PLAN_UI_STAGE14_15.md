# UI Task — Stage 14/15 Working Plan

Status: **in progress** (started 2026-10-29). This file is the on-repo copy of
the two stages being executed now, so the work can stop and resume at any
commit. Progress is tracked in the checklist at the bottom; every finished
block lands as its own commit.

The stages below continue the `ws::ui` arc (stages 1-13 are merged; see
`libs/ui/CLAUDE.md` for their summary).

## Stage 14: canvas layer and canvas view (UI side)

- `Canvas2D` interface, backend-agnostic: lines and polylines (joins
  miter/round/bevel, caps butt/round/square), dashed and dotted strokes,
  arcs, circles, ellipses, rounded rects, quadratic/cubic beziers, filled
  polygons (convex and simple concave), text, images, clip push/pop,
  transform stack. Strokes are anti-aliased with widths in screen-space
  pixels (independent of zoom). Curve flattening picks segment count from
  the screen-space size.
- `Renderer2DCanvas` implementation over the existing renderer. Add
  whatever primitives the renderer lacks (thick-line tessellation, AA
  edges, dash generation) inside `ws::ui`; if Part A already added
  thick-line and dash helpers, reuse them.
- `CommandListCanvas`: a recording implementation for tests (assert
  emitted primitives, no GPU).
- Text measurement API on the canvas so host logic never touches the font
  system.
- Geometry helpers in `ws::math`: point-to-segment distance,
  point-in-polygon, polyline distance and nearest point, rect/segment
  intersection, AABB union and expand.
- `ViewTransform2D`: pan, zoom (and optional rotation) with
  world<->screen mapping, zoom-at-point, zoom limits, fit-to-rect,
  animated transitions (needs Clock). Adaptive grid renderer (major/minor
  lines or dots, fades by zoom, snaps in world units).
- `CanvasView` widget: allocates a rect, applies a configurable pan/zoom
  input policy (wheel, middle drag, space+drag, touch pinch/pan), hands a
  `Canvas2D&` and the transform to a host draw callback, and supports
  retained layers (offscreen cache of static content, invalidated by
  version counters) with a documented fallback if offscreen rendering is
  unsupported.
- Tests: stroke tessellation output against expected vertex counts and
  winding, join/cap geometry, flattening tolerance, world/screen
  round-trip, zoom-at-point invariant (the point under the cursor does
  not move), grid line spacing at many zoom levels, all geometry helpers
  including degenerate input.
- Gallery page: a canvas with every primitive, joins/caps/dashes, a
  zoomable infinite grid, a stress test of 100k primitives with a
  frame-time readout.

### Stage 14 design notes (decisions taken)

- New namespace `ws::ui::canvas`; headers under `include/ws/ui/canvas/`,
  sources under `src/canvas/` — they join the `ws_ui_gui` pure archive
  (draw through the r2d free-function seam, no Vulkan/GPU link dep).
- r2d gains ONE primitive: `draw_vertex_quad(pos[4][2], uv[4][2],
  rgba[4][4], texture)` — a free-form quad with per-corner color. Triangles
  are degenerate quads (corner duplicated), matching the fixed
  0,1,2/2,3,0 index pattern; `DrawCommand` grows from one packed color to
  one per corner.
- AA is CPU-tessellated: cores at full alpha plus 1px fringe quads whose
  corner alpha ramps 1 -> 0 (coverage is 50% at the geometric edge).
  Hairlines (width <= 1px) are two alpha-ramped quads.
- Stroke widths and AA fringes are SCREEN pixels (independent of camera
  zoom and canvas transform scale); dash lengths are PATH-LOCAL units
  (the SVG convention). The canvas multiplies the camera zoom (set by
  `CanvasView` via `set_view_scale`) with its own transform scale.
- All tessellation is pure and public (`canvas/tessellation.hpp`) so the
  tests can assert vertex counts and winding directly.
- Retained layers: r2d v1 has no offscreen render target, so a retained
  layer is a CPU-side `CommandList` cache (record once, replay through the
  canvas each frame) invalidated by a version counter — the documented
  fallback; the GPU offscreen cache is where a future render-target
  extension plugs in (same invalidation contract).

## Stage 15: workspace, panel framework and AppShell

Needs Part A Stage 7 (docking) and Stage 6 (property grid), plus Stages
1-6 of this task. Do not start until both are confirmed merged.

**Prerequisite gap found in this repo (2026-10-29):** neither Part A's
docking nor its property grid exists here — the only docking in tree is
ImGui's own backend, and no property-grid widget has landed. Decision:
build minimal in-repo versions as part of this stage (a split-tree dock
host with tabbed leaves, splitters and json serialization; a
schema-driven settings/property grid over the existing widget rows) and
mark the seam clearly so a future full Part A can replace them.

- Panel registration API: id, title, icon, default dock placement, draw
  callback, `onFocus`/`onClose` hooks, multi-instance support, a
  per-panel shortcut context, and an optional per-panel settings scope.
  Focus and active-panel tracking; panel-scoped shortcut contexts hook
  into Stage 5.
- Layouts: per-user and per-project persistence via Stage 2 and Part A's
  dock serialization; named layouts (save, restore, reset to default);
  an automatic "Window" menu listing panels and layouts.
- `AppShell`: ties it all together into one reusable skeleton. Menu bar
  and toolbar from the registry, dock space, status bar, command palette,
  toast host, modal host, window title with dirty state, drag-and-drop
  of files, recent files, autosave/crash recovery, settings panel
  auto-generated from the schema (property grid), theme and font from
  settings with live reaction, a job-progress indicator in the status bar
  that lists running jobs with cancel, and clean shutdown (cancel jobs,
  flush settings, save layout).
- A document abstraction: `Document` interface (path, dirty flag via undo
  save-point, load/save through Result, per-document job group,
  per-document undo stack), plus `DocumentManager` (open, close with a
  save/discard/cancel prompt, activate, multiple documents, restore-
  session list).
- The shell contains no product-specific code. A host app registers
  commands, panels, a document type, and a settings schema, and
  everything else is provided.
- Tests: panel registration and layout restore with missing panels,
  document close-with-dirty flow, session restore, shutdown ordering
  (jobs cancelled before panels destroyed), shell startup with an empty
  config directory and with a corrupt one.
- `apps/shell_example`: a minimal app (a text-and-plot "scratchpad")
  built only from the shell: two panels (an editor and a plot fed by a
  background job), a document type, a few commands, a settings schema.
  It is the acceptance test of the whole task.

### Stage 15 design notes (decisions taken)

- New sources under `src/shell/` join the `ws_ui_gui` archive too; the
  archive gains a PUBLIC `ws::platform` dep (headless services: recent
  files, autosave, title; the OS halves stay in `ws_platform`'s win/
  android TUs). `AppShell` never calls a `make_*` factory — the host
  injects `FileDialogs`/`MessageBoxes` interfaces (fake-friendly).
- `AppShell` is a plain class the host's `App` subclass owns and drives
  from `on_update()`; it never touches `ws::ui::App` itself, so the whole
  shell remains headless-testable.
- Panel-scoped shortcuts: `ws::app`'s `KeyContext::panel` rung is the
  context; a panel-scoped command's `enabled` predicate is gated on the
  shell's active panel (the resolver stays untouched).

## Progress checklist

- [x] Stage 14 — geometry helpers in ws::math (point_segment, polyline,
      point_in_polygon, segment_clip_rect, bounds; + Vec2 cross,
      right_normal, unpack_color, arc_step for the canvas)
- [x] Stage 14 — r2d per-corner-color primitive (draw_vertex_quad,
      already in renderer2d.hpp)
- [x] Stage 14 — tessellation core + tests (stroke/fill/flatten/dash,
      math-lib vector math, Vec2[4]/Vec4[4] seam)
- [x] Stage 14 — Canvas2D / CommandListCanvas / Renderer2DCanvas
      (interface + recording sink for tests + r2d-driving sink)
- [ ] Stage 14 — ViewTransform2D + adaptive grid + CanvasView + tests
- [ ] Stage 14 — gallery canvas page
- [ ] Stage 15 — property grid + tests
- [ ] Stage 15 — dock host (split tree + serialization) + tests
- [ ] Stage 15 — panel framework + tests
- [ ] Stage 15 — Document/DocumentManager + session restore + tests
- [ ] Stage 15 — AppShell + shutdown ordering + tests
- [ ] Stage 15 — apps/shell_example
- [ ] docs refresh (CLAUDE.md files) + final zip
