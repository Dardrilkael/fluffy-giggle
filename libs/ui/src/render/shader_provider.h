#pragma once

// Internal: shader source for the batch pipelines. This header never
// reaches apps - the public surface is <ws/ui/renderer2d.hpp>.
//
// Default (WS_UI_DEV_SHADER_RELOAD undefined): the committed spirv.h words,
// and nothing else - no process spawning, no file watching, zero code for it
// even links in. The build needs no shader compiler (the same pattern as
// the ImGui Vulkan backend's embedded SPIR-V).
//
// Dev hot reload (WS_UI_DEV_SHADER_RELOAD defined, Debug-only, desktop
// only): poll_reload() compares the .vert/.frag modification times and, on
// change, recompiles them with glslang (same flags as
// tools/compile_shaders.ps1, resolved the same way) into a temp .spv,
// validates it, and swaps it in. A failed compile logs the compiler's
// message and keeps the last good words - a dev iteration tool must never
// crash the app or hand Vulkan a null module.

#include "render/shaders/mesh_spirv.h"
#include "render/shaders/spirv.h"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace ws::ui::r2d::spirv {

// A compiled stage's words. Points either at the committed spirv.h array
// or (dev reload) at a heap buffer that outlives every consumer - the
// VkShaderModule wraps a copy, so nothing here is lifetime-sensitive past
// vkCreateShaderModule returning.
struct ShaderWords {
    const std::uint32_t* words = nullptr;
    std::size_t count = 0;
};

// PCF filtering modes (must match mesh.frag PCF_MODE defines):
// 0 = 3x3 PCF (baseline, 9 taps)
// 1 = 2x2 PCF (4 taps)
// 2 = 1-tap (single comparison, no filtering)
// 3 = Optimized 3x3 PCF (9 taps, manually unrolled)
inline int pcf_mode() {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4996)
#endif
    const char* value = std::getenv("WS_SHADOW_PCF_MODE");
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    if (value) {
        int mode = std::atoi(value);
        if (mode >= 0 && mode <= 3) return mode;
    }
    return 0;  // default: 3x3 PCF
}

#ifdef WS_UI_DEV_SHADER_RELOAD

// The renderer calls this once per frame BEFORE frame recording begins;
// true means the stages changed and the pipelines must be recreated. Only
// one recompile per change; spurious mtime jitter (editor save-with-same-
// mtime, touch) is harmless - the compile either succeeds and swaps in or
// fails and keeps the last good words.
bool poll_reload(const std::string& shader_dir);

// The stages the renderer should build pipelines from RIGHT NOW (call
// after poll_reload told you to recreate). frag_words(bindless) picks the
// tier's variant: the chunked 32-slot-table shader or the bindless
// global-table one (nonuniformEXT) - the renderer passes the tier its
// device probe picked at init, fixed for the process.
ShaderWords vert_words();
ShaderWords frag_words(bool bindless);

#else  // !WS_UI_DEV_SHADER_RELOAD

inline bool poll_reload(const std::string&) { return false; }

inline ShaderWords vert_words() {
    return {kBatchVertSpv, sizeof(kBatchVertSpv) / sizeof(kBatchVertSpv[0])};
}
inline ShaderWords frag_words(bool bindless) {
    if (bindless) {
        return {kBatchFragBindlessSpv,
                sizeof(kBatchFragBindlessSpv) / sizeof(kBatchFragBindlessSpv[0])};
    }
    return {kBatchFragSpv, sizeof(kBatchFragSpv) / sizeof(kBatchFragSpv[0])};
}

#endif  // WS_UI_DEV_SHADER_RELOAD

// The r3d mesh pipeline's stages: ALWAYS the committed mesh_spirv.h words,
// dev hot reload or not (the reload provider above covers the batch stages
// only - the mesh shaders are small and change rarely; regenerate with
// tools/compile_shaders.ps1). Same tier split as the batch fragment stage.
inline ShaderWords mesh_vert_words() {
    return {kMeshVertSpv, sizeof(kMeshVertSpv) / sizeof(kMeshVertSpv[0])};
}
inline ShaderWords mesh_frag_words(bool bindless) {
    int mode = pcf_mode();
    if (bindless) {
        const std::uint32_t* base = kMeshFragBindlessSpv + kMeshFragBindlessSpvOffsets[mode];
        return {base, static_cast<std::size_t>(kMeshFragBindlessSpvSizes[mode])};
    }
    const std::uint32_t* base = kMeshFragSpv + kMeshFragSpvOffsets[mode];
    return {base, static_cast<std::size_t>(kMeshFragSpvSizes[mode])};
}
// The shadow-map pass's vertex stage (depth-only; no fragment stage).
inline ShaderWords shadow_vert_words() {
    return {kShadowVertSpv, sizeof(kShadowVertSpv) / sizeof(kShadowVertSpv[0])};
}

}  // namespace ws::ui::r2d::spirv