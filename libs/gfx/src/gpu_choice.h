#pragma once

// The pure decision helpers behind ws::gfx's device selection - kept free
// of every Vulkan type so tests cover the decision tables headless, no
// device faked. gpu.cpp fills these inputs from the Vulkan queries, then
// commits the result.

#include <cstdint>

#include <ws/gfx/gpu.hpp>

namespace ws::gfx {

// What the device actually advertised, already queried by gpu.cpp.
struct IndexingCaps {
    bool loader_1_1 = false;         // the loader reached the Vulkan 1.1 API
    bool dialect = false;            // core 1.2 or VK_EXT_descriptor_indexing
    bool nonuniform = false;         // shaderSampledImageArrayNonUniformIndexing
    bool partially_bound = false;    // descriptorBindingPartiallyBound
    bool update_after_bind = false;  // descriptorBindingSampledImageUpdateAfterBind
    bool limits_cover_request = false;  // per-stage/set sampled-image limits >= the table
};

// The descriptor-indexing tier decision (the renderer's texture tier):
// bindless needs every feature AND the limits to cover the requested table;
// update-after-bind rides only when the device offers it. Anything less is
// the chunked fallback - never a failure.
inline DescriptorIndexing decide_indexing(const IndexingCaps& caps, bool want_bindless) {
    DescriptorIndexing tier{};
    if (want_bindless && caps.loader_1_1 && caps.dialect && caps.nonuniform &&
        caps.partially_bound && caps.limits_cover_request) {
        tier.bindless = true;
        tier.update_after_bind = caps.update_after_bind;
    }
    return tier;
}

// The depth-format walk: the first candidate the predicate confirms as
// depth-attachable (gpu.cpp passes the optimal-tiling format-properties
// query). Candidates are VkFormat values spelled as uint32 to keep this
// header Vulkan-free. Returns 0 (VK_FORMAT_UNDEFINED) when none qualify -
// the caller treats that as a hard error: no depth attachment, no device.
inline uint32_t pick_depth_format(
    const uint32_t* candidates, uint32_t count,
    bool (*supports_depth_attach)(uint32_t format, void* user), void* user) {
    for (uint32_t i = 0; i < count; ++i) {
        if (supports_depth_attach(candidates[i], user)) {
            return candidates[i];
        }
    }
    return 0;
}

}  // namespace ws::gfx
