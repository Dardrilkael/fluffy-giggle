#version 450
// 2D batch fragment shader, two tiers from one source (the compile script
// picks with -DWS_BINDLESS; both array sizes are comment-tied to
// batch_core.h and the descriptor set layout's binding count):
// - default (chunked tier): u_textures is the CHUNK's sampler table and
//   the index is the chunk-local slot (kMaxTexturesPerChunk).
// - WS_BINDLESS (bindless tier): u_textures is the GLOBAL texture table
//   and the index is the texture's registry id (kMaxBindlessTextures).
//   The id varies per draw, so the access is nonuniform - the
//   nonuniformEXT qualifier is what descriptor indexing promises it by,
//   and it must ride the INDEX: `u_textures[nonuniformEXT(index)]`
//   decorates the access chain's index operand (SPIR-V 1.3's NonUniform),
//   while `texture(nonuniformEXT(u_textures[index]), ...)` decorates only
//   the loaded image downstream - the fetch itself stays formally
//   uniform, and a driver may then reuse one descriptor for a whole
//   wave, sampling the wrong page wherever a chunk mixes ids.
// A NEGATIVE v_tex_index never names a texture in either tier: it is the
// rounded-rect SDF channel (draw_rounded_quad's radius, normalized to the
// rect's width at record time). That branch is shared, ahead of the tier
// split below.

#ifdef WS_BINDLESS
#extension GL_EXT_nonuniform_qualifier : require
#endif

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in flat float v_tex_index;

layout(location = 0) out vec4 out_color;

#ifdef WS_BINDLESS
layout(set = 0, binding = 0) uniform sampler2D u_textures[1024];
#else
layout(set = 0, binding = 0) uniform sampler2D u_textures[32];
#endif

void main() {
    if (v_tex_index < 0.0) {
        // Rounded-box SDF, in pixels. v_uv are the rect's normalized
        // local coordinates, so their screen gradient measures the pixel
        // scale: |grad v_uv.x| = 1 / rect_width_px - constant across the
        // primitive for the 2D camera's affine, zoom-only VP (so the
        // reconstruction is exact even under rotation; the flat qualifier
        // keeps the whole primitive on this one branch), and locally
        // adapted under the 3D block's perspective. The channel is
        // -radius/w, so dividing out the gradient returns the radius in
        // pixels: zero vertex-layout change, zero extra bandwidth.
        const float du = length(vec2(dFdx(v_uv.x), dFdy(v_uv.x)));
        const float dv = length(vec2(dFdx(v_uv.y), dFdy(v_uv.y)));
        if (du <= 0.0 || dv <= 0.0) {
            out_color = vec4(0.0);  // degenerate on screen: shade nothing
            return;
        }
        const vec2 size = vec2(1.0 / du, 1.0 / dv);  // the rect, in pixels
        const float radius = -v_tex_index / du;
        // Centered box SDF: p the pixel offset from the rect's center, q
        // the corner region (negative inside the straight-edge bands).
        const vec2 p = (v_uv - 0.5) * size;
        const vec2 q = abs(p) - (size * 0.5 - radius);
        const float dist = length(max(q, 0.0))
                         + min(max(q.x, q.y), 0.0) - radius;
        // One-pixel antialiasing ramp (fwidth-widened under
        // minification), straight alpha into the batch's blend.
        const float aa = max(fwidth(dist), 1e-4);
        const float coverage = clamp(0.5 - dist / aa, 0.0, 1.0);
        out_color = vec4(v_color.rgb, v_color.a * coverage);
        return;
    }
    const int index = int(v_tex_index + 0.5);  // round: never truncate
#ifdef WS_BINDLESS
    out_color = v_color * texture(u_textures[nonuniformEXT(index)], v_uv);
#else
    out_color = v_color * texture(u_textures[index], v_uv);
#endif
}
