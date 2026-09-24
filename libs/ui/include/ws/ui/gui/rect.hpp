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
};

// True when the point (px, py) lies inside `r`. Half-open - the right and
// bottom edges are outside - so a pixel on a shared edge belongs to exactly
// one of two adjacent rects, and empty rects contain nothing.
inline bool contains(const Rect& r, float px, float py) {
    return px >= r.x && px < r.x + r.width && py >= r.y && py < r.y + r.height;
}

}  // namespace ws::ui::gui
