// Pure, engine-free geometry for the nine-slot gameplay hotbar.
#pragma once

#include <array>
#include <cstddef>

namespace voxel::hotbar {

inline constexpr int kSlots = 9;
inline constexpr float kSlotSize = 50.0f;
inline constexpr float kSlotStep = 54.0f;
inline constexpr float kBottomMargin = 16.0f;
inline constexpr float kPanelPadding = 8.0f;
inline constexpr float kPanelHeight = 64.0f;

struct Rect {
  float x = 0.0f;
  float y = 0.0f;
  float w = 0.0f;
  float h = 0.0f;
};

struct Slot {
  Rect bounds{};
  int index = -1;
};

[[nodiscard]] constexpr bool hit(const Rect& r, float x,
                                 float y) noexcept {
  return x >= r.x && x < r.x + r.w &&
         y >= r.y && y < r.y + r.h;
}

// Slots are centered horizontally and sit above the bottom margin.
// The panel extends one padding unit around the slot row.
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

// Returns the slot under the pointer, or -1 when outside the hotbar.
[[nodiscard]] constexpr int selected(float frame_w, float frame_h,
                                     float x, float y) noexcept {
  const auto slots = layout(frame_w, frame_h);
  for (const Slot& slot : slots) {
    if (hit(slot.bounds, x, y)) return slot.index;
  }
  return -1;
}

}  // namespace voxel::hotbar
