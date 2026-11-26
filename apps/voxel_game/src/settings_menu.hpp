// Pure, engine-free settings values and slider geometry for the voxel game.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>

namespace voxel::settings_menu {

inline constexpr int kSliderCount = 3;
inline constexpr float kPanelWidth = 520.0f;
inline constexpr float kPanelHeight = 300.0f;
inline constexpr float kSliderWidth = 340.0f;
inline constexpr float kSliderHeight = 12.0f;
inline constexpr float kSliderTop = 112.0f;
inline constexpr float kSliderStep = 62.0f;

struct Settings {
  float mouse_sensitivity = 0.0025f;
  float fov_degrees = 70.0f;
  float render_distance = 260.0f;
};

struct Rect {
  float x = 0.0f;
  float y = 0.0f;
  float w = 0.0f;
  float h = 0.0f;
};

struct Slider {
  Rect bounds{};
  int index = -1;
  float minimum = 0.0f;
  float maximum = 1.0f;
};

[[nodiscard]] constexpr std::array<Slider, kSliderCount> layout(
    float frame_w, float frame_h) noexcept {
  constexpr float mins[kSliderCount] = {0.0005f, 60.0f, 32.0f};
  constexpr float maxs[kSliderCount] = {0.0060f, 110.0f, 256.0f};
  std::array<Slider, kSliderCount> out{};
  const float x = (frame_w - kPanelWidth) * 0.5f + 32.0f;
  const float y = (frame_h - kPanelHeight) * 0.5f + kSliderTop;
  for (int i = 0; i < kSliderCount; ++i) {
    out[static_cast<std::size_t>(i)] = {
        Rect{x, y + static_cast<float>(i) * kSliderStep,
             kSliderWidth, kSliderHeight},
        i, mins[i], maxs[i]};
  }
  return out;
}

[[nodiscard]] constexpr bool hit(const Rect& r, float x,
                                 float y) noexcept {
  return x >= r.x && x < r.x + r.w &&
         y >= r.y && y < r.y + r.h;
}

// Returns slider index, or -1 when the pointer is outside every track.
[[nodiscard]] constexpr int selected(float frame_w, float frame_h,
                                     float x, float y) noexcept {
  const auto sliders = layout(frame_w, frame_h);
  for (const Slider& slider : sliders) {
    // A generous vertical hit area makes the narrow visual track easy to use.
    const Rect target{slider.bounds.x, slider.bounds.y - 12.0f,
                      slider.bounds.w, slider.bounds.h + 24.0f};
    if (hit(target, x, y)) return slider.index;
  }
  return -1;
}

[[nodiscard]] constexpr float value_from_x(const Slider& slider,
                                           float x) noexcept {
  const float t = std::clamp((x - slider.bounds.x) / slider.bounds.w,
                             0.0f, 1.0f);
  return slider.minimum + t * (slider.maximum - slider.minimum);
}

}  // namespace voxel::settings_menu
