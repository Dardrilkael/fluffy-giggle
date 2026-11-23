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
// A tex_index >= kTextCoverageFlag (4096) marks a GLYPH quad (draw_text
// records it with text=true). The shader subtracts the flag to recover
// the real texture index, then samples the texel's ALPHA as the
// coverage mask (glyph pages are white-RGB/coverage-in-alpha) and
// applies the luminance-dependent contrast curve instead of the plain
// v_color * texel multiply. This is the runtime text contrast feature:
// one atlas serves all themes/text colors without re-bake on theme flip.

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

// The GLYPH channel of v_tex_index: a quad recorded with text=true
// carries (slot + kTextCoverageFlag). 4096 sits past every legal slot
// in both tiers (32 chunk-local, 1024 bindless) and far below float's
// exact-integer limit, so it never collides with a texture index and
// never loses the slot to rounding. Comment-tied to
// batch::kTextCoverageFlag in batch_core.h.
const float kTextCoverageFlag = 4096.0f;

// Text contrast curve. The glyph atlas texel is white RGB with the
// LINEAR coverage in ALPHA (font.cpp's "RGB white, coverage in alpha"
// contract; the pages upload as R8G8B8A8_UNORM) - so coverage is the
// .a channel, NEVER .r: sampling .r reads the white tint (1.0
// everywhere, including fully transparent texels) and every glyph
// renders as a solid rectangle of its bounding box. The plain sprite
// path below shows why that channel is the right one there too: the
// premultiply-by-alpha in v_color * texel relies on the same layout.
//
// The curve mirrors the bake-time LUT it replaces (see
// FontBakeOptions::gamma/boost): the bake lifted the mids as
// a' = a^(1/gamma) with a per-theme gamma, plus a straight boost
// multiplier. Here the TEXT COLOR's luminance picks the tuning
// continuously - light text (L=1) takes the dark chrome's pair
// (gamma 1.6, boost 1.1), dark text (L=0) the light chrome's pair
// (gamma 1.05, boost 1.0) - so one linear atlas serves every theme
// without a re-bake. The exponent stays on the LUT's SIDE of 1
// (a^(1/g), 1/g <= 1): an a^gamma with gamma > 1 would thin the
// mid-tones - the exact opposite of what both stock chromes tuned.
float text_coverage(float raw_cov, vec3 text_rgb) {
    // Rec. 709 relative luminance, clamped to [0,1].
    float L = clamp(dot(text_rgb, vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0);
    // Blend the two stock tunings by luminance: light text (L=1) takes
    // the dark chrome's pair, dark text (L=0) the light chrome's pair.
    float inv_gamma = mix(1.0 / 1.6, 1.0 / 1.05, 1.0 - L);
    float boost = mix(1.1, 1.0, 1.0 - L);
    float c = pow(raw_cov, inv_gamma);
    c = c * boost;
    return clamp(c, 0.0, 1.0);
}

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
    // Glyph channel: tex_index >= kTextCoverageFlag (4096).
    // Subtract the flag to recover the real texture slot, then sample
    // the texel's ALPHA as the coverage mask (the atlas layout above)
    // and apply the text contrast curve.
    if (v_tex_index >= kTextCoverageFlag) {
        const int index = int((v_tex_index - kTextCoverageFlag) + 0.5);
        float raw_cov;
#ifdef WS_BINDLESS
        raw_cov = texture(u_textures[nonuniformEXT(index)], v_uv).a;
#else
        raw_cov = texture(u_textures[index], v_uv).a;
#endif
        float cov = text_coverage(raw_cov, v_color.rgb);
        out_color = vec4(v_color.rgb, v_color.a * cov);
        return;
    }
    const int index = int(v_tex_index + 0.5);  // round: never truncate
#ifdef WS_BINDLESS
    out_color = v_color * texture(u_textures[nonuniformEXT(index)], v_uv);
#else
    out_color = v_color * texture(u_textures[index], v_uv);
#endif
}
