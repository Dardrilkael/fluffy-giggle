#pragma once

// Generational slot table: the machinery behind BodyId/JointId. O(1)
// allocate/free with dense slot indices; freeing bumps the slot's
// generation so stale handles are detectable instead of aliasing. No
// allocation after construction (both vectors are reserved up front) -
// this is a per-step hot structure in worlds that pre-size everything.
//
// All slots start at generation 1, so the all-zero Handle{} (BodyId{}/
// JointId{} defaults) can never test alive - the public contract "the
// zero handle is never issued" holds without a special case.

#include <cstdint>
#include <optional>
#include <vector>

namespace ws::physics::detail {

class HandleTable {
 public:
  explicit HandleTable(std::uint32_t capacity)
      : slots_(capacity), free_(capacity) {
    // Reserve + fill without reallocation churn: slots grow lazily as the
    // free stack hands out indices, but the storage is final here.
    free_.clear();
    for (std::uint32_t i = 0; i < capacity; ++i) {
      slots_[i] = Slot{1u, false};
      free_.push_back(capacity - 1u - i);  // stack: hand out 0, 1, 2, ...
    }
  }

  HandleTable(const HandleTable&) = delete;
  HandleTable& operator=(const HandleTable&) = delete;

  // Slot index to use as the table key, or nullopt when full.
  [[nodiscard]] std::optional<std::uint32_t> allocate() {
    if (free_.empty()) {
      return std::nullopt;
    }
    const std::uint32_t slot = free_.back();
    free_.pop_back();
    slots_[slot].alive = true;
    ++live_;
    return slot;
  }

  // Returns false for unknown/out-of-range slots (defensive; the world
  // layer never frees twice - a second free is a stale-handle error there).
  bool free(std::uint32_t slot) {
    if (slot >= slots_.size() || !slots_[slot].alive) {
      return false;
    }
    slots_[slot].alive = false;
    std::uint32_t& gen = slots_[slot].generation;
    ++gen;
    if (gen == 0) {
      gen = 1;  // wrap must skip 0: generation 0 would resurrect Handle{}
    }
    free_.push_back(slot);
    --live_;
    return true;
  }

  [[nodiscard]] bool alive(std::uint32_t slot, std::uint32_t generation) const {
    return slot < slots_.size() && slots_[slot].alive &&
           slots_[slot].generation == generation;
  }

  // Slot live at all (any generation)? The event collector resolves slots
  // to handles with live() + generation() after a tick.
  [[nodiscard]] bool live(std::uint32_t slot) const {
    return slot < slots_.size() && slots_[slot].alive;
  }

  [[nodiscard]] std::uint32_t generation(std::uint32_t slot) const {
    return slot < slots_.size() ? slots_[slot].generation : 0u;
  }

  [[nodiscard]] std::uint32_t capacity() const {
    return static_cast<std::uint32_t>(slots_.size());
  }
  [[nodiscard]] std::uint32_t size() const { return live_; }

 private:
  struct Slot {
    std::uint32_t generation;
    bool alive;
  };

  std::vector<Slot> slots_;
  std::vector<std::uint32_t> free_;  // stack of dead slot indices
  std::uint32_t live_ = 0;
};

}  // namespace ws::physics::detail
