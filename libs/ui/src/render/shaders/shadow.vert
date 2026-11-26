#version 450
// Shadow-map (r3d) vertex shader: the depth-only pass that renders the frame's
// shadow casters from the directional light. No fragment shader - depth is
// all the pass writes. The push block is renderer3d_math.h's ShadowPush
// (vertex stage, 96 bytes): the light-space matrix and the same placement
// (quaternion + translation/scale) the mesh vertex shader takes, so a
// caster lands in the map exactly where it lands in the scene.

layout(push_constant) uniform PushConstants {
    mat4 light_vp;
    vec4 rotation;
    vec4 translation_scale;
} pc;

layout(location = 0) in vec3 a_pos;  // r3d::Vertex3D position (pos only)

vec3 rotate(vec4 q, vec3 v) {
    const vec3 t = 2.0 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

void main() {
    const vec3 world =
        pc.translation_scale.xyz + pc.translation_scale.w * rotate(pc.rotation, a_pos);
    gl_Position = pc.light_vp * vec4(world, 1.0);
}
