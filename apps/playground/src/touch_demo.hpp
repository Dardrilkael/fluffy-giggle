// Multi-touch demo: per-contact breadcrumb trails, plus a two-finger
// pinch-zoom that scales the 2D camera — unless a finger is over a gui
// panel, in which case the pinch is the widgets' and the camera stays put.
// The panel rects are passed in, not looked up: the caller owns the layout.
#pragma once

#include <ws/input/input.hpp>    // event.hpp comes with it; InputFrame too
#include <ws/ui/gui/rect.hpp>    // for gui::Rect
#include <ws/ui/renderer2d.hpp>

#include <cstdint>
#include <map>
#include <span>
#include <utility>
#include <vector>

namespace playground {

class TouchDemo {
public:
    // Append each live contact's position to its trail (capped); lift the
    // head and retract retired trails one point per frame.
    void update_trails(const ws::input::InputFrame& in);

    // Two fingers off the gui panels = one pinch: the frame-to-frame span
    // scales camera.zoom. A finger over any gui_rect cancels the pinch. A
    // third finger aborts until it is two again.
    void apply_pinch(const ws::input::InputFrame& in,
                     ws::ui::r2d::Camera2D& camera,
                     std::span<const ws::ui::gui::Rect> gui_rects);

    // Trail overlay (call inside a screen-space r2d block, layer 5).
    void draw_overlay() const;

private:
    struct Trail {
        static constexpr std::size_t kMaxPoints = 32;  // ~half a second @60fps
        std::vector<std::pair<float, float>> points;   // oldest first
        bool down = false;                             // seen this frame
    };
    std::map<std::uint32_t, Trail> trails_;
    float pinch_span_ = 0.0f;  // last pinch frame's span; 0 = none
};

}  // namespace playground