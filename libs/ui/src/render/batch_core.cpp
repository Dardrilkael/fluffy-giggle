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
                  float out[4][3]) {
    quad_corners_pivot(x, y, w, h, x + w * 0.5f, y + h * 0.5f, rotation, out);
}

void quad_corners_pivot(float x, float y, float w, float h, float pivot_x,
                        float pivot_y, float rotation, float out[4][3]) {
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
        out[i][2] = 0.0f;  // 2D draws sit on the world's z=0 plane
    }
}

bool line_quad(float x0, float y0, float x1, float y1, float thickness,
               float out_pos[4][3], float out_uv[4][2]) {
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
    for (int i = 0; i < 4; ++i) {
        out_pos[i][2] = 0.0f;  // 2D draws sit on the world's z=0 plane
    }
    out_uv[0][0] = 0.0f; out_uv[0][1] = 0.0f;
    out_uv[1][0] = 0.0f; out_uv[1][1] = 1.0f;
    out_uv[2][0] = 1.0f; out_uv[2][1] = 1.0f;
    out_uv[3][0] = 1.0f; out_uv[3][1] = 0.0f;
    return true;
}

// --- 3D camera (perspective) ----------------------------------------------------

CamBasis camera_basis(const Camera3D& camera) {
    using ws::math::Vec3;
    // Degenerate checks in the "not greater than" form: false for NaN too, so
    // no NaN input can produce a NaN basis (a NaN matrix would poison the
    // whole batch, camera_vp's contract). ws::math::normalize is undefined
    // for zero-length input - these guards are what make every normalize
    // below safe.
    Vec3 forward{camera.target[0] - camera.eye[0],
                 camera.target[1] - camera.eye[1],
                 camera.target[2] - camera.eye[2]};
    if (!(ws::math::dot(forward, forward) > 1e-12f)) {
        forward = Vec3{0.0f, 0.0f, 1.0f};
    }
    forward = ws::math::normalize(forward);

    // A unit forward cannot be parallel to both fallback axes, so the chain
    // always finds a usable right (the up hint may be zero, NaN, or parallel
    // to the view).
    const Vec3 hint{camera.up[0], camera.up[1], camera.up[2]};
    Vec3 right = ws::math::cross(forward, hint);
    if (!(ws::math::dot(right, right) > 1e-12f)) {
        right = ws::math::cross(forward, Vec3{0.0f, 0.0f, 1.0f});
    }
    if (!(ws::math::dot(right, right) > 1e-12f)) {
        right = ws::math::cross(forward, Vec3{0.0f, 1.0f, 0.0f});
    }
    right = ws::math::normalize(right);

    // up is unit by construction (right and forward are unit and
    // perpendicular) - the hint itself is never normalized; an unnormalized
    // hint just bends the basis until it is orthonormal again.
    return CamBasis{right, ws::math::cross(right, forward), forward};
}

Mat4 camera_vp3d(const Camera3D& camera, float frame_width, float frame_height) {
    const CamBasis b = camera_basis(camera);
    const float aspect = (frame_width > 0.0f && frame_height > 0.0f)
                             ? frame_width / frame_height
                             : 1.0f;
    // NaN-safe guards, camera_vp's style: every fallback keeps all sixteen
    // entries finite.
    const float half_tan = std::tan(camera.fov_y * 0.5f);
    const float t = (half_tan > 1e-4f && half_tan < 1e4f) ? half_tan : 1.0f;
    const float f_s = 1.0f / t;
    const float near_z = camera.near_z > 1e-4f ? camera.near_z : 1e-4f;
    const float far_z = camera.far_z > near_z ? camera.far_z : near_z + 1.0f;
    const float inv_range = 1.0f / (far_z - near_z);
    const ws::math::Vec3 eye{camera.eye[0], camera.eye[1], camera.eye[2]};

    // Row order clip.x / clip.y / clip.z / clip.w, column-major m[] exactly
    // like camera_vp. With w = p - eye:
    //   clip.x = f_s * dot(w, right) / aspect
    //   clip.y = f_s * dot(w, -up)      (Vulkan NDC +y is down: -up maps up)
    //   clip.z = far * (dot(w, forward) - near) / (far - near)
    //   clip.w = dot(w, forward)        the view depth - the homogeneous
    //                                   divide IS the perspective divide.
    Mat4 vp{};
    vp.m[0] = f_s * b.right.x / aspect;
    vp.m[4] = f_s * b.right.y / aspect;
    vp.m[8] = f_s * b.right.z / aspect;
    vp.m[12] = -f_s * ws::math::dot(b.right, eye) / aspect;
    vp.m[1] = -f_s * b.up.x;
    vp.m[5] = -f_s * b.up.y;
    vp.m[9] = -f_s * b.up.z;
    vp.m[13] = f_s * ws::math::dot(b.up, eye);
    vp.m[2] = far_z * b.forward.x * inv_range;
    vp.m[6] = far_z * b.forward.y * inv_range;
    vp.m[10] = far_z * b.forward.z * inv_range;
    vp.m[14] = -far_z * (ws::math::dot(b.forward, eye) + near_z) * inv_range;
    vp.m[3] = b.forward.x;
    vp.m[7] = b.forward.y;
    vp.m[11] = b.forward.z;
    vp.m[15] = -ws::math::dot(b.forward, eye);
    return vp;
}

void billboard_corners(float x, float y, float z, float w, float h,
                       const CamBasis& basis, float out[4][3]) {
    // TL, TR, BR, BL - the 2D quads' order and UV assignment: centered on
    // (x, y, z), spread w/h along the basis, every corner on the plane
    // through the center facing the camera (cull is NONE; the orientation,
    // not the winding, is what matters here).
    const ws::math::Vec3 c{x, y, z};
    const float along[4][2] = {{-w * 0.5f, +h * 0.5f},  // TL
                               {+w * 0.5f, +h * 0.5f},  // TR
                               {+w * 0.5f, -h * 0.5f},  // BR
                               {-w * 0.5f, -h * 0.5f}}; // BL
    for (int i = 0; i < 4; ++i) {
        const ws::math::Vec3 corner =
            c + basis.right * along[i][0] + basis.up * along[i][1];
        out[i][0] = corner.x;
        out[i][1] = corner.y;
        out[i][2] = corner.z;
    }
}

bool line_quad3d(float x0, float y0, float z0, float x1, float y1, float z1,
                 float thickness, const CamBasis& basis,
                 float out_pos[4][3], float out_uv[4][2]) {
    using ws::math::Vec3;
    const Vec3 p0{x0, y0, z0};
    const Vec3 p1{x1, y1, z1};
    const Vec3 segment = p1 - p0;
    if (!(ws::math::dot(segment, segment) > 1e-12f) ||
        !(thickness > 0.0f)) {
        return false;
    }
    const Vec3 dir = ws::math::normalize(segment);  // guarded above

    // Ribbon normal: perpendicular to segment AND view. A segment along the
    // view axis degenerates the cross - the screen-up fallback keeps the
    // ribbon on screen (any perpendicular works there, and the width stays
    // world-scaled either way).
    Vec3 normal = ws::math::cross(dir, basis.forward);
    float len2 = ws::math::dot(normal, normal);
    if (!(len2 > 1e-12f)) {
        normal = basis.up;
        len2 = 1.0f;  // the basis is orthonormal - up is already unit
    }
    normal = normal * (thickness * 0.5f / std::sqrt(len2));

    // A,B at p0 and C,D at p1 - line_quad's structure and UVs (the thickness
    // is in WORLD units here, framebuffer pixels there).
    const Vec3 pos[4] = {p0 + normal, p0 - normal, p1 - normal, p1 + normal};
    for (int i = 0; i < 4; ++i) {
        out_pos[i][0] = pos[i].x;
        out_pos[i][1] = pos[i].y;
        out_pos[i][2] = pos[i].z;
    }
    out_uv[0][0] = 0.0f; out_uv[0][1] = 0.0f;
    out_uv[1][0] = 0.0f; out_uv[1][1] = 1.0f;
    out_uv[2][0] = 1.0f; out_uv[2][1] = 1.0f;
    out_uv[3][0] = 1.0f; out_uv[3][1] = 0.0f;
    return true;
}

void BatchCore::start_chunk(const Mat4& vp, const float (&clip)[4],
                            bool depth_test) {
    Chunk chunk;
    chunk.vp = vp;
    chunk.depth_test = depth_test;
    for (int i = 0; i < 4; ++i) {
        chunk.clip[i] = clip[i];
    }
    chunk.vertex_offset = static_cast<std::uint32_t>(vertices_.size());
    chunks_.push_back(chunk);
}

BatchCore::BatchCore() {
    // The old caps, as reservations: steady-state frames allocate nothing,
    // and a frame past them grows geometrically (never drops - M9).
    vertices_.reserve(kInitialVertexCapacity);
    chunks_.reserve(kInitialChunkCapacity);
}

void BatchCore::begin_frame() {
    vertices_.clear();
    chunks_.clear();
    segment_open_ = false;
    dropped_quads_ = 0;
}

void BatchCore::open_segment(const Mat4& vp, const float (&clip)[4],
                             bool depth_test) {
    start_chunk(vp, clip, depth_test);
    segment_open_ = true;
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
    // split inherits the segment's VP, clip and depth mode - it is the same
    // segment.
    const Mat4 vp = chunk.vp;
    start_chunk(vp, chunk.clip, chunk.depth_test);
    chunks_.back().texture_ids[0] = texture_id;
    chunks_.back().texture_count = 1;
    return 0;
}

bool BatchCore::push_quad(const float (&pos)[4][3], const float (&uv)[4][2],
                          const float (&color)[4], std::uint32_t texture_id) {
    if (!segment_open_ || chunks_.empty()) {
        ++dropped_quads_;
        return false;
    }
    // Chunked tier: the chunk-local table slot (with splits). Bindless
    // tier: the global registry id - the shader's table is the registry,
    // so no remap, no split, and the chunk's own table stays empty.
    const std::uint32_t tex_index =
        global_tex_ids_ ? texture_id : texture_slot(texture_id);
    for (int i = 0; i < 4; ++i) {
        Vertex v;
        v.pos[0] = pos[i][0];
        v.pos[1] = pos[i][1];
        v.pos[2] = pos[i][2];
        v.color[0] = color[0];
        v.color[1] = color[1];
        v.color[2] = color[2];
        v.color[3] = color[3];
        v.uv[0] = uv[i][0];
        v.uv[1] = uv[i][1];
        v.tex_index = static_cast<float>(tex_index);
        vertices_.push_back(v);
    }
    ++chunks_.back().quad_count;
    return true;
}

}  // namespace ws::ui::r2d::batch
