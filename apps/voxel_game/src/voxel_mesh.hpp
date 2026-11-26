#pragma once

// CPU-side voxel mesh data: no renderer, graphics API, GPU handle or
// platform dependency. Safe to test headlessly and pass to worker jobs.

#include <cstdint>
#include <vector>

namespace voxel {

struct CpuMeshVertex {
  float pos[3]{};
  float normal[3]{};
  float uv[2]{};
};

struct CpuMeshData {
  std::vector<CpuMeshVertex> vertices;
  std::vector<std::uint32_t> indices;
};

struct ChunkMeshData {
  int tile = -1;
  CpuMeshData geometry;
};

}  // namespace voxel
