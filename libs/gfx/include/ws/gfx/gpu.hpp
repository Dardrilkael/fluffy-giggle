#pragma once

#include <cstdint>
#include <memory>

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
    // VkSurfaceKHR to `*surface_out`.
    bool (*create_surface)(void* instance, void** surface_out, void* user) = nullptr;
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

// The Vulkan device layer: loader, instance (+ optional validation),
// surface, physical-device/queue selection, device and VMA allocator.
//
// Handles cross the boundary as opaque void* - the no-leak rule keeps
// Vulkan headers out of include/, so the consumer casts them at its own
// single unwrap point (it includes volk.h privately). Every getter is
// null/0 until its stage is up.
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
    void* surface() const;         // VkSurfaceKHR (window stage only)
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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ws::gfx
