#version 450
// Mesh (r3d) vertex shader. One draw call per mesh draw. The push block's
// vertex half is the draw's whole placement (comment-tied to
// render/renderer3d_math.h's MeshPush and the mesh pipeline layout's range;
// the vertex shader reads bytes 0..95):
//   vp                 the block's camera view-projection (the same matrix
//                      the 2D batch pushes - batch::camera_vp3d)
//   rotation           the draw's unit quaternion (x, y, z, w), baked on the
//                      CPU from r3d::Transform3D's yaw/pitch/roll
//   translation_scale  xyz = translation, w = the uniform scale
// The vertex layout is r3d::Vertex3D verbatim: pos, normal, uv (32 bytes).

layout(push_constant) uniform PushConstants {
    mat4 vp;
    vec4 rotation;
    vec4 translation_scale;
} pc;

layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;

layout(location = 0) out vec3 v_normal;
layout(location = 1) out vec2 v_uv;
layout(location = 2) out vec3 v_world;

// Rotates v by the unit quaternion q: v + w*t + cross(q.xyz, t) with
// t = 2*cross(q.xyz, v).
vec3 rotate(vec4 q, vec3 v) {
    const vec3 t = 2.0 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

void main() {
    const vec3 world =
        pc.translation_scale.xyz + pc.translation_scale.w * rotate(pc.rotation, a_pos);
    gl_Position = pc.vp * vec4(world, 1.0);
    // Uniform scale only (Transform3D.scale is a float), so the rotation
    // alone carries the normal; the fragment shader renormalizes.
    v_normal = rotate(pc.rotation, a_normal);
    v_uv = a_uv;
    v_world = world;
}
