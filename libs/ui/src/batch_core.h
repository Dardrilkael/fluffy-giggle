#pragma once

// Internal: pure-CPU batch builder (2D + the 3D camera math) - no Vulkan,
// no third-party, fully unit-testable headless. The GPU side
// (src/renderer2d.*) drains the chunks this builds, in creation order, at
// one fixed point per frame. Never include this from include/ws/ui/.

#include <cfloat>
#include <cstddef>
#include <cstdint>
#include <vector>

// The workspace's fixed-size vecmath (Vec3, dot/cross/normalize) for the 3D
// camera path. ws::math is a ws lib, not a third-party - the no-leak rule
// targets GLFW/Vulkan/ImGui-style deps - and this include stays internal,
// like the whole file (the public header keeps plain floats).
#include <ws/math/vecmath.hpp>
#include <ws/ui/renderer2d.hpp>

namespace ws::ui::r2d::batch {

inline constexpr std::uint32_t kMaxTexturesPerChunk = 32;   // = the sampler2D[32] array in shaders/batch.frag
inline constexpr std::size_t kMaxVerticesPerFrame = 131072; // ~32k quads
// One chunk per (camera block, clip) pair, and every gui panel pushes its
// own clip - panels are the budget's main consumer. 8 saturated exactly
// (the playground: panel chrome at ambient clip + three panels + a nested
// scroll region + the 2D world + the 3D block + the screen overlay), and
// the next panel's whole segment dropped with one warn per frame. 16
// restores headroom; the cost scales with it (one descriptor set per
// swapchain image per chunk).
inline constexpr std::uint32_t kMaxChunksPerFrame = 16;

// The "no clip pushed" rect: the whole framebuffer. The flush intersects
// every chunk clip with the frame extent, so infinite extents mean
// "everything". Draws recorded with this clip render unscissored.
inline constexpr float kFullClip[4] = {0.0f, 0.0f, FLT_MAX, FLT_MAX};

struct Vertex {
    float pos[3];  // x, y, z - 2D draws land on z = 0 (the 3D camera owns z)
    float color[4];
    float uv[2];
    float tex_index;
};
static_assert(sizeof(Vertex) == 40, "vertex layout is load-bearing (shaders, pipeline)");

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
    // The chunk's depth mode: true = a perspective block's chunks (rendered
    // by the depth-tested pipeline), false = the 2D painter's path. One
    // begin() block is one kind, so the epoch order already keeps chunks of
    // different kinds apart - the flag rides the segment, like the clip.
    bool depth_test = false;
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
// (0,1,2 / 2,3,0) and the UV assignment in Renderer2D. z is written 0: every
// 2D draw sits on the world's z=0 plane.
void quad_corners(float x, float y, float w, float h, float rotation,
                  float out[4][3]);

// quad_corners with an explicit pivot: draw_rounded_quad rotates each of
// its nine slices about the WHOLE rect's center, so the rounded rect moves
// as one rigid shape. quad_corners is this with the slice's own center.
void quad_corners_pivot(float x, float y, float w, float h, float pivot_x,
                        float pivot_y, float rotation, float out[4][3]);

// Expands a segment into a thickness quad (u runs along the line, v
// across). Returns false for degenerate input - dropped silently, that is
// plausibly live app data (e.g. a zero-length drag). z is written 0.
bool line_quad(float x0, float y0, float x1, float y1, float thickness,
               float out_pos[4][3], float out_uv[4][2]);

// --- 3D camera (perspective) --------------------------------------------------

// The 3D block's screen basis in world space: orthonormal, shared by the VP
// and the record-time geometry so projection and billboards can never
// disagree. forward points into the screen; up is the perpendicularized up
// hint (world -y with the default {0,-1,0}: the y-down convention); right =
// cross(forward, up) - the OPPOSITE order from the usual left-handed
// look-at, because the y-down hint flips the handedness (the other order
// mirrors the view). Axes are ws::math::Vec3 - the workspace's vecmath -
// since the 3D helpers speak dot/cross/normalize (the push-constant Mat4
// stays batch-local). Degenerate inputs (zero/NaN up, target == eye) always
// yield a finite, orthonormal basis.
struct CamBasis {
    ws::math::Vec3 right;
    ws::math::Vec3 up;
    ws::math::Vec3 forward;
};
CamBasis camera_basis(const Camera3D& camera);

// Perspective view-projection for Vulkan NDC (y down, z in [0,1] forward):
// clip.w IS the view depth, so the homogeneous divide is the perspective
// divide. Degenerate inputs stay finite, camera_vp's contract - a NaN
// matrix would poison the whole batch.
Mat4 camera_vp3d(const Camera3D& camera, float frame_width, float frame_height);

// Billboard corners TL, TR, BR, BL - the 2D quads' order and UV assignment:
// centered on (x, y, z), spread w/h along the basis, facing the camera.
void billboard_corners(float x, float y, float z, float w, float h,
                       const CamBasis& basis, float out[4][3]);

// line_quad's 3D sibling: a thickness quad perpendicular to segment and
// view, thickness in WORLD units (the 2D form's is framebuffer pixels).
// False for degenerate input, line_quad's contract.
bool line_quad3d(float x0, float y0, float z0, float x1, float y1, float z1,
                 float thickness, const CamBasis& basis,
                 float out_pos[4][3], float out_uv[4][2]);

// --- frame builder ----------------------------------------------------------

class BatchCore {
public:
    void begin_frame();
    // Closes the current chunk (if any) and opens one with the given VP,
    // clip and depth mode. Returns false at the chunk cap - the segment
    // stays closed and its draws are dropped.
    bool open_segment(const Mat4& vp, const float (&clip)[4] = kFullClip,
                      bool depth_test = false);
    void close_segment();

    // Appends one quad (4 vertices) to the open segment. Returns false when
    // the draw must be dropped: no open segment, vertex cap, or chunk cap.
    bool push_quad(const float (&pos)[4][3], const float (&uv)[4][2],
                   const float (&color)[4], std::uint32_t texture_id);

    bool empty() const { return vertices_.empty(); }
    std::uint32_t chunk_count() const {
        return static_cast<std::uint32_t>(chunks_.size());
    }
    const Chunk& chunk(std::uint32_t i) const { return chunks_[i]; }
    const std::vector<Vertex>& vertices() const { return vertices_; }

private:
    bool start_chunk(const Mat4& vp, const float (&clip)[4], bool depth_test);
    // Slot for texture_id in the current chunk, splitting the chunk when
    // its table is full. Returns >= kMaxTexturesPerChunk when the draw must
    // be dropped (chunk cap).
    std::uint32_t texture_slot(std::uint32_t texture_id);

    std::vector<Vertex> vertices_;
    std::vector<Chunk> chunks_;
    bool segment_open_ = false;
};

}  // namespace ws::ui::r2d::batch
