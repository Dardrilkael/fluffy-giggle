#include "batch_core.h"

#include <cmath>

namespace ws::ui::r2d::batch {

Mat4 camera_vp(const Camera2D& camera, float frame_width, float frame_height) {
    const float zoom = camera.zoom > 1e-4f ? camera.zoom : 1e-4f;
    const float half_w = (frame_width > 0.0f ? frame_width : 1.0f) * 0.5f / zoom;
    const float half_h = (frame_height > 0.0f ? frame_height : 1.0f) * 0.5f / zoom;
    const float c = std::cos(camera.rotation);
    const float s = std::sin(camera.rotation);

    // clip.x = ( c*(px-cx) + s*(py-cy) ) / half_w
    // clip.y = ( -s*(px-cx) + c*(py-cy) ) / half_h
    // (positive rotation rolls the view clockwise on screen; +y maps down)
    Mat4 vp{};
    vp.m[0] = c / half_w;
    vp.m[1] = -s / half_h;
    vp.m[4] = s / half_w;
    vp.m[5] = c / half_h;
    vp.m[10] = 1.0f;
    vp.m[12] = -(c * camera.x + s * camera.y) / half_w;
    vp.m[13] = (s * camera.x - c * camera.y) / half_h;
    vp.m[15] = 1.0f;
    return vp;
}

void unpack_rgba(std::uint32_t color, float out[4]) {
    out[0] = static_cast<float>((color >> 24) & 0xFFu) / 255.0f;
    out[1] = static_cast<float>((color >> 16) & 0xFFu) / 255.0f;
    out[2] = static_cast<float>((color >> 8) & 0xFFu) / 255.0f;
    out[3] = static_cast<float>(color & 0xFFu) / 255.0f;
}

void quad_corners(float x, float y, float w, float h, float rotation,
                  float out[4][2]) {
    quad_corners_pivot(x, y, w, h, x + w * 0.5f, y + h * 0.5f, rotation, out);
}

void quad_corners_pivot(float x, float y, float w, float h, float pivot_x,
                        float pivot_y, float rotation, float out[4][2]) {
    const float c = std::cos(rotation);
    const float s = std::sin(rotation);

    // Slice center relative to the pivot; the corners hang off that.
    const float mx = x + w * 0.5f - pivot_x;
    const float my = y + h * 0.5f - pivot_y;
    const float local[4][2] = {
        {mx - w * 0.5f, my - h * 0.5f},  // TL
        {mx + w * 0.5f, my - h * 0.5f},  // TR
        {mx + w * 0.5f, my + h * 0.5f},  // BR
        {mx - w * 0.5f, my + h * 0.5f},  // BL
    };
    for (int i = 0; i < 4; ++i) {
        out[i][0] = pivot_x + c * local[i][0] - s * local[i][1];
        out[i][1] = pivot_y + s * local[i][0] + c * local[i][1];
    }
}

bool line_quad(float x0, float y0, float x1, float y1, float thickness,
               float out_pos[4][2], float out_uv[4][2]) {
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float len2 = dx * dx + dy * dy;
    if (len2 <= 1e-12f || thickness <= 0.0f) {
        return false;
    }
    const float inv_len = 1.0f / std::sqrt(len2);
    const float ox = -dy * inv_len * thickness * 0.5f;  // + half normal
    const float oy = dx * inv_len * thickness * 0.5f;

    // A,B at p0 and C,D at p1 - a plain quad loop, no self-intersection.
    out_pos[0][0] = x0 + ox; out_pos[0][1] = y0 + oy;
    out_pos[1][0] = x0 - ox; out_pos[1][1] = y0 - oy;
    out_pos[2][0] = x1 - ox; out_pos[2][1] = y1 - oy;
    out_pos[3][0] = x1 + ox; out_pos[3][1] = y1 + oy;
    out_uv[0][0] = 0.0f; out_uv[0][1] = 0.0f;
    out_uv[1][0] = 0.0f; out_uv[1][1] = 1.0f;
    out_uv[2][0] = 1.0f; out_uv[2][1] = 1.0f;
    out_uv[3][0] = 1.0f; out_uv[3][1] = 0.0f;
    return true;
}

bool BatchCore::start_chunk(const Mat4& vp, const float (&clip)[4]) {
    if (chunks_.size() >= kMaxChunksPerFrame) {
        return false;
    }
    Chunk chunk;
    chunk.vp = vp;
    for (int i = 0; i < 4; ++i) {
        chunk.clip[i] = clip[i];
    }
    chunk.vertex_offset = static_cast<std::uint32_t>(vertices_.size());
    chunks_.push_back(chunk);
    return true;
}

void BatchCore::new_frame() {
    vertices_.clear();
    chunks_.clear();
    segment_open_ = false;
}

bool BatchCore::open_segment(const Mat4& vp, const float (&clip)[4]) {
    segment_open_ = start_chunk(vp, clip);
    return segment_open_;
}

void BatchCore::close_segment() {
    segment_open_ = false;
}

std::uint32_t BatchCore::texture_slot(std::uint32_t texture_id) {
    Chunk& chunk = chunks_.back();
    for (std::uint32_t i = 0; i < chunk.texture_count; ++i) {
        if (chunk.texture_ids[i] == texture_id) {
            return i;
        }
    }
    if (chunk.texture_count < kMaxTexturesPerChunk) {
        const std::uint32_t slot = chunk.texture_count++;
        chunk.texture_ids[slot] = texture_id;
        return slot;
    }
    // Table full: split. The stale chunk keeps its vertices; the fresh one
    // continues right where it left off, so flush order is unchanged. The
    // split inherits the segment's VP and clip - it is the same segment.
    const Mat4 vp = chunk.vp;
    if (!start_chunk(vp, chunk.clip)) {
        return kMaxTexturesPerChunk;
    }
    chunks_.back().texture_ids[0] = texture_id;
    chunks_.back().texture_count = 1;
    return 0;
}

bool BatchCore::push_quad(const float (&pos)[4][2], const float (&uv)[4][2],
                          const float (&color)[4], std::uint32_t texture_id) {
    if (!segment_open_ || chunks_.empty()) {
        return false;
    }
    if (vertices_.size() + 4 > kMaxVerticesPerFrame) {
        return false;
    }
    const std::uint32_t slot = texture_slot(texture_id);
    if (slot >= kMaxTexturesPerChunk) {
        return false;
    }
    for (int i = 0; i < 4; ++i) {
        Vertex v;
        v.pos[0] = pos[i][0];
        v.pos[1] = pos[i][1];
        v.color[0] = color[0];
        v.color[1] = color[1];
        v.color[2] = color[2];
        v.color[3] = color[3];
        v.uv[0] = uv[i][0];
        v.uv[1] = uv[i][1];
        v.tex_index = static_cast<float>(slot);
        vertices_.push_back(v);
    }
    ++chunks_.back().quad_count;
    return true;
}

}  // namespace ws::ui::r2d::batch
