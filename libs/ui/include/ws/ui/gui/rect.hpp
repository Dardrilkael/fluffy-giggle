#pragma once

namespace ws::ui::gui {

// Axis-aligned rect in gui space: framebuffer pixels, origin top-left -
// the same space as r2d's screen mode (begin() without a camera), so a
// rect laid out by LayoutCursor renders exactly where hit-testing says it
// is. Layout, hit-testing and (later) clip rects all speak Rect, so
// geometry computed by one works with the others unchanged.
struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    // True when the point lies inside. Half-open - the right and bottom
    // edges are outside - so a pixel on a shared edge belongs to exactly
    // one of two adjacent rects, and empty rects contain nothing.
    bool contains(float px, float py) const {
        return px >= x && px < x + width && py >= y && py < y + height;
    }

    // The far edges and the center: layout and centering read against
    // these instead of x + width arithmetic repeated at every call site.
    float right() const { return x + width; }
    float bottom() const { return y + height; }
    float center_x() const { return x + width * 0.5f; }
    float center_y() const { return y + height * 0.5f; }

    // Grown / shrunk by `m` on every side, symmetric so the rect never
    // drifts toward an edge (hit-slop inflates, padding insets). Shrunk
    // past empty keeps the raw arithmetic: a negative-extent rect
    // contains nothing under the half-open test, the safe answer for an
    // over-inset rect.
    Rect expanded(float m) const {
        return {x - m, y - m, width + 2.0f * m, height + 2.0f * m};
    }
    Rect shrunk(float m) const {
        return {x + m, y + m, width - 2.0f * m, height - 2.0f * m};
    }
};

}  // namespace ws::ui::gui
