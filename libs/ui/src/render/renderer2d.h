#pragma once

// Internal: shell-owned 2D renderer, CPU-facing half. This header never
// reaches apps - the public surface is <ws/ui/renderer2d.hpp>. App-facing
// methods only RECORD commands (a draw is data until the frame ends);
// frame_end() sorts the frame's commands - explicit layer first, then
// begin() block (stable, so same-layer draws never flip) - and replays
// them into the batch, splitting a fresh chunk wherever the clip changes
// in record order. The Vulkan half extends this class in
// renderer2d_device.cpp (beside it since stage 3b, when the device layer
// left for ws::gfx) and flushes that batch inside the render pass.

#include "text/atlas_packer.h"
#include "render/batch_core.h"

#include <array>
#include <cfloat>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include <volk.h>  // internal header: the Vk* types in signatures and
                  // storage below never reach apps - that surface is
                  // <ws/ui/renderer2d.hpp>.
#include <ws/gfx/gpu.hpp>  // Gpu: the device layer's memory + object ops
                           // (buffer/image create-destroy, flush, shader
                           // modules) this half draws its handles from - ui
                           // links no VMA since stage 3b phase C

#include <ws/ui/renderer2d.hpp>

namespace ws::ui::r2d {

// One recorded quad - lines are expanded into quads at record time. Kept
// as data so the whole frame can be sorted before batching: state changes,
// not submission order alone, decide the segment structure.
struct DrawCommand {
    // Sort key components, most significant first (see frame_end()):
    std::uint32_t sort_layer = 0;    // set_layer(): the explicit z escape hatch
    std::uint32_t camera_epoch = 0;  // which begin() block recorded this:
                                     // frame_cameras_[epoch - 1] holds its
                                     // camera - one snapshot per block, not
                                     // one camera pair per command
    float clip[4] = {0.0f, 0.0f, FLT_MAX, FLT_MAX};  // active clip at record
                                    // time - NOT a sort key: the replay
                                    // splits chunks wherever it changes in
                                    // record order (one chunk = one scissor)
    Texture texture;            // {} = flat color (the white texture)
    std::uint32_t color = 0;  // packed 0xRRGGBBAA, unpacked at replay

    float pos[4][3] = {};  // TL, TR, BR, BL; z = 0 (2D) or the 3D camera's world
    float uv[4][2] = {};
    float radius = 0.0f;  // 0 = the sample path (every record but
                          // draw_rounded_quad's SDF form); > 0 = that form's
                          // corner radius, world units - the replay routes
                          // the command to push_rounded_quad, which packs it
                          // into a NEGATIVE tex_index (never a texture id:
                          // that is the shader's discriminator)
};

// One begin() block's camera state, snapshotted at stash time. The block's
// commands reference it through their camera_epoch - a frame's blocks are
// few, its commands many, so the camera is stored once per block. The kind
// (2D vs perspective) rides the snapshot; one block is one kind, so the
// replay reads it only at an epoch boundary.
struct CameraSnapshot {
    Camera2D camera{};
    Camera3D camera3d{};
    bool perspective = false;
};

// How many frames may be recorded while an earlier one still executes -
// the frames-in-flight model, deliberately decoupled from the swapchain
// image count. The shell's frame slots (fence, command pool, buffer,
// acquire semaphore), this renderer's vertex arenas and the chunk
// descriptor storage are all keyed by it; 2 buys one frame of CPU/GPU
// overlap under vsync without doubling per-frame memory. Both sides must
// agree, so the constant lives here rather than in two files.
inline constexpr std::uint32_t kMaxFramesInFlight = 2;

// The texture path app.cpp's device probe picked (M9). bindless: one
// global kMaxBindlessTextures-slot descriptor table per frame slot, bound
// once per frame and indexed by the texture's registry id
// (descriptor indexing + nonuniformEXT; texture variety never splits a
// chunk). update_after_bind: the UAB feature - table writes land
// immediately at load time; without it they defer to each slot's next
// idle flush (its fence was waited), so a mid-run load shows white for at
// most one frame per slot. Both false: the fallback tier - today's
// per-chunk 32-slot tables, unchanged semantics, everything grown
// instead of capped.
struct TextureTier {
    bool bindless = false;
    bool update_after_bind = false;
};

class Renderer2D {
public:
    // The Vulkan handles (and the gfx memory handles paired with them) are
    // single-owned: a copied renderer would destroy them twice at shutdown.
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
    // One-time setup after the swapchain exists. `tier` is the device
    // probe's pick (app.cpp enables the features before calling); it fixes
    // the descriptor set layout, the frag shader variant and the flush path
    // for the process. `gpu` is the device layer: every buffer, image and
    // shader module created below goes through its methods (and the VMA
    // allocator behind them never reaches ui). Order inside: push-constant
    // limit check, upload
    // pool/fence, sampler, white texture (registry index 0 - every
    // descriptor slot starts bound to it), set layout (the tier's), pipeline
    // layout, per-frame-slot resources (descriptor sets + vertex and index
    // arenas - keyed by kMaxFramesInFlight, never by the swapchain image
    // count), pipeline (the tier's frag variant). On false the startup
    // fails; shutdown() is the failure-path cleanup and cleans partial
    // state.
    bool init(VkDevice device, VkPhysicalDevice physical_device,
              const gfx::Gpu& gpu, VkQueue queue, uint32_t queue_family,
              VkRenderPass render_pass, VkFormat format,
              const TextureTier& tier);
    // Swapchain drift. The per-frame-slot resources are swapchain-
    // independent, so only format drift matters: it rebuilds the pipeline (a
    // same-format resize needs nothing - that is the render-pass
    // compatibility the ImGui backend relies on). Extents are per-flush
    // (dynamic viewport/scissor).
    bool on_swapchain_recreated(VkRenderPass render_pass, VkFormat format);
    // Dev-only (WS_UI_DEV_SHADER_RELOAD): destroys both pipelines and
    // rebakes them from the provider's current words. Call only after every
    // in-flight frame retired - the shell waits its frame-slot fences, which
    // cover every submitted draw (the GPU may still execute draws through
    // the old pipelines). Only the dev build ever calls it, but it is
    // harmless and cheap to compile everywhere.
    bool reload_pipelines();
    // Destroys everything init()/load_texture() created. Idempotent.
    void shutdown();
    // Draws the batch inside the render pass, before ImGui's draw data (2D
    // world under widgets). frame_slot selects this slot's arenas and
    // descriptor sets; that slot's fence is waited before recording starts,
    // so they are safely writable here - and grown here, when the frame
    // outgrew them (the one point the slot is idle; see the grow helpers).
    void flush(VkCommandBuffer cmd, uint32_t frame_slot, float fb_width,
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
    // The overlay spelling: pushes WITHOUT intersecting the enclosing
    // clips (see the public header's push_clip_escape).
    void push_clip_escape(float x, float y, float width, float height);
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
    // Bakes a font (src/text/font.*, pure CPU) and uploads its glyph pages -
    // the font's OWN pages, never shared with load_atlas. The loaders'
    // failure contract: warn once, return an empty Font.
    Font load_font(const char* path, float pixel_height);

    // Test/observability seam (internal header only): the built batch as
    // frame_end() left it.
    const batch::BatchCore& batch() const { return batch_; }

private:
    // Per-frame warn kinds, one bit each: frame_begin() clears the mask, a
    // kind's first occurrence fires and sets its bit - so distinct problems
    // each get their one warn instead of the first problem silencing the
    // rest. The warns outside this mask keep their own one-shot bools and
    // never reset: clip_overflow_warned_ (per renderer) and
    // table_full_warned_ (per process - registry state).
    enum WarnKind : std::uint32_t {
        kWarnDrop = 1u << 0,            // draw outside on_update()/begin()
        kWarnKindMismatch = 1u << 1,     // 3D overload inside a 2D block
        kWarnCaps = 1u << 2,            // replay hit a cap, quads dropped
        kWarnUnknownTexture = 1u << 3,   // draw with an unregistered id
        kWarnSprite = 1u << 4,          // unresolvable sprite
    };
    // Central guard: false when calls must drop (no active frame) - warns
    // once per frame for its kind.
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
    // frame warn) when no begin() block is open. `radius > 0` marks the
    // command as draw_rounded_quad's SDF form (see DrawCommand::radius).
    void record(const char* what, const float (&pos)[4][3],
                const float (&uv)[4][2], std::uint32_t color, Texture texture,
                float radius = 0.0f);

    // --- GPU half helpers ---
    // Unknown texture id, while a registry exists: once per frame.
    void warn_unknown_texture(uint32_t id);
    // Unresolvable sprite (bad page or index): once per frame.
    void warn_sprite(const Sprite& sprite);
    // Resolves a sprite to its sub-rect; null when the page is not a
    // registered atlas page or the index is out of range. O(1): the dense
    // page_of_texture_ map carries texture id -> atlas_pages_ index (this
    // runs once per sprite and once per glyph, so it must not scale with
    // the page count). Page 0 is never looked up - it is the white
    // sentinel.
    const atlas::Rect* find_sprite_rect(const Sprite& sprite) const;
    // Graphics pipeline against the given render pass; the shader modules
    // are created and destroyed inside (they are baked into the pipeline).
    // The fragment module is the tier's variant (see TextureTier).
    bool create_pipeline(VkRenderPass render_pass);
    // Per-frame-slot resources: the tier's descriptor sets (bindless: one
    // global-table set per slot; chunked: one pool + kInitialChunkCapacity
    // sets per slot, grown at flush) plus one persistently-mapped vertex
    // arena and one index arena per slot, sized by the initial capacities.
    // Created once at init(); nothing here depends on the swapchain.
    bool create_frame_resources();
    void destroy_frame_resources();
    // Both arena halves grow at flush, the one point the frame slot is
    // idle (its fence was waited): destroy + recreate the vertex arena at
    // `min_vertices` (geometric - double, then double again if needed)
    // and the index arena at the matching quad capacity (its contents are
    // rewritten per frame at flush, so growth needs no refill). False
    // only when the (re)allocation fails - the caller keeps the old
    // buffer and skips the frame's draws (logged).
    bool grow_vertex_arena(uint32_t frame_slot, std::size_t min_vertices);
    bool grow_index_arena(uint32_t frame_slot, std::size_t min_quads);
    // Staging upload of a 4-byte-per-pixel R8G8B8A8 image (one-off submit +
    // fence wait). Cleans its partial state and returns false on failure.
    // In the bindless tier this also queues the id's descriptor table
    // write (immediate with update-after-bind, else at the slot-idle
    // flushes); ids past kMaxBindlessTextures are refused so a garbage
    // index can never reach the shader's table.
    bool upload_texture(const void* pixels, uint32_t width, uint32_t height,
                        Texture* out);
    // Binds a chunk's used slots [0, texture_count); the rest keep the
    // initial full-white write. Chunked tier only.
    void write_chunk_set(VkDescriptorSet set, const batch::Chunk& chunk);
    // Bindless tier: writes every pending id (>= table_written_[slot])
    // into the slot's table set and advances the high-water mark. Called
    // at the slot's idle flush, and once per slot at init when nothing is
    // in flight yet.
    void apply_pending_table_writes(uint32_t frame_slot);
    // Chunked tier: grows the slot's chunk-set pool to `min_chunks` sets -
    // rebuild it bigger (allocate + full-white-init the fresh sets,
    // destroy the old pool; the slot is idle, so nothing references it).
    // False = allocation failed; the old pool stays and the caller skips
    // the frame's draws (logged).
    bool grow_chunk_sets(uint32_t frame_slot, uint32_t min_chunks);

    std::vector<DrawCommand> commands_;  // capacity reused across frames
    batch::BatchCore batch_;
    Camera3D current_camera3d_{};  // the open 3D block's camera (billboards
                                  // and 3D lines expand against it at
                                  // record time; the replay reads the
                                  // snapshot, not this)
    // The open block's kind - what stash_camera() will snapshot, and what
    // the 3D overloads check against (a 3D overload inside a 2D block
    // warns and records nothing). Set by every begin(), never read
    // without an open block (camera_stashed_ gates that).
    bool current_perspective_ = false;
    std::uint32_t current_layer_ = 0;  // frame-scoped: reset with the clips
    std::uint32_t current_epoch_ = 0;  // bumped by every begin()
    // This frame's begin() blocks, in epoch order: a command recorded
    // under epoch e replays through frame_cameras_[e - 1]. Cleared with
    // the epochs at frame_begin; capacity reused across frames (a frame's
    // blocks are few, its commands many - that asymmetry is the point).
    std::vector<CameraSnapshot> frame_cameras_;
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
    std::uint32_t warned_mask_ = 0;  // WarnKind bits; frame_begin clears it

    // --- Vulkan state (null/empty until init()) ---
    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    // The device layer: buffers, images and shader modules come from its
    // methods, and the VMA allocator behind them stays inside gfx.
    const gfx::Gpu* gpu_ = nullptr;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;

    VkRenderPass render_pass_ = VK_NULL_HANDLE;  // last seen (rebuild input)
    VkFormat format_ = VK_FORMAT_UNDEFINED;     // drift sentinel

    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;  // the tier's layout
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
        void* allocation = nullptr;  // the gfx image allocation (destroy_image's pair)
    };
    std::vector<TextureEntry> textures_;

    // Atlas pages (load_atlas, or test injection): a page's texture id is
    // its key in Sprite::page. Only grows, so sprites stay valid for the
    // process lifetime - the same contract as textures_.
    struct AtlasPage {
        Texture texture;
        std::vector<atlas::Rect> rects;
    };
    std::vector<AtlasPage> atlas_pages_;
    // Dense texture id -> atlas_pages_ index, filled at
    // register_atlas_page (grown with fill value kNoAtlasPage = not a
    // page). find_sprite_rect's O(1) lookup; only grows, never rewritten.
    std::vector<std::uint32_t> page_of_texture_;
    static constexpr std::uint32_t kNoAtlasPage = 0xFFFFFFFFu;

    // One vertex arena per frame slot (kMaxFramesInFlight of them),
    // persistently mapped, starting at kInitialVertexCapacity vertices and
    // grown at the slot's idle flush. A slot's fence gates its reuse, so
    // the memcpy in flush() - and the growth around it - never races an
    // in-flight submission.
    struct FrameArena {
        VkBuffer buffer = VK_NULL_HANDLE;
        void* allocation = nullptr;  // the gfx buffer allocation (flush/destroy's handle)
        batch::Vertex* mapped = nullptr;
        std::size_t capacity = 0;  // vertices
    };
    std::vector<FrameArena> arenas_;

    // One index arena per frame slot, same growth contract as the vertex
    // arena. Per-slot (not shared) precisely so it CAN grow at the slot's
    // idle flush: a shared buffer would be bound by the other slot's
    // in-flight submission. u16, and the indices are CHUNK-LOCAL (quad q
    // of the chunk -> 4q + {0,1,2,2,3,0}): the flush writes each chunk's
    // pattern every frame, so growth needs no refill - and one chunk's
    // indices never leave the u16 space (batch::kMaxQuadsPerChunk).
    struct IndexArena {
        VkBuffer buffer = VK_NULL_HANDLE;
        void* allocation = nullptr;  // the gfx buffer allocation (flush/destroy's handle)
        std::uint16_t* mapped = nullptr;
        std::size_t capacity = 0;  // quads
    };
    std::vector<IndexArena> index_arenas_;

    VkCommandPool upload_pool_ = VK_NULL_HANDLE;  // texture uploads
    VkFence upload_fence_ = VK_NULL_HANDLE;

    // --- texture tier state (M9; see TextureTier) ---
    TextureTier tier_{};
    // Bindless: one global-table set per frame slot, all slots white at
    // creation (every index a malformed draw could round to stays valid).
    VkDescriptorPool table_pool_ = VK_NULL_HANDLE;  // owns table_sets_
    std::vector<VkDescriptorSet> table_sets_;       // [frame slot]
    // How far each slot's table has caught up: ids [0, table_written_[s])
    // are bound in slot s's set. Ids only grow, so a high-water mark
    // suffices. Without update-after-bind a fresh texture waits for the
    // slot's next idle flush; with it, upload_texture() writes every slot
    // immediately and this only trails between init and the first frames.
    std::array<std::uint32_t, kMaxFramesInFlight> table_written_{};
    // Texture ids queued for table writes (drained per slot at its idle
    // flush, or immediately when the last slot catches up).
    std::vector<std::uint32_t> pending_table_ids_;
    // One warn for the whole process: the bindless table refusing loads
    // past kMaxBindlessTextures.
    bool table_full_warned_ = false;
    // Chunked: one pool + its chunk sets per frame slot. A set belongs to
    // exactly one (frame slot, chunk) pair, whose slot fence is waited
    // before any recording starts - that is what makes the flush-time
    // rewrite (and growth) safe.
    struct ChunkSets {
        VkDescriptorPool pool = VK_NULL_HANDLE;
        std::vector<VkDescriptorSet> sets;  // [chunk index]
        std::uint32_t capacity = 0;
    };
    std::vector<ChunkSets> chunk_sets_;  // [frame slot]
};

// Points the public free functions at the live renderer. app.cpp installs it
// for the duration of App::run() and clears it at shutdown.
void set_active(Renderer2D* renderer);

}  // namespace ws::ui::r2d
