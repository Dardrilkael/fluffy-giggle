// Inventory and crafting rules for the voxel sandbox.
// Kept independent from rendering/input so recipes and stack behavior are testable.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace voxel {

enum class Item : std::uint8_t {
  grass, dirt, stone, wood, sand, cobblestone, planks, sticks,
  bricks, glass, leaves, coal, iron_ore, iron_ingot, count
};

inline constexpr std::size_t kItemCount = static_cast<std::size_t>(Item::count);
inline constexpr std::size_t kHotbarSize = 9;
inline constexpr std::size_t kInventorySize = 27;
inline constexpr std::uint16_t kMaxStack = 64;

struct ItemStack {
  Item item = Item::grass;
  std::uint16_t count = 0;
  [[nodiscard]] constexpr bool empty() const noexcept { return count == 0; }
};

class Inventory {
 public:
  [[nodiscard]] const ItemStack& slot(std::size_t i) const noexcept {
    return slots_[i < slots_.size() ? i : 0];
  }
  [[nodiscard]] std::size_t size() const noexcept { return slots_.size(); }

  // Swaps two slots without changing stack counts. Used by the inventory
  // screen to move items into the selected hotbar position.
  void swap_slots(std::size_t a, std::size_t b) noexcept {
    if (a >= slots_.size() || b >= slots_.size() || a == b) return;
    const ItemStack tmp = slots_[a];
    slots_[a] = slots_[b];
    slots_[b] = tmp;
  }

  // Adds as much as possible, merging matching stacks before using empty slots.
  [[nodiscard]] std::uint16_t add(Item item, std::uint16_t amount) noexcept {
    const std::uint16_t requested = amount;
    for (auto& s : slots_) {
      if (s.count && s.item == item && s.count < kMaxStack) {
        const auto moved = static_cast<std::uint16_t>(
            (amount < kMaxStack - s.count) ? amount : kMaxStack - s.count);
        s.count = static_cast<std::uint16_t>(s.count + moved);
        amount = static_cast<std::uint16_t>(amount - moved);
        if (!amount) return requested;
      }
    }
    for (auto& s : slots_) {
      if (!s.count) {
        const auto moved = amount < kMaxStack ? amount : kMaxStack;
        s = ItemStack{item, moved};
        amount = static_cast<std::uint16_t>(amount - moved);
        if (!amount) return requested;
      }
    }
    return static_cast<std::uint16_t>(requested - amount);
  }

  [[nodiscard]] std::uint16_t count(Item item) const noexcept {
    std::uint16_t total = 0;
    for (const auto& s : slots_)
      if (s.count && s.item == item)
        total = static_cast<std::uint16_t>(total + s.count);
    return total;
  }

  [[nodiscard]] bool has(Item item, std::uint16_t amount) const noexcept {
    return count(item) >= amount;
  }

  [[nodiscard]] bool remove(Item item, std::uint16_t amount) noexcept {
    if (!has(item, amount)) return false;
    for (auto& s : slots_) {
      if (!s.count || s.item != item) continue;
      const auto moved = s.count < amount ? s.count : amount;
      s.count = static_cast<std::uint16_t>(s.count - moved);
      amount = static_cast<std::uint16_t>(amount - moved);
      if (!s.count) s = {};
      if (!amount) return true;
    }
    return true;
  }

 private:
  std::array<ItemStack, kInventorySize> slots_{};
};

struct Ingredient { Item item; std::uint8_t count; };
struct Recipe {
  std::array<Ingredient, 4> ingredients{};
  std::uint8_t ingredient_count = 0;
  Item output = Item::planks;
  std::uint8_t output_count = 1;
};

// Small, deterministic starter recipe book. A recipe is crafted atomically:
// ingredients are only removed after all requirements have been checked.
inline constexpr std::array<Recipe, 6> kRecipes{{
    Recipe{{Ingredient{Item::wood, 1}, {}, {}, {}}, 1, Item::planks, 4},
    Recipe{{Ingredient{Item::planks, 2}, {}, {}, {}}, 1, Item::sticks, 4},
    Recipe{{Ingredient{Item::stone, 4}, {}, {}, {}}, 1, Item::cobblestone, 4},
    Recipe{{Ingredient{Item::cobblestone, 4}, {}, {}, {}}, 1, Item::bricks, 1},
    Recipe{{Ingredient{Item::sand, 4}, {}, {}, {}}, 1, Item::glass, 1},
    Recipe{{Ingredient{Item::coal, 1}, Ingredient{Item::iron_ore, 1}, {}, {}},
           2, Item::iron_ingot, 1},
}};

[[nodiscard]] inline bool can_craft(const Inventory& inventory,
                                    const Recipe& recipe) noexcept {
  for (std::size_t i = 0; i < recipe.ingredient_count; ++i)
    if (!inventory.has(recipe.ingredients[i].item,
                       recipe.ingredients[i].count))
      return false;
  return true;
}

[[nodiscard]] inline bool craft(Inventory& inventory,
                                const Recipe& recipe) noexcept {
  if (!can_craft(inventory, recipe)) return false;
  // Ensure output capacity before consuming ingredients. Since ingredients
  // are removed first, a full inventory can still craft when it frees a slot.
  Inventory after = inventory;
  for (std::size_t i = 0; i < recipe.ingredient_count; ++i)
    (void)after.remove(recipe.ingredients[i].item, recipe.ingredients[i].count);
  if (after.add(recipe.output, recipe.output_count) != recipe.output_count)
    return false;
  inventory = after;
  return true;
}

}  // namespace voxel
