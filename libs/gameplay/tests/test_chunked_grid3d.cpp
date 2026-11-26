#include <doctest/doctest.h>

#include <cstdint>

#include <ws/gameplay/chunked_grid3d.hpp>

namespace {
using Grid = ws::gameplay::ChunkedGrid3D<std::uint8_t, 2, 3, 4, 5, 6>;
}

TEST_CASE("chunked grid: reset creates one bounded chunk-major volume") {
  Grid grid;
  CHECK_FALSE(grid.initialized());
  CHECK(grid.try_at(0, 0, 0) == nullptr);
  grid.reset(7);
  CHECK(grid.initialized());
  CHECK(Grid::kCellCount == 2u * 3u * 4u * 5u * 6u);
  CHECK(grid.chunk(1, 2).size() == Grid::kCellsPerChunk);
  CHECK(grid.chunk(-1, 0).empty());
  CHECK(grid.chunk(0, 3).empty());
  CHECK(*grid.try_at(0, 0, 0) == 7);
}

TEST_CASE("chunked grid: coordinates map across chunk seams without overlap") {
  Grid grid;
  grid.reset(0);

  *grid.try_at(3, 4, 5) = 11;  // final cell of chunk (0,0)
  *grid.try_at(4, 4, 5) = 12;  // first x cell of chunk (1,0)
  *grid.try_at(3, 4, 6) = 13;  // first z cell of chunk (0,1)
  CHECK(*grid.try_at(3, 4, 5) == 11);
  CHECK(*grid.try_at(4, 4, 5) == 12);
  CHECK(*grid.try_at(3, 4, 6) == 13);
  CHECK(grid.try_at(-1, 0, 0) == nullptr);
  CHECK(grid.try_at(0, 5, 0) == nullptr);
  CHECK(grid.try_at(8, 0, 0) == nullptr);
  CHECK(grid.try_at(0, 0, 18) == nullptr);
}

TEST_CASE("chunked grid: chunk spans reference the same underlying cells") {
  Grid grid;
  grid.reset(0);
  auto chunk = grid.chunk(1, 1);
  chunk[Grid::cell_index(2, 3, 4)] = 99;
  CHECK(*grid.try_at(6, 3, 10) == 99);

  const Grid& read_only = grid;
  const auto const_chunk = read_only.chunk(1, 1);
  CHECK(const_chunk[Grid::cell_index(2, 3, 4)] == 99);
}
