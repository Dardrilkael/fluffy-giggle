#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

#include <ws/core/result.hpp>

namespace ws::gfx {

// The platform seam: gfx never includes a windowing header - the consumer
// (ws::ui's app shell) adapts its window host to this shape. The instance
// extension list is read ONCE, during init(); the name POINTERS must stay
// valid for the lifetime of the Gpu (the platforms hand out static
// strings). create_surface runs at every attach_surface().
struct SurfaceSource {
    const char* const* instance_extensions = nullptr;
    uint32_t instance_extension_count = 0;
    // `instance` is the Gpu's raw VkInstance (opaque here; the adapter
    // casts it with its own Vulkan headers). On success writes the raw
    // VkSurfaceKHR to `*surface_out` - a non-dispatchable handle, so it
    // crosses as a 64-bit integer (handle_to_u64 below).
    bool (*create_surface)(void* instance, std::uint64_t* surface_out, void* user) = nullptr;
    void* user = nullptr;
};

// Device-stage requests. `want_bindless` + `bindless_descriptor_limit`
// drive the descriptor-indexing probe: when asked, gfx enables the
// features and checks the limits for one sampled-image binding of
// `bindless_descriptor_limit` slots, and reports what landed via
// descriptor_indexing() - a miss is the documented chunked fallback,
// never a failure.
struct DeviceOptions {
    const char* application_name = "ws";  // read once, during init()
    bool enable_validation = false;  // opt-in; warns + runs without when the SDK is absent
    bool want_bindless = false;
    uint32_t bindless_descriptor_limit = 0;  // sampled-image slots the table must cover
};

// The probe's answer: what the device can actually do.
struct DescriptorIndexing {
    bool bindless = false;           // one global descriptor-indexed table is possible
    bool update_after_bind = false;  // ... and its writes may land mid-flight
};

// Non-dispatchable Vulkan handles (VkBuffer, VkImage, VkSurfaceKHR,
// VkRenderPass, VkShaderModule) are real pointers on 64-bit ABIs but
// plain uint64_t values on 32-bit ones - a void* cannot carry them on
// 32-bit, so they cross this boundary as a 64-bit integer. Neither cast
// direction is a plain static_cast on every ABI (integer -> pointer is
// not a static_cast at all), so both sides go through helpers:
// handle_to_u64 folds a handle in, handle_from_u64 unwraps it back -
// reinterpret_cast where the handle is a pointer, static_cast where it
// is an integer. VmaAllocation rides the same integer but is VMA's own
// pointer typedef on every ABI; the consumer never unwraps an
// allocation, it only hands it back to gfx.
template <class Handle>
std::uint64_t handle_to_u64(Handle handle) {
    if constexpr (std::is_pointer_v<Handle>) {
        return reinterpret_cast<std::uint64_t>(handle);
    } else {
        return static_cast<std::uint64_t>(handle);
    }
}

// The unwrap twin, for the owner's unwrap points (gfx internally, and
// ui where it includes volk.h). VmaAllocation (VMA's own pointer
// typedef) unwraps through this too, inside gfx.
template <class Handle>
Handle handle_from_u64(std::uint64_t value) {
    if constexpr (std::is_pointer_v<Handle>) {
        return reinterpret_cast<Handle>(value);
    } else {
        return static_cast<Handle>(value);
    }
}

// Device-memory + object descriptors (phase C). The `usage`/`format`
// fields carry the Vulkan enums as plain numbers - the consumer spells
// them with its own Vulkan headers; no Vulkan type crosses the boundary.
struct BufferDesc {
    uint64_t size = 0;   // bytes
    uint32_t usage = 0;  // VkBufferUsageFlags
};

// create_buffer's answer: the handles to keep and the write window.
// `buffer` + `allocation` pair up for destroy_buffer; `mapped` is the
// host-visible window to write through (the buffer stays mapped for its
// lifetime).
struct BufferOut {
    std::uint64_t buffer = 0;      // VkBuffer (non-dispatchable)
    std::uint64_t allocation = 0;  // VmaAllocation (non-dispatchable)
    void* mapped = nullptr;        // host-visible write window (a real pointer)
};

// The image shape ui uploads: 2D, one layer, optimal tiling, starting
// undefined - the boilerplate lives in gfx. The mip count is the
// descriptor's one degree of freedom: 1 is the flat image (no chain),
// and the full chain for a w*h image is floor(log2(max(w, h))) + 1.
struct ImageDesc {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;  // VkFormat
    uint32_t usage = 0;   // VkImageUsageFlags
    uint32_t mip_levels = 1;
};

// create_image's answer: the device-local image plus its allocation pair
// for destroy_image. The image view stays the consumer's (a plain vk
// object, not memory).
struct ImageOut {
    std::uint64_t image = 0;       // VkImage (non-dispatchable)
    std::uint64_t allocation = 0;  // VmaAllocation (non-dispatchable)
};

// The Vulkan device layer: loader, instance (+ optional validation),
// surface, physical-device/queue selection, device and VMA allocator.
//
// Handles cross the boundary without Vulkan vocabulary - the no-leak
// rule keeps Vulkan headers out of include/, so the consumer casts them
// at its own single unwrap point (it includes volk.h privately).
// Dispatchable handles (instance, physical device, device, queue) and
// VmaAllocator are pointers on every ABI and cross as void*;
// non-dispatchable ones (the surface, buffers, images, shader modules,
// VmaAllocation) cross as a 64-bit integer (handle_to_u64 above).
// Every getter is null/0 until its stage is up.
//
// The stages mirror the shell's Android-surface-loss contract:
//   init()            context stage, once per process (loader + instance)
//   attach_surface()  window stage; the FIRST attach also creates the
//                     device stage (physical device, device, queue,
//                     allocator), which survives every detach
//   detach_surface()  window-stage teardown (the surface alone)
//   shutdown()        everything, guarded + idempotent
class Gpu {
public:
    Gpu();
    ~Gpu();
    Gpu(const Gpu&) = delete;
    Gpu& operator=(const Gpu&) = delete;

    core::Status init(const SurfaceSource& source, const DeviceOptions& options);
    core::Status attach_surface();
    void detach_surface();
    void shutdown();

    // Opaque raw handles - the named Vulkan/VMA objects, null until their
    // stage is up:
    void* instance() const;         // VkInstance
    std::uint64_t surface() const;  // VkSurfaceKHR (window stage only)
    void* physical_device() const;  // VkPhysicalDevice (device stage)
    void* device() const;          // VkDevice (device stage)
    void* queue() const;            // VkQueue (device stage)
    void* allocator() const;       // VmaAllocator (device stage)
    uint32_t queue_family() const;
    // VkFormat, numeric: the depth-attachment format chosen once at device
    // selection - every render pass the process builds must use it (the
    // shell's render-pass-compatibility guarantee).
    uint32_t depth_format() const;

    DescriptorIndexing descriptor_indexing() const;

    // --- device memory + object helpers (phase C; src/memory.cpp) ---
    // The ops ui's renderer draws through, sized to exactly its use:
    // buffers are host-write and persistently mapped (the arena + upload
    // staging profile), images are device-local 2D sample targets. Each
    // create clears its out record, validates the descriptor, then needs
    // the device stage - every failure is the Status above, never a
    // crash. The destroys and the flush are null-tolerant no-ops, so
    // failure-path teardown calls them uniformly.
    core::Status create_buffer(const BufferDesc& desc, BufferOut& out) const;
    void destroy_buffer(std::uint64_t buffer, std::uint64_t allocation) const;
    // Makes the buffer's host writes in [offset, offset + size) visible -
    // vma may hand back non-coherent memory, so a writer flushes after
    // filling `mapped`. The defaults keep the whole-allocation spelling;
    // a caller that knows its written prefix (the renderer's arenas)
    // passes it and spares the cache maintenance of the tail. size may
    // be UINT64_MAX for "to the end" (VK_WHOLE_SIZE under the hood).
    void flush_buffer(std::uint64_t allocation, std::uint64_t offset = 0,
                      std::uint64_t size = UINT64_MAX) const;
    core::Status create_image(const ImageDesc& desc, ImageOut& out) const;
    void destroy_image(std::uint64_t image, std::uint64_t allocation) const;
    core::Status create_shader_module(const uint32_t* code,
                                      std::size_t word_count,
                                      std::uint64_t* module) const;
    void destroy_shader_module(std::uint64_t module) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ws::gfx
