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
#include "text/font.h"  // font::BakedFont (publish_baked_font's parameter)
#include "render/batch_core.h"

#include <ws/ui/renderer3d.hpp>  // r3d: Mesh/Transform3D/MeshMaterial (the
                                  // mesh registry + commands below carry
                                  // them; the public header stays
                                  // third-party-free)

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
    std::uint32_t color[4] = {};  // packed 0xRRGGBBAA, one per corner
                                 // (TL, TR, BR, BL), unpacked at replay.
                                 // The plain draws fill all four with one
                                 // value; the canvas tessellation path
                                 // (draw_vertex_quad) ramps alpha across
                                 // the corners for its AA fringes.

    float pos[4][3] = {};  // TL, TR, BR, BL; z = 0 (2D) or the 3D camera's world
    float uv[4][2] = {};
    float radius = 0.0f;  // 0 = the sample path (every record but
                          // draw_rounded_quad's SDF form); > 0 = that form's
                          // corner radius, world units - the replay routes
                          // the command to push_rounded_quad, which packs it
                          // into a NEGATIVE tex_index (never a texture id:
                          // that is the shader's discriminator)
    bool text = false;    // true = a glyph quad (draw_text): the replay
                          // pushes it with batch::kTextCoverageFlag, so the
                          // shader reads the texel as a coverage mask and
                          // applies its text contrast curve
    // SDF text (the second glyph mode): set only for quads from an
    // sdf-baked font (draw_text on FontBakeOptions::sdf). The replay
    // then adds batch::kTextSdfFlag INSTEAD of the coverage flag, and
    // the shader reconstructs the coverage from the distance field per
    // draw. Never both: the two bands are disjoint (see batch.frag).
    bool text_sdf = false;
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

// Texture id packing: id = (generation << kGenShift) | slot. The slot is
// the registry index (bindless caps it at kMaxBindlessTextures; the
// chunked tier's chunk tables hold it too); the generation counts the
// slot's reuses - destroy_texture bumps it at once, so a stale handle the
// app kept drawing with fails record()'s gen check and renders white
// instead of aliasing the slot's next tenant. 12 bits wrap after 4096
// reuses of one slot: a handle older than that can false-positive as
// fresh (the documented limit).
inline constexpr std::uint32_t kSlotMask = 0xFFFFFu;  // 20-bit slot, 1M
inline constexpr std::uint32_t kGenShift = 20;
inline constexpr std::uint32_t kGenMask = 0xFFFu;  // 12-bit generation

// --- the mesh path (r3d, Phase 1) ---------------------------------------------------
//
// The mesh command and registry live here, on the Renderer2D, because the
// mesh path is a THIRD pipeline over the same frame plumbing: the same
// begin() epochs, the same layer/clip carry, the same accept/kind gates
// the 3D billboards walk. The commands replay beside the quad commands
// (same (layer, epoch) sort); the Phase 2 device half flushes them through
// their own pipeline (pipeline_mesh_, indexed draws, Vertex3D layout) -
// the batch's 40-byte Vertex and batch.vert/.frag stay untouched.

// One recorded mesh draw (r3d::draw_mesh): the mesh's CONTENT is not
// copied into the command - the registry entry outlives the frame by the
// texture-registry contract (destroy kills the handle at once; a draw
// recorded this frame with a handle destroyed this frame was already
// rejected at record time). What rides is the counts (the Phase 2 flush
// reads them against the registry's storage), the material, the
// transform, and the same placement keys the quad commands carry.
struct MeshCommand {
    std::uint32_t sort_layer = 0;    // set_layer(): the explicit z hatch
    std::uint32_t camera_epoch = 0;  // the recording begin(Camera3D) block
    float clip[4] = {0.0f, 0.0f, FLT_MAX, FLT_MAX};
    std::uint32_t tint = 0xFFFFFFFFu;  // 0xRRGGBBAA
    Texture texture{};                // {} = untextured (white)
    r3d::Transform3D transform{};
    std::uint32_t vertex_count = 0;  // the registry entry's, snapshotted
    std::uint32_t index_count = 0;   // (the flush's indexed draw)
    // The GPU half (Phase 2): the mesh's raw VkBuffer handle (vertices at
    // offset 0, u32 indices at gpu_index_offset, ONE buffer per mesh) and
    // the byte offset of the index run. Snapshotted at record time - NOT
    // looked up through the registry at flush - because the registry slot
    // can be destroyed AND reused between a draw's record and the flush;
    // destroy_mesh retires the buffer for kMaxFramesInFlight + 1 flushes,
    // so a recorded draw's handle outlives anything that could replace its
    // slot. 0 = no GPU copy (headless): the flush has nothing to bind.
    std::uint64_t gpu_buffer = 0;
    std::uint64_t gpu_index_offset = 0;
};

// The texture path app.cpp's device probe picked (M9). bindless: one
// global kMaxBindlessTextures-slot descriptor table per frame slot, bound
// once per frame and indexed by the texture's registry SLOT
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
    // layout, pipeline cache, per-frame-slot resources (descriptor sets +
    // vertex and index arenas - keyed by kMaxFramesInFlight, never by the
    // swapchain image count), pipeline (the tier's frag variant). On false
    // the startup fails; shutdown() is the failure-path cleanup and cleans
    // partial state.
    bool init(VkDevice device, VkPhysicalDevice physical_device,
              const gfx::Gpu& gpu, VkQueue queue, uint32_t queue_family,
              VkRenderPass render_pass, VkFormat format,
              const TextureTier& tier,
              uint32_t sample_count = 1u);
    // Swapchain drift. The per-frame-slot resources are swapchain-
    // independent, so only format drift matters: it rebuilds the pipeline (a
    // same-format resize needs nothing - that is the render-pass
    // compatibility the ImGui backend relies on). Extents are per-flush
    // (dynamic viewport/scissor). Phase 4: sample-count drift (an MSAA
    // request that lands a different count on the rebuilt swapchain)
    // rides the same rebuild as the format - the pipelines must always
    // match the pass they draw into.
    bool on_swapchain_recreated(VkRenderPass render_pass, VkFormat format,
                                uint32_t sample_count = 1u);
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
    // The layered pair (see the public header): outline = 2 rounded quads
    // (outer color + inner face), shadow = the documented 4-layer stack.
    // Both compose from draw_rounded_quad's own record path - no new batch
    // plumbing, the stub sees plain rounded quads.
    void draw_rounded_outline(float x, float y, float w, float h, float radius,
                              float thickness, std::uint32_t color,
                              std::uint32_t face);
    void draw_shadow(float x, float y, float w, float h, float radius,
                     float blur, std::uint32_t color);
    // The canvas layer's free-form quad: four corners in the current
    // camera's space (2D blocks only - a 3D block warns like the 2D
    // overloads' reverse), per-corner rgba floats (rasterize-ready
    // 0..1, NOT packed), caller-managed UVs, optional texture. Winding
    // is the caller's: both index triangles (0,1,2 / 2,3,0) carry
    // whatever order the corners name, and a triangle is a quad with
    // its fourth corner duplicated (degenerate, zero-area - the batch
    // needs no triangle path). This is the tessellation sink the
    // ws::ui::canvas layer emits into; nothing else should need it.
    // MSVC-friendly array reference via typedefs (avoids array-reference
    // declarator parsing quirk with parenthesized parameters).
    using VertexQuadPositions = const float[4][2];
    using VertexQuadTexCoords = const float[4][2];
    using VertexQuadColors = const float[4][4];
    void draw_vertex_quad(VertexQuadPositions& pos, VertexQuadTexCoords& uv,
                          VertexQuadColors& rgba, Texture texture = {});
    FrameSize frame_size() const;
    Texture load_texture(const char* path);
    std::vector<Sprite> load_atlas(std::initializer_list<const char*> paths);
    void draw_sprite(float x, float y, float w, float h, std::uint32_t color,
                     Sprite sprite, float rotation = 0.0f);
    void draw_sprite(float x, float y, float z, float w, float h,
                     std::uint32_t color, Sprite sprite);
    // The glyph draw path's quad constructor: the sprite's rect supplies
    // pos/uv, the color is uniform, and the quad is recorded on the glyph
    // channel so the replay tags it for the shader's contrast curve - the
    // coverage flag for bitmap fonts (text), or the SDF flag for
    // distance-field fonts (text_sdf; the two bands are disjoint).
    void draw_sprite_quad(const Sprite& sprite, const float (&pos)[4][3],
                          const float (&uv)[4][2], std::uint32_t color,
                          bool text_sdf = false);
    FrameSize sprite_size(Sprite sprite) const;
    // Installs a page's sub-rect table; Sprite::page is its key. Production
    // (load_atlas) calls this per uploaded page; tests inject synthetic
    // pages headlessly - the sprite contract is pure CPU state.
    void register_atlas_page(Texture page, std::vector<atlas::Rect> rects);
    // The one page-upload step every packed surface funnels through
    // (load_atlas, load_font, font_bake_pending): stages the pixels as a
    // square RGBA8 page texture and registers its rects. Texture{} when
    // headless or the upload fails - callers keep their degraded paths
    // (white sprites; page-0 glyph quads that drop but still advance).
    Texture upload_atlas_page(const void* pixels, std::uint32_t size,
                              std::vector<atlas::Rect> rects);
    // Text: one sprite-quad per visible glyph through the same recording
    // path as draw_sprite (a glyph IS a Sprite). Returns the pen advance
    // - exactly Font::measure_text's answer, by construction.
    float draw_text(float x, float y, const char* utf8_text,
                    const Font& font, std::uint32_t color = 0xFFFFFFFFu);
    // Bakes a font (src/text/font.*, pure CPU) and uploads its glyph pages -
    // the font's OWN pages, never shared with load_atlas. The loaders'
    // failure contract: warn once, return an empty Font.
    Font load_font(const char* path, float pixel_height,
                   const FontBakeOptions& options = {});
    // The same page pipeline over the BUNDLED default face (Inter,
    // embedded in src/text/default_font_*.cpp - the stock look, no
    // filesystem). `weight` picks regular or semibold.
    Font load_default_font(DefaultFontWeight weight, float pixel_height,
                           const FontBakeOptions& options = {});
    // The bundled MONO face's pipeline (JetBrains Mono, embedded the
    // same way) - the public load_default_mono_font routes here.
    Font load_default_mono_font(float pixel_height,
                                const FontBakeOptions& options = {});
    Font publish_baked_font(const font::BakedFont& baked,
                            const FontBakeOptions& options);
    // The texture-lifecycle pair (the public header's
    // load_texture_from_memory / destroy_texture route here).
    // from_memory is the in-memory spelling of load_texture; destroy
    // retires the handle at once - record() rejects its generation from
    // this call on, so only the frames already recorded can still see the
    // image - and queues the GPU free for the flush kMaxFramesInFlight
    // frames out. Headless both warn and answer white/no-op.
    Texture load_texture_from_memory(const void* data, std::size_t size);
    void destroy_texture(Texture texture);
    // The last frame's counts (snapshotted at frame_end) plus the live
    // count and tier, filled at call time. The member behind stats().
    Stats stats() const;

    // --- the mesh path (r3d, Phase 1) ------------------------------------------------
    // The registry: create/destroy generation-counted handles, the
    // Texture contract verbatim (slot 0 reserved for Mesh{}'s id 0).
    // Headless they are fully live (pure CPU state) - only the Phase 2
    // GPU buffers wait for a device. draw_mesh walks the 3D gates
    // (accept -> camera_stashed_ -> current_perspective_) and records a
    // MeshCommand; the gates' warn contract is the billboard rule's.
    [[nodiscard]] ws::core::Result<r3d::Mesh> create_mesh(
        const r3d::MeshData& data);
    [[nodiscard]] ws::core::Status destroy_mesh(r3d::Mesh mesh);
    void draw_mesh(r3d::Mesh mesh, const r3d::Transform3D& transform,
                   const r3d::MeshMaterial& material);
    [[nodiscard]] ws::core::Status set_light(const r3d::Light3D& light);
    [[nodiscard]] const r3d::Light3D& light() const { return light_; }

    // Test/observability seam (internal header only): the built batch as
    // frame_end() left it.
    const batch::BatchCore& batch() const { return batch_; }
    // The mesh half of the same seam: the frame's recorded mesh
    // commands as frame_end's (layer, epoch) sort left them.
    const std::vector<MeshCommand>& mesh_commands() const {
        return mesh_commands_;
    }
    // The weave's other half (Phase 2): mesh_chunk_marks()[i] is the batch
    // chunk count at the moment mesh_commands()[i] was woven into the
    // replay - the flush draws that mesh after chunks [0, mark) and
    // before chunk `mark`. Parallel to mesh_commands(), non-decreasing.
    const std::vector<std::uint32_t>& mesh_chunk_marks() const {
        return mesh_chunk_marks_;
    }

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
    // `text_sdf` selects the SECOND glyph channel (SDF-baked fonts): the
    // command then carries text_sdf, never text, and the replay flags the
    // quad with batch::kTextSdfFlag instead.
    void record(const char* what, const float (&pos)[4][3],
                const float (&uv)[4][2], std::uint32_t color, Texture texture,
                float radius = 0.0f, bool text = false,
                bool text_sdf = false);
    // record()'s per-corner spelling (draw_vertex_quad): same guards, same
    // command list, but each corner's packed color comes from its own rgba
    // row (clamped 0..1 before packing - the canvas math may overshoot on
    // the way to a weighted average).
    void record_vcol(const char* what, const float (&pos)[4][3],
                     const float (&uv)[4][2], const float (&rgba)[4][4],
                     Texture texture, bool text = false);

    // --- GPU half helpers ---
    // Unknown texture id, while a registry exists: once per frame.
    void warn_unknown_texture(uint32_t id);
    // Unresolvable sprite (bad page or index): once per frame.
    void warn_sprite(const Sprite& sprite);
    // Resolves a sprite to its sub-rect; null when the page is not a
    // registered atlas page or the index is out of range. O(1): the dense
    // page_of_texture_ map carries texture slot -> atlas_pages_ index (this
    // runs once per sprite and once per glyph, so it must not scale with
    // the page count); a stale page id fails the full-id equality check.
    // Page 0 is never looked up - it is the white sentinel.
    const atlas::Rect* find_sprite_rect(const Sprite& sprite) const;
    // Graphics pipeline against the given render pass; the shader modules
    // are created and destroyed inside (they are baked into the pipeline).
    // The fragment module is the tier's variant (see TextureTier).
    bool create_pipeline(VkRenderPass render_pass);
    // The mesh pipeline against the given render pass (the same
    // lifecycle points as create_pipeline: init, format-drift rebake, dev
    // reload). Non-fatal on failure: pipeline_mesh_ stays null and the
    // flush skips mesh draws.
    bool create_mesh_pipeline(VkRenderPass render_pass);
    // The mesh path's layouts + per-frame scene blocks (once, at init,
    // after the tier's set layout exists) and their teardown.
    bool create_mesh_resources();
    void destroy_mesh_resources();
    // The mesh retire queue's drain at flush entry (free_retired_textures'
    // sibling).
    void free_retired_meshes();
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
    // Slots come off free_slots_ first (destroy_texture's retirees); the
    // registry only grows when the freelist is empty. In the bindless
    // tier this also queues the slot's descriptor table rebind (immediate
    // with update-after-bind, else at the slot-idle flushes); fresh slots
    // past kMaxBindlessTextures are refused so a garbage index can never
    // reach the shader's table (a reused slot always fits - its table
    // index never changed).
    bool upload_texture(const void* pixels, uint32_t width, uint32_t height,
                        Texture* out);
    // Binds a chunk's used slots [0, texture_count); the rest keep the
    // initial full-white write. Chunked tier only.
    void write_chunk_set(VkDescriptorSet set, const batch::Chunk& chunk);
    // Bindless tier: marks one texture slot's table binding stale in
    // EVERY frame slot and queues the rewrite. Uploads (the slot's
    // content changed) and frees (the view died; the row parks on
    // white) both land here - the cleared bitmap bits are what make a
    // REUSED slot rebind like a fresh one.
    void queue_table_write(uint32_t slot);
    // Bindless tier: rebinds every queued texture slot whose up-to-date
    // bit is clear in this frame slot's table (see table_written_) and
    // drains the queue entries every frame slot has caught up on. Called
    // at the slot's idle flush, immediately per slot at upload time when
    // update-after-bind is up, and once per slot at init when nothing is
    // in flight yet.
    void apply_pending_table_writes(uint32_t frame_slot);
    // Frees the retire queue's due entries at flush entry (the device
    // half's one lifecycle step): destroy view + image, park the row on
    // white's view, return the slot to free_slots_, and queue the table
    // rebind. Safe exactly there: this flush's acquire waited its slot
    // fence - the one the submit kMaxFramesInFlight back signaled, and
    // by the same acquire chain every older one - so the destroy's
    // frame, the last that could reference the image, has fully
    // retired; later frames never recorded it (destroy already bumped
    // the generation).
    void free_retired_textures();
    // Chunked tier: grows the slot's chunk-set pool to `min_chunks` sets -
    // rebuild it bigger (allocate + full-white-init the fresh sets,
    // destroy the old pool; the slot is idle, so nothing references it).
    // False = allocation failed; the old pool stays and the caller skips
    // the frame's draws (logged).
    bool grow_chunk_sets(uint32_t frame_slot, uint32_t min_chunks);
    // Debug-utils object name (validation captures name what they see).
    // No-ops unless the instance carries VK_EXT_debug_utils - gfx enables
    // it only when validation runs, and volk leaves the entry point null
    // otherwise, so a plain build names nothing and pays nothing.
    void name_object(VkObjectType object_type, std::uint64_t handle,
                     const char* name);

    // --- the mesh path's private helpers + state (r3d, Phase 1) --------------------
    // The registry's row: the mesh's CPU payload (Phase 2 adds the GPU
    // buffer pair beside it) and the generation the slot's id carries.
    // Slot 0 is the reserved sentinel (Mesh{}'s id 0), like textures'
    // white row.
    struct MeshEntry {
        std::vector<r3d::Vertex3D> vertices;
        std::vector<std::uint32_t> indices;  // whole triangles only
        std::uint32_t generation = 0;  // 0 = never used (the sentinel's)
        // The GPU copy (Phase 2): one host-visible buffer holding the
        // vertices, then the indices at gpu_index_offset. 0 = none (the
        // headless renderer, or an empty index list). Raw 64-bit handles,
        // the gfx::Gpu spelling (see TextureEntry).
        std::uint64_t gpu_buffer = 0;
        std::uint64_t gpu_allocation = 0;
        std::uint64_t gpu_index_offset = 0;
    };
    // The generation check (the texture id check's mirror): false for
    // id 0, an out-of-table slot, or a generation mismatch.
    [[nodiscard]] bool mesh_alive(r3d::Mesh mesh) const;
    // The record step after draw_mesh's gates passed: snapshots the
    // placement keys (epoch/layer/clip) and the material/transform onto
    // a MeshCommand and appends it. The mesh's own CONTENT stays in the
    // registry entry (see MeshCommand's comment).
    void record_mesh(const r3d::MeshMaterial& material,
                      const r3d::Transform3D& transform,
                      const MeshEntry& entry);
    // Creates the GPU buffer for one registry entry (vertices + indices,
    // host-visible, one allocation) and fills entry.gpu_*. False on
    // failure (logged); the entry is left GPU-less.
    bool upload_mesh(MeshEntry& entry);
    std::vector<MeshEntry> meshes_;
    std::vector<std::uint32_t> free_mesh_slots_;

    std::vector<DrawCommand> commands_;  // capacity reused across frames
    batch::BatchCore batch_;
    Camera3D current_camera3d_{};  // the open 3D block's camera (billboards
                                  // and 3D lines expand against it at
                                  // record time; the replay reads the
                                  // snapshot, not this)

    // The frame's mesh draws, the quad commands' sibling list: recorded
    // through draw_mesh, cleared with the frame, sorted at frame_end by
    // (layer, epoch) beside the quad sort (stable, painter's order).
    std::vector<MeshCommand> mesh_commands_;
    // The scene light (r3d::set_light): frame-scoped state, not a draw.
    // The default matches r3d::Light3D's own - a top-down key plus an
    // ambient floor, so a frame that never sets one still shades sanely.
    r3d::Light3D light_{};
    // draw_mesh's reject warns (stale mesh, wrong block, no transform
    // validity) - the once-per-frame mask's r3d bits.
    static constexpr std::uint32_t kWarnMeshUnknown = 1u << 5;  // stale handle
    static constexpr std::uint32_t kWarnMeshTransform = 1u << 6;  // scale <= 0
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
    // The SUBMISSION clock, bumped at the top of every flush - each flush
    // call is one submit, and dropped frames (out-of-date, hidden) never
    // bump it. The texture retire queue's timeline: destroy_texture stamps
    // free_at = now + kMaxFramesInFlight + 1, because the recording
    // frame - the last that can still reference a just-destroyed image -
    // is the NEXT submit, and the flush that reaches free_at has, at
    // entry, already waited the fence that submit signaled.
    std::uint64_t frame_index_ = 0;
    // frame_end's snapshot of the frame; stats() fills the registry/tier
    // fields on top of it at call time. frame_drop zeroes it.
    Stats last_stats_{};

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
    // Phase 4: the pass's sample count (VkSampleCountFlagBits numeric;
    // 1 = the classic single-sample pass). Every pipeline's
    // rasterizationSamples reads it, and on_swapchain_recreated treats a
    // change as the same rebuild-worthy drift the format is.
    uint32_t sample_count_ = 1u;

    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;  // the tier's layout
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    // The depth-tested sibling (LEQUAL + write, for Camera3D chunks): same
    // shaders/layout/render pass, only the depth state differs. Created
    // and destroyed in lockstep with pipeline_.
    VkPipeline pipeline_3d_ = VK_NULL_HANDLE;
    // --- the mesh path's device half (r3d, Phase 2) -------------------------
    // The mesh pipeline (indexed Vertex3D triangles, depth LEQUAL + write,
    // back-face cull, shaders/mesh.*). Its OWN pipeline layout: set 0 is
    // the same texture-table layout the batch pipelines bind (so the one
    // bound table serves both), set 1 the per-frame-slot scene block, and
    // a 120-byte push range (renderer3d_math.h's MeshPush). Failure to
    // build it is non-fatal: 2D keeps rendering, mesh draws are skipped
    // (logged once).
    VkDescriptorSetLayout scene_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout mesh_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_mesh_ = VK_NULL_HANDLE;
    // One persistently-mapped scene uniform block (the frame's light) +
    // its descriptor set per frame slot: rewritten at that slot's idle
    // flush, so it never races an in-flight submission.
    struct SceneSlot {
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkBuffer buffer = VK_NULL_HANDLE;
        std::uint64_t allocation = 0;
        void* mapped = nullptr;
    };
    VkDescriptorPool scene_pool_ = VK_NULL_HANDLE;  // owns the scene sets
    std::vector<SceneSlot> scene_slots_;            // [frame slot]
    // destroy_mesh's pending GPU frees: due once the submission clock
    // reaches free_at (the texture retire queue's contract).
    struct MeshRetireEntry {
        std::uint64_t buffer = 0;
        std::uint64_t allocation = 0;
        std::uint64_t free_at = 0;
    };
    std::vector<MeshRetireEntry> mesh_retire_queue_;
    // frame_end's weave result, parallel to mesh_commands_ (see
    // mesh_chunk_marks()).
    std::vector<std::uint32_t> mesh_chunk_marks_;
    // Dev-reload-free by design: the mesh stages always come from the
    // committed mesh_spirv.h words (the hot-reload provider covers the
    // batch stages only).
    // The in-memory pipeline cache every create_pipeline call funnels
    // through - init's, a format-drift rebake's, a dev hot reload's - so a
    // rebake reuses the driver's compiled stages instead of recompiling.
    // Destroyed only at shutdown (nothing else invalidates it); never
    // persisted - two tiny pipelines make a disk file pure liability.
    VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;

    // A Texture's id packs (generation << kGenShift) | slot; the slot is
    // this table's index, [0] is the white texture (id 0, never
    // destroyed). destroy_texture bumps the generation at once - the
    // handle is dead from that moment - and free_retired_textures
    // reclaims the image at the flush kMaxFramesInFlight frames later,
    // parks the entry on white's view (a table must never hold a dead
    // view) and pushes the slot onto free_slots_ for the next upload to
    // reuse (which hands out the bumped generation).
    struct TextureEntry {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        std::uint64_t allocation = 0;  // the gfx image allocation (destroy_image's pair; a 64-bit handle)
        std::uint32_t generation = 0;  // the slot's reuse counter (id's high bits)
    };
    std::vector<TextureEntry> textures_;
    // Pending GPU frees, drained at flush entry (see
    // free_retired_textures). The one lifecycle queue: app-side destroys
    // enqueue, the device half frees.
    struct RetireEntry {
        std::uint32_t slot;
        std::uint64_t free_at;  // frame_index_ the flush must have reached
    };
    std::vector<RetireEntry> retire_queue_;
    // Slots whose images are reclaimed - upload_texture pops here before
    // the registry grows. Shutdown clears it with the registry.
    std::vector<std::uint32_t> free_slots_;

    // Atlas pages (load_atlas, or test injection): a page's texture id is
    // its key in Sprite::page. Only grows, so sprites stay valid for the
    // process lifetime - the same contract as textures_.
    struct AtlasPage {
        Texture texture;
        std::vector<atlas::Rect> rects;
    };
    std::vector<AtlasPage> atlas_pages_;
    // Dense texture SLOT -> atlas_pages_ index, filled at
    // register_atlas_page (grown with fill value kNoAtlasPage = not a
    // page). find_sprite_rect's O(1) lookup; a freed page's slot is
    // re-sentineled at the free.
    std::vector<std::uint32_t> page_of_texture_;
    static constexpr std::uint32_t kNoAtlasPage = 0xFFFFFFFFu;

    // One vertex arena per frame slot (kMaxFramesInFlight of them),
    // persistently mapped, starting at kInitialVertexCapacity vertices and
    // grown at the slot's idle flush. A slot's fence gates its reuse, so
    // the memcpy in flush() - and the growth around it - never races an
    // in-flight submission.
    struct FrameArena {
        VkBuffer buffer = VK_NULL_HANDLE;
        std::uint64_t allocation = 0;  // the gfx buffer allocation (flush/destroy's handle; a 64-bit handle)
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
        std::uint64_t allocation = 0;  // the gfx buffer allocation (flush/destroy's handle; a 64-bit handle)
        std::uint16_t* mapped = nullptr;
        std::size_t capacity = 0;  // quads
        // The skip-rewrite cache: the chunk layout (vertex_offset +
        // quad_count per chunk, see flush) this buffer's pattern was last
        // written for, and whether that write ever happened. A steady
        // frame re-records the identical chunk structure - only vertex
        // POSITIONS moved - and the u16 pattern is a pure function of the
        // layout, so the rewrite (and its cache flush) is skipped whole.
        // Any growth replaces the buffer with a fresh one: contents_valid
        // drops, the next flush rewrites.
        std::vector<std::uint32_t> layout;
        bool contents_valid = false;
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
    // Per frame slot, one up-to-date bit per texture slot: SET = this
    // frame slot's table binding for that texture slot holds the slot's
    // current content (its live view, or the parked white of a freed
    // slot); CLEAR = a rebind is owed. Uploads and frees clear the slot's
    // bits everywhere and queue it below; the init's full-white write
    // makes every bit start SET. This replaced M9's high-water marks -
    // reused slots would have slipped past a high-water check silently.
    std::array<std::array<std::uint64_t, batch::kMaxBindlessTextures / 64>,
               kMaxFramesInFlight>
        table_written_{};
    // Texture slots queued for table rebinds (drained per frame slot at
    // its idle flush, or immediately under update-after-bind once every
    // slot catches up). Duplicate entries are harmless - the writes are
    // idempotent and the drain removes them together.
    std::vector<std::uint32_t> pending_table_slots_;
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

// The r3d free functions' routing seam: the live renderer, or null when
// no app is running (set_active's mirror for renderer3d.cpp - the r2d
// layer keeps g_active file-local, so the sibling TU reads it through
// here). Same lifetime: installed for App::run(), cleared at shutdown.
[[nodiscard]] Renderer2D* active_renderer();

// The r3d layer's no-app warn (renderer3d.cpp): once per process, the
// r2d warn_no_app convention - an app bug, not per-call spam.
void warn_no_app_r3d(const char* what);

}  // namespace ws::ui::r2d
