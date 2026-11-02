#pragma once

// The schematic grid (stage 16): one spec shared by the DRAW and the
// SNAP, a pure math half that picks the level and the index ranges,
// and a draw half that paints pixel-snapped dots or hairlines in
// SCREEN space. The editor's canvas panel and CanvasView both ride it
// - there is exactly one grid implementation in the library.
//
// The density model is the x5 ladder (major_interval): the finest
// rung's spacing is the snap grid (GridSpec::base) times
// major_interval^k, with k the smallest exponent whose cell covers
// min_cell_px on screen. The next level's minors ARE this level's
// majors, so a level change never moves a visible mark - and because
// a mark's alpha, color and size are functions of its OWN cell size
// alone (never of its minor/major role), the handoff at a level
// change is pixel-identical: the marks that drop out are already at
// zero alpha, and every mark that survives keeps its exact look.
//
// All pure math + Canvas2D draws - no gui, no input, no GPU. The
// compute half allocates nothing and is unit-testable headless; the
// draw half batches each rung into fill_rects (a stack buffer, one
// canvas call per style group - never a fill_circle per dot).

#include <cmath>
#include <cstdint>

#include <ws/ui/canvas/canvas2d.hpp>

namespace ws::ui::canvas {

// The single source of truth for one grid: the spacing ladder, the
// faces, the density policy. The renderer reads it; the host's snap
// reads it (snap_coord below) - the editor builds it from
// Schematic::grid and paints its theme's tokens over the faces.
struct GridSpec {
    // The finest spacing, in world units - the snap grid. Never
    // subdivided: the ladder only coarsens.
    double base = 1.0;
    // The ladder factor and the major interval in one number: majors
    // sit every `major_interval` minors, and the next level's minors
    // are this level's majors. 5 is the schematic convention. Values
    // below 2 disable the ladder (one rung, density left to the cap).
    int major_interval = 5;
    // The faces, packed 0xRRGGBBAA. minor ~6-8% alpha, major ~16-20%
    // (the theme's grid_minor/grid_major tokens); a mark's color
    // blends from minor toward major as its cell grows past
    // full_cell_px (its alpha curve is below).
    std::uint32_t minor_color = 0xFFFFFF12u;  // white, 7%
    std::uint32_t major_color = 0xFFFFFF2Eu;  // white, 18%
    // The axes and the origin marker: an accent-token face at a
    // modest alpha - a distinct but subtle reading of x=0 / y=0, not
    // a loud crosshair.
    std::uint32_t axis_color = 0x5490D04Cu;  // the stock accent, 30%
    // Dots at the intersections (the schematic look) vs 1px
    // hairlines (the graph-paper look).
    bool show_dots = true;
    // The density policy, in screen px per cell:
    //   min_cell_px    - below this a rung's marks are invisible
    //                    (the minors "hidden" band; the ladder's
    //                    level boundary rides here too).
    //   full_cell_px   - a fading-in rung reaches full alpha.
    //   major_cell_px  - a rung this wide reads as major: the dot
    //                    grows 1px -> 2px and the color finishes its
    //                    minor -> major blend.
    //   vanish_cell_px - the TOP rung fades out over
    //                    [vanish_cell_px, 5 * vanish_cell_px] so the
    //                    coarsest marks never pop in at a level
    //                    change (the curve bottoms out at 0).
    float min_cell_px = 10.0f;
    float full_cell_px = 24.0f;
    float major_cell_px = 120.0f;
    float vanish_cell_px = 600.0f;
    // The per-frame mark cap: over it the grid degrades by hiding the
    // FINEST rungs first (minors before majors), never by thinning
    // within a rung.
    int max_marks = 20000;
};

// One frame's view of the world: everything the math needs. The
// viewport is centered on (center_x, center_y) at `zoom` screen px
// per world unit; (ox, oy) is the viewport's top-left in the canvas's
// current space. y_flip maps a y-up world (the schematic model) onto
// the y-down screen; a y-down world (CanvasView's convention) passes
// false.
struct GridFrame {
    double center_x = 0.0;
    double center_y = 0.0;
    double zoom = 1.0;
    float ox = 0.0f;
    float oy = 0.0f;
    double view_w = 0.0;
    double view_h = 0.0;
    bool y_flip = true;
};

// The brightness curve one rung's marks follow as their cell size
// grows: 0 below min_cell_px, smoothstep up to 1 at full_cell_px, a
// plateau, then 1 -> 0 over [vanish_cell_px, 5 * vanish_cell_px].
// Public because it IS the no-pop contract - the tests pin it.
[[nodiscard]] float grid_rung_alpha(float cell_px, const GridSpec& spec);

// One ladder rung, resolved for a frame: its spacing, its screen
// cell, its alpha (the curve above), whether it reads as major (the
// 2px dot), its viewport-culled inclusive index ranges, and whether
// it is the top rung (the only one that draws the marks the rung
// above it would own - i % interval == 0).
struct GridRung {
    double spacing = 1.0;   // world units between marks
    float cell_px = 10.0f;  // spacing * zoom
    float alpha = 0.0f;     // grid_rung_alpha(cell_px, spec)
    bool major = false;     // cell_px >= major_cell_px
    bool draw_all = false;  // the top rung draws every index
    // The culled inclusive index ranges (world = index * spacing).
    long long ix0 = 0, ix1 = 0;
    long long iy0 = 0, iy1 = 0;
};

// compute_grid's answer: the rungs to draw, finest first, plus the
// frame (kept for the mark -> screen helpers) and the facts the tests
// and the host's feedback want. `step` is the finest DRAWN rung's
// spacing (== GridSpec::base after the cap may have hidden it, the
// "minors hidden, only majors" state).
struct GridMetrics {
    GridFrame frame{};
    double step = 0.0;           // 0 = nothing to draw (bad frame)
    float minor_fade = 0.0f;     // the finest rung's alpha
    bool minors_visible = true;  // false when the cap hid rung 0
    int rung_count = 0;
    GridRung rungs[4]{};         // rung_count of them, finest first
    long long mark_estimate = 0; // the total marks the rungs would draw
    bool x_axis_visible = false; // the world x=0 line crosses the view
    bool y_axis_visible = false;

    // A mark's unsnapped canvas-space centre, computed fresh from the
    // integer index every call (never by accumulating floats).
    [[nodiscard]] double mark_x(const GridRung& r, long long i) const {
        return frame.ox + frame.view_w * 0.5 +
               (static_cast<double>(i) * r.spacing - frame.center_x) *
                   frame.zoom;
    }
    [[nodiscard]] double mark_y(const GridRung& r, long long j) const {
        const double dy = (static_cast<double>(j) * r.spacing -
                           frame.center_y) * frame.zoom;
        return frame.y_flip ? frame.oy + frame.view_h * 0.5 - dy
                            : frame.oy + frame.view_h * 0.5 + dy;
    }
};

// The pure half: pick the level, cull the ranges, apply the cap. No
// allocation, no drawing - the unit tests' whole target.
[[nodiscard]] GridMetrics compute_grid(const GridFrame& frame,
                                       const GridSpec& spec);

// Snap one coordinate to the spec's lattice: the fine grid (base)
// always; `coarse` snaps to the major lattice instead (base *
// major_interval - an integer multiple, so a coarse position is
// always a fine position too). The host's snap function calls this so
// the grid the user sees and the grid the editor snaps to can never
// disagree.
[[nodiscard]] inline double snap_coord(double v, const GridSpec& spec,
                                       bool coarse) {
    const double lattice =
        coarse && spec.major_interval >= 2
            ? spec.base * static_cast<double>(spec.major_interval)
            : spec.base;
    return lattice > 0.0 ? std::round(v / lattice) * lattice : v;
}

// The device-pixel placement for a mark of `size` px whose centre
// falls near `c`: odd sizes sit centred on a pixel centre (floor +
// 0.5), even sizes on a whole pixel (round), so the rect's corners
// are always exact device pixels - a 1px dot covers exactly one pixel
// column/row, a 2px dot exactly two, at any pan/zoom/ui_scale.
[[nodiscard]] inline float snap_mark(double c, float size) {
    const bool odd = static_cast<int>(size) % 2 != 0;
    return odd ? static_cast<float>(std::floor(c)) + 0.5f - size * 0.5f
               : static_cast<float>(std::llround(c)) - size * 0.5f;
}

// The draw half. Dots: each rung batches into fill_rects (a stack
// buffer, no per-dot allocation), minors 1px, majors 2px, the axes as
// thin snapped lines in the axis face plus a 3px origin marker.
// Lines: one snapped stroke_line per mark, centre on floor + 0.5.
// `brightness` (0..1+) scales every face's alpha - the editor's
// context-aware dim rides here. Drawn in the canvas's CURRENT space;
// push nothing, pop nothing.
void draw_grid_marks(Canvas2D& canvas, const GridSpec& spec,
                     const GridMetrics& metrics, float brightness = 1.0f);

// Compute + draw in one call - the CanvasView path.
void draw_grid(Canvas2D& canvas, const GridFrame& frame,
               const GridSpec& spec = {}, float brightness = 1.0f);

}  // namespace ws::ui::canvas
