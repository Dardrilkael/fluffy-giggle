#pragma once

// Allocation-free, engine-independent layout and hit-test policies shared
// by apps. These are geometry contracts only; input ownership, drawing and
// menu state remain with the consuming application.

#include <array>
#include <cstddef>

namespace ws::ui::layout {

struct Rect {
  float x = 0.0f;
  float y = 0.0f;
  float w = 0.0f;
  float h = 0.0f;
};

// Half-open rectangle: left/top inclusive, right/bottom exclusive.
[[nodiscard]] constexpr bool hit(
    const Rect& rect, float x, float y) noexcept {
  return x >= rect.x && x < rect.x + rect.w &&
         y >= rect.y && y < rect.y + rect.h;
}

namespace menu {

struct Point { float x = 0.0f; float y = 0.0f; };
using Button = Rect;

inline constexpr int kButtonCount = 2;
inline constexpr float kButtonW = 240.0f;
inline constexpr float kButtonH = 44.0f;
inline constexpr float kButtonGap = 14.0f;

[[nodiscard]] constexpr Point layout_title(
    float frame_w, float frame_h, float text_width) noexcept {
  return {(frame_w - text_width) * 0.5f, frame_h * 0.5f - 96.0f};
}

[[nodiscard]] constexpr std::array<Button, kButtonCount> layout(
    float frame_w, float frame_h) noexcept {
  const float x = (frame_w - kButtonW) * 0.5f;
  const float pair_h = kButtonCount * kButtonH +
                       (kButtonCount - 1) * kButtonGap;
  const float y0 = frame_h * 0.5f - pair_h * 0.5f;
  return {Button{x, y0, kButtonW, kButtonH},
          Button{x, y0 + kButtonH + kButtonGap, kButtonW, kButtonH}};
}

[[nodiscard]] constexpr bool hit(
    const Button& button, float x, float y) noexcept {
  return ::ws::ui::layout::hit(button, x, y);
}

// 0 = primary action, 1 = secondary action, -1 = no action.
[[nodiscard]] constexpr int click_action(
    float frame_w, float frame_h, float x, float y) noexcept {
  const auto buttons = layout(frame_w, frame_h);
  for (int i = 0; i < kButtonCount; ++i) {
    if (hit(buttons[static_cast<std::size_t>(i)], x, y)) return i;
  }
  return -1;
}

}  // namespace menu

namespace hotbar {

inline constexpr int kSlots = 9;
inline constexpr float kSlotSize = 50.0f;
inline constexpr float kSlotStep = 54.0f;
inline constexpr float kBottomMargin = 16.0f;
inline constexpr float kPanelPadding = 8.0f;
inline constexpr float kPanelHeight = 64.0f;

using Rect = ::ws::ui::layout::Rect;

struct Slot {
  Rect bounds{};
  int index = -1;
};

[[nodiscard]] constexpr bool hit(
    const Rect& rect, float x, float y) noexcept {
  return ::ws::ui::layout::hit(rect, x, y);
}

[[nodiscard]] constexpr std::array<Slot, kSlots> layout(
    float frame_w, float frame_h) noexcept {
  std::array<Slot, kSlots> out{};
  const float bar_w = kSlotStep * static_cast<float>(kSlots);
  const float bx = (frame_w - bar_w) * 0.5f;
  const float by = frame_h - kBottomMargin - kSlotSize;
  for (int i = 0; i < kSlots; ++i) {
    out[static_cast<std::size_t>(i)] = {
        Rect{bx + static_cast<float>(i) * kSlotStep, by,
             kSlotSize, kSlotSize}, i};
  }
  return out;
}

[[nodiscard]] constexpr int selected(
    float frame_w, float frame_h, float x, float y) noexcept {
  const auto slots = layout(frame_w, frame_h);
  for (const Slot& slot : slots) {
    if (hit(slot.bounds, x, y)) return slot.index;
  }
  return -1;
}

}  // namespace hotbar

namespace settings {

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
  Rect bounds{};
  float minimum = 0.0f;
  float maximum = 1.0f;
  int index = -1;
};

[[nodiscard]] constexpr std::array<Slider, kSliderCount> layout(
    float frame_w, float frame_h) noexcept {
  std::array<Slider, kSliderCount> out{};
  const float x = (frame_w - kSliderWidth) * 0.5f;
  const float y = (frame_h - kPanelHeight) * 0.5f + kSliderTop;
  constexpr float min_values[kSliderCount] = {0.0005f, 60.0f, 32.0f};
  constexpr float max_values[kSliderCount] = {0.0060f, 110.0f, 256.0f};
  for (int i = 0; i < kSliderCount; ++i) {
    out[static_cast<std::size_t>(i)] = {
        Rect{x, y + static_cast<float>(i) * kSliderStep,
             kSliderWidth, kSliderHeight},
        min_values[i], max_values[i], i};
  }
  return out;
}

[[nodiscard]] constexpr int selected(
    float frame_w, float frame_h, float x, float y) noexcept {
  const auto sliders = layout(frame_w, frame_h);
  for (const Slider& slider : sliders) {
    const Rect expanded{slider.bounds.x, slider.bounds.y - 12.0f,
                        slider.bounds.w, slider.bounds.h + 24.0f};
    if (::ws::ui::layout::hit(expanded, x, y)) return slider.index;
  }
  return -1;
}

[[nodiscard]] constexpr float value_from_x(
    const Slider& slider, float x) noexcept {
  if (slider.bounds.w <= 0.0f) return slider.minimum;
  const float raw = (x - slider.bounds.x) / slider.bounds.w;
  const float t = raw < 0.0f ? 0.0f : (raw > 1.0f ? 1.0f : raw);
  return slider.minimum + (slider.maximum - slider.minimum) * t;
}

}  // namespace settings
}  // namespace ws::ui::layout
