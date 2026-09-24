#pragma once

// Internal: shell-owned 2D renderer, CPU-facing half. This header never
// reaches apps - the public surface is <ws/ui/renderer2d.hpp>. App-facing
// methods only RECORD commands (a draw is data until the frame ends);
// frame_end() sorts the frame's commands - explicit layer first, then
// begin() block, then material (stable, so same-state draws never flip) -
// and replays them into the batch. The Vulkan half extends this class in
// src/renderer2d.cpp and flushes that batch inside the render pass.

#include "batch_core.h"

#include <cstdint>
#include <vector>

#include <volk.h>  // internal header: Vk*/Vma types live in signatures and
#include <vk_mem_alloc.h>  // storage. They never reach apps - that surface is
                          // <ws/ui/renderer2d.hpp>.

#include <ws/ui/renderer2d.hpp>

namespace ws::ui::r2d {

// One recorded quad - lines are expanded into quads at record time. Kept
// as data so the whole frame can be sorted before batching: state changes,
// not submission order alone, decide the segment structure.
struct DrawCommand {
    // Sort key components, most significant first:
    std::uint32_t sort_layer = 0;    // set_layer(): the explicit z escape hatch
    std::uint32_t camera_epoch = 0;  // which begin() block recorded this
    std::uint64_t material_key = 0;  // texture (+ future blend/shader bits)

    Camera2D camera;           // camera state at record time
    Texture texture;            // {} = flat color (the white texture)
    std::uint32_t color = 0;  // packed 0xRRGGBBAA, unpacked at replay

    float pos[4][2] = {};  // TL, TR, BR, BL
    float uv[4][2] = {};
};

// v1 packs just the texture id; blend mode and shader id are the reserved
// upper bits (shader << 40 | blend << 32) for the day they exist - the
// sort comparator's shape never changes.
inline std::uint64_t make_material_key(Texture texture) {
    return static_cast<std::uint64_t>(texture.id);
}

class Renderer2D {
public:
    // Vulkan/VMA handles are single-owned: a copied renderer would destroy
    // them twice at shutdown.
    Renderer2D() = default;
    Renderer2D(const Renderer2D&) = delete;
    Renderer2D& operator=(const Renderer2D&) = delete;

    // --- shell plumbing (app.cpp) ---
    // Starts a frame: stores the extent, resets the batch and the command
    // list (capacity kept). Draws are accepted until frame_end/frame_drop.
    void frame_begin(float fb_width, float fb_height);
    // The batching point: sorts the recorded commands and replays them
    // into the batch. Call once per frame, after on_update() returned.
    void frame_end();
    // Abandons the frame: commands and batch are dropped, never deferred -
    // same contract as RenderCommandQueue::clear().
    void frame_drop();

    // --- GPU lifecycle (app.cpp only; needs a live Vulkan context) ---
    // One-time setup after the swapchain exists. Order inside: push-constant
    // limit check, upload pool/fence, sampler, white texture (registry index
    // 0 - every descriptor slot starts bound to it), set layout (32 immutable
    // samplers), pipeline layout, per-image objects (descriptor pool, sets,
    // vertex regions), shared index pattern, pipeline. On false the startup
    // fails; shutdown() is the failure-path cleanup and cleans partial state.
    bool init(VkDevice device, VkPhysicalDevice physical_device,
              VmaAllocator allocator, VkQueue queue, uint32_t queue_family,
              VkRenderPass render_pass, VkFormat format, uint32_t image_count);
    // Swapchain drift. Format drift rebuilds the pipeline (a same-format
    // resize needs nothing - that is the render-pass compatibility the ImGui
    // backend relies on); image-count drift rebuilds the per-image sets and
    // vertex regions. Extents are per-flush (dynamic viewport/scissor).
    bool on_swapchain_recreated(VkRenderPass render_pass, VkFormat format,
                                uint32_t image_count);
    // Destroys everything init()/load_texture() created. Idempotent.
    void shutdown();
    // Draws the batch inside the render pass, before ImGui's draw data (2D
    // world under widgets). The image's fence is waited before recording
    // starts, so its vertex region and chunk sets are safely writable here.
    void flush(VkCommandBuffer cmd, uint32_t image_index, float fb_width,
               float fb_height);

    // --- app-facing entry points (forwarded from the free functions) ---
    void begin_screen();
    void begin(const Camera2D& camera);
    void end();
    void set_layer(std::uint32_t layer);
    void draw_quad(float x, float y, float w, float h, std::uint32_t color,
                   Texture texture = {}, float rotation = 0.0f);
    void draw_line(float x0, float y0, float x1, float y1, float thickness,
                   std::uint32_t color);
    FrameSize frame_size() const;
    Texture load_texture(const char* path);

    // Test/observability seam (internal header only): the built batch as
    // frame_end() left it.
    const batch::BatchCore& batch() const { return batch_; }

private:
    // Central guard: false when calls must drop (no active frame) - warns
    // once per frame, for any reason.
    bool accept(const char* what);
    // Misuse warn (draw outside on_update() / begin() block): once per frame.
    void warn_drop(const char* what);
    // Caps hit during replay: once per frame.
    void warn_caps();
    // Stashes camera state and opens a new begin() block (bumps the epoch).
    void stash_camera(const Camera2D& camera);
    // Appends one fully-expanded quad command; drops it (with the once-per-
    // frame warn) when no begin() block is open.
    void record(const char* what, const float (&pos)[4][2],
                const float (&uv)[4][2], std::uint32_t color, Texture texture);

    // --- GPU half helpers ---
    // Unknown texture id, while a registry exists: once per frame.
    void warn_unknown_texture(uint32_t id);
    // Graphics pipeline against the given render pass; the shader modules
    // are created and destroyed inside (they are baked into the pipeline).
    bool create_pipeline(VkRenderPass render_pass);
    // Descriptor pool + image_count*kMaxChunksPerFrame sets (initial
    // full-white write) + one persistently-mapped vertex region per image.
    bool create_per_image_objects(uint32_t image_count);
    void destroy_per_image_objects();
    // Shared absolute index pattern (quad q -> 4q + {0,1,2,2,3,0}), filled
    // once and never rewritten.
    bool create_index_buffer();
    // Staging upload of a 4-byte-per-pixel R8G8B8A8 image (one-off submit +
    // fence wait). Cleans its partial state and returns false on failure.
    bool upload_texture(const void* pixels, uint32_t width, uint32_t height,
                        Texture* out);
    // Binds a chunk's used slots [0, texture_count); the rest keep the
    // initial full-white write.
    void write_chunk_set(VkDescriptorSet set, const batch::Chunk& chunk);

    std::vector<DrawCommand> commands_;  // capacity reused across frames
    batch::BatchCore batch_;
    Camera2D current_camera_{};
    std::uint32_t current_layer_ = 0;  // sticky across frames until set_layer()
    std::uint32_t current_epoch_ = 0;  // bumped by every begin()
    float fb_width_ = 0.0f;
    float fb_height_ = 0.0f;
    bool camera_stashed_ = false;  // set by begin(), cleared by end()
    bool frame_active_ = false;
    bool warned_ = false;

    // --- Vulkan state (null/empty until init()) ---
    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;

    VkRenderPass render_pass_ = VK_NULL_HANDLE;  // last seen (rebuild input)
    VkFormat format_ = VK_FORMAT_UNDEFINED;     // drift sentinel
    uint32_t image_count_ = 0;

    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;  // owns sets_
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;

    // A Texture's id is its index here; [0] is the white texture. The
    // registry only grows (v1 never unloads), so stale descriptor-set
    // bindings can never dangle mid-run.
    struct TextureEntry {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
    };
    std::vector<TextureEntry> textures_;

    // One vertex region per swapchain image, sized by the static frame cap
    // (a resize never reallocates them), persistently mapped.
    struct ImageBuffers {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        batch::Vertex* mapped = nullptr;
    };
    std::vector<ImageBuffers> buffers_;

    VkBuffer index_buffer_ = VK_NULL_HANDLE;
    VmaAllocation index_allocation_ = VK_NULL_HANDLE;

    VkCommandPool upload_pool_ = VK_NULL_HANDLE;  // texture uploads
    VkFence upload_fence_ = VK_NULL_HANDLE;
    // image_count_ * kMaxChunksPerFrame; slot = image_index * kMaxChunks + c.
    std::vector<VkDescriptorSet> sets_;
};

// Points the public free functions at the live renderer. app.cpp installs it
// for the duration of App::run() and clears it at shutdown.
void set_active(Renderer2D* renderer);

}  // namespace ws::ui::r2d
