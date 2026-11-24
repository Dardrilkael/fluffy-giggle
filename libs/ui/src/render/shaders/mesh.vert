#version 450
// Mesh (r3d) vertex shader. One draw call per mesh draw. The push block's
// vertex half is the draw's whole placement (comment-tied to
// render/renderer3d_math.h's MeshPush and the mesh pipeline layout's
// vertex range, bytes 0..111):
//   vp            the block's camera view-projection (same matrix the 2D
//                 batch pushes - batch::camera_vp3d)
//   m0 / m1 / m2  the draw's 3x4 model matrix, ONE ROW per vec4 - the
//                 rotation*uniform-scale in .xyz and the translation in
//                 .w - baked on the CPU from r3d::Transform3D.
// The vertex layout is r3d::Vertex3D verbatim: pos, normal, uv (32 bytes).

layout(push_constant) uniform PushConstants {
    mat4 vp;
    vec4 m0;
    vec4 m1;
    vec4 m2;
} pc;

layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;

layout(location = 0) out vec3 v_normal;
layout(location = 1) out vec2 v_uv;

void main() {
    const vec4 p = vec4(a_pos, 1.0);
    const vec3 world = vec3(dot(pc.m0, p), dot(pc.m1, p), dot(pc.m2, p));
    gl_Position = pc.vp * vec4(world, 1.0);
    // Uniform scale only (Transform3D.scale is a float), so the model's
    // 3x3 is rotation*s and carries the normal correctly up to length -
    // the fragment shader renormalizes, so no inverse-transpose needed.
    v_normal = vec3(dot(pc.m0.xyz, a_normal),
                    dot(pc.m1.xyz, a_normal),
                    dot(pc.m2.xyz, a_normal));
    v_uv = a_uv;
}
