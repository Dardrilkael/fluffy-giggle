// ws::ui::App - the application shell (GLFW window + Vulkan + Dear ImGui).
//
// All GLFW/Vulkan state lives in this file, behind App::Impl. The public
// header exposes no third-party types on purpose: apps subclass App and draw
// via ImGui/ImPlot inside on_setup()/on_frame().
//
// The frame flow mirrors Dear ImGui's example_glfw_vulkan (rotating acquire/
// present semaphore sets, recreate-on-out-of-date), but the swapchain code is
// ours - the ImGui_ImplVulkanH_* helpers are example-only per the backend's
// own docs, so this file owns its own swapchain objects instead.

#include <ws/ui/app.hpp>

#include <ws/core/log.hpp>

#include <volk.h>  // meta-loader: every vk* symbol below is a volk global

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <implot.h>

#include <vk_mem_alloc.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ws::ui {
namespace {

// Backend bookkeeping minimum. Must stay >= 2 (ImGui_ImplVulkan requirement)
// and never change while the backend is alive (it cannot update the value).
constexpr uint32_t kMinImageCount = 2;

// Swapchain format preference, most preferred first. SRGB_NONLINEAR is the
// only colorspace required by the spec, so we require it alongside.
constexpr std::array<VkFormat, 2> kPreferredFormats{
    VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};

// Descriptor sets for the font atlas plus a handful of app textures
// registered through ImGui_ImplVulkan_AddTexture().
constexpr uint32_t kDescriptorSetCount = 16;

void glfw_error(int code, const char* description) {
    core::error("ui: GLFW error {}: {}", code, description);
}

// Mid-frame Vulkan failures leave the shell with no state to roll back to
// (a command buffer may be half recorded, semaphores in flight), so log and
// abort - the same policy as Dear ImGui's own examples.
void vk_check(VkResult err, const char* what) {
    if (err != VK_SUCCESS) {
        core::error("ui: {} failed (VkResult {})", what, static_cast<int>(err));
        std::abort();
    }
}

// Handed to ImGui so backend-internal Vulkan failures surface in our logs.
void imgui_vk_check(VkResult err) {
    if (err >= 0) {
        return;
    }
    core::error("ui: ImGui Vulkan backend error (VkResult {})", static_cast<int>(err));
    std::abort();
}

std::string_view present_mode_name(VkPresentModeKHR mode) {
    switch (mode) {
        case VK_PRESENT_MODE_IMMEDIATE_KHR: return "immediate";
        case VK_PRESENT_MODE_MAILBOX_KHR: return "mailbox";
        case VK_PRESENT_MODE_FIFO_KHR: return "FIFO";
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO relaxed";
        default: return "other";
    }
}

}  // namespace

struct App::Impl {
    // Per-swapchain-image command recording resources.
    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer buffer = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;  // created SIGNALED: the first wait must not block
    };

    // Acquire/present semaphore pair. One extra set beyond the image count so
    // vkAcquireNextImageKHR never picks a set the previous frame may still be
    // presenting from (same scheme as example_glfw_vulkan).
    struct Sync {
        VkSemaphore acquired = VK_NULL_HANDLE;
        VkSemaphore rendered = VK_NULL_HANDLE;
    };

    AppConfig config;

    GLFWwindow* window = nullptr;
    bool glfw_initialized = false;

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    VkSurfaceKHR surface = VK_NULL_HANDLE;

    VmaAllocator allocator = VK_NULL_HANDLE;  // ready for future GPU-side data work
    VkDescriptorPool imgui_pool = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    VkExtent2D extent{};
    uint32_t image_count = 0;
    std::vector<VkImage> images;  // owned by the swapchain, not by us
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<Frame> frames;
    std::vector<Sync> syncs;
    uint32_t image_index = 0;  // swapchain image being rendered this frame
    uint32_t sync_index = 0;   // rotating acquire/present semaphore set
    bool rebuild_swapchain = false;

    bool imgui_glfw_up = false;
    bool imgui_vk_up = false;
    uint32_t imgui_image_count = 0;  // values the backend was initialized with;
    VkFormat imgui_format = VK_FORMAT_UNDEFINED;  // drift means it must be re-inited
    bool viewport_style_set = false;

    ~Impl() { shutdown(); }

    // --- startup ------------------------------------------------------------

    bool create_window() {
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        window = glfwCreateWindow(config.width, config.height, config.title.c_str(),
                                  nullptr, nullptr);
        if (window == nullptr) {
            core::error("ui: failed to create window \"{}\"", config.title);
            return false;
        }
        return true;
    }

    bool create_instance() {
        const VkResult err = volkInitialize();
        if (err != VK_SUCCESS) {
            core::error("ui: Vulkan loader not found (volkInitialize = VkResult {})",
                        static_cast<int>(err));
            return false;
        }

        uint32_t extension_count = 0;
        const char** extensions = glfwGetRequiredInstanceExtensions(&extension_count);

        // The shell needs nothing beyond WSI basics, so request the lowest API
        // version to run on the widest range of drivers.
        VkApplicationInfo app_info{};
        app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app_info.pApplicationName = config.title.c_str();
        app_info.apiVersion = VK_API_VERSION_1_0;

        VkInstanceCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        create_info.pApplicationInfo = &app_info;
        create_info.enabledExtensionCount = extension_count;
        create_info.ppEnabledExtensionNames = extensions;
        if (vkCreateInstance(&create_info, nullptr, &instance) != VK_SUCCESS) {
            core::error("ui: vkCreateInstance failed");
            return false;
        }
        volkLoadInstance(instance);
        return true;
    }

    bool create_surface() {
        if (glfwCreateWindowSurface(instance, window, nullptr, &surface) != VK_SUCCESS) {
            core::error("ui: failed to create the window surface");
            return false;
        }
        return true;
    }

    static bool device_has_swapchain(VkPhysicalDevice candidate) {
        uint32_t count = 0;
        vkEnumerateDeviceExtensionProperties(candidate, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> available(count);
        vkEnumerateDeviceExtensionProperties(candidate, nullptr, &count, available.data());
        for (const VkExtensionProperties& extension : available) {
            if (std::string_view(extension.extensionName) ==
                VK_KHR_SWAPCHAIN_EXTENSION_NAME) {
                return true;
            }
        }
        return false;
    }

    bool select_device() {
        uint32_t device_count = 0;
        if (vkEnumeratePhysicalDevices(instance, &device_count, nullptr) != VK_SUCCESS ||
            device_count == 0) {
            core::error("ui: no Vulkan-capable devices found");
            return false;
        }
        std::vector<VkPhysicalDevice> devices(device_count);
        vkEnumeratePhysicalDevices(instance, &device_count, devices.data());

        // A device qualifies when it has a queue family that can both draw and
        // present on our surface, plus the swapchain extension. One family
        // doing both is true for essentially every setup.
        struct Candidate {
            VkPhysicalDevice device;
            uint32_t family;
        };
        std::vector<Candidate> candidates;
        for (VkPhysicalDevice physical : devices) {
            uint32_t family_count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
            std::vector<VkQueueFamilyProperties> families(family_count);
            vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count,
                                                     families.data());
            for (uint32_t i = 0; i < family_count; ++i) {
                if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
                    continue;
                }
                VkBool32 can_present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, surface, &can_present);
                if (can_present != VK_TRUE || !device_has_swapchain(physical)) {
                    continue;
                }
                candidates.push_back({physical, i});
                break;
            }
        }
        if (candidates.empty()) {
            core::error("ui: no device can present to the window surface");
            return false;
        }

        // Prefer a discrete GPU, fall back to the first qualifier.
        const Candidate* chosen = &candidates[0];
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(chosen->device, &properties);
        for (const Candidate& candidate : candidates) {
            VkPhysicalDeviceProperties candidate_properties{};
            vkGetPhysicalDeviceProperties(candidate.device, &candidate_properties);
            if (candidate_properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                chosen = &candidate;
                properties = candidate_properties;
                break;
            }
        }

        physical_device = chosen->device;
        queue_family = chosen->family;
        core::info("ui: using GPU \"{}\"", properties.deviceName);
        return true;
    }

    bool create_device() {
        const float queue_priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info{};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = queue_family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &queue_priority;

        const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        create_info.queueCreateInfoCount = 1;
        create_info.pQueueCreateInfos = &queue_info;
        create_info.enabledExtensionCount = 1;
        create_info.ppEnabledExtensionNames = extensions;
        if (vkCreateDevice(physical_device, &create_info, nullptr, &device) != VK_SUCCESS) {
            core::error("ui: vkCreateDevice failed");
            return false;
        }
        volkLoadDevice(device);
        vkGetDeviceQueue(device, queue_family, 0, &queue);
        return true;
    }

    bool create_allocator() {
        // VMA fetches every other function at runtime through these two
        // (VMA_DYNAMIC_VULKAN_FUNCTIONS, see src/vma_impl.cpp) - since 3.2 they
        // must be handed over explicitly; VMA no longer picks up the globals
        // by itself. Create the allocator only after volkLoadInstance and
        // volkLoadDevice have filled the pointers.
        VmaVulkanFunctions functions{};
        functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;  // volk globals
        functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

        VmaAllocatorCreateInfo create_info{};
        create_info.physicalDevice = physical_device;
        create_info.device = device;
        create_info.instance = instance;
        create_info.vulkanApiVersion = VK_API_VERSION_1_0;
        create_info.pVulkanFunctions = &functions;
        if (vmaCreateAllocator(&create_info, &allocator) != VK_SUCCESS) {
            core::error("ui: failed to create the memory allocator");
            return false;
        }
        return true;
    }

    bool create_imgui_pool() {
        VkDescriptorPoolSize pool_size{};
        pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        pool_size.descriptorCount = kDescriptorSetCount;

        VkDescriptorPoolCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        // FREE_DESCRIPTOR_SET_BIT is a hard requirement of the backend.
        create_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        create_info.maxSets = kDescriptorSetCount;
        create_info.poolSizeCount = 1;
        create_info.pPoolSizes = &pool_size;
        if (vkCreateDescriptorPool(device, &create_info, nullptr, &imgui_pool) !=
            VK_SUCCESS) {
            core::error("ui: failed to create the ImGui descriptor pool");
            return false;
        }
        return true;
    }

    // --- swapchain ----------------------------------------------------------

    VkSurfaceFormatKHR choose_surface_format() const {
        uint32_t count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &count, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device, surface, &count,
                                             formats.data());
        if (formats.empty()) {
            return {VK_FORMAT_B8G8R8A8_UNORM, VK_COLORSPACE_SRGB_NONLINEAR_KHR};
        }
        // One entry with UNDEFINED format means "any format is acceptable".
        if (formats.size() == 1 && formats[0].format == VK_FORMAT_UNDEFINED) {
            return {VK_FORMAT_B8G8R8A8_UNORM, VK_COLORSPACE_SRGB_NONLINEAR_KHR};
        }
        for (VkFormat preferred : kPreferredFormats) {
            for (const VkSurfaceFormatKHR& surface_format : formats) {
                if (surface_format.format == preferred &&
                    surface_format.colorSpace == VK_COLORSPACE_SRGB_NONLINEAR_KHR) {
                    return surface_format;
                }
            }
        }
        return formats[0];  // driver knows best; virtually always BGRA8 anyway
    }

    VkPresentModeKHR choose_present_mode() const {
        uint32_t count = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &count,
                                                  nullptr);
        std::vector<VkPresentModeKHR> modes(count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device, surface, &count,
                                                 modes.data());
        const auto has = [modes](VkPresentModeKHR mode) {
            return std::find(modes.begin(), modes.end(), mode) != modes.end();
        };
        if (!config.vsync) {
            // Uncapped: mailbox first (still tear-free), then immediate.
            if (has(VK_PRESENT_MODE_MAILBOX_KHR)) {
                return VK_PRESENT_MODE_MAILBOX_KHR;
            }
            if (has(VK_PRESENT_MODE_IMMEDIATE_KHR)) {
                return VK_PRESENT_MODE_IMMEDIATE_KHR;
            }
            core::info("ui: no mailbox/immediate support, falling back to FIFO");
        }
        return VK_PRESENT_MODE_FIFO_KHR;  // the only mode the spec guarantees
    }

    static VkCompositeAlphaFlagBitsKHR choose_composite_alpha(
        const VkSurfaceCapabilitiesKHR& caps) {
        if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) {
            return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        }
        // Opaque is universal in practice; take whatever the surface allows.
        for (uint32_t bit = 1; bit != 0; bit <<= 1) {
            if (caps.supportedCompositeAlpha & bit) {
                return static_cast<VkCompositeAlphaFlagBitsKHR>(bit);
            }
        }
        return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    }

    // Creates the swapchain and everything tied to its extent: image views,
    // render pass, framebuffers and per-frame command/fence/semaphore objects.
    bool create_swapchain(int fb_width, int fb_height) {
        VkSurfaceCapabilitiesKHR caps{};
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device, surface, &caps) !=
            VK_SUCCESS) {
            core::error("ui: failed to query surface capabilities");
            return false;
        }

        // 0xFFFFFFFF means "match the window's current framebuffer size".
        if (caps.currentExtent.width != 0xFFFFFFFFu) {
            extent = caps.currentExtent;
        } else {
            extent.width = std::clamp(static_cast<uint32_t>(fb_width),
                                      caps.minImageExtent.width,
                                      caps.maxImageExtent.width);
            extent.height = std::clamp(static_cast<uint32_t>(fb_height),
                                       caps.minImageExtent.height,
                                       caps.maxImageExtent.height);
        }
        if (extent.width == 0 || extent.height == 0) {
            core::error("ui: swapchain extent is zero (window has no visible area)");
            return false;
        }

        const VkSurfaceFormatKHR surface_format = choose_surface_format();
        format = surface_format.format;
        present_mode = choose_present_mode();

        uint32_t image_request = caps.minImageCount + 1;  // room to render while presenting
        if (caps.maxImageCount != 0) {
            image_request = std::min(image_request, caps.maxImageCount);
        }

        VkSwapchainCreateInfoKHR create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        create_info.surface = surface;
        create_info.minImageCount = image_request;
        create_info.imageFormat = surface_format.format;
        create_info.imageColorSpace = surface_format.colorSpace;
        create_info.imageExtent = extent;
        create_info.imageArrayLayers = 1;
        create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        create_info.preTransform = caps.currentTransform;
        create_info.compositeAlpha = choose_composite_alpha(caps);
        create_info.presentMode = present_mode;
        create_info.clipped = VK_TRUE;
        if (vkCreateSwapchainKHR(device, &create_info, nullptr, &swapchain) !=
            VK_SUCCESS) {
            core::error("ui: vkCreateSwapchainKHR failed");
            return false;
        }

        uint32_t actual_count = 0;
        if (vkGetSwapchainImagesKHR(device, swapchain, &actual_count, nullptr) !=
            VK_SUCCESS) {
            core::error("ui: vkGetSwapchainImagesKHR failed");
            return false;
        }
        image_count = actual_count;
        images.resize(image_count);
        if (vkGetSwapchainImagesKHR(device, swapchain, &actual_count, images.data()) !=
            VK_SUCCESS) {
            core::error("ui: vkGetSwapchainImagesKHR failed");
            return false;
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
            if (vkCreateImageView(device, &view_info, nullptr, &views[i]) != VK_SUCCESS) {
                core::error("ui: vkCreateImageView failed");
                return false;
            }
        }

        VkAttachmentDescription color{};
        color.format = format;
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color_ref;

        VkRenderPassCreateInfo render_pass_info{};
        render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        render_pass_info.attachmentCount = 1;
        render_pass_info.pAttachments = &color;
        render_pass_info.subpassCount = 1;
        render_pass_info.pSubpasses = &subpass;
        if (vkCreateRenderPass(device, &render_pass_info, nullptr, &render_pass) !=
            VK_SUCCESS) {
            core::error("ui: vkCreateRenderPass failed");
            return false;
        }

        framebuffers.resize(image_count);
        for (uint32_t i = 0; i < image_count; ++i) {
            VkFramebufferCreateInfo framebuffer_info{};
            framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebuffer_info.renderPass = render_pass;
            framebuffer_info.attachmentCount = 1;
            framebuffer_info.pAttachments = &views[i];
            framebuffer_info.width = extent.width;
            framebuffer_info.height = extent.height;
            framebuffer_info.layers = 1;
            if (vkCreateFramebuffer(device, &framebuffer_info, nullptr,
                                    &framebuffers[i]) != VK_SUCCESS) {
                core::error("ui: vkCreateFramebuffer failed");
                return false;
            }
        }

        frames.resize(image_count);
        for (Frame& frame : frames) {
            VkCommandPoolCreateInfo pool_info{};
            pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pool_info.queueFamilyIndex = queue_family;
            if (vkCreateCommandPool(device, &pool_info, nullptr, &frame.pool) !=
                VK_SUCCESS) {
                core::error("ui: vkCreateCommandPool failed");
                return false;
            }
            VkCommandBufferAllocateInfo buffer_info{};
            buffer_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            buffer_info.commandPool = frame.pool;
            buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            buffer_info.commandBufferCount = 1;
            if (vkAllocateCommandBuffers(device, &buffer_info, &frame.buffer) !=
                VK_SUCCESS) {
                core::error("ui: vkAllocateCommandBuffers failed");
                return false;
            }
            VkFenceCreateInfo fence_info{};
            fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            if (vkCreateFence(device, &fence_info, nullptr, &frame.fence) != VK_SUCCESS) {
                core::error("ui: vkCreateFence failed");
                return false;
            }
        }

        syncs.resize(image_count + 1);
        for (Sync& sync : syncs) {
            VkSemaphoreCreateInfo semaphore_info{};
            semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            if (vkCreateSemaphore(device, &semaphore_info, nullptr, &sync.acquired) !=
                    VK_SUCCESS ||
                vkCreateSemaphore(device, &semaphore_info, nullptr, &sync.rendered) !=
                    VK_SUCCESS) {
                core::error("ui: vkCreateSemaphore failed");
                return false;
            }
        }
        return true;
    }

    // Destroys everything create_swapchain() made. Safe on partial state
    // (null handles are no-ops per the Vulkan spec) and idempotent.
    void destroy_swapchain_objects() {
        if (device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device);
        }
        if (render_pass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device, render_pass, nullptr);
        }
        for (VkFramebuffer framebuffer : framebuffers) {
            vkDestroyFramebuffer(device, framebuffer, nullptr);
        }
        for (const Frame& frame : frames) {
            vkDestroyFence(device, frame.fence, nullptr);
            vkDestroyCommandPool(device, frame.pool, nullptr);
        }
        for (const Sync& sync : syncs) {
            vkDestroySemaphore(device, sync.acquired, nullptr);
            vkDestroySemaphore(device, sync.rendered, nullptr);
        }
        for (VkImageView view : views) {
            vkDestroyImageView(device, view, nullptr);
        }
        if (swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device, swapchain, nullptr);
        }

        framebuffers.clear();
        frames.clear();
        syncs.clear();
        views.clear();
        images.clear();  // swapchain-owned; freed with the swapchain above
        render_pass = VK_NULL_HANDLE;
        swapchain = VK_NULL_HANDLE;
        image_count = 0;
        image_index = 0;
        sync_index = 0;
    }

    bool recreate_swapchain() {
        int fb_width = 0;
        int fb_height = 0;
        glfwGetFramebufferSize(window, &fb_width, &fb_height);
        destroy_swapchain_objects();
        if (!create_swapchain(fb_width, fb_height)) {
            return false;
        }
        rebuild_swapchain = false;

        // The backend bakes MinImageCount/RenderPass/its buffer ring at Init.
        // An identical format keeps the render passes (hence its pipeline)
        // compatible; if the image count or format changed it must be
        // re-initialized.
        if (image_count != imgui_image_count || format != imgui_format) {
            ImGui_ImplVulkan_Shutdown();
            imgui_vk_up = false;
            if (!init_imgui_backend()) {
                return false;
            }
        }
        return true;
    }

    // --- Dear ImGui ---------------------------------------------------------

    bool init_imgui_backend() {
        ImGui_ImplVulkan_InitInfo info{};
        info.Instance = instance;
        info.PhysicalDevice = physical_device;
        info.Device = device;
        info.QueueFamily = queue_family;
        info.Queue = queue;
        info.DescriptorPool = imgui_pool;
        info.RenderPass = render_pass;
        info.MinImageCount = kMinImageCount;  // never changes: the backend cannot
        info.ImageCount = image_count;         // update it after init
        info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        info.CheckVkResultFn = &imgui_vk_check;
        if (!ImGui_ImplVulkan_Init(&info)) {
            core::error("ui: ImGui Vulkan backend failed to initialize");
            return false;
        }
        imgui_image_count = image_count;
        imgui_format = format;
        imgui_vk_up = true;
        return true;
    }

    bool init_imgui(App& app) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImPlot::CreateContext();

        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        // Multi-viewport is off by default (still rough with the Vulkan
        // backend); apps may flip ImGuiConfigFlags_ViewportsEnable in
        // on_setup() - the main loop honors it either way.

        ImGui::StyleColorsDark();

        if (!ImGui_ImplGlfw_InitForVulkan(window, true)) {
            core::error("ui: ImGui GLFW backend failed to initialize");
            return false;
        }
        imgui_glfw_up = true;
        if (!init_imgui_backend()) {
            return false;
        }

        app.on_setup();
        return true;
    }

    // --- frame loop ---------------------------------------------------------

    void render_frame(ImDrawData* draw_data) {
        Sync& sync = syncs[sync_index];
        const VkResult err = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX,
                                                   sync.acquired, VK_NULL_HANDLE,
                                                   &image_index);
        if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
            rebuild_swapchain = true;  // present_frame() skips; next loop recreates
            return;
        }
        vk_check(err, "vkAcquireNextImageKHR");

        Frame& frame = frames[image_index];
        vk_check(vkWaitForFences(device, 1, &frame.fence, VK_TRUE, UINT64_MAX),
                 "vkWaitForFences");
        vk_check(vkResetFences(device, 1, &frame.fence), "vkResetFences");
        vk_check(vkResetCommandPool(device, frame.pool, 0), "vkResetCommandPool");

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk_check(vkBeginCommandBuffer(frame.buffer, &begin_info), "vkBeginCommandBuffer");

        VkClearValue clear{};  // dark slate background; apps draw their UI on top
        clear.color.float32[0] = 0.08f;
        clear.color.float32[1] = 0.08f;
        clear.color.float32[2] = 0.10f;
        clear.color.float32[3] = 1.0f;

        VkRenderPassBeginInfo render_pass_info{};
        render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        render_pass_info.renderPass = render_pass;
        render_pass_info.framebuffer = framebuffers[image_index];
        render_pass_info.renderArea.extent = extent;
        render_pass_info.clearValueCount = 1;
        render_pass_info.pClearValues = &clear;
        vkCmdBeginRenderPass(frame.buffer, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);

        ImGui_ImplVulkan_RenderDrawData(draw_data, frame.buffer);

        vkCmdEndRenderPass(frame.buffer);

        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.waitSemaphoreCount = 1;
        submit_info.pWaitSemaphores = &sync.acquired;
        submit_info.pWaitDstStageMask = &wait_stage;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &frame.buffer;
        submit_info.signalSemaphoreCount = 1;
        submit_info.pSignalSemaphores = &sync.rendered;
        vk_check(vkEndCommandBuffer(frame.buffer), "vkEndCommandBuffer");
        vk_check(vkQueueSubmit(queue, 1, &submit_info, frame.fence), "vkQueueSubmit");
    }

    void present_frame() {
        if (rebuild_swapchain) {
            return;
        }
        Sync& sync = syncs[sync_index];
        VkPresentInfoKHR present_info{};
        present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present_info.waitSemaphoreCount = 1;
        present_info.pWaitSemaphores = &sync.rendered;
        present_info.swapchainCount = 1;
        present_info.pSwapchains = &swapchain;
        present_info.pImageIndices = &image_index;
        const VkResult err = vkQueuePresentKHR(queue, &present_info);
        if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
            rebuild_swapchain = true;
            return;
        }
        vk_check(err, "vkQueuePresentKHR");
        sync_index = (sync_index + 1) % static_cast<uint32_t>(syncs.size());
    }

    int main_loop(App& app) {
        while (glfwWindowShouldClose(window) == GLFW_FALSE) {
            glfwPollEvents();

            // Recreate when the framebuffer size changed or Vulkan flagged the
            // swapchain out of date. Skip while the size is zero (iconified).
            int fb_width = 0;
            int fb_height = 0;
            glfwGetFramebufferSize(window, &fb_width, &fb_height);
            const bool size_changed = fb_width != static_cast<int>(extent.width) ||
                                      fb_height != static_cast<int>(extent.height);
            if (fb_width > 0 && fb_height > 0 && (rebuild_swapchain || size_changed)) {
                if (!recreate_swapchain()) {
                    return 1;
                }
            }
            if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) == GLFW_TRUE) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            // If the app enabled viewports in on_setup(), align platform-window
            // styling once (rounding/transparency artifacts otherwise).
            if (!viewport_style_set &&
                (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0) {
                ImGuiStyle& style = ImGui::GetStyle();
                style.WindowRounding = 0.0f;
                style.Colors[ImGuiCol_WindowBg].w = 1.0f;
                viewport_style_set = true;
            }

            ImGui_ImplVulkan_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            try {
                app.on_frame();
            } catch (const std::exception& e) {
                core::error("ui: exception in on_frame(): {}", e.what());
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            } catch (...) {
                core::error("ui: unknown exception in on_frame()");
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }

            ImGui::Render();
            ImDrawData* draw_data = ImGui::GetDrawData();
            const bool hidden =
                draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f;
            if (!hidden) {
                render_frame(draw_data);
            }

            if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
                ImGui::UpdatePlatformWindows();
                ImGui::RenderPlatformWindowsDefault();
            }
            if (!hidden) {
                present_frame();
            }
        }
        return 0;
    }

    // --- lifecycle ----------------------------------------------------------

    bool startup(App& app) {
        if (config.width < 1 || config.height < 1) {
            core::error("ui: invalid initial window size {}x{}", config.width,
                        config.height);
            return false;
        }
        if (!create_window() || !create_instance() || !create_surface() ||
            !select_device() || !create_device() || !create_allocator() ||
            !create_imgui_pool()) {
            return false;
        }
        int fb_width = 0;
        int fb_height = 0;
        glfwGetFramebufferSize(window, &fb_width, &fb_height);
        if (fb_width <= 0 || fb_height <= 0 || !create_swapchain(fb_width, fb_height)) {
            return false;
        }
        if (!init_imgui(app)) {
            return false;
        }
        core::info("ui: \"{}\" ready: {}x{}, {} swapchain images, {} present",
                   config.title, extent.width, extent.height, image_count,
                   present_mode_name(present_mode));
        return true;
    }

    // Idempotent: every destroy is guarded, so it doubles as the failure-path
    // cleanup for partially completed startup.
    void shutdown() {
        if (imgui_vk_up) {
            ImGui_ImplVulkan_Shutdown();
            imgui_vk_up = false;
        }
        if (imgui_glfw_up) {
            ImGui_ImplGlfw_Shutdown();
            imgui_glfw_up = false;
        }
        // Backend shutdown touches ImGui::GetIO(), so the contexts go last.
        if (ImPlot::GetCurrentContext() != nullptr) {
            ImPlot::DestroyContext();
        }
        if (ImGui::GetCurrentContext() != nullptr) {
            ImGui::DestroyContext();
        }

        destroy_swapchain_objects();

        if (allocator != VK_NULL_HANDLE) {
            vmaDestroyAllocator(allocator);
            allocator = VK_NULL_HANDLE;
        }
        if (imgui_pool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device, imgui_pool, nullptr);
            imgui_pool = VK_NULL_HANDLE;
        }
        if (surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance, surface, nullptr);
            surface = VK_NULL_HANDLE;
        }
        if (device != VK_NULL_HANDLE) {
            vkDestroyDevice(device, nullptr);
            device = VK_NULL_HANDLE;
        }
        if (instance != VK_NULL_HANDLE) {
            vkDestroyInstance(instance, nullptr);
            instance = VK_NULL_HANDLE;
            // Must be last: this zeroes every function pointer volk loaded,
            // so no vk* call may happen after it.
            volkFinalize();
        }
        if (window != nullptr) {
            glfwDestroyWindow(window);
            window = nullptr;
        }
        if (glfw_initialized) {
            glfwTerminate();
            glfw_initialized = false;
        }
    }

    int run(App& app) {
        glfwSetErrorCallback(&glfw_error);
        if (!glfwInit()) {
            core::error("ui: glfwInit failed");
            return 1;
        }
        glfw_initialized = true;

        int result = 1;
        if (startup(app)) {
            result = main_loop(app);
        }
        shutdown();
        return result;
    }
};

App::App(AppConfig config) : impl_(std::make_unique<Impl>()) {
    impl_->config = std::move(config);
}

App::~App() = default;

int App::run() {
    return impl_->run(*this);
}

}  // namespace ws::ui
