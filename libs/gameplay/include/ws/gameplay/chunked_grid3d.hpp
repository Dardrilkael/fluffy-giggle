#pragma once

// Fixed-dimension chunked 3D storage with a single contiguous allocation.
// Chunk-major layout preserves locality for chunk meshing while exposing
// spans for fast inner loops. Coordinate access is bounds-checked; callers
// that already own a chunk span can index it directly with no extra work.

#include <cstddef>
#include <span>
#include <vector>

namespace ws::gameplay {

template <class T, int ChunksX, int ChunksZ,
          int ChunkSizeX, int ChunkSizeY, int ChunkSizeZ>
class ChunkedGrid3D {
  static_assert(ChunksX > 0 && ChunksZ > 0);
  static_assert(ChunkSizeX > 0 && ChunkSizeY > 0 && ChunkSizeZ > 0);

 public:
  static constexpr int kChunksX = ChunksX;
  static constexpr int kChunksZ = ChunksZ;
  static constexpr int kChunkSizeX = ChunkSizeX;
  static constexpr int kChunkSizeY = ChunkSizeY;
  static constexpr int kChunkSizeZ = ChunkSizeZ;
  static constexpr int kWorldSizeX = ChunksX * ChunkSizeX;
  static constexpr int kWorldSizeZ = ChunksZ * ChunkSizeZ;
  static constexpr std::size_t kCellsPerChunk =
      static_cast<std::size_t>(ChunkSizeX) * ChunkSizeY * ChunkSizeZ;
  static constexpr std::size_t kChunkCount =
      static_cast<std::size_t>(ChunksX) * ChunksZ;
  static constexpr std::size_t kCellCount = kCellsPerChunk * kChunkCount;

  void reset(const T& fill_value = T{}) {
    storage_.assign(kCellCount, fill_value);
  }

  [[nodiscard]] bool initialized() const noexcept {
    return storage_.size() == kCellCount;
  }

  // Chunk-local linear indexing matches the x-fastest, y-slowest layout:
  // x + z*ChunkSizeX + y*ChunkSizeX*ChunkSizeZ.
  [[nodiscard]] static constexpr std::size_t cell_index(
      int x, int y, int z) noexcept {
    return static_cast<std::size_t>(x) +
           static_cast<std::size_t>(z) * ChunkSizeX +
           static_cast<std::size_t>(y) * ChunkSizeX * ChunkSizeZ;
  }

  [[nodiscard]] std::span<T> chunk(int cx, int cz) noexcept {
    if (!initialized() || cx < 0 || cx >= ChunksX ||
        cz < 0 || cz >= ChunksZ) {
      return {};
    }
    const std::size_t index =
        static_cast<std::size_t>(cz * ChunksX + cx) * kCellsPerChunk;
    return {storage_.data() + index, kCellsPerChunk};
  }

  [[nodiscard]] std::span<const T> chunk(int cx, int cz) const noexcept {
    if (!initialized() || cx < 0 || cx >= ChunksX ||
        cz < 0 || cz >= ChunksZ) {
      return {};
    }
    const std::size_t index =
        static_cast<std::size_t>(cz * ChunksX + cx) * kCellsPerChunk;
    return {storage_.data() + index, kCellsPerChunk};
  }

  [[nodiscard]] T* try_at(int x, int y, int z) noexcept {
    if (!initialized() || x < 0 || x >= kWorldSizeX ||
        y < 0 || y >= ChunkSizeY || z < 0 || z >= kWorldSizeZ) {
      return nullptr;
    }
    const int cx = x / ChunkSizeX;
    const int cz = z / ChunkSizeZ;
    const std::size_t chunk_offset =
        static_cast<std::size_t>(cz * ChunksX + cx) * kCellsPerChunk;
    return storage_.data() + chunk_offset +
           cell_index(x % ChunkSizeX, y, z % ChunkSizeZ);
  }

  [[nodiscard]] const T* try_at(int x, int y, int z) const noexcept {
    if (!initialized() || x < 0 || x >= kWorldSizeX ||
        y < 0 || y >= ChunkSizeY || z < 0 || z >= kWorldSizeZ) {
      return nullptr;
    }
    const int cx = x / ChunkSizeX;
    const int cz = z / ChunkSizeZ;
    const std::size_t chunk_offset =
        static_cast<std::size_t>(cz * ChunksX + cx) * kCellsPerChunk;
    return storage_.data() + chunk_offset +
           cell_index(x % ChunkSizeX, y, z % ChunkSizeZ);
  }

 private:
  std::vector<T> storage_;
};

}  // namespace ws::gameplay
