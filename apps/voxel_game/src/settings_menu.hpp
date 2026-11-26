#pragma once

// Voxel-game settings semantics stay local; ws::ui supplies only the
// reusable slider geometry and mapping math.
#include <array>
#include <cstddef>

#include <ws/ui/layout.hpp>

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

struct Slider {
  ws::ui::layout::Rect bounds{};
  float minimum = 0.0f;
  float maximum = 1.0f;
  int index = -1;
};

[[nodiscard]] constexpr std::array<Slider, kSliderCount> layout(
    float frame_w, float frame_h) noexcept {
  const auto geometry =
      ws::ui::layout::settings::layout<kSliderCount>(
          frame_w, frame_h, kPanelWidth, kPanelHeight,
          kSliderWidth, kSliderHeight, kSliderTop, kSliderStep);
  constexpr float min_values[kSliderCount] = {0.0005f, 60.0f, 32.0f};
  constexpr float max_values[kSliderCount] = {0.0060f, 110.0f, 256.0f};
  std::array<Slider, kSliderCount> out{};
  for (int i = 0; i < kSliderCount; ++i) {
    const auto index = static_cast<std::size_t>(i);
    out[index] = {geometry[index].bounds, min_values[i], max_values[i], i};
  }
  return out;
}

[[nodiscard]] constexpr int selected(
    float frame_w, float frame_h, float x, float y) noexcept {
  const auto sliders = layout(frame_w, frame_h);
  for (const Slider& slider : sliders) {
    const ws::ui::layout::Rect expanded{
        slider.bounds.x, slider.bounds.y - 12.0f,
        slider.bounds.w, slider.bounds.h + 24.0f};
    if (ws::ui::layout::hit(expanded, x, y)) return slider.index;
  }
  return -1;
}

[[nodiscard]] constexpr float value_from_x(
    const Slider& slider, float x) noexcept {
  return ws::ui::layout::settings::value_from_x(
      slider.bounds, slider.minimum, slider.maximum, x);
}

}  // namespace voxel::settings_menu
