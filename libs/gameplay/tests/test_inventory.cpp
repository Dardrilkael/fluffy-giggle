#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <type_traits>

#include <ws/gameplay/inventory.hpp>

namespace {
enum class Item : unsigned char { wood, planks, sticks, stone, count };
using Inventory = ws::gameplay::Inventory<Item, 3, 8>;
using Recipe = ws::gameplay::Recipe<Item, 4>;

static_assert(std::is_trivially_copyable_v<Inventory>);
static_assert(std::is_standard_layout_v<Inventory>);
}  // namespace

TEST_CASE("inventory: merge before empty slots and report accepted amount") {
  Inventory inventory;
  CHECK(inventory.add(Item::wood, 6) == 6);
  CHECK(inventory.add(Item::wood, 5) == 5);
  CHECK(inventory.slot(0).count == 8);
  CHECK(inventory.slot(1).count == 3);
  CHECK(inventory.count(Item::wood) == 11);
  CHECK(inventory.add(Item::stone, 30) == 8);
  CHECK(inventory.count(Item::stone) == 8);
}

TEST_CASE("inventory: removal is all-or-nothing and clears empty stacks") {
  Inventory inventory;
  REQUIRE(inventory.add(Item::wood, 10) == 10);
  CHECK_FALSE(inventory.remove(Item::wood, 11));
  CHECK(inventory.count(Item::wood) == 10);
  CHECK(inventory.remove(Item::wood, 8));
  CHECK(inventory.slot(0).empty());
  CHECK(inventory.slot(1).count == 2);
}

TEST_CASE("inventory: selected-slot removal is bounded and all-or-nothing") {
  Inventory inventory;
  REQUIRE(inventory.add(Item::wood, 10) == 10);
  REQUIRE(inventory.add(Item::stone, 3) == 3);
  REQUIRE(inventory.add(Item::wood, 4) == 4);
  CHECK(inventory.slot(0).item == Item::wood);
  CHECK(inventory.slot(0).count == 8);
  CHECK(inventory.slot(2).item == Item::wood);
  CHECK(inventory.slot(2).count == 2);
  CHECK_FALSE(inventory.remove_from_slot(2, 3));
  CHECK(inventory.slot(2).count == 2);
  CHECK(inventory.remove_from_slot(2, 1));
  CHECK(inventory.slot(2).count == 1);
  CHECK(inventory.slot(0).count == 8);
  CHECK_FALSE(inventory.remove_from_slot(3, 1));
  CHECK_FALSE(inventory.remove_from_slot(99, 1));
  CHECK(inventory.remove_from_slot(2, 1));
  CHECK(inventory.slot(2).empty());
}

TEST_CASE("inventory: slot swapping is bounded and value-semantic") {
  Inventory inventory;
  REQUIRE(inventory.add(Item::wood, 5) == 5);
  REQUIRE(inventory.add(Item::stone, 2) == 2);
  inventory.swap_slots(0, 1);
  CHECK(inventory.slot(0).item == Item::stone);
  CHECK(inventory.slot(0).count == 2);
  CHECK(inventory.slot(1).item == Item::wood);
  CHECK(inventory.slot(1).count == 5);
  inventory.swap_slots(0, 99);
  CHECK(inventory.slot(0).item == Item::stone);
}

TEST_CASE("crafting: repeated ingredients are accumulated before consuming") {
  Inventory inventory;
  REQUIRE(inventory.add(Item::wood, 3) == 3);
  const Recipe recipe{{
      ws::gameplay::Ingredient<Item>{Item::wood, 2},
      ws::gameplay::Ingredient<Item>{Item::wood, 2},
      {}, {}}, 2, Item::planks, 4};
  CHECK_FALSE(ws::gameplay::can_craft(inventory, recipe));
  CHECK_FALSE(ws::gameplay::craft(inventory, recipe));
  CHECK(inventory.count(Item::wood) == 3);
  REQUIRE(inventory.add(Item::wood, 1) == 1);
  CHECK(ws::gameplay::craft(inventory, recipe));
  CHECK(inventory.count(Item::wood) == 0);
  CHECK(inventory.count(Item::planks) == 4);
}

TEST_CASE("crafting: output-capacity failure leaves ingredients untouched") {
  Inventory inventory;
  REQUIRE(inventory.add(Item::wood, 8) == 8);
  REQUIRE(inventory.add(Item::stone, 8) == 8);
  REQUIRE(inventory.add(Item::sticks, 8) == 8);
  const Recipe recipe{{
      ws::gameplay::Ingredient<Item>{Item::wood, 1},
      {}, {}, {}}, 1, Item::planks, 1};
  CHECK_FALSE(ws::gameplay::craft(inventory, recipe));
  CHECK(inventory.count(Item::wood) == 8);
  CHECK(inventory.count(Item::planks) == 0);
}
