#include "render/renderer2d.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstring>

#include "text/font.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include <stb_image.h>  // declarations only; the implementation TU is stb_impl.cpp
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <ws/core/log.hpp>

namespace ws::ui::r2d {

namespace {

// TL, TR, BR, BL - matches the shared index pattern (0,1,2 / 2,3,0).
constexpr float kQuadUV[4][2] = {
    {0.0f, 0.0f},  // TL
    {1.0f, 0.0f},  // TR
    {1.0f, 1.0f},  // BR
    {0.0f, 1.0f},  // BL
};

}  // namespace

// --- Renderer2D: shell plumbing --------------------------------------------

void Renderer2D::frame_begin(float fb_width, float fb_height) {
    fb_width_ = fb_width;
    fb_height_ = fb_height;
    batch_.begin_frame();
    commands_.clear();  // capacity kept - the frame reuses the arena
    // The epochs restart at 1, so the snapshots go with them. Capacity
    // kept, like every per-frame arena here.
    frame_cameras_.clear();
    current_epoch_ = 0;
    current_layer_ = 0;  // layers never leak across frames either
    camera_stashed_ = false;
    current_perspective_ = false;
    clip_depth_ = 0;  // clips never leak across frames
    frame_active_ = true;
    warned_mask_ = 0;
}

void Renderer2D::frame_end() {
    frame_active_ = false;
    // Captured before the empty check: the replay clears commands_, and
    // stats() reports what the app recorded whatever the replay did.
    const std::uint32_t command_count =
        static_cast<std::uint32_t>(commands_.size());
    if (commands_.empty()) {
        last_stats_ = Stats{};  // a frame that recorded nothing
        return;
    }
    // Layer first (the explicit z escape hatch), then begin() block. Stable,
    // so within one layer+epoch draws replay in the exact order the app
    // recorded them - real painter's order. Clip is deliberately NOT a
    // sort key: sorting by clip VALUES made overlapping panels with
    // different clips swap z-order whenever the rects compared backwards.
    // Instead the replay below splits chunks wherever the clip changes in
    // record order - the scissor still gets its own chunk, without the
    // reordering. Material (texture) is not a sort key either - the
    // 32-slot table already lets one chunk hold many textures, so sorting
    // by it bought nothing but reordered overlapping draws that share a
    // layer/epoch and differ in texture (a button's face vs. its label).
    std::stable_sort(commands_.begin(), commands_.end(),
                     [](const DrawCommand& a, const DrawCommand& b) {
                         if (a.sort_layer != b.sort_layer) {
                             return a.sort_layer < b.sort_layer;
                         }
                         return a.camera_epoch < b.camera_epoch;
                     });

    // Replay: a segment opens per begin() block AND per clip change - both
    // are hard chunk boundaries (one VP and one scissor per chunk). Within
    // a segment the block's 32-slot texture table absorbs material variety,
    // so material changes never split. Runs of the same block (same camera,
    // different layers) share the segment - their sorted vertex order
    // already paints correctly.
    std::uint32_t open_epoch = 0;  // 0 never matches a recorded block
    float open_clip[4] = {batch::kFullClip[0], batch::kFullClip[1],
                          batch::kFullClip[2], batch::kFullClip[3]};
    for (const DrawCommand& cmd : commands_) {
        bool clip_changed = false;
        for (int i = 0; i < 4; ++i) {
            clip_changed = clip_changed || cmd.clip[i] != open_clip[i];
        }
        if (cmd.camera_epoch != open_epoch || clip_changed) {
            batch_.close_segment();
            // The block's snapshot picks the camera half: a perspective
            // block replays through camera_vp3d and flags its chunks
            // depth-tested (the flush's per-chunk pipeline bind), a 2D
            // block through camera_vp with the flag off. The kind only
            // ever changes at an epoch boundary - one begin() block is
            // one kind - so it never splits a segment by itself.
            const CameraSnapshot& snap =
                frame_cameras_[cmd.camera_epoch - 1];
            const batch::Mat4 vp =
                snap.perspective
                    ? batch::camera_vp3d(snap.camera3d, fb_width_, fb_height_)
                    : batch::camera_vp(snap.camera, fb_width_, fb_height_);
            batch_.open_segment(vp, cmd.clip, snap.perspective);
            open_epoch = cmd.camera_epoch;
            for (int i = 0; i < 4; ++i) {
                open_clip[i] = cmd.clip[i];
            }
        }
        float color[4][4];
        for (int i = 0; i < 4; ++i) {
            batch::unpack_rgba(cmd.color[i], color[i]);
        }
        if (cmd.radius > 0.0f) {
            // draw_rounded_quad's SDF form: one quad, no texture slot -
            // the batch packs the radius into a negative tex_index.
            (void)batch_.push_rounded_quad(cmd.pos, cmd.uv, color[0],
                                            cmd.radius);
        } else {
            // The SLOT, not the full id: the batch's tables (chunk
            // tables, the bindless tex_index) key on registry slots, and
            // record() already proved the id's generation. Per-corner
            // colors ride the vcol push (identical rows for the plain
            // draws - the same vertices push_quad would have built).
            (void)batch_.push_quad_vcol(cmd.pos, cmd.uv, color,
                                        cmd.texture.id & kSlotMask);
        }
    }
    batch_.close_segment();
    commands_.clear();
    // The frame's accounting, for stats(): captured now that the whole
    // replay has run (the counts are only complete here). The registry
    // and tier fields fill in at call time, on the device half.
    last_stats_ = Stats{};
    last_stats_.commands = command_count;
    last_stats_.quads =
        static_cast<std::uint32_t>(batch_.vertices().size() / 4);
    last_stats_.chunks = batch_.chunk_count();
    last_stats_.vertices =
        static_cast<std::uint32_t>(batch_.vertices().size());
    last_stats_.dropped_quads = batch_.dropped_quads();
    // Warn once per frame with the frame's FINAL drop totals - the counts
    // are only complete after the whole replay, so the warn lives here,
    // not inside the loop.
    if (batch_.dropped_quads() > 0) {
        warn_caps();
    }
}

void Renderer2D::frame_drop() {
    frame_active_ = false;
    camera_stashed_ = false;
    current_perspective_ = false;
    current_layer_ = 0;
    clip_depth_ = 0;
    commands_.clear();
    frame_cameras_.clear();
    batch_.begin_frame();
    last_stats_ = Stats{};  // an abandoned frame renders nothing
}

// --- Renderer2D: guards -----------------------------------------------------

bool Renderer2D::accept(const char* what) {
    if (frame_active_) {
        return true;
    }
    warn_drop(what);
    return false;
}

void Renderer2D::warn_drop(const char* what) {
    if (warned_mask_ & kWarnDrop) {
        return;
    }
    warned_mask_ |= kWarnDrop;
    ws::core::warn("ui: r2d::{} dropped - draws are only valid inside "
                   "on_update(), within a begin()/end() block",
                   what);
}

void Renderer2D::warn_kind(const char* what) {
    if (warned_mask_ & kWarnKindMismatch) {
        return;
    }
    warned_mask_ |= kWarnKindMismatch;
    ws::core::warn("ui: r2d::{} dropped - 3D draws are only valid inside a "
                   "begin(Camera3D) block",
                   what);
}

void Renderer2D::warn_caps() {
    if (warned_mask_ & kWarnCaps) {
        return;
    }
    warned_mask_ |= kWarnCaps;
    // M9: chunks and vertices grow on demand, so the only drop left is a
    // quad pushed with no open segment - push_quad with a segment always
    // appends.
    ws::core::warn("ui: r2d dropped {} quad(s) - a draw with no open "
                   "segment (outside any begin()/end() block); capacity "
                   "itself grows on demand",
                   batch_.dropped_quads());
}

void Renderer2D::warn_unknown_texture(uint32_t id) {
    if (warned_mask_ & kWarnUnknownTexture) {
        return;
    }
    warned_mask_ |= kWarnUnknownTexture;
    ws::core::warn("ui: r2d draw with unknown or destroyed texture id {} - "
                   "rendered white",
                   id);
}

void Renderer2D::warn_sprite(const Sprite& sprite) {
    if (warned_mask_ & kWarnSprite) {
        return;
    }
    warned_mask_ |= kWarnSprite;
    ws::core::warn("ui: r2d sprite (page {}, index {}) is unknown - rendered "
                   "white",
                   sprite.page, sprite.index);
}

// --- Renderer2D: app-facing entry points -----------------------------------

void Renderer2D::stash_camera(const Camera2D& camera) {
    ++current_epoch_;
    current_perspective_ = false;  // this block records the 2D kind
    camera_stashed_ = true;
    // The block's snapshot: its commands replay through
    // frame_cameras_[epoch - 1]. current_epoch_ was just bumped, so the
    // push lands exactly on this epoch's slot.
    frame_cameras_.push_back(CameraSnapshot{camera, Camera3D{}, false});
}

void Renderer2D::stash_camera(const Camera3D& camera) {
    ++current_epoch_;
    current_camera3d_ = camera;
    current_perspective_ = true;  // this block records the perspective kind
    camera_stashed_ = true;
    frame_cameras_.push_back(CameraSnapshot{Camera2D{}, camera, true});
}

void Renderer2D::record(const char* what, const float (&pos)[4][3],
                        const float (&uv)[4][2], std::uint32_t color,
                        Texture texture, float radius) {
    if (!camera_stashed_) {
        warn_drop(what);  // outside any begin()/end() block
        return;
    }
    // Unknown or stale ids render white: the id is generation-counted
    // (gen << kGenShift | slot), so a destroyed handle - or one whose
    // slot a later load reused - fails the generation check exactly
    // like a garbage index, and none of them may reach the descriptor
    // write at flush time (out of bounds, or the wrong image). Clamped
    // here, at record time, before the command exists. Only while a
    // registry exists (init() puts white at index 0 before any frame):
    // headless there is nothing to index and no flush, so synthetic ids
    // pass through untouched - the CPU-side tests rely on exactly that.
    if (!textures_.empty() && texture.id != 0) {
        const std::uint32_t slot = texture.id & kSlotMask;
        const std::uint32_t gen = (texture.id >> kGenShift) & kGenMask;
        if (slot >= textures_.size() ||
            textures_[slot].generation != gen) {
            warn_unknown_texture(texture.id);
            texture = Texture{};
        }
    }
    DrawCommand cmd;
    cmd.sort_layer = current_layer_;
    // The camera rides the epoch: frame_end replays this command through
    // frame_cameras_[camera_epoch - 1], not through a copy of the camera
    // embedded in the command.
    cmd.camera_epoch = current_epoch_;
    cmd.texture = texture;
    cmd.color[0] = color;
    cmd.color[1] = color;
    cmd.color[2] = color;
    cmd.color[3] = color;
    cmd.radius = radius;
    const float* clip = active_clip();
    for (int i = 0; i < 4; ++i) {
        cmd.clip[i] = clip[i];
        cmd.pos[i][0] = pos[i][0];
        cmd.pos[i][1] = pos[i][1];
        cmd.pos[i][2] = pos[i][2];
        cmd.uv[i][0] = uv[i][0];
        cmd.uv[i][1] = uv[i][1];
    }
    commands_.push_back(cmd);
}

void Renderer2D::record_vcol(const char* what, const float (&pos)[4][3],
                             const float (&uv)[4][2], const float (&rgba)[4][4],
                             Texture texture) {
    if (!camera_stashed_) {
        warn_drop(what);  // outside any begin()/end() block
        return;
    }
    // Same generation check as record(): a stale handle must never reach
    // the descriptor write at flush time.
    if (!textures_.empty() && texture.id != 0) {
        const std::uint32_t slot = texture.id & kSlotMask;
        const std::uint32_t gen = (texture.id >> kGenShift) & kGenMask;
        if (slot >= textures_.size() || textures_[slot].generation != gen) {
            warn_unknown_texture(texture.id);
            texture = Texture{};
        }
    }
    DrawCommand cmd;
    cmd.sort_layer = current_layer_;
    cmd.camera_epoch = current_epoch_;
    cmd.texture = texture;
    for (int i = 0; i < 4; ++i) {
        // Clamp then pack: the tessellator's weighted blends may overshoot
        // [0, 1] by an epsilon, and a negative float would pack garbage.
        float r = rgba[i][0];
        float g = rgba[i][1];
        float b = rgba[i][2];
        float a = rgba[i][3];
        r = r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r);
        g = g < 0.0f ? 0.0f : (g > 1.0f ? 1.0f : g);
        b = b < 0.0f ? 0.0f : (b > 1.0f ? 1.0f : b);
        a = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
        cmd.color[i] = (std::uint32_t(r * 255.0f + 0.5f) << 24) |
                       (std::uint32_t(g * 255.0f + 0.5f) << 16) |
                       (std::uint32_t(b * 255.0f + 0.5f) << 8) |
                       std::uint32_t(a * 255.0f + 0.5f);
    }
    const float* clip = active_clip();
    for (int i = 0; i < 4; ++i) {
        cmd.clip[i] = clip[i];
        cmd.pos[i][0] = pos[i][0];
        cmd.pos[i][1] = pos[i][1];
        cmd.pos[i][2] = pos[i][2];
        cmd.uv[i][0] = uv[i][0];
        cmd.uv[i][1] = uv[i][1];
    }
    commands_.push_back(cmd);
}

void Renderer2D::begin_screen() {
    if (!accept("begin")) {
        return;
    }
    // Top-left origin: center the default camera on the extent midpoint.
    const Camera2D screen{fb_width_ * 0.5f, fb_height_ * 0.5f, 0.0f, 1.0f};
    stash_camera(screen);
}

void Renderer2D::begin(const Camera2D& camera) {
    if (!accept("begin")) {
        return;
    }
    stash_camera(camera);
}

void Renderer2D::begin(const Camera3D& camera) {
    if (!accept("begin")) {
        return;
    }
    stash_camera(camera);
}

void Renderer2D::end() {
    if (!accept("end")) {
        return;
    }
    camera_stashed_ = false;  // draws after end() drop until the next begin()
}

void Renderer2D::set_layer(std::uint32_t layer) {
    if (!accept("set_layer")) {
        return;
    }
    current_layer_ = layer;
}

bool Renderer2D::clip_stack_full(int depth) {
    if (depth < kMaxClipDepth) {
        return false;
    }
    if (!clip_overflow_warned_) {
        clip_overflow_warned_ = true;
        ws::core::warn("ui: r2d clip stack clamped at {} - deeper clips stop "
                       "shrinking (push/pop stays balanced)",
                       kMaxClipDepth);
    }
    return true;
}

const float* Renderer2D::active_clip() const {
    if (clip_depth_ == 0) {
        return batch::kFullClip;
    }
    // Past the overflow clamp the slot stops deepening - the same policy
    // as the gui stacks, so overflowed scopes derive from the deepest
    // real clip.
    const int slot = std::min(clip_depth_, kMaxClipDepth) - 1;
    return clip_stack_[slot];
}

void Renderer2D::push_clip(float x, float y, float width, float height) {
    if (!accept("push_clip")) {
        return;
    }
    if (clip_stack_full(clip_depth_)) {
        ++clip_depth_;  // balanced with the matching pop_clip()
        return;
    }
    const float* top = active_clip();
    float* slot = clip_stack_[clip_depth_];
    if (std::isfinite(x) && std::isfinite(y) && std::isfinite(width) &&
        std::isfinite(height)) {
        // Intersect with the active clip: nesting always shrinks. The
        // !(> 0) form normalizes empty, inverted and negative extents to
        // a degenerate rect - the clip-change comparison must never see a
        // NaN, and an empty clip means "render nothing", which is the
        // only safe meaning for such input.
        const float left = std::max(x, top[0]);
        const float upper = std::max(y, top[1]);
        const float right = std::min(x + width, top[0] + top[2]);
        const float lower = std::min(y + height, top[1] + top[3]);
        slot[0] = left;
        slot[1] = upper;
        slot[2] = right > left ? right - left : 0.0f;
        slot[3] = lower > upper ? lower - upper : 0.0f;
    } else {
        slot[0] = 0.0f;
        slot[1] = 0.0f;
        slot[2] = 0.0f;  // non-finite input clips everything
        slot[3] = 0.0f;
    }
    ++clip_depth_;
}

void Renderer2D::pop_clip() {
    if (!accept("pop_clip")) {
        return;
    }
    if (clip_depth_ > 0) {
        --clip_depth_;  // popping an empty stack is the documented no-op
    }
}

void Renderer2D::push_clip_escape(float x, float y, float width,
                                  float height) {
    if (!accept("push_clip_escape")) {
        return;
    }
    if (clip_stack_full(clip_depth_)) {
        ++clip_depth_;  // balanced with the matching pop_clip()
        return;
    }
    float* slot = clip_stack_[clip_depth_];
    if (std::isfinite(x) && std::isfinite(y) && std::isfinite(width) &&
        std::isfinite(height)) {
        // NO intersect with the active clip: the escape REPLACES the top
        // for everything recorded until the matching pop, so an overlay
        // draws past the panel it floats over. The same !(> 0)
        // normalization as push_clip - the clip-change comparison must
        // never see a NaN, and an empty clip means "render nothing".
        slot[0] = x;
        slot[1] = y;
        slot[2] = width > 0.0f ? width : 0.0f;
        slot[3] = height > 0.0f ? height : 0.0f;
    } else {
        slot[0] = 0.0f;
        slot[1] = 0.0f;
        slot[2] = 0.0f;  // non-finite input clips everything
        slot[3] = 0.0f;
    }
    ++clip_depth_;
}

void Renderer2D::draw_quad(float x, float y, float w, float h,
                           std::uint32_t color, Texture texture,
                           float rotation) {
    if (!accept("draw_quad")) {
        return;
    }
    float pos[4][3];
    batch::quad_corners(x, y, w, h, rotation, pos);
    record("draw_quad", pos, kQuadUV, color, texture);
}

void Renderer2D::draw_quad(float x, float y, float z, float w, float h,
                           std::uint32_t color, Texture texture) {
    if (!accept("draw_quad")) {
        return;
    }
    if (!camera_stashed_) {
        warn_drop("draw_quad");  // outside any begin()/end() block
        return;
    }
    if (!current_perspective_) {
        warn_kind("draw_quad");  // a 2D block is open: the wrong kind
        return;
    }
    // Billboard: CENTER-anchored at (x, y, z), w/h world units along the
    // camera's screen axes. The basis is recomputed per draw - it is a
    // handful of cross products, nothing next to the record itself.
    float pos[4][3];
    batch::billboard_corners(x, y, z, w, h,
                             batch::camera_basis(current_camera3d_), pos);
    record("draw_quad", pos, kQuadUV, color, texture);
}

void Renderer2D::draw_line(float x0, float y0, float x1, float y1,
                           float thickness, std::uint32_t color) {
    if (!accept("draw_line")) {
        return;
    }
    float pos[4][3];
    float uv[4][2];
    if (!batch::line_quad(x0, y0, x1, y1, thickness, pos, uv)) {
        return;  // degenerate: live app data, not worth a warning
    }
    record("draw_line", pos, uv, color, Texture{});  // lines are solid color
}

void Renderer2D::draw_vertex_quad(const float (&pos)[4][2],
                                  const float (&uv)[4][2],
                                  const float (&rgba)[4][4], Texture texture) {
    if (!accept("draw_vertex_quad")) {
        return;
    }
    if (!camera_stashed_) {
        warn_drop("draw_vertex_quad");  // outside any begin()/end() block
        return;
    }
    if (current_perspective_) {
        warn_kind("draw_vertex_quad");  // a 3D block is open: 2D-only form
        return;
    }
    // Straight into the per-corner record path; z is written 0 (every 2D
    // draw sits on the world's z=0 plane, quad_corners' rule).
    float pos3[4][3];
    for (int i = 0; i < 4; ++i) {
        pos3[i][0] = pos[i][0];
        pos3[i][1] = pos[i][1];
        pos3[i][2] = 0.0f;
    }
    record_vcol("draw_vertex_quad", pos3, uv, rgba, texture);
}

void Renderer2D::draw_line(float x0, float y0, float z0, float x1, float y1,
                           float z1, float thickness, std::uint32_t color) {
    if (!accept("draw_line")) {
        return;
    }
    if (!camera_stashed_) {
        warn_drop("draw_line");  // outside any begin()/end() block
        return;
    }
    if (!current_perspective_) {
        warn_kind("draw_line");  // a 2D block is open: the wrong kind
        return;
    }
    float pos[4][3];
    float uv[4][2];
    if (!batch::line_quad3d(x0, y0, z0, x1, y1, z1, thickness,
                            batch::camera_basis(current_camera3d_), pos,
                            uv)) {
        return;  // degenerate: live app data, not worth a warning
    }
    record("draw_line", pos, uv, color, Texture{});  // lines are solid color
}

void Renderer2D::draw_rounded_quad(float x, float y, float w, float h,
                                   float radius, std::uint32_t color,
                                   float rotation) {
    if (!accept("draw_rounded_quad")) {
        return;
    }
    if (w <= 0.0f || h <= 0.0f) {
        return;  // degenerate: live app data, not worth a warning
    }
    // Corners cannot overlap: the radius caps at half the shorter side.
    radius = std::min(radius, std::min(w, h) * 0.5f);
    if (radius <= 0.5f) {
        // A sub-pixel radius is an honest square quad - an
        // antialiasing ramp narrower than a pixel buys nothing.
        float pos[4][3];
        batch::quad_corners(x, y, w, h, rotation, pos);
        record("draw_rounded_quad", pos, kQuadUV, color, Texture{});
        return;
    }
    // One quad, shaded by an SDF: uv are the rect's normalized local
    // coordinates (they ride the TL/TR/BR/BL corners, so rotation keeps
    // them rigid), and the radius rides the command for the replay's
    // push_rounded_quad route - the batch packs it into a NEGATIVE
    // tex_index, the never-a-texture discriminator shaders/batch.frag
    // branches on to reconstruct the rounded-box coverage analytically.
    // No mask texture, no nine slices, no upload-failure degradation:
    // headless tests record the same geometry the GPU shades.
    float pos[4][3];
    batch::quad_corners(x, y, w, h, rotation, pos);
    record("draw_rounded_quad", pos, kQuadUV, color, Texture{}, radius);
}

FrameSize Renderer2D::frame_size() const {
    return FrameSize{fb_width_, fb_height_};
}

Texture Renderer2D::load_texture(const char* path) {
    Texture texture{};
    if (device_ == VK_NULL_HANDLE) {
        // No GPU context (headless tests, or a call before init()) - white,
        // not a crash.
        ws::core::warn("ui: r2d::load_texture('{}') fell back to white - no "
                       "GPU context is alive", path);
        return texture;
    }
    int width = 0;
    int height = 0;
    // Force 4 B/px: the registry is R8G8B8A8_UNORM (matches the UNORM
    // swapchain and the ImGui font atlas, so colors render as authored).
    stbi_uc* pixels = stbi_load(path, &width, &height, nullptr, STBI_rgb_alpha);
    if (pixels == nullptr) {
        ws::core::warn("ui: r2d::load_texture('{}') failed - {} (using white)",
                       path, stbi_failure_reason());
        return texture;
    }
    if (!upload_texture(pixels, static_cast<uint32_t>(width),
                       static_cast<uint32_t>(height), &texture)) {
        ws::core::warn("ui: r2d::load_texture('{}') upload failed - using "
                       "white", path);
        texture = Texture{};
    }
    stbi_image_free(pixels);
    return texture;
}

Texture Renderer2D::load_texture_from_memory(const void* data,
                                             std::size_t size) {
    Texture texture{};
    if (device_ == VK_NULL_HANDLE) {
        // No GPU context (headless tests, or a call before init()) -
        // the load_texture contract: white, not a crash.
        ws::core::warn("ui: r2d::load_texture_from_memory() fell back to "
                       "white - no GPU context is alive");
        return texture;
    }
    if (data == nullptr || size == 0 ||
        size > static_cast<std::size_t>(INT_MAX)) {
        // stb's length parameter is an int; anything past INT_MAX would
        // wrap it, not size it.
        ws::core::warn("ui: r2d::load_texture_from_memory() failed - {} "
                       "bytes is not decodable image data (using white)",
                       size);
        return texture;
    }
    int width = 0;
    int height = 0;
    // Force 4 B/px: the registry is R8G8B8A8_UNORM (matches the UNORM
    // swapchain and the ImGui font atlas, so colors render as authored).
    stbi_uc* pixels = stbi_load_from_memory(
        static_cast<const stbi_uc*>(data), static_cast<int>(size), &width,
        &height, nullptr, STBI_rgb_alpha);
    if (pixels == nullptr) {
        ws::core::warn("ui: r2d::load_texture_from_memory() failed - {} "
                       "(using white)",
                       stbi_failure_reason());
        return texture;
    }
    if (!upload_texture(pixels, static_cast<uint32_t>(width),
                        static_cast<uint32_t>(height), &texture)) {
        ws::core::warn("ui: r2d::load_texture_from_memory() upload failed "
                       "- using white");
        texture = Texture{};
    }
    stbi_image_free(pixels);
    return texture;
}

void Renderer2D::destroy_texture(Texture texture) {
    if (device_ == VK_NULL_HANDLE) {
        // No GPU context: nothing was ever uploaded through this handle -
        // the loaders' warn-and-degrade contract.
        ws::core::warn("ui: r2d::destroy_texture({}) dropped - no GPU "
                       "context is alive",
                       texture.id);
        return;
    }
    if (texture.id == 0) {
        ws::core::warn("ui: r2d::destroy_texture(0) refused - the white "
                       "fallback is never destroyed");
        return;
    }
    const std::uint32_t slot = texture.id & kSlotMask;
    const std::uint32_t gen = (texture.id >> kGenShift) & kGenMask;
    if (slot >= textures_.size() || textures_[slot].generation != gen) {
        // Unknown, already destroyed, or its slot reused by a later load:
        // a warned no-op either way. Cold (load-granularity) path, so the
        // warn needs no once-per-frame guard.
        ws::core::warn("ui: r2d::destroy_texture({}) dropped - unknown or "
                       "already destroyed texture id",
                       texture.id);
        return;
    }
    // The generation bump is what kills the handle: record() fails every
    // later draw with it, so the frame this call runs in is the LAST
    // that can hold the image. frame_index_ is the SUBMISSION clock
    // (flush() bumps it; a frame that drops never submits and never
    // counts), so at this moment the recording frame - if it renders -
    // is submit number frame_index_ + 1: the stamp buys TWO more submits
    // of slack, and the flush that reaches it has, at entry, already
    // waited the fence the submit kMaxFramesInFlight back signaled (and
    // by the acquire chain, every older one) - which covers this frame
    // and everything before it. A dropped frame recorded nothing the
    // batch replayed, so its absence only delays the free, never
    // unsafens it.
    textures_[slot].generation =
        (textures_[slot].generation + 1) & kGenMask;
    retire_queue_.push_back(
        RetireEntry{slot, frame_index_ + kMaxFramesInFlight + 1});
}

void Renderer2D::register_atlas_page(Texture page,
                                     std::vector<atlas::Rect> rects) {
    if (page.id == kNoAtlasPage) {
        // The sentinel itself can never be a map key (id + 1 would wrap).
        // Unreachable from production - ids are registry indexes, and the
        // bindless tier caps them far below the sentinel - so this is
        // garbage armor, not a path any real caller can take.
        ws::core::warn("ui: r2d::register_atlas_page refused texture id {} "
                       "- its sprites render white",
                       page.id);
        return;
    }
    const std::uint32_t index =
        static_cast<std::uint32_t>(atlas_pages_.size());
    atlas_pages_.push_back(AtlasPage{page, std::move(rects)});
    // The dense map is keyed by the page's SLOT, so a freed slot a later
    // page reuses rebinds to the new tenant; the stored page keeps the
    // FULL id, and that is what find_sprite_rect compares, so a stale
    // handle never resolves to the slot's new page. Fresh entries are
    // the sentinel (not a page); synthetic test ids land here
    // untouched, registry or not.
    const std::uint32_t slot = page.id & kSlotMask;
    if (page_of_texture_.size() <= slot) {
        page_of_texture_.resize(slot + 1, kNoAtlasPage);
    }
    page_of_texture_[slot] = index;
}

Texture Renderer2D::upload_atlas_page(const void* pixels, std::uint32_t size,
                                      std::vector<atlas::Rect> rects) {
    // Headless/pre-init or a failed staging: Texture{} (id 0), and the
    // caller's degraded path takes over - load_atlas hands white sprites,
    // the lazy bake keeps page-0 glyph quads that drop but still advance.
    if (device_ == VK_NULL_HANDLE) {
        return Texture{};
    }
    Texture texture{};
    if (!upload_texture(pixels, size, size, &texture)) {
        return Texture{};
    }
    register_atlas_page(texture, std::move(rects));
    return texture;
}

const atlas::Rect* Renderer2D::find_sprite_rect(const Sprite& sprite) const {
    // O(1): the dense map carries SLOT -> atlas_pages_ index. This runs
    // once per sprite and once per glyph in draw_text, so it must not
    // scale with the page count. Slots past the map = never registered
    // as a page.
    const std::uint32_t slot = sprite.page & kSlotMask;
    if (slot >= page_of_texture_.size()) {
        return nullptr;  // not a registered atlas page
    }
    const std::uint32_t index = page_of_texture_[slot];
    if (index == kNoAtlasPage) {
        return nullptr;  // not a page - or a page whose slot was freed
    }
    const AtlasPage& page = atlas_pages_[index];
    // Full-id equality: a destroyed page, or a stale handle whose slot a
    // later page reused, must not resolve to whatever the slot holds now.
    if (page.texture.id != sprite.page) {
        return nullptr;  // the handle predates the page at this slot
    }
    if (sprite.index >= page.rects.size()) {
        return nullptr;  // the right page, but no such sprite
    }
    return &page.rects[sprite.index];
}

void Renderer2D::draw_sprite(float x, float y, float w, float h,
                             std::uint32_t color, Sprite sprite,
                             float rotation) {
    if (!accept("draw_sprite")) {
        return;
    }
    float pos[4][3];
    batch::quad_corners(x, y, w, h, rotation, pos);
    // Page 0 is the white sentinel (failed loads degrade to it) - it draws
    // silently. Any other unresolvable sprite is an app bug worth one warn.
    const atlas::Rect* rect =
        sprite.page == 0 ? nullptr : find_sprite_rect(sprite);
    if (rect != nullptr) {
        const float uv[4][2] = {
            {rect->u0, rect->v0},  // TL
            {rect->u1, rect->v0},  // TR
            {rect->u1, rect->v1},  // BR
            {rect->u0, rect->v1},  // BL
        };
        record("draw_sprite", pos, uv, color, Texture{sprite.page});
    } else {
        if (sprite.page != 0) {
            warn_sprite(sprite);
        }
        record("draw_sprite", pos, kQuadUV, color, Texture{});
    }
}

void Renderer2D::draw_sprite(float x, float y, float z, float w, float h,
                             std::uint32_t color, Sprite sprite) {
    if (!accept("draw_sprite")) {
        return;
    }
    if (!camera_stashed_) {
        warn_drop("draw_sprite");  // outside any begin()/end() block
        return;
    }
    if (!current_perspective_) {
        warn_kind("draw_sprite");  // a 2D block is open: the wrong kind
        return;
    }
    float pos[4][3];
    batch::billboard_corners(x, y, z, w, h,
                             batch::camera_basis(current_camera3d_), pos);
    // Rect resolution is the 2D body's, verbatim - only the corners differ
    // (billboard, center-anchored). Page 0 is the white sentinel (failed
    // loads degrade to it) - it draws silently. Any other unresolvable
    // sprite is an app bug worth one warn.
    const atlas::Rect* rect =
        sprite.page == 0 ? nullptr : find_sprite_rect(sprite);
    if (rect != nullptr) {
        const float uv[4][2] = {
            {rect->u0, rect->v0},  // TL
            {rect->u1, rect->v0},  // TR
            {rect->u1, rect->v1},  // BR
            {rect->u0, rect->v1},  // BL
        };
        record("draw_sprite", pos, uv, color, Texture{sprite.page});
    } else {
        if (sprite.page != 0) {
            warn_sprite(sprite);
        }
        record("draw_sprite", pos, kQuadUV, color, Texture{});
    }
}

FrameSize Renderer2D::sprite_size(Sprite sprite) const {
    const atlas::Rect* rect =
        sprite.page == 0 ? nullptr : find_sprite_rect(sprite);
    if (rect == nullptr) {
        return FrameSize{};
    }
    return FrameSize{static_cast<float>(rect->w), static_cast<float>(rect->h)};
}

float Renderer2D::draw_text(float x, float y, const char* utf8_text,
                            const Font& font, std::uint32_t color) {
    if (utf8_text == nullptr) {
        return 0.0f;  // measure_text(nullptr) reports the same nothing
    }
    // Text is just more sprites: one quad per visible glyph through the
    // draw_sprite path, so a string carries the caller's layer, clip and
    // camera block, and its glyphs land in one batch run (one page texture
    // = one texture-table slot). (x, y) is the LINE BOX's top-left - the
    // box widgets center on their rows; glyph offsets already reach down
    // to the baseline from there. No early-outs on the font's data: every
    // glyph the font answers advances (visible or not), nothing else does
    // - so the return is structurally measure_text's exact value, whatever
    // the font.
    //
    // Whole-pixel snap: on an axis-aligned, zoom-1 2D camera - the gui's
    // screen mode - each glyph's quad origin rounds onto the pixel grid,
    // so the baked texels land texel-on-pixel and the text reads crisp.
    // The PEN keeps its exact fractional advance; each glyph rounds its
    // own box, so spacing stays the bake's, and one bake's shared
    // fractional ascent (plus integer bitmap boxes) keeps the per-glyph
    // Y rounds mutually aligned. A rotated or zoomed camera, or a 3D
    // block, skips the snap: fractional positions are the truth there,
    // and sub-pixel camera motion must stay smooth. Camera pan is not a
    // gate - a fractional pan shifts every glyph by one common phase;
    // the snap kills per-glyph jitter, not uniform motion.
    bool snap = false;
    if (camera_stashed_ && !current_perspective_) {
        const Camera2D& cam = frame_cameras_[current_epoch_ - 1].camera;
        snap = cam.rotation == 0.0f && cam.zoom == 1.0f;
    }
    const char* s = utf8_text;
    float pen_x = x;
    for (std::uint32_t cp = font::utf8_next(s); cp != 0u;
         cp = font::utf8_next(s)) {
        const Font::Glyph* g = font.glyph(cp);
        if (g == nullptr) {
            continue;  // the controls (cp < 32): no draw, no advance
        }
        // sprite.page 0 = a page that failed to upload: a white box would
        // read as a glyph-shaped artifact, so the quad drops - the advance
        // stays honest, measure_text never counted the bitmap anyway.
        if (g->width > 0.0f && g->height > 0.0f && g->sprite.page != 0) {
            const float gx = pen_x + g->offset_x;
            const float gy = y + g->offset_y;
            draw_sprite(snap ? std::roundf(gx) : gx,
                        snap ? std::roundf(gy) : gy, g->width, g->height,
                        color, g->sprite);
        }
        pen_x += g->advance;
    }
    return pen_x - x;
}

std::vector<Sprite> Renderer2D::load_atlas(
    std::initializer_list<const char*> paths) {
    const std::size_t count = paths.size();
    std::vector<Sprite> sprites(count);  // failures keep Sprite{} (white)
    if (device_ == VK_NULL_HANDLE) {
        // Headless or pre-init: the load_texture contract - white, no crash.
        ws::core::warn("ui: r2d::load_atlas() fell back to white sprites - "
                       "no GPU context is alive");
        return sprites;
    }

    // Load everything first (pure CPU), pack (which copies), then the stb
    // buffers can go. Each page uploads once, afterwards.
    std::vector<atlas::SourceImage> sources(count);
    std::vector<stbi_uc*> stbi_pixels(count, nullptr);
    std::size_t i = 0;
    for (const char* path : paths) {
        int width = 0;
        int height = 0;
        stbi_pixels[i] =
            stbi_load(path, &width, &height, nullptr, STBI_rgb_alpha);
        if (stbi_pixels[i] != nullptr) {
            sources[i] = {stbi_pixels[i], static_cast<std::uint32_t>(width),
                          static_cast<std::uint32_t>(height)};
        } else {
            ws::core::warn("ui: r2d::load_atlas('{}') failed - {} (white "
                           "sprite)",
                           path, stbi_failure_reason());
        }
        ++i;
    }
    atlas::Packing packed = atlas::pack(sources.data(), count);
    for (stbi_uc* pixels : stbi_pixels) {
        if (pixels != nullptr) {
            stbi_image_free(pixels);
        }
    }

    // A failed page upload degrades only its sprites, not the whole atlas.
    std::vector<std::uint32_t> page_ids(packed.pages.size(), 0);
    for (std::size_t p = 0; p < packed.pages.size(); ++p) {
        atlas::Page& page = packed.pages[p];
        const Texture texture = upload_atlas_page(
            page.pixels.data(), page.size, std::move(page.rects));
        page_ids[p] = texture.id;
    }
    for (std::size_t s = 0; s < count; ++s) {
        const atlas::Placement& place = packed.assigned[s];
        if (place.page < page_ids.size() && page_ids[place.page] != 0) {
            sprites[s] = Sprite{page_ids[place.page], place.index};
        }
    }
    return sprites;
}

Font Renderer2D::load_font(const char* path, float pixel_height) {
    if (device_ == VK_NULL_HANDLE) {
        // Headless or pre-init: the loaders' contract - warn, degrade to
        // empty (draw_text then draws nothing, measure_text reports 0).
        ws::core::warn("ui: r2d::load_font('{}') fell back to an empty font "
                       "- no GPU context is alive",
                       path != nullptr ? path : "(null)");
        return {};
    }

    // Bake first (pure CPU), then the same page pipeline as load_atlas.
    // The font gets its OWN pages: atlas::pack is one-shot (a packed
    // atlas never grows), so glyph sprites can never share a page with
    // icon sprites - two Font instances bake two page sets, by design.
    const font::BakedFont baked = font::bake_file(path, pixel_height);
    if (!baked.ok) {
        ws::core::warn("ui: r2d::load_font('{}') failed - empty font",
                       path != nullptr ? path : "(null)");
        return {};
    }

    // Every glyph's metrics first (advances and offsets need no atlas);
    // sprites patch in after packing. Only glyphs WITH bitmaps feed the
    // packer - the space and other blanks advance but never draw, and a
    // blank cell would only waste page room. The baked fallback box is
    // a glyph like any other here: it rides the pages with the rest.
    std::vector<Font::Glyph> glyphs(baked.glyphs.size());
    std::vector<atlas::SourceImage> sources;
    std::vector<std::size_t> baked_index;  // sources[i] -> baked.glyphs slot
    sources.reserve(baked.glyphs.size());
    for (std::size_t i = 0; i < baked.glyphs.size(); ++i) {
        const font::BakedGlyph& g = baked.glyphs[i];
        Font::Glyph& out = glyphs[i];
        out.advance = g.advance;
        out.offset_x = g.offset_x;
        out.offset_y = g.offset_y;
        out.width = static_cast<float>(g.w);
        out.height = static_cast<float>(g.h);
        if (g.w > 0 && g.h > 0) {
            sources.push_back(atlas::SourceImage{g.rgba.data(), g.w, g.h});
            baked_index.push_back(i);
        }
    }

    if (!sources.empty()) {
        atlas::Packing packed = atlas::pack(sources.data(), sources.size());
        // A failed page upload degrades only its glyphs, like load_atlas.
        std::vector<std::uint32_t> page_ids(packed.pages.size(), 0);
        for (std::size_t p = 0; p < packed.pages.size(); ++p) {
            atlas::Page& page = packed.pages[p];
            const Texture texture = upload_atlas_page(
                page.pixels.data(), page.size, std::move(page.rects));
            page_ids[p] = texture.id;
        }
        for (std::size_t s = 0; s < baked_index.size(); ++s) {
            const atlas::Placement& place = packed.assigned[s];
            if (place.page < page_ids.size() && page_ids[place.page] != 0) {
                glyphs[baked_index[s]].sprite =
                    Sprite{page_ids[place.page], place.index};
            }
        }
    }
    Font font(baked.pixel_height, baked.line_height, glyphs.data(),
              static_cast<std::uint32_t>(glyphs.size()));
    // The growing half's baseline comes from the primary bake's own
    // ascent - the number a stacked face's glyphs fold against, so its
    // lazy bakes sit on the same baseline the inline glyphs drew on.
    font.extras_state()->baseline_y = baked.ascent_px;
    font.extras_state()->baseline_known = true;
    return font;
}

// --- public free functions ----------------------------------------------------

namespace {

Renderer2D* g_active = nullptr;

// No app is running at all (or run() has returned): warn once per process -
// this is an app bug, not something to spam per call.
void warn_no_app(const char* what) {
    static bool warned = false;
    if (warned) {
        return;
    }
    warned = true;
    ws::core::warn("ui: r2d::{} dropped - no app is running (the 2D API only "
                   "draws while App::run() is active)", what);
}

}  // namespace

void set_active(Renderer2D* renderer) {
    g_active = renderer;
}

void begin() {
    if (g_active != nullptr) {
        g_active->begin_screen();
    } else {
        warn_no_app("begin");
    }
}

void begin(const Camera2D& camera) {
    if (g_active != nullptr) {
        g_active->begin(camera);
    } else {
        warn_no_app("begin");
    }
}

void begin(const Camera3D& camera) {
    if (g_active != nullptr) {
        g_active->begin(camera);
    } else {
        warn_no_app("begin");
    }
}

void end() {
    if (g_active != nullptr) {
        g_active->end();
    } else {
        warn_no_app("end");
    }
}

void set_layer(std::uint32_t layer) {
    if (g_active != nullptr) {
        g_active->set_layer(layer);
    } else {
        warn_no_app("set_layer");
    }
}

void push_clip(float x, float y, float width, float height) {
    if (g_active != nullptr) {
        g_active->push_clip(x, y, width, height);
    } else {
        warn_no_app("push_clip");
    }
}

void pop_clip() {
    if (g_active != nullptr) {
        g_active->pop_clip();
    } else {
        warn_no_app("pop_clip");
    }
}

void push_clip_escape(float x, float y, float width, float height) {
    if (g_active != nullptr) {
        g_active->push_clip_escape(x, y, width, height);
    } else {
        warn_no_app("push_clip_escape");
    }
}

void draw_quad(float x, float y, float w, float h, std::uint32_t color,
               Texture texture, float rotation) {
    if (g_active != nullptr) {
        g_active->draw_quad(x, y, w, h, color, texture, rotation);
    } else {
        warn_no_app("draw_quad");
    }
}

void draw_quad(float x, float y, float z, float w, float h,
               std::uint32_t color, Texture texture) {
    if (g_active != nullptr) {
        g_active->draw_quad(x, y, z, w, h, color, texture);
    } else {
        warn_no_app("draw_quad");
    }
}

void draw_line(float x0, float y0, float x1, float y1, float thickness,
               std::uint32_t color) {
    if (g_active != nullptr) {
        g_active->draw_line(x0, y0, x1, y1, thickness, color);
    } else {
        warn_no_app("draw_line");
    }
}

void draw_vertex_quad(const float (&pos)[4][2], const float (&uv)[4][2],
                      const float (&rgba)[4][4], Texture texture) {
    if (g_active != nullptr) {
        g_active->draw_vertex_quad(pos, uv, rgba, texture);
    } else {
        warn_no_app("draw_vertex_quad");
    }
}

void draw_line(float x0, float y0, float z0, float x1, float y1, float z1,
               float thickness, std::uint32_t color) {
    if (g_active != nullptr) {
        g_active->draw_line(x0, y0, z0, x1, y1, z1, thickness, color);
    } else {
        warn_no_app("draw_line");
    }
}

void draw_rounded_quad(float x, float y, float w, float h, float radius,
                       std::uint32_t color, float rotation) {
    if (g_active != nullptr) {
        g_active->draw_rounded_quad(x, y, w, h, radius, color, rotation);
    } else {
        warn_no_app("draw_rounded_quad");
    }
}

FrameSize frame_size() {
    if (g_active != nullptr) {
        return g_active->frame_size();
    }
    return FrameSize{};
}

Texture load_texture(const char* path) {
    if (g_active != nullptr) {
        return g_active->load_texture(path);
    }
    warn_no_app("load_texture");
    return Texture{};
}

Texture load_texture_from_memory(const void* data, std::size_t size) {
    if (g_active != nullptr) {
        return g_active->load_texture_from_memory(data, size);
    }
    warn_no_app("load_texture_from_memory");
    return Texture{};
}

void destroy_texture(Texture texture) {
    if (g_active != nullptr) {
        g_active->destroy_texture(texture);
    } else {
        warn_no_app("destroy_texture");
    }
}

std::vector<Sprite> load_atlas(std::initializer_list<const char*> paths) {
    if (g_active != nullptr) {
        return g_active->load_atlas(paths);
    }
    warn_no_app("load_atlas");
    return {};
}

void draw_sprite(float x, float y, float w, float h, std::uint32_t color,
                 Sprite sprite, float rotation) {
    if (g_active != nullptr) {
        g_active->draw_sprite(x, y, w, h, color, sprite, rotation);
    } else {
        warn_no_app("draw_sprite");
    }
}

void draw_sprite(float x, float y, float z, float w, float h,
                 std::uint32_t color, Sprite sprite) {
    if (g_active != nullptr) {
        g_active->draw_sprite(x, y, z, w, h, color, sprite);
    } else {
        warn_no_app("draw_sprite");
    }
}

FrameSize sprite_size(Sprite sprite) {
    if (g_active != nullptr) {
        return g_active->sprite_size(sprite);
    }
    return FrameSize{};
}

Font load_font(const char* path, float pixel_height) {
    if (g_active != nullptr) {
        return g_active->load_font(path, pixel_height);
    }
    warn_no_app("load_font");
    return {};
}

std::uint32_t font_bake_pending(Font& font, std::uint32_t batch_cap) {
    // Deliberately NOT the warn_no_app shape: baking fills the font, and
    // the metrics half is real work without a renderer (tests, and any
    // pre-first-frame call) - only the page upload waits for one, by
    // leaving the sprites at page 0 (draw_text drops the quad, the
    // advance stays honest). The public header spells this contract.
    font::FontExtra* extra = font.extras_state();
    if (extra->pending.empty() || batch_cap == 0u) {
        return 0u;
    }
    const std::size_t take =
        std::min<std::size_t>(batch_cap, extra->pending.size());

    // Bake the batch (pure CPU), keeping only meaningful glyphs: one with
    // a bitmap or a real advance. Anything else - a face that stopped
    // carrying its pending codepoint, a degenerate raster - leaves the
    // queue without an entry, and the fallback box keeps answering it.
    std::vector<std::uint32_t> cps;
    std::vector<font::BakedGlyph> baked;
    cps.reserve(take);
    baked.reserve(take);
    for (std::size_t i = 0; i < take; ++i) {
        const std::uint32_t cp = extra->pending[i];
        const font::FaceBake* face = extra->resolve(cp);
        if (face == nullptr) {
            continue;
        }
        font::BakedGlyph g =
            font::bake_face_glyph(*face, cp, extra->baseline_y);
        if (g.w > 0u || g.advance > 0.0f) {
            cps.push_back(cp);
            baked.push_back(std::move(g));
        }
    }
    // The whole consumed prefix leaves the queue, baked or dropped.
    for (std::size_t i = 0; i < take; ++i) {
        extra->pending_set.erase(extra->pending[i]);
    }
    extra->pending.erase(
        extra->pending.begin(),
        extra->pending.begin() + static_cast<std::ptrdiff_t>(take));
    if (cps.empty()) {
        return 0u;
    }

    // Metrics first, sprites patch in after packing - load_font's shape.
    // Only glyphs WITH bitmaps feed the packer; blanks (the space-likes)
    // advance without pages.
    std::vector<Font::Glyph> glyphs(cps.size());
    std::vector<atlas::SourceImage> sources;
    std::vector<std::size_t> baked_index;  // sources[i] -> cps/baked slot
    sources.reserve(cps.size());
    for (std::size_t i = 0; i < baked.size(); ++i) {
        const font::BakedGlyph& g = baked[i];
        Font::Glyph& out = glyphs[i];
        out.advance = g.advance;
        out.offset_x = g.offset_x;
        out.offset_y = g.offset_y;
        out.width = static_cast<float>(g.w);
        out.height = static_cast<float>(g.h);
        if (g.w > 0u && g.h > 0u) {
            sources.push_back(atlas::SourceImage{g.rgba.data(), g.w, g.h});
            baked_index.push_back(i);
        }
    }

    if (g_active != nullptr && !sources.empty()) {
        // One pack for the whole batch, fresh pages per pack - atlas::pack
        // is one-shot, so each batch grows the font by its own page set.
        // Headless skips the pack entirely: sprites were only ever the
        // upload's business, and the metrics are already in place.
        atlas::Packing packed = atlas::pack(sources.data(), sources.size());
        std::vector<std::uint32_t> page_ids(packed.pages.size(), 0u);
        for (std::size_t p = 0; p < packed.pages.size(); ++p) {
            atlas::Page& page = packed.pages[p];
            const Texture texture = g_active->upload_atlas_page(
                page.pixels.data(), page.size, std::move(page.rects));
            page_ids[p] = texture.id;
        }
        for (std::size_t s = 0; s < baked_index.size(); ++s) {
            const atlas::Placement& place = packed.assigned[s];
            if (place.page < page_ids.size() && page_ids[place.page] != 0u) {
                glyphs[baked_index[s]].sprite =
                    Sprite{page_ids[place.page], place.index};
            }
        }
    }
    for (std::size_t i = 0; i < cps.size(); ++i) {
        extra->glyphs.insert_or_assign(cps[i], glyphs[i]);
    }
    return static_cast<std::uint32_t>(cps.size());
}

float draw_text(float x, float y, const char* utf8_text, const Font& font,
                std::uint32_t color) {
    if (g_active != nullptr) {
        return g_active->draw_text(x, y, utf8_text, font, color);
    }
    // No app: the draw drops (warned once, like every call here), but the
    // width is still the honest measure - layout code keeps working.
    warn_no_app("draw_text");
    return font.measure_text(utf8_text);
}

Stats stats() {
    if (g_active != nullptr) {
        return g_active->stats();
    }
    // No warn - the frame_size()/sprite_size() contract: a query with no
    // app running is a zero answer, not an app bug.
    return Stats{};
}

}  // namespace ws::ui::r2d
