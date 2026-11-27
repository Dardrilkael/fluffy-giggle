#pragma once

// Fixed-capacity, value-semantic inventory and crafting primitives.
// All hot operations are inline over std::array: no allocations, virtual
// dispatch, hash maps, or per-slot indirection are introduced.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace ws::gameplay {

template <class Item>
struct ItemStack {
  Item item{};
  std::uint16_t count = 0;

  [[nodiscard]] constexpr bool empty() const noexcept { return count == 0; }
};

template <class Item, std::size_t SlotCount, std::uint16_t MaxStack>
class Inventory {
  static_assert(SlotCount > 0, "an inventory needs at least one slot");
  static_assert(MaxStack > 0, "a stack needs positive capacity");
  static_assert(SlotCount <=
                    std::numeric_limits<std::uint16_t>::max() / MaxStack,
                "the total inventory count must fit in uint16_t");

 public:
  using item_type = Item;
  using stack_type = ItemStack<Item>;
  static constexpr std::size_t kSlotCount = SlotCount;
  static constexpr std::uint16_t kMaxStackSize = MaxStack;

  [[nodiscard]] constexpr const stack_type& slot(
      std::size_t index) const noexcept {
    return slots_[index < SlotCount ? index : 0];
  }

  [[nodiscard]] constexpr std::size_t size() const noexcept {
    return SlotCount;
  }

  constexpr void swap_slots(std::size_t a, std::size_t b) noexcept {
    if (a >= SlotCount || b >= SlotCount || a == b) return;
    const stack_type tmp = slots_[a];
    slots_[a] = slots_[b];
    slots_[b] = tmp;
  }

  // Adds as much as possible, merging matching stacks before using empty
  // slots. The returned amount is the number accepted.
  [[nodiscard]] constexpr std::uint16_t add(
      Item item, std::uint16_t amount) noexcept {
    const std::uint16_t requested = amount;
    for (auto& stack : slots_) {
      if (stack.count != 0 && stack.item == item &&
          stack.count < MaxStack) {
        const auto moved = static_cast<std::uint16_t>(
            amount < MaxStack - stack.count ? amount
                                            : MaxStack - stack.count);
        stack.count = static_cast<std::uint16_t>(stack.count + moved);
        amount = static_cast<std::uint16_t>(amount - moved);
        if (amount == 0) return requested;
      }
    }
    for (auto& stack : slots_) {
      if (stack.count == 0) {
        const auto moved = static_cast<std::uint16_t>(
            amount < MaxStack ? amount : MaxStack);
        stack = stack_type{item, moved};
        amount = static_cast<std::uint16_t>(amount - moved);
        if (amount == 0) return requested;
      }
    }
    return static_cast<std::uint16_t>(requested - amount);
  }

  [[nodiscard]] constexpr std::uint16_t count(Item item) const noexcept {
    std::uint16_t total = 0;
    for (const auto& stack : slots_) {
      if (stack.count != 0 && stack.item == item) {
        total = static_cast<std::uint16_t>(total + stack.count);
      }
    }
    return total;
  }

  [[nodiscard]] constexpr bool has(
      Item item, std::uint16_t amount) const noexcept {
    return count(item) >= amount;
  }

  [[nodiscard]] constexpr bool remove(
      Item item, std::uint16_t amount) noexcept {
    if (!has(item, amount)) return false;
    for (auto& stack : slots_) {
      if (stack.count == 0 || stack.item != item) continue;
      const auto moved = static_cast<std::uint16_t>(
          stack.count < amount ? stack.count : amount);
      stack.count = static_cast<std::uint16_t>(stack.count - moved);
      amount = static_cast<std::uint16_t>(amount - moved);
      if (stack.count == 0) stack = {};
      if (amount == 0) return true;
    }
    return true;
  }

  // Removes only from the requested slot. Failure leaves every slot intact.
  [[nodiscard]] constexpr bool remove_from_slot(
      std::size_t index, std::uint16_t amount) noexcept {
    if (index >= SlotCount) return false;
    auto& stack = slots_[index];
    if (stack.count < amount) return false;
    if (amount == 0) return true;
    stack.count = static_cast<std::uint16_t>(stack.count - amount);
    if (stack.count == 0) stack = {};
    return true;
  }

 private:
  std::array<stack_type, SlotCount> slots_{};
};

template <class Item>
struct Ingredient {
  Item item{};
  std::uint16_t count = 0;
};

template <class Item, std::size_t MaxIngredients>
struct Recipe {
  std::array<Ingredient<Item>, MaxIngredients> ingredients{};
  std::size_t ingredient_count = 0;
  Item output{};
  std::uint16_t output_count = 1;
};

// Checks repeated ingredient types cumulatively. This matters for generic
// recipes even though a small starter recipe book may not repeat an item.
template <class Item, std::size_t SlotCount, std::uint16_t MaxStack,
          std::size_t MaxIngredients>
[[nodiscard]] constexpr bool can_craft(
    const Inventory<Item, SlotCount, MaxStack>& inventory,
    const Recipe<Item, MaxIngredients>& recipe) noexcept {
  if (recipe.ingredient_count > MaxIngredients || recipe.output_count == 0) {
    return false;
  }

  for (std::size_t i = 0; i < recipe.ingredient_count; ++i) {
    const Item item = recipe.ingredients[i].item;
    std::uint32_t required = 0;
    bool seen_before = false;
    for (std::size_t previous = 0; previous < i; ++previous) {
      if (recipe.ingredients[previous].item == item) {
        seen_before = true;
        break;
      }
    }
    if (seen_before) continue;

    for (std::size_t j = i; j < recipe.ingredient_count; ++j) {
      if (recipe.ingredients[j].item == item) {
        if (recipe.ingredients[j].count == 0) return false;
        required += recipe.ingredients[j].count;
      }
    }
    if (required > inventory.count(item)) return false;
  }
  return true;
}

template <class Item, std::size_t SlotCount, std::uint16_t MaxStack,
          std::size_t MaxIngredients>
[[nodiscard]] constexpr bool craft(
    Inventory<Item, SlotCount, MaxStack>& inventory,
    const Recipe<Item, MaxIngredients>& recipe) noexcept {
  if (!can_craft(inventory, recipe)) return false;

  // Work on a stack-local copy: failure to fit the output never consumes
  // ingredients. This also means a recipe can free slots for its output.
  Inventory<Item, SlotCount, MaxStack> after = inventory;
  for (std::size_t i = 0; i < recipe.ingredient_count; ++i) {
    if (!after.remove(recipe.ingredients[i].item,
                      recipe.ingredients[i].count)) {
      return false;
    }
  }
  if (after.add(recipe.output, recipe.output_count) != recipe.output_count) {
    return false;
  }
  inventory = after;
  return true;
}

}  // namespace ws::gameplay
