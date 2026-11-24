// ws::gfx::Swapchain - the presentation layer (stage 3b, phase B): the
// swapchain, the depth attachment, the color+depth render pass,
// framebuffers and the frame sync (fixed frame slots + per-image
// render-finished semaphores), promoted out of ws::ui's app shell. ws::ui
// stays the only consumer - apps never include this lib.
//
// All Vulkan/VMA types live in this TU behind the pimpl (include/ is
// ws-spelled only; handles cross as opaque void* and the consumer casts
// them at its own single unwrap point). Creation/teardown return
// core::Status with the diagnostic - the shell logs it; the frame-flow
// calls instead log and ABORT on a Vulkan failure: mid-frame there is no
// state to roll back to (semaphores in flight, a command buffer half
// recorded), the same policy the shell documented when it owned this
// code.
//
// The frame model is the classic frames-in-flight: kMaxFramesInFlight
// slots each own a fence, command pool/buffer and acquire semaphore,
// while each swapchain image owns its render-finished semaphore - the
// slot count, not the image count, bounds how many frames may be in
// flight. Recreate-on-out-of-date as in Dear ImGui's examples, but the
// swapchain code is ours - the ImGui_ImplVulkanH_* helpers are
// example-only per the backend's own docs.

#include <ws/gfx/swapchain.hpp>

#include <ws/gfx/gpu.hpp>  // Gpu: the device layer this presents through

#include <ws/core/log.hpp>

#include "swapchain_choice.h"

#include <volk.h>  // meta-loader: every vk* symbol below is a volk global

// vk_mem_alloc.h is a third-party header - keep it clear of our /W4 /WX
// settings on MSVC and of the stricter clang sets on Android.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include <vk_mem_alloc.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <array>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace ws::gfx {
namespace {

// gfx's swapchain error codes, grouped after the device stage's (gpu.cpp:
// lifecycle misuse 1, context 100s, surface 200s, device 300s). The shell
// logs the Status it gets; nothing branches on the numbers.
constexpr std::int32_t kErrOrder = 1;           // create/recreate misuse
constexpr std::int32_t kErrCapabilities = 400;  // surface capabilities query
constexpr std::int32_t kErrNoArea = 401;        // zero extent - no visible area
constexpr std::int32_t kErrSwapchain = 402;      // vkCreateSwapchainKHR
constexpr std::int32_t kErrImages = 403;          // vkGetSwapchainImagesKHR
constexpr std::int32_t kErrView = 404;            // image views (color, depth)
constexpr std::int32_t kErrDepthImage = 405;      // vmaCreateImage (depth)
constexpr std::int32_t kErrRenderPass = 406;      // vkCreateRenderPass
constexpr std::int32_t kErrFramebuffer = 407;     // vkCreateFramebuffer
constexpr std::int32_t kErrSlotObjects = 408;      // pool/buffer/fence/semaphore
constexpr std::int32_t kErrMsaaImage = 409;        // Phase 4: the multisample target

// Swapchain format preference, most preferred first (VkFormat values as
// plain uint32 for the Vulkan-free choice helper, swapchain_choice.h).
// SRGB_NONLINEAR is the only colorspace required by the spec, so we
// require it alongside.
constexpr std::array<uint32_t, 2> kPreferredFormats{
    static_cast<uint32_t>(VK_FORMAT_B8G8R8A8_UNORM),
    static_cast<uint32_t>(VK_FORMAT_R8G8B8A8_UNORM)};

// One frame slot per frame in flight (kMaxFramesInFlight of them) - the
// unit the CPU throttles on. Everything the GPU may still be reading from
// a previous frame lives behind a slot: its command pool/buffer, the
// consumer's per-slot renderer state (keyed by the Frame::frame_index it
// was handed), and the acquire semaphore (re-signaled every acquire, so it
// must retire first - that is why the slot fence is waited BEFORE the
// acquire). The fence is created SIGNALED: the first wait must not block.
struct FrameSlot {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer buffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
};

// Mid-frame Vulkan failures leave the caller with no state to roll back
// to (a command buffer may be half recorded, semaphores in flight), so
// log and abort - the policy the shell's frame loop documented when it
// owned this code. Creation/teardown instead return core::Status and the
// consumer logs it.
void vk_check(VkResult err, const char* what) {
    if (err != VK_SUCCESS) {
        core::error("gfx: {} failed (VkResult {})", what, static_cast<int>(err));
        std::abort();
    }
}

}  // namespace

struct Swapchain::Impl {
    // The device layer this swapchain presents through (stored at
    // create(); the Gpu must outlive the Swapchain). May stay set with a
    // dead device stage after a Gpu::shutdown() - every call below is
    // guarded, exactly like the null-state paths in gpu.cpp.
    Gpu* gpu = nullptr;
    SwapchainOptions options{};

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    VkExtent2D extent{};
    uint32_t image_count = 0;
    std::vector<VkImage> images;  // owned by the swapchain, not by us
    std::vector<VkImageView> views;
    // The depth attachment, one per swapchain (extent == color): VMA owns
    // the image (the color images are swapchain-owned), the view is ours.
    // The format is the Gpu's depth_format(), chosen ONCE at device
    // selection, so every render pass ever built uses the same depth
    // format.
    VkImage depth_image = VK_NULL_HANDLE;
    VmaAllocation depth_allocation = VK_NULL_HANDLE;
    VkImageView depth_view = VK_NULL_HANDLE;
    // Phase 4 (MSAA): the multisample color target the subpass renders
    // into (extent == color; VMA owns the image, the view is ours) and
    // the sample count chosen from the device limits (1 when the flag
    // is off or nothing useful is supported - the single-sample pass
    // below renders unchanged, no resolve attachment, no extra image).
    // The swapchain images become the pass's RESOLVE targets: same
    // extent, same format, single-sample - the framebuffers bind all
    // three views and the pass's resolve subpass lands the averaged
    // pixels into the presentable image at end of render.
    VkImage msaa_image = VK_NULL_HANDLE;
    VmaAllocation msaa_allocation = VK_NULL_HANDLE;
    VkImageView msaa_view = VK_NULL_HANDLE;
    VkSampleCountFlagBits sample_count = VK_SAMPLE_COUNT_1_BIT;
    std::vector<VkFramebuffer> framebuffers;
    std::array<FrameSlot, kMaxFramesInFlight> frame_slots{};
    // Render-finished semaphores are per IMAGE, not per slot: the present
    // of image i must wait exactly the submission that rendered i, and
    // while that submission is still queued the image cannot be
    // re-acquired - so two frames in flight always render two different
    // images.
    std::vector<VkSemaphore> image_rendered;
    uint32_t frame_index = 0;  // frame slot recording the current frame
    // The acquire returned VK_SUBOPTIMAL_KHR: the frame still runs and
    // presents (the image IS acquired, its semaphore IS signaled), and the
    // swapchain is recreated only after that frame has retired - present()
    // folds it into needs_rebuild.
    bool suboptimal = false;

    // Intra-lib unwrap: the Gpu's opaque void* handles back into the
    // Vulkan/VMA types (this TU includes volk.h and vk_mem_alloc.h; the
    // PUBLIC boundary keeps them void*).
    VkPhysicalDevice physical() const {
        return static_cast<VkPhysicalDevice>(gpu->physical_device());
    }
    VkSurfaceKHR surface() const { return handle_from_u64<VkSurfaceKHR>(gpu->surface()); }
    VkDevice device() const { return static_cast<VkDevice>(gpu->device()); }
    VkQueue queue() const { return static_cast<VkQueue>(gpu->queue()); }
    VmaAllocator allocator() const {
        return static_cast<VmaAllocator>(gpu->allocator());
    }
    uint32_t queue_family() const { return gpu->queue_family(); }
    VkFormat depth_format() const { return static_cast<VkFormat>(gpu->depth_format()); }

    // Creates the swapchain and everything tied to its extent: image
    // views, render pass, framebuffers and per-frame command/fence/
    // semaphore objects. The caller (create/recreate) cleans up on
    // failure - destroy_objects() is safe on partial state.
    core::Status build(int fb_width, int fb_height);
    void destroy_objects();
};

core::Status Swapchain::Impl::build(int fb_width, int fb_height) {
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical(), surface(), &caps) !=
        VK_SUCCESS) {
        return core::fail(kErrCapabilities, "gfx",
                          "failed to query surface capabilities");
    }

    // 0xFFFFFFFF means "match the window's current framebuffer size".
    const ExtentChoice current{caps.currentExtent.width, caps.currentExtent.height};
    const ExtentChoice min_extent{caps.minImageExtent.width, caps.minImageExtent.height};
    const ExtentChoice max_extent{caps.maxImageExtent.width, caps.maxImageExtent.height};
    ExtentChoice chosen{};
    if (!choose_extent(current, min_extent, max_extent,
                       static_cast<uint32_t>(fb_width),
                       static_cast<uint32_t>(fb_height), &chosen)) {
        return core::fail(kErrNoArea, "gfx",
                          "swapchain extent is zero (the window has no visible "
                          "area)");
    }
    extent = VkExtent2D{chosen.width, chosen.height};

    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical(), surface(), &format_count,
                                         nullptr);
    std::vector<VkSurfaceFormatKHR> queried_formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical(), surface(), &format_count,
                                         queried_formats.data());
    // One entry with UNDEFINED format means "any format is acceptable" -
    // fold it into the choice's empty list (no restrictions).
    if (queried_formats.size() == 1 &&
        queried_formats[0].format == VK_FORMAT_UNDEFINED) {
        queried_formats.clear();
    }
    std::vector<SurfaceFormatChoice> formats;
    formats.reserve(queried_formats.size());
    for (const VkSurfaceFormatKHR& queried : queried_formats) {
        formats.push_back({static_cast<uint32_t>(queried.format),
                           static_cast<uint32_t>(queried.colorSpace)});
    }
    const SurfaceFormatChoice unrestricted{
        static_cast<uint32_t>(VK_FORMAT_B8G8R8A8_UNORM),
        static_cast<uint32_t>(VK_COLORSPACE_SRGB_NONLINEAR_KHR)};
    const SurfaceFormatChoice surface_format = choose_surface_format(
        formats.data(), static_cast<uint32_t>(formats.size()),
        kPreferredFormats.data(),
        static_cast<uint32_t>(kPreferredFormats.size()),
        static_cast<uint32_t>(VK_COLORSPACE_SRGB_NONLINEAR_KHR), unrestricted);
    format = static_cast<VkFormat>(surface_format.format);

    uint32_t mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical(), surface(), &mode_count,
                                              nullptr);
    std::vector<VkPresentModeKHR> queried_modes(mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical(), surface(), &mode_count,
                                              queried_modes.data());
    if (options.vsync) {
        present_mode = VK_PRESENT_MODE_FIFO_KHR;  // the only mode the spec guarantees
    } else {
        // Uncapped: mailbox first (still tear-free), then immediate.
        std::vector<uint32_t> available_modes;
        available_modes.reserve(queried_modes.size());
        for (const VkPresentModeKHR queried : queried_modes) {
            available_modes.push_back(static_cast<uint32_t>(queried));
        }
        const uint32_t preferences[] = {
            static_cast<uint32_t>(VK_PRESENT_MODE_MAILBOX_KHR),
            static_cast<uint32_t>(VK_PRESENT_MODE_IMMEDIATE_KHR)};
        present_mode = static_cast<VkPresentModeKHR>(choose_present_mode(
            preferences, sizeof(preferences) / sizeof(preferences[0]),
            available_modes.data(), static_cast<uint32_t>(available_modes.size()),
            static_cast<uint32_t>(VK_PRESENT_MODE_FIFO_KHR)));
        if (present_mode == VK_PRESENT_MODE_FIFO_KHR) {
            core::info("gfx: no mailbox/immediate support, falling back to FIFO");
        }
    }

    const uint32_t image_request =
        choose_image_count(caps.minImageCount, caps.maxImageCount);

    VkSwapchainCreateInfoKHR create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    create_info.surface = surface();
    create_info.minImageCount = image_request;
    create_info.imageFormat = static_cast<VkFormat>(surface_format.format);
    create_info.imageColorSpace =
        static_cast<VkColorSpaceKHR>(surface_format.color_space);
    create_info.imageExtent = extent;
    create_info.imageArrayLayers = 1;
    create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    // Identity on purpose, on every platform: the app renders upright in
    // window space and the compositor applies any window rotation at
    // present time. Android's currentTransform goes non-identity once
    // the app follows device rotation; mirroring it here would promise
    // pre-rotated buffers the shell never produces (a sideways image).
    // The zero-compositor fast path - currentTransform + pre-rotated
    // content - stays a deliberate future opt-in.
    create_info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    create_info.compositeAlpha = static_cast<VkCompositeAlphaFlagBitsKHR>(
        choose_composite_alpha(caps.supportedCompositeAlpha,
                               static_cast<uint32_t>(
                                   VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)));
    create_info.presentMode = present_mode;
    create_info.clipped = VK_TRUE;
    if (vkCreateSwapchainKHR(device(), &create_info, nullptr, &swapchain) !=
        VK_SUCCESS) {
        return core::fail(kErrSwapchain, "gfx", "vkCreateSwapchainKHR failed");
    }

    uint32_t actual_count = 0;
    if (vkGetSwapchainImagesKHR(device(), swapchain, &actual_count, nullptr) !=
        VK_SUCCESS) {
        return core::fail(kErrImages, "gfx", "vkGetSwapchainImagesKHR failed");
    }
    image_count = actual_count;
    images.resize(image_count);
    if (vkGetSwapchainImagesKHR(device(), swapchain, &actual_count, images.data()) !=
        VK_SUCCESS) {
        return core::fail(kErrImages, "gfx", "vkGetSwapchainImagesKHR failed");
    }

    views.resize(image_count);
    for (uint32_t i = 0; i < image_count; ++i) {
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = images[i];
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = format;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device(), &view_info, nullptr, &views[i]) !=
            VK_SUCCESS) {
            return core::fail(kErrView, "gfx", "vkCreateImageView failed");
        }
    }

    // Depth attachment, same extent as the color images. VMA owns the
    // image; the view takes the DEPTH aspect only (a D24S8 stencil half
    // stays unused, the standard depth-only view). No barrier needed -
    // the render pass's UNDEFINED initialLayout transition covers first
    // use, exactly like the color attachment.
    VkImageCreateInfo depth_info{};
    depth_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    depth_info.imageType = VK_IMAGE_TYPE_2D;
    depth_info.format = depth_format();
    depth_info.extent.width = extent.width;
    depth_info.extent.height = extent.height;
    depth_info.extent.depth = 1;
    depth_info.mipLevels = 1;
    depth_info.arrayLayers = 1;
    depth_info.samples = VK_SAMPLE_COUNT_1_BIT;
    depth_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    depth_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    VmaAllocationCreateInfo depth_alloc{};
    depth_alloc.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(allocator(), &depth_info, &depth_alloc, &depth_image,
                       &depth_allocation, nullptr) != VK_SUCCESS) {
        return core::fail(kErrDepthImage, "gfx", "vmaCreateImage (depth) failed");
    }
    VkImageViewCreateInfo depth_view_info{};
    depth_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    depth_view_info.image = depth_image;
    depth_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    depth_view_info.format = depth_format();
    depth_view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depth_view_info.subresourceRange.levelCount = 1;
    depth_view_info.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device(), &depth_view_info, nullptr, &depth_view) !=
        VK_SUCCESS) {
        return core::fail(kErrView, "gfx", "vkCreateImageView (depth) failed");
    }

    // --- Phase 4: the sample-count decision, before any pass geometry ---
    // A pass that samples BOTH the color and depth attachments only runs
    // at a count both support, so the mask is the limits' AND. The pure
    // chooser (swapchain_choice.h) walks 8/4/2 against it, flag-gated -
    // a count the flag could not get is the device's own answer, not an
    // error (the bindless probe's shape). The depth image above was
    // created at 1 sample: an MSAA pass needs it at the chosen count, so
    // it is destroyed and recreated when the count is useful.
    VkPhysicalDeviceProperties device_properties{};
    vkGetPhysicalDeviceProperties(physical(), &device_properties);
    const VkPhysicalDeviceLimits& limits = device_properties.limits;
    const uint32_t supported =
        limits.framebufferColorSampleCounts & limits.framebufferDepthSampleCounts;
    const uint32_t chosen_count = choose_sample_count(
        options.want_msaa, supported,
        static_cast<uint32_t>(VK_SAMPLE_COUNT_1_BIT));
    sample_count = static_cast<VkSampleCountFlagBits>(chosen_count);
    const bool msaa =
        chosen_count != static_cast<uint32_t>(VK_SAMPLE_COUNT_1_BIT);
    if (msaa) {
        // The depth attachment recreated at the pass's count (the pass
        // samples it; the 1-sample image above was the flag-off shape).
        if (depth_image != VK_NULL_HANDLE) {
            vmaDestroyImage(allocator(), depth_image, depth_allocation);
            depth_image = VK_NULL_HANDLE;
            depth_allocation = VK_NULL_HANDLE;
        }
        VkImageCreateInfo depth_msaa_info = depth_info;
        depth_msaa_info.samples = sample_count;
        VmaAllocationCreateInfo depth_msaa_alloc{};
        depth_msaa_alloc.usage = VMA_MEMORY_USAGE_AUTO;
        if (vmaCreateImage(allocator(), &depth_msaa_info, &depth_msaa_alloc,
                           &depth_image, &depth_allocation, nullptr) !=
            VK_SUCCESS) {
            return core::fail(kErrDepthImage, "gfx",
                              "vmaCreateImage (depth, msaa) failed");
        }
        // The MSAA color target: extent == color, format == color,
        // samples == the chosen count, one view. The swapchain images
        // keep their single-sample views - they become the pass's
        // RESOLVE targets, written by the resolve at end of subpass and
        // read at present. TRANSIENT keeps the tile memory lazily
        // allocated (the image never leaves the GPU).
        VkImageCreateInfo msaa_info{};
        msaa_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        msaa_info.imageType = VK_IMAGE_TYPE_2D;
        msaa_info.format = format;
        msaa_info.extent.width = extent.width;
        msaa_info.extent.height = extent.height;
        msaa_info.extent.depth = 1;
        msaa_info.mipLevels = 1;
        msaa_info.arrayLayers = 1;
        msaa_info.samples = sample_count;
        msaa_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        msaa_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                           VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
        VmaAllocationCreateInfo msaa_alloc{};
        msaa_alloc.usage = VMA_MEMORY_USAGE_AUTO;
        if (vmaCreateImage(allocator(), &msaa_info, &msaa_alloc, &msaa_image,
                           &msaa_allocation, nullptr) != VK_SUCCESS) {
            return core::fail(kErrMsaaImage, "gfx", "vmaCreateImage (msaa) failed");
        }
        VkImageViewCreateInfo msaa_view_info{};
        msaa_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        msaa_view_info.image = msaa_image;
        msaa_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        msaa_view_info.format = format;
        msaa_view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        msaa_view_info.subresourceRange.levelCount = 1;
        msaa_view_info.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device(), &msaa_view_info, nullptr, &msaa_view) !=
            VK_SUCCESS) {
            return core::fail(kErrView, "gfx", "vkCreateImageView (msaa) failed");
        }
    }

    VkAttachmentDescription color{};
    color.format = format;
    color.samples = sample_count;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    // The resolve target: the swapchain image, single-sample, presented
    // after the pass. DONT_CARE ops (the resolve writes it, nothing
    // loads), UNDEFINED initial (the resolve is the first write), and
    // PRESENT as the final layout - the one the presentation engine
    // reads.
    VkAttachmentDescription resolve{};
    resolve.format = format;
    resolve.samples = VK_SAMPLE_COUNT_1_BIT;
    resolve.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    resolve.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    resolve.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    resolve.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    resolve.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    resolve.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    // Cleared to 1.0 each frame, never read after the pass (only the
    // subpass's own test/write touches it) - DONT_CARE keeps the store
    // free, and the pass's UNDEFINED transition puts it into layout.
    VkAttachmentDescription depth{};
    depth.format = depth_format();
    depth.samples = sample_count;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    // depth_ref's INDEX differs per shape: slot 2 in the MSAA table
    // (color 0, resolve 1, depth 2), slot 1 in the classic two-slot
    // pass. One reference per shape, spelled below beside the tables.
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    VkAttachmentReference resolve_ref{1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth_ref_msaa{2,
                                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth_ref_plain{
        1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    subpass.pDepthStencilAttachment =
        msaa ? &depth_ref_msaa : &depth_ref_plain;
    subpass.pResolveAttachments = msaa ? &resolve_ref : nullptr;

    VkAttachmentDescription attachments_msaa[3] = {color, resolve, depth};
    VkAttachmentDescription attachments_plain[2] = {color, depth};
    attachments_plain[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments_plain[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentDescription* const attachment_table =
        msaa ? attachments_msaa : attachments_plain;
    const uint32_t attachment_count = msaa ? 3u : 2u;

    // External dependencies, the frames-in-flight contract spelled out
    // for the driver. Without them the pass is a WAW hazard: consecutive
    // frames on the queue would race on the attachments, the shared
    // depth image above all.
    // (1) color: the acquire's semaphore (waited at this stage by the
    //     submit) orders the presentation engine's last read of THIS
    //     image; the dependency chains the load-clear after it.
    //     srcAccess 0: nothing of ours runs before that wait.
    // (2) depth: ONE image backs every framebuffer, so back-to-back
    //     frames are WAW (this frame's clear vs. the previous frame's
    //     depth writes) and WAR (vs. its depth tests). The execution
    //     chain from EARLY/LATE_FRAGMENT_TESTS to EARLY/LATE plus the
    //     depth-write availability flush serializes consecutive passes
    //     on the single graphics queue - that barrier IS what makes the
    //     single shared image safe; per-frame depth images would only
    //     buy back its small serialization bubble.
    VkSubpassDependency dependencies[2]{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].srcAccessMask = 0;
    dependencies[0].dstStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].dstSubpass = 0;
    dependencies[1].srcStageMask =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].srcAccessMask =
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstStageMask =
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].dstAccessMask =
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo render_pass_info{};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_info.attachmentCount = attachment_count;
    render_pass_info.pAttachments = attachment_table;
    render_pass_info.subpassCount = 1;
    render_pass_info.pSubpasses = &subpass;
    render_pass_info.dependencyCount = 2;
    render_pass_info.pDependencies = dependencies;
    if (vkCreateRenderPass(device(), &render_pass_info, nullptr, &render_pass) !=
        VK_SUCCESS) {
        return core::fail(kErrRenderPass, "gfx", "vkCreateRenderPass failed");
    }

    framebuffers.resize(image_count);
    for (uint32_t i = 0; i < image_count; ++i) {
        // Every framebuffer binds the pass's attachments in table order:
        // the multisample color target and the resolve view (the
        // swapchain image's own view) in the MSAA shape, or the classic
        // color-plus-depth pair at one sample - the table's own count.
        VkImageView framebuffer_views_msaa[3] = {msaa_view, views[i],
                                                 depth_view};
        VkImageView framebuffer_views_plain[2] = {views[i], depth_view};
        VkFramebufferCreateInfo framebuffer_info{};
        framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebuffer_info.renderPass = render_pass;
        framebuffer_info.attachmentCount = attachment_count;
        framebuffer_info.pAttachments =
            msaa ? framebuffer_views_msaa : framebuffer_views_plain;
        framebuffer_info.width = extent.width;
        framebuffer_info.height = extent.height;
        framebuffer_info.layers = 1;
        if (vkCreateFramebuffer(device(), &framebuffer_info, nullptr,
                                &framebuffers[i]) != VK_SUCCESS) {
            return core::fail(kErrFramebuffer, "gfx", "vkCreateFramebuffer failed");
        }
    }

    // Frame slots: a FIXED count, independent of image_count - that
    // separation is the frames-in-flight model. The fences start
    // SIGNALED so the first wait of each slot passes immediately; the
    // acquire semaphore is re-signaled every acquire and retired by the
    // submit's wait, both gated by the slot fence.
    for (FrameSlot& slot : frame_slots) {
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.queueFamilyIndex = queue_family();
        if (vkCreateCommandPool(device(), &pool_info, nullptr, &slot.pool) !=
            VK_SUCCESS) {
            return core::fail(kErrSlotObjects, "gfx", "vkCreateCommandPool failed");
        }
        VkCommandBufferAllocateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        buffer_info.commandPool = slot.pool;
        buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        buffer_info.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device(), &buffer_info, &slot.buffer) !=
            VK_SUCCESS) {
            return core::fail(kErrSlotObjects, "gfx",
                              "vkAllocateCommandBuffers failed");
        }
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vkCreateFence(device(), &fence_info, nullptr, &slot.fence) !=
            VK_SUCCESS) {
            return core::fail(kErrSlotObjects, "gfx", "vkCreateFence failed");
        }
        VkSemaphoreCreateInfo semaphore_info{};
        semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(device(), &semaphore_info, nullptr,
                              &slot.acquired) != VK_SUCCESS) {
            return core::fail(kErrSlotObjects, "gfx", "vkCreateSemaphore failed");
        }
    }

    // Render-finished semaphores: one per swapchain image (see the member
    // comment). Counted with the swapchain, recreated with it.
    image_rendered.resize(image_count);
    for (VkSemaphore& semaphore : image_rendered) {
        VkSemaphoreCreateInfo semaphore_info{};
        semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(device(), &semaphore_info, nullptr, &semaphore) !=
            VK_SUCCESS) {
            return core::fail(kErrSlotObjects, "gfx", "vkCreateSemaphore failed");
        }
    }
    return {};
}

void Swapchain::Impl::destroy_objects() {
    // Destroys everything build() made. Safe on partial state and
    // idempotent - but only the STATE RESETS below the guard are
    // unconditional: every vk*/vma* call sits under a live device handle,
    // because with VK_NO_PROTOTYPES those are volk/VMA function pointers
    // that stay NULL until a device exists (a Swapchain that never
    // created - or whose Gpu already shut down - must tear down clean;
    // ~Swapchain calls destroy()).
    // The null-handle-no-op contract never gets a chance to apply
    // without the entry points, and frame_slots is a FIXED array - its
    // destroy loop cannot lean on emptiness the way the vectors can.
    if (gpu != nullptr && device() != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device());
        if (render_pass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device(), render_pass, nullptr);
        }
        for (VkFramebuffer framebuffer : framebuffers) {
            vkDestroyFramebuffer(device(), framebuffer, nullptr);
        }
        for (const FrameSlot& slot : frame_slots) {
            vkDestroySemaphore(device(), slot.acquired, nullptr);
            vkDestroyFence(device(), slot.fence, nullptr);
            vkDestroyCommandPool(device(), slot.pool, nullptr);
        }
        for (VkSemaphore semaphore : image_rendered) {
            vkDestroySemaphore(device(), semaphore, nullptr);
        }
        for (VkImageView view : views) {
            vkDestroyImageView(device(), view, nullptr);
        }
        if (depth_view != VK_NULL_HANDLE) {
            vkDestroyImageView(device(), depth_view, nullptr);
        }
        if (depth_image != VK_NULL_HANDLE) {
            // The allocator is a device-stage object - it outlives
            // every window-stage teardown, including this one.
            vmaDestroyImage(allocator(), depth_image, depth_allocation);
        }
        if (msaa_view != VK_NULL_HANDLE) {
            vkDestroyImageView(device(), msaa_view, nullptr);
        }
        if (msaa_image != VK_NULL_HANDLE) {
            vmaDestroyImage(allocator(), msaa_image, msaa_allocation);
        }
        if (swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device(), swapchain, nullptr);
        }
    }

    framebuffers.clear();
    image_rendered.clear();
    views.clear();
    images.clear();  // swapchain-owned; freed with the swapchain above
    for (FrameSlot& slot : frame_slots) {
        slot = FrameSlot{};  // handles back to null for the next build
    }
    depth_view = VK_NULL_HANDLE;
    depth_image = VK_NULL_HANDLE;
    depth_allocation = VK_NULL_HANDLE;
    msaa_view = VK_NULL_HANDLE;
    msaa_image = VK_NULL_HANDLE;
    msaa_allocation = VK_NULL_HANDLE;
    render_pass = VK_NULL_HANDLE;
    swapchain = VK_NULL_HANDLE;
    image_count = 0;
    frame_index = 0;
    suboptimal = false;
}

Swapchain::Swapchain() : impl_(std::make_unique<Impl>()) {}

Swapchain::~Swapchain() {
    destroy();
}

core::Status Swapchain::create(Gpu& gpu, int fb_width, int fb_height,
                               const SwapchainOptions& options) {
    if (impl_->swapchain != VK_NULL_HANDLE) {
        return core::fail(kErrOrder, "gfx",
                          "create() on an already-created Swapchain - "
                          "destroy() first");
    }
    if (gpu.device() == nullptr || gpu.surface() == 0) {
        return core::fail(kErrOrder, "gfx",
                          "create() needs a Gpu with its device stage up and "
                          "a surface attached");
    }
    impl_->gpu = &gpu;
    impl_->options = options;
    const core::Status built = impl_->build(fb_width, fb_height);
    if (!built) {
        destroy();  // failure-path cleanup; guarded + idempotent
    }
    return built;
}

void Swapchain::destroy() {
    impl_->destroy_objects();
}

core::Status Swapchain::recreate(int fb_width, int fb_height) {
    // A lifecycle call guards like create(): a never-created Swapchain or
    // one whose Gpu went down (device stage torn, surface detached) is a
    // Status, never a crash - the same null-state contract gpu.cpp keeps.
    if (impl_->gpu == nullptr || impl_->gpu->device() == nullptr ||
        impl_->gpu->surface() == 0) {
        return core::fail(kErrOrder, "gfx",
                          "recreate() before create(), or after its Gpu went "
                          "down - create() again instead");
    }
    destroy();
    const core::Status built = impl_->build(fb_width, fb_height);
    if (!built) {
        destroy();
    }
    return built;
}

AcquireStatus Swapchain::acquire(Frame* out) {
    if (impl_->swapchain == VK_NULL_HANDLE) {
        // Defensive (the consumer frames only with its window stage up):
        // an empty swapchain answers the bail, so the caller's recreate
        // path runs instead of a crash.
        return AcquireStatus::out_of_date;
    }
    FrameSlot& slot = impl_->frame_slots[impl_->frame_index];
    // The slot fence is the CPU throttle: waiting it means the previous
    // submission that used this slot (pool, command buffer, the
    // consumer's per-slot renderer state, the acquire semaphore) has
    // fully retired. kMaxFramesInFlight slots is what bounds frames in
    // flight - the swapchain image count has no say in it.
    vk_check(vkWaitForFences(impl_->device(), 1, &slot.fence, VK_TRUE, UINT64_MAX),
             "vkWaitForFences");

    // Acquire only after the slot retired: the semaphore this signals is
    // the one the slot's previous submission waited on, so signaling it
    // again before that wait executed would be a semaphore-in-use error.
    uint32_t acquired_index = 0;
    const VkResult err = vkAcquireNextImageKHR(
        impl_->device(), impl_->swapchain, UINT64_MAX, slot.acquired,
        VK_NULL_HANDLE, &acquired_index);
    if (err == VK_ERROR_OUT_OF_DATE_KHR) {
        // This frame cannot run: the acquire failed and signaled nothing.
        // The caller drops the commands submitted for the frame instead
        // of letting them leak into a later one (its queues and renderer
        // keep the drop-never-defer contract). The fence reset below
        // never ran, so the slot stays signaled and pristine for the
        // rebuilt swapchain's fresh slots.
        return AcquireStatus::out_of_date;
    }
    // VK_SUBOPTIMAL_KHR is a success code: the image IS acquired and
    // slot.acquired IS signaled, so the frame must run and present -
    // only a submit's semaphore wait consumes that signal, and the
    // rebuild's destroy would otherwise take a live signaled semaphore
    // down with the swapchain. Presenting a suboptimal swapchain is
    // valid; present() raises needs_rebuild once the frame has fully
    // retired.
    const bool was_suboptimal = (err == VK_SUBOPTIMAL_KHR);
    if (!was_suboptimal) {
        vk_check(err, "vkAcquireNextImageKHR");
    }
    // The slot is committed to this frame from here: reset the fence
    // AFTER the acquire so a bail above never leaves a never-submitted
    // unsignaled fence behind.
    vk_check(vkResetFences(impl_->device(), 1, &slot.fence), "vkResetFences");
    vk_check(vkResetCommandPool(impl_->device(), slot.pool, 0),
             "vkResetCommandPool");

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(slot.buffer, &begin_info), "vkBeginCommandBuffer");

    impl_->suboptimal = was_suboptimal;
    out->image_index = acquired_index;
    out->frame_index = impl_->frame_index;
    out->command_buffer = slot.buffer;
    return AcquireStatus::ok;
}

void Swapchain::begin_render_pass(const Frame& frame, const float clear_color[4]) {
    if (impl_->render_pass == VK_NULL_HANDLE) {
        return;  // never frames without a swapchain; guarded, not crashed
    }
    // The depth clear is 1.0 - far plane, the LEQUAL convention: a 3D
    // chunk's equal depths keep the submission order the consumer's
    // stable sort built.
    VkClearValue clears[2]{};
    clears[0].color.float32[0] = clear_color[0];
    clears[0].color.float32[1] = clear_color[1];
    clears[0].color.float32[2] = clear_color[2];
    clears[0].color.float32[3] = clear_color[3];
    clears[1].depthStencil.depth = 1.0f;
    clears[1].depthStencil.stencil = 0;

    VkRenderPassBeginInfo render_pass_info{};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_pass_info.renderPass = impl_->render_pass;
    render_pass_info.framebuffer = impl_->framebuffers[frame.image_index];
    render_pass_info.renderArea.extent = impl_->extent;
    render_pass_info.clearValueCount = 2;
    render_pass_info.pClearValues = clears;
    vkCmdBeginRenderPass(static_cast<VkCommandBuffer>(frame.command_buffer),
                         &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);
}

void Swapchain::end_render_pass(const Frame& frame) {
    if (impl_->render_pass == VK_NULL_HANDLE) {
        return;
    }
    vkCmdEndRenderPass(static_cast<VkCommandBuffer>(frame.command_buffer));
}

void Swapchain::submit(const Frame& frame) {
    if (impl_->swapchain == VK_NULL_HANDLE) {
        return;  // guarded like the rest of the frame flow
    }
    FrameSlot& slot = impl_->frame_slots[frame.frame_index];
    // Wait the acquire at the color-attachment stage (nothing earlier
    // touches the image), signal THIS image's render-finished semaphore
    // for its present, and the slot's fence for the next slot user.
    const VkPipelineStageFlags wait_stage =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkCommandBuffer buffer = static_cast<VkCommandBuffer>(frame.command_buffer);
    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = &slot.acquired;
    submit_info.pWaitDstStageMask = &wait_stage;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &buffer;
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores =
        &impl_->image_rendered[frame.image_index];
    vk_check(vkEndCommandBuffer(buffer), "vkEndCommandBuffer");
    vk_check(vkQueueSubmit(impl_->queue(), 1, &submit_info, slot.fence),
             "vkQueueSubmit");

    // Next frame takes the next slot - at most kMaxFramesInFlight
    // submissions are outstanding before someone waits a fence again.
    impl_->frame_index = (impl_->frame_index + 1) % kMaxFramesInFlight;
}

PresentStatus Swapchain::present(const Frame& frame) {
    if (impl_->swapchain == VK_NULL_HANDLE) {
        return PresentStatus::needs_rebuild;  // guarded: the recreate path
    }
    VkPresentInfoKHR present_info{};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores =
        &impl_->image_rendered[frame.image_index];
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &impl_->swapchain;
    present_info.pImageIndices = &frame.image_index;
    const VkResult err = vkQueuePresentKHR(impl_->queue(), &present_info);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
        return PresentStatus::needs_rebuild;
    }
    vk_check(err, "vkQueuePresentKHR");
    if (impl_->suboptimal) {
        // The acquire said the swapchain no longer matches the surface;
        // this frame presented fine, so recreate before the next acquire -
        // the image's semaphore is fully retired now (this present
        // consumed it).
        impl_->suboptimal = false;
        return PresentStatus::needs_rebuild;
    }
    return PresentStatus::ok;
}

void Swapchain::wait_frames() {
    // Guarded on every path that could hold no fences: never created,
    // destroyed (destroy_objects nulls the slots), or a Gpu already down.
    if (impl_->gpu == nullptr || impl_->device() == VK_NULL_HANDLE ||
        impl_->swapchain == VK_NULL_HANDLE) {
        return;  // nothing was ever submitted
    }
    for (const FrameSlot& slot : impl_->frame_slots) {
        vk_check(vkWaitForFences(impl_->device(), 1, &slot.fence, VK_TRUE,
                                 UINT64_MAX),
                 "vkWaitForFences");
    }
}

std::uint64_t Swapchain::render_pass() const {
    return handle_to_u64(impl_->render_pass);  // VkRenderPass
}

uint32_t Swapchain::format() const {
    return static_cast<uint32_t>(impl_->format);  // VkFormat, numeric
}

uint32_t Swapchain::present_mode() const {
    return static_cast<uint32_t>(impl_->present_mode);  // VkPresentModeKHR, numeric
}

uint32_t Swapchain::image_count() const {
    return impl_->image_count;
}

uint32_t Swapchain::width() const {
    return impl_->extent.width;
}

uint32_t Swapchain::height() const {
    return impl_->extent.height;
}

uint32_t Swapchain::sample_count() const {
    return static_cast<uint32_t>(impl_->sample_count);  // VkSampleCountFlagBits, numeric
}

}  // namespace ws::gfx
