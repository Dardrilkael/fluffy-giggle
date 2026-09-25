#pragma once

// Internal: pure-CPU 2D batch builder - no Vulkan, no third-party, fully
// unit-testable headless. The GPU side (src/renderer2d.*) drains the chunks
// this builds, in creation order, at one fixed point per frame. Never
// include this from include/ws/ui/.

#include <cfloat>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <ws/ui/renderer2d.hpp>

namespace ws::ui::r2d::batch {

inline constexpr std::uint32_t kMaxTexturesPerChunk = 32;   // = the sampler2D[32] array in shaders/batch.frag
inline constexpr std::size_t kMaxVerticesPerFrame = 131072; // ~32k quads
inline constexpr std::uint32_t kMaxChunksPerFrame = 8;     // per frame: segments + texture-overflow splits

// The "no clip pushed" rect: the whole framebuffer. The flush intersects
// every chunk clip with the frame extent, so infinite extents mean
// "everything". Draws recorded with this clip render unscissored.
inline constexpr float kFullClip[4] = {0.0f, 0.0f, FLT_MAX, FLT_MAX};

struct Vertex {
    float pos[2];
    float color[4];
    float uv[2];
    float tex_index;
};
static_assert(sizeof(Vertex) == 36, "vertex layout is load-bearing (shaders, pipeline)");

// Column-major 4x4, GLSL upload order (m[0..3] = column 0).
struct Mat4 {
    float m[16];
};

// One draw-call-sized slice of the frame: shared VP, own texture table. A
// full table splits into a fresh chunk with the same VP - painter order
// survives because vertex ranges stay contiguous and chunks flush in
// creation order.
struct Chunk {
    Mat4 vp{};
    // Framebuffer-space clip rect {x, y, w, h} this chunk renders within
    // (kFullClip until a clip is pushed). One chunk = one vkCmdSetScissor,
    // so a clip change always breaks the chunk.
    float clip[4] = {0.0f, 0.0f, FLT_MAX, FLT_MAX};
    std::uint32_t vertex_offset = 0;  // first vertex, into the frame's buffer
    std::uint32_t quad_count = 0;
    std::uint32_t texture_ids[kMaxTexturesPerChunk] = {};
    std::uint32_t texture_count = 0;  // used bindings; the rest bind white
};

// --- free helpers -----------------------------------------------------------

// y-down orthographic view-projection. Vulkan NDC +y points down, so the
// world maps with no sign flip. Degenerate inputs (zoom <= 0, empty frame)
// stay finite - the GPU flush guards zero extents anyway, but a NaN matrix
// would poison the whole batch.
Mat4 camera_vp(const Camera2D& camera, float frame_width, float frame_height);

// 0xRRGGBBAA -> [0,1] floats, in that order.
void unpack_rgba(std::uint32_t color, float out[4]);

// Quad corners around the center pivot, rotation positive = clockwise on
// screen (y-down). Order TL, TR, BR, BL - matches the shared index pattern
// (0,1,2 / 2,3,0) and the UV assignment in Renderer2D.
void quad_corners(float x, float y, float w, float h, float rotation,
                  float out[4][2]);

// Expands a segment into a thickness quad (u runs along the line, v
// across). Returns false for degenerate input - dropped silently, that is
// plausibly live app data (e.g. a zero-length drag).
bool line_quad(float x0, float y0, float x1, float y1, float thickness,
               float out_pos[4][2], float out_uv[4][2]);

// --- frame builder ----------------------------------------------------------

class BatchCore {
public:
    void new_frame();
    // Closes the current chunk (if any) and opens one with the given VP and
    // clip. Returns false at the chunk cap - the segment stays closed and
    // its draws are dropped.
    bool open_segment(const Mat4& vp, const float (&clip)[4] = kFullClip);
    void close_segment();

    // Appends one quad (4 vertices) to the open segment. Returns false when
    // the draw must be dropped: no open segment, vertex cap, or chunk cap.
    bool push_quad(const float (&pos)[4][2], const float (&uv)[4][2],
                   const float (&color)[4], std::uint32_t texture_id);

    bool empty() const { return vertices_.empty(); }
    std::uint32_t chunk_count() const {
        return static_cast<std::uint32_t>(chunks_.size());
    }
    const Chunk& chunk(std::uint32_t i) const { return chunks_[i]; }
    const std::vector<Vertex>& vertices() const { return vertices_; }

private:
    bool start_chunk(const Mat4& vp, const float (&clip)[4]);
    // Slot for texture_id in the current chunk, splitting the chunk when
    // its table is full. Returns >= kMaxTexturesPerChunk when the draw must
    // be dropped (chunk cap).
    std::uint32_t texture_slot(std::uint32_t texture_id);

    std::vector<Vertex> vertices_;
    std::vector<Chunk> chunks_;
    bool segment_open_ = false;
};

}  // namespace ws::ui::r2d::batch
