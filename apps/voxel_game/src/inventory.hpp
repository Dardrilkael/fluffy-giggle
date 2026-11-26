// The voxel game's item vocabulary and starter recipes.
// Stack storage and transactional crafting live in reusable ws::gameplay.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include <ws/gameplay/inventory.hpp>

namespace voxel {

enum class Item : std::uint8_t {
  grass, dirt, stone, wood, sand, cobblestone, planks, sticks,
  bricks, glass, leaves, coal, iron_ore, iron_ingot, count
};

inline constexpr std::size_t kItemCount =
    static_cast<std::size_t>(Item::count);
inline constexpr std::size_t kHotbarSize = 9;
inline constexpr std::size_t kInventorySize = 27;
inline constexpr std::uint16_t kMaxStack = 64;

using ItemStack = ws::gameplay::ItemStack<Item>;
using Inventory =
    ws::gameplay::Inventory<Item, kInventorySize, kMaxStack>;
using Ingredient = ws::gameplay::Ingredient<Item>;
using Recipe = ws::gameplay::Recipe<Item, 4>;

inline constexpr std::array<Recipe, 6> kRecipes{{
    Recipe{{Ingredient{Item::wood, 1}, {}, {}, {}}, 1, Item::planks, 4},
    Recipe{{Ingredient{Item::planks, 2}, {}, {}, {}}, 1, Item::sticks, 4},
    Recipe{{Ingredient{Item::stone, 4}, {}, {}, {}}, 1, Item::cobblestone, 4},
    Recipe{{Ingredient{Item::cobblestone, 4}, {}, {}, {}}, 1, Item::bricks, 1},
    Recipe{{Ingredient{Item::sand, 4}, {}, {}, {}}, 1, Item::glass, 1},
    Recipe{{Ingredient{Item::coal, 1}, Ingredient{Item::iron_ore, 1}, {}, {}},
           2, Item::iron_ingot, 1},
}};

using ws::gameplay::can_craft;
using ws::gameplay::craft;

}  // namespace voxel
