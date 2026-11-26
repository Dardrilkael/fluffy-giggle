// apps/voxel_game - menu.hpp: the pause menu's pure geometry and hit
// test, engine-free (no ws:: includes) so the logic suite can pin it
// headless exactly the way world.hpp's rules are pinned. The app side
// (main.cpp) owns the state: paused_ gates the simulation and the
// cursor mode; this header only answers where the title/buttons sit
// and which button, if any, did the click land in.

#pragma once

#include <array>

namespace voxel::menu {

struct Button {
  float x, y, w, h;
};

struct Point {
  float x, y;
};

inline constexpr int kButtonCount = 2;
inline constexpr float kButtonW = 240.0f;
inline constexpr float kButtonH = 44.0f;
inline constexpr float kButtonGap = 14.0f;

// The title width is measured by the renderer; this returns its centered
// origin and keeps the title's vertical position part of the pure layout.
[[nodiscard]] constexpr Point layout_title(float frame_w, float frame_h,
                                           float text_width) noexcept {
  return {(frame_w - text_width) * 0.5f, frame_h * 0.5f - 96.0f};
}

// Two buttons, centered horizontally and vertically as a pair:
// resume on top, quit below.
[[nodiscard]] constexpr std::array<Button, kButtonCount> layout(
    float frame_w, float frame_h) noexcept {
  const float x = (frame_w - kButtonW) * 0.5f;
  const float pair_h = kButtonCount * kButtonH +
                       (kButtonCount - 1) * kButtonGap;
  const float y0 = frame_h * 0.5f - pair_h * 0.5f;
  return {Button{x, y0, kButtonW, kButtonH},
          Button{x, y0 + kButtonH + kButtonGap, kButtonW, kButtonH}};
}

// The [x, x+w) hit: left/top inclusive, right/bottom exclusive.
[[nodiscard]] constexpr bool hit(const Button& b, float x,
                                 float y) noexcept {
  return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

// 0 = resume, 1 = quit, -1 = no button.
[[nodiscard]] constexpr int click_action(float frame_w, float frame_h,
                                         float x, float y) noexcept {
  const auto buttons = layout(frame_w, frame_h);
  for (int i = 0; i < kButtonCount; ++i) {
    if (hit(buttons[static_cast<std::size_t>(i)], x, y)) {
      return i;
    }
  }
  return -1;
}

}  // namespace voxel::menu
