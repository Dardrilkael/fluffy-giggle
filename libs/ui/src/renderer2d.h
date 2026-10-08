#pragma once

// Internal: shell-owned 2D renderer, CPU-facing half. This header never
// reaches apps - the public surface is <ws/ui/renderer2d.hpp>. App-facing
// methods only RECORD commands (a draw is data until the frame ends);
// frame_end() sorts the frame's commands - explicit layer first, then
// begin() block (stable, so same-layer draws never flip) - and replays
// them into the batch, splitting a fresh chunk wherever the clip changes
// in record order. The Vulkan half extends this class in
// src/renderer2d.cpp and flushes that batch inside the render pass.

#include "atlas_packer.h"
#include "batch_core.h"

#include <cfloat>
#include <cstdint>
#include <initializer_list>
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
    // Sort key components, most significant first (see frame_end()):
    std::uint32_t sort_layer = 0;    // set_layer(): the explicit z escape hatch
    std::uint32_t camera_epoch = 0;  // which begin() block recorded this
    float clip[4] = {0.0f, 0.0f, FLT_MAX, FLT_MAX};  // active clip at record
                                    // time - NOT a sort key: the replay
                                    // splits chunks wherever it changes in
                                    // record order (one chunk = one scissor)
    // NOT a sort key: the 32-slot per-chunk texture table already lets one
    // chunk hold many materials, and sorting by this would reorder
    // overlapping same-layer draws that use different textures (a button's
    // face vs. its label bar). Kept as metadata only.
    std::uint64_t material_key = 0;  // texture (+ future blend/shader bits)

    Camera2D camera;            // 2D camera state at record time
    Camera3D camera3d;          // the perspective half, recorded when kind is 3D
    bool perspective = false;   // the block kind: which camera half replays.
                                // NOT a sort key either - one begin() block
                                // is one kind, so it rides the epoch.
    Texture texture;            // {} = flat color (the white texture)
    std::uint32_t color = 0;  // packed 0xRRGGBBAA, unpacked at replay

    float pos[4][3] = {};  // TL, TR, BR, BL; z = 0 (2D) or the 3D camera's world
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
    // Dev-only (WS_UI_DEV_SHADER_RELOAD): destroys both pipelines and
    // rebakes them from the provider's current words. Call only after
    // vkDeviceWaitIdle - the GPU may still execute draws through the old
    // pipelines. Only the dev build ever calls it, but it is harmless and
    // cheap to compile everywhere.
    bool reload_pipelines();
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
    void begin(const Camera3D& camera);  // perspective block: the 3D overloads
                                         // below are its recording surface
    void end();
    void set_layer(std::uint32_t layer);
    // Clip stack. Draws snapshot the active clip at record time; frame_end
    // batches clip runs apart (one chunk = one scissor) and flush applies
    // each chunk's clip. The stack empties at frame_begin - a clip never
    // leaks across frames.
    static constexpr int kMaxClipDepth = 8;
    void push_clip(float x, float y, float width, float height);
    void pop_clip();
    void draw_quad(float x, float y, float w, float h, std::uint32_t color,
                   Texture texture = {}, float rotation = 0.0f);
    void draw_line(float x0, float y0, float x1, float y1, float thickness,
                   std::uint32_t color);
    // 3D overloads: valid only inside a begin(Camera3D) block - in a 2D
    // block they warn once per frame and record nothing. Quads and sprites
    // are camera-facing billboards, CENTER-anchored at (x, y, z), with w/h
    // in world units; the 3D line's thickness is world units too.
    void draw_quad(float x, float y, float z, float w, float h,
                   std::uint32_t color, Texture texture = {});
    void draw_line(float x0, float y0, float z0, float x1, float y1, float z1,
                   float thickness, std::uint32_t color);
    void draw_rounded_quad(float x, float y, float w, float h, float radius,
                           std::uint32_t color, float rotation = 0.0f);
    FrameSize frame_size() const;
    Texture load_texture(const char* path);
    std::vector<Sprite> load_atlas(std::initializer_list<const char*> paths);
    void draw_sprite(float x, float y, float w, float h, std::uint32_t color,
                     Sprite sprite, float rotation = 0.0f);
    void draw_sprite(float x, float y, float z, float w, float h,
                     std::uint32_t color, Sprite sprite);
    FrameSize sprite_size(Sprite sprite) const;
    // Installs a page's sub-rect table; Sprite::page is its key. Production
    // (load_atlas) calls this per uploaded page; tests inject synthetic
    // pages headlessly - the sprite contract is pure CPU state.
    void register_atlas_page(Texture page, std::vector<atlas::Rect> rects);
    // Text: one sprite-quad per visible glyph through the same recording
    // path as draw_sprite (a glyph IS a Sprite). Returns the pen advance
    // - exactly Font::measure_text's answer, by construction.
    float draw_text(float x, float y, const char* utf8_text,
                    const Font& font, std::uint32_t color = 0xFFFFFFFFu);
    // Bakes a font (src/font.*, pure CPU) and uploads its glyph pages -
    // the font's OWN pages, never shared with load_atlas. The loaders'
    // failure contract: warn once, return an empty Font.
    Font load_font(const char* path, float pixel_height);

    // Test/observability seam (internal header only): the built batch as
    // frame_end() left it.
    const batch::BatchCore& batch() const { return batch_; }

private:
    // Central guard: false when calls must drop (no active frame) - warns
    // once per frame, for any reason.
    bool accept(const char* what);
    // Misuse warn (draw outside on_update() / begin() block): once per frame.
    void warn_drop(const char* what);
    // Misuse warn (a 3D overload inside a 2D block): once per frame.
    void warn_kind(const char* what);
    // Caps hit during replay: once per frame.
    void warn_caps();
    // Stashes camera state and opens a new begin() block (bumps the epoch).
    void stash_camera(const Camera2D& camera);
    void stash_camera(const Camera3D& camera);
    // Appends one fully-expanded quad command; drops it (with the once-per-
    // frame warn) when no begin() block is open.
    void record(const char* what, const float (&pos)[4][3],
                const float (&uv)[4][2], std::uint32_t color, Texture texture);

    // --- GPU half helpers ---
    // Unknown texture id, while a registry exists: once per frame.
    void warn_unknown_texture(uint32_t id);
    // Unresolvable sprite (bad page or index): once per frame.
    void warn_sprite(const Sprite& sprite);
    // Resolves a sprite to its sub-rect; null when the page is not a
    // registered atlas page or the index is out of range. Page 0 is never
    // looked up - it is the white sentinel.
    const atlas::Rect* find_sprite_rect(const Sprite& sprite) const;
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
    Camera3D current_camera3d_{};
    // The open block's kind - which camera half record() snapshots and which
    // VP frame_end() builds. Set by every begin(), never read without an
    // open block (camera_stashed_ gates that).
    bool current_perspective_ = false;
    std::uint32_t current_layer_ = 0;  // sticky across frames until set_layer()
    std::uint32_t current_epoch_ = 0;  // bumped by every begin()
    // Clip stack state: one level = one {x, y, w, h} rect. Overflow keeps
    // push/pop balanced (depth counts on, the slot stops deepening), the
    // same policy as the gui stacks; the warn fires once per renderer.
    bool clip_stack_full(int depth);
    const float* active_clip() const;  // stack top; kFullClip when empty
    float clip_stack_[kMaxClipDepth][4] = {};
    int clip_depth_ = 0;
    bool clip_overflow_warned_ = false;
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
    // The depth-tested sibling (LEQUAL + write, for Camera3D chunks): same
    // shaders/layout/render pass, only the depth state differs. Created
    // and destroyed in lockstep with pipeline_.
    VkPipeline pipeline_3d_ = VK_NULL_HANDLE;

    // A Texture's id is its index here; [0] is the white texture. The
    // registry only grows (v1 never unloads), so stale descriptor-set
    // bindings can never dangle mid-run.
    struct TextureEntry {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
    };
    std::vector<TextureEntry> textures_;

    // The builtin rounded-rect mask draw_rounded_quad 9-slices (uploaded
    // at init, after white). White (square corners) until init runs or if
    // the upload fails - headless tests record geometry against white.
    Texture rounded_texture_{};

    // Atlas pages (load_atlas, or test injection): a page's texture id is
    // its key in Sprite::page. Only grows, so sprites stay valid for the
    // process lifetime - the same contract as textures_.
    struct AtlasPage {
        Texture texture;
        std::vector<atlas::Rect> rects;
    };
    std::vector<AtlasPage> atlas_pages_;

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
