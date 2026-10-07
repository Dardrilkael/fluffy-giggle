#pragma once

#include <cstdint>

namespace ws::physics {

// How a body participates in the simulation. 'static' is a C++ keyword, so
// its enumerator carries the conventional trailing underscore.
enum class BodyKind : std::uint8_t {
  static_ = 0,    // never moves; other bodies collide with it
  kinematic = 1,  // moves only via set_transform/set_velocity; ignores forces
  dynamic = 2,    // fully simulated: forces, impulses, collision response
};

// Published per-world caps; World Desc values above these fail create().
inline constexpr std::uint32_t kMaxBodiesPerWorld = 65535;
inline constexpr std::uint32_t kMaxJointsPerWorld = 16383;

// Hard cap on BodyDesc::parts (multi-shape bodies). The backends keep flat
// per-slot part tables sized to it and the create-time desc copy reuses a
// pre-reserved buffer, so a mid-tick compound spawn stays allocation-free;
// more parts than this fail create().
inline constexpr std::uint32_t kMaxPartsPerBody = 8;

// Default worker-thread count for world stepping:
// - the default is hardware_concurrency - 1, clamped to [1, 4] on Android
//   (big.LITTLE: oversubscribing the little cluster costs more than it
//   wins) and to [1, hardware_concurrency - 1] elsewhere;
// - 0 (explicit) selects single-threaded stepping - tests and deterministic
//   replays.
[[nodiscard]] std::uint32_t default_thread_count() noexcept;

}  // namespace ws::physics
