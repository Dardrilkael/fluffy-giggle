// grid.hpp's implementation. compute_grid is the pure half (level
// pick, culling, cap); draw_grid_marks is the draw half (pixel-snapped
// dots or hairlines through Canvas2D). The no-pop contract lives in
// grid_rung_alpha: a mark's alpha, color and size are functions of its
// OWN cell size alone, so the level ladder's handoffs are invisible.

#include <ws/ui/canvas/grid.hpp>

#include <algorithm>
#include <cmath>

namespace ws::ui::canvas {

namespace {

// smoothstep over [a, b], clamped; a degenerate window (b <= a) is a
// step at b so a hand-edited spec can never divide by zero.
[[nodiscard]] float smooth01(float a, float b, float x) {
    if (b <= a) {
        return x >= b ? 1.0f : 0.0f;
    }
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// The inclusive index range of `bound / spacing`, with a relative
// epsilon so a mark sitting exactly on the viewport's edge is never
// lost to double rounding (a fractional center or zoom must not be
// able to shave a mark).
[[nodiscard]] long long index_lo(double bound, double spacing) {
    const double v = bound / spacing;
    const double eps = 1e-9 * (std::fabs(v) + 1.0);
    return static_cast<long long>(std::ceil(v - eps));
}
[[nodiscard]] long long index_hi(double bound, double spacing) {
    const double v = bound / spacing;
    const double eps = 1e-9 * (std::fabs(v) + 1.0);
    return static_cast<long long>(std::floor(v + eps));
}

// Packed-color helpers (r2d order: 0xRRGGBBAA, alpha the LOW byte).
[[nodiscard]] std::uint32_t scale_alpha(std::uint32_t color, float f) {
    float a = static_cast<float>(color & 0xFFu) * f;
    a = std::clamp(a, 0.0f, 255.0f);
    return (color & 0xFFFFFF00u) | static_cast<std::uint32_t>(a);
}

// Linear blend of every channel (rgb AND alpha) - the minor -> major
// color migration a mark follows as its cell grows.
[[nodiscard]] std::uint32_t blend_color(std::uint32_t a, std::uint32_t b,
                                        float t) {
    std::uint32_t out = 0;
    for (int shift = 0; shift <= 24; shift += 8) {
        const float ca = static_cast<float>((a >> shift) & 0xFFu);
        const float cb = static_cast<float>((b >> shift) & 0xFFu);
        const float c = ca + (cb - ca) * t;
        out |= static_cast<std::uint32_t>(std::lround(c)) << shift;
    }
    return out;
}

// The stack chunk the dot batches flush through - no per-dot heap
// allocation on any path.
constexpr int kDotBatch = 512;

}  // namespace

// --- the curve ----------------------------------------------------------------

float grid_rung_alpha(float cell_px, const GridSpec& spec) {
    if (cell_px <= spec.min_cell_px) {
        return 0.0f;  // the "minors hidden" band
    }
    if (cell_px < spec.full_cell_px) {
        return smooth01(spec.min_cell_px, spec.full_cell_px, cell_px);
    }
    const float vanish_end = spec.vanish_cell_px * 5.0f;
    if (cell_px < spec.vanish_cell_px) {
        return 1.0f;  // the plateau
    }
    if (cell_px < vanish_end) {
        return 1.0f - smooth01(spec.vanish_cell_px, vanish_end, cell_px);
    }
    return 0.0f;  // the coarsest rung bottoms out: nothing pops in
}

// --- the pure half -------------------------------------------------------------

GridMetrics compute_grid(const GridFrame& frame, const GridSpec& spec) {
    GridMetrics m;
    m.frame = frame;
    if (!(spec.base > 0.0) || !(frame.zoom > 0.0) || frame.view_w <= 0.0 ||
        frame.view_h <= 0.0) {
        return m;  // step 0: nothing to draw
    }
    const bool ladder = spec.major_interval >= 2;
    const double factor =
        ladder ? static_cast<double>(spec.major_interval) : 1.0;

    // The level: the finest rung whose cell clears min_cell_px. Never
    // below base - the snap grid is never subdivided.
    double s = spec.base;
    if (ladder) {
        for (int guard = 0; s * frame.zoom < spec.min_cell_px && guard < 64;
             ++guard) {
            s *= factor;
        }
    }
    m.step = s;

    // The rungs: finest first, up to four (the vanish curve bottoms
    // out inside the fifth), each with its culled index ranges.
    const float min_alpha = 0.004f;  // ~1/255: below this is invisible
    double rung_spacing = s;
    for (int r = 0; r < 4; ++r) {
        const float cell = static_cast<float>(rung_spacing * frame.zoom);
        const float alpha = grid_rung_alpha(cell, spec);
        if (r > 0 && alpha <= min_alpha) {
            break;
        }
        GridRung& rung = m.rungs[m.rung_count];
        rung.spacing = rung_spacing;
        rung.cell_px = cell;
        rung.alpha = alpha;
        rung.major = cell >= spec.major_cell_px;
        const double half_w = frame.view_w * 0.5 / frame.zoom;
        const double half_h = frame.view_h * 0.5 / frame.zoom;
        rung.ix0 = index_lo(frame.center_x - half_w, rung_spacing);
        rung.ix1 = index_hi(frame.center_x + half_w, rung_spacing);
        rung.iy0 = index_lo(frame.center_y - half_h, rung_spacing);
        rung.iy1 = index_hi(frame.center_y + half_h, rung_spacing);
        // The degenerate-spec guard: a spec that disables the ladder
        // and zeroes the thresholds cannot make the draw iterate a
        // billion indices (the cap still hides the rung if a coarser
        // one exists).
        constexpr long long kMaxSpan = 65536;
        if (rung.ix1 - rung.ix0 > kMaxSpan) {
            rung.ix1 = rung.ix0 + kMaxSpan;
        }
        if (rung.iy1 - rung.iy0 > kMaxSpan) {
            rung.iy1 = rung.iy0 + kMaxSpan;
        }
        ++m.rung_count;
        rung_spacing *= factor;
        if (!ladder) {
            break;  // one rung, no ladder
        }
    }
    if (m.rung_count == 0) {
        m.step = 0.0;
        return m;
    }
    // Only the top rung draws the marks the rung above it would own.
    m.rungs[m.rung_count - 1].draw_all = true;

    // The axes cross the viewport at all (the draw snaps them).
    const double half_w = frame.view_w * 0.5 / frame.zoom;
    const double half_h = frame.view_h * 0.5 / frame.zoom;
    m.x_axis_visible = frame.center_x - half_w <= 0.0 &&
                       0.0 <= frame.center_x + half_w;
    m.y_axis_visible = frame.center_y - half_h <= 0.0 &&
                       0.0 <= frame.center_y + half_h;

    // The cap: hide the FINEST rungs first, never thin within one.
    const auto estimate = [&m](int count) {
        long long total = 0;
        for (int r = 0; r < count; ++r) {
            const GridRung& g = m.rungs[r];
            total += (g.ix1 - g.ix0 + 1) * (g.iy1 - g.iy0 + 1);
        }
        return total;
    };
    while (m.rung_count > 1 && estimate(m.rung_count) > spec.max_marks) {
        --m.rung_count;
        m.minors_visible = false;
        // Shift the kept rungs down so rungs[0] is still the finest
        // drawn one, and re-flag the top.
        for (int r = 0; r < m.rung_count; ++r) {
            m.rungs[r] = m.rungs[r + 1];
        }
        m.rungs[m.rung_count - 1].draw_all = true;
        m.rungs[m.rung_count].draw_all = false;
        m.step = m.rungs[0].spacing;
    }
    m.mark_estimate = estimate(m.rung_count);
    m.minor_fade = m.rungs[0].alpha;
    return m;
}

// --- the draw half -------------------------------------------------------------

void draw_grid_marks(Canvas2D& canvas, const GridSpec& spec,
                     const GridMetrics& m, float brightness) {
    if (m.rung_count <= 0 || !(m.step > 0.0)) {
        return;
    }
    const int interval = spec.major_interval >= 2 ? spec.major_interval : 0;

    // A rung's face: the minor -> major color blend its cell has
    // reached, its alpha scaled by the curve and the brightness.
    const auto rung_color = [&](const GridRung& g) {
        const float t = smooth01(spec.full_cell_px, spec.major_cell_px,
                                 g.cell_px);
        return scale_alpha(blend_color(spec.minor_color, spec.major_color, t),
                           g.alpha * brightness);
    };

    if (spec.show_dots) {
        // Dots: one batched fill_rects per rung (chunked through a
        // stack buffer). Minor 1px, major 2px, every centre snapped
        // from its integer index - never by accumulating floats.
        Point batch[kDotBatch];
        for (int r = 0; r < m.rung_count; ++r) {
            const GridRung& g = m.rungs[r];
            if (g.alpha <= 0.004f || g.ix1 < g.ix0 || g.iy1 < g.iy0) {
                continue;
            }
            const float size = g.major ? 2.0f : 1.0f;
            const std::uint32_t color = rung_color(g);
            int n = 0;
            for (long long j = g.iy0; j <= g.iy1; ++j) {
                const float py = snap_mark(m.mark_y(g, j), size);
                for (long long i = g.ix0; i <= g.ix1; ++i) {
                    // The marks the rung above owns (major-major
                    // intersections) are its to draw; the top rung
                    // (draw_all) has no one above. The (0,0) mark is
                    // the origin marker's - it only ever belongs to
                    // the top rung (0 is a multiple of the interval),
                    // and the marker draws when and only when the
                    // mark would.
                    if (!g.draw_all && interval > 0 && i % interval == 0 &&
                        j % interval == 0) {
                        continue;
                    }
                    if (i == 0 && j == 0) {
                        continue;
                    }
                    batch[n] = {snap_mark(m.mark_x(g, i), size), py};
                    if (++n == kDotBatch) {
                        canvas.fill_rects(batch, n, size, size, color);
                        n = 0;
                    }
                }
            }
            if (n > 0) {
                canvas.fill_rects(batch, n, size, size, color);
            }
        }
    } else {
        // Hairlines: one snapped stroke per mark, the centre on
        // floor + 0.5 so a 1px stroke covers exactly one pixel
        // column/row. The rung above owns the i % interval == 0
        // lines; the top rung draws all.
        Paint paint;
        paint.width = 1.0f;
        paint.cap = StrokeCap::butt;
        const float left = m.frame.ox;
        const float top = m.frame.oy;
        const float right = m.frame.ox + static_cast<float>(m.frame.view_w);
        const float bottom = m.frame.oy + static_cast<float>(m.frame.view_h);
        for (int r = 0; r < m.rung_count; ++r) {
            const GridRung& g = m.rungs[r];
            if (g.alpha <= 0.004f) {
                continue;
            }
            paint.color = rung_color(g);
            for (long long i = g.ix0; i <= g.ix1; ++i) {
                if (!g.draw_all && interval > 0 && i % interval == 0) {
                    continue;
                }
                if (i == 0) {
                    continue;  // the axis line owns x=0 (it draws iff
                               // this mark would: 0 in range == visible)
                }
                const float px = std::floor(
                                     static_cast<float>(m.mark_x(g, i))) +
                                 0.5f;
                canvas.stroke_line(Point{px, top}, Point{px, bottom}, paint);
            }
            for (long long j = g.iy0; j <= g.iy1; ++j) {
                if (!g.draw_all && interval > 0 && j % interval == 0) {
                    continue;
                }
                if (j == 0) {
                    continue;  // the axis line owns y=0
                }
                const float py = std::floor(
                                     static_cast<float>(m.mark_y(g, j))) +
                                 0.5f;
                canvas.stroke_line(Point{left, py}, Point{right, py}, paint);
            }
        }
    }

    // The axes: thin snapped lines in the accent face - a subtle
    // x=0/y=0 reading, never a loud crosshair (the face's own alpha is
    // the volume knob) - plus the 3px origin marker where they cross.
    const auto axis_pos = [&](bool vertical) {
        // world 0 on the wanted axis, unsnapped.
        return vertical ? m.mark_x(m.rungs[0], 0) : m.mark_y(m.rungs[0], 0);
    };
    const std::uint32_t axis =
        scale_alpha(spec.axis_color, brightness);
    Paint axis_paint;
    axis_paint.width = 1.0f;
    axis_paint.cap = StrokeCap::butt;
    axis_paint.color = axis;
    const float left = m.frame.ox;
    const float top = m.frame.oy;
    const float right = m.frame.ox + static_cast<float>(m.frame.view_w);
    const float bottom = m.frame.oy + static_cast<float>(m.frame.view_h);
    if (m.x_axis_visible) {
        const float px =
            std::floor(static_cast<float>(axis_pos(true))) + 0.5f;
        canvas.stroke_line(Point{px, top}, Point{px, bottom}, axis_paint);
    }
    if (m.y_axis_visible) {
        const float py =
            std::floor(static_cast<float>(axis_pos(false))) + 0.5f;
        canvas.stroke_line(Point{left, py}, Point{right, py}, axis_paint);
    }
    if (m.x_axis_visible && m.y_axis_visible) {
        const Point origin{snap_mark(axis_pos(true), 3.0f),
                           snap_mark(axis_pos(false), 3.0f)};
        canvas.fill_rects(&origin, 1, 3.0f, 3.0f, axis);
    }
}

void draw_grid(Canvas2D& canvas, const GridFrame& frame,
               const GridSpec& spec, float brightness) {
    const GridMetrics m = compute_grid(frame, spec);
    draw_grid_marks(canvas, spec, m, brightness);
}

}  // namespace ws::ui::canvas
