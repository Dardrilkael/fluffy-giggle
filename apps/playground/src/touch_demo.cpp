#include "touch_demo.hpp"

#include <algorithm>
#include <cmath>

namespace playground {
namespace {

constexpr std::uint8_t kPalette[4][3] = {
    {255, 150, 40},   // orange
    {90, 200, 110},   // green
    {80, 160, 255},   // blue
    {200, 110, 255},  // purple
};

}  // namespace

void TouchDemo::update_trails(const ws::input::InputFrame& in) {
    for (auto& entry : trails_) entry.second.down = false;

    for (int i = 0; i < in.touch_count; ++i) {
        const ws::input::TouchPoint& t =
            in.touches[static_cast<std::size_t>(i)];
        Trail& trail = trails_[t.id.value];
        if (t.down || t.released) {
            trail.points.emplace_back(t.x, t.y);
            if (trail.points.size() > Trail::kMaxPoints) {
                trail.points.erase(trail.points.begin());
            }
        }
        trail.down = t.down;
    }

    for (auto it = trails_.begin(); it != trails_.end();) {
        if (it->second.down) { ++it; continue; }
        if (!it->second.points.empty()) {
            it->second.points.erase(it->second.points.begin());
        }
        it = it->second.points.empty() ? trails_.erase(it) : std::next(it);
    }
}

void TouchDemo::apply_pinch(const ws::input::InputFrame& in,
                            ws::ui::r2d::Camera2D& camera,
                            std::span<const ws::ui::gui::Rect> gui_rects) {
    const ws::input::TouchPoint* live[2] = {nullptr, nullptr};
    int downs = 0;
    for (int i = 0; i < in.touch_count; ++i) {
        const ws::input::TouchPoint& t =
            in.touches[static_cast<std::size_t>(i)];
        if (!t.down) continue;
        if (downs < 2) live[downs] = &t;
        ++downs;
    }

    bool pinching = downs == 2;
    if (pinching) {
        for (const ws::input::TouchPoint* t : live) {
            for (const auto& r : gui_rects) {
                if (r.contains(t->x, t->y)) { pinching = false; break; }
            }
        }
    }
    if (!pinching) {
        pinch_span_ = 0.0f;
        return;
    }

    const float span =
        std::hypot(live[1]->x - live[0]->x, live[1]->y - live[0]->y);
    if (pinch_span_ > 0.0f && span > 0.0f) {
        camera.zoom = std::clamp(camera.zoom * (span / pinch_span_),
                                 0.1f, 4.0f);
    }
    pinch_span_ = span;
}

void TouchDemo::draw_overlay() const {
    namespace r2d = ws::ui::r2d;
    for (const auto& entry : trails_) {
        const Trail& trail = entry.second;
        const auto& pal = kPalette[entry.first % 4u];
        const std::size_t n = trail.points.size();
        for (std::size_t i = 0; i < n; ++i) {
            const float toward_head =
                static_cast<float>(i + 1) / static_cast<float>(n);
            const auto alpha =
                static_cast<std::uint8_t>(48.0f + 207.0f * toward_head);
            r2d::draw_quad(trail.points[i].first - 5.0f,
                           trail.points[i].second - 5.0f, 10.0f, 10.0f,
                           r2d::rgba(pal[0], pal[1], pal[2], alpha));
        }
        if (trail.down && !trail.points.empty()) {
            r2d::draw_quad(trail.points.back().first - 9.0f,
                           trail.points.back().second - 9.0f, 18.0f, 18.0f,
                           r2d::rgba(pal[0], pal[1], pal[2], 230));
        }
    }
}

}  // namespace playground