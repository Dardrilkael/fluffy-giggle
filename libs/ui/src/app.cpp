// ws::ui::App - the application shell (window + Vulkan + Dear ImGui).
//
// All platform/Vulkan state lives behind App::Impl. This file is
// windowing-agnostic: it talks to the OS only through the WindowHost seam
// (src/window.h - window_glfw.cpp on desktop, window_android.cpp on
// Android), and no GLFW/Android symbol appears here. The public header
// exposes no third-party types on purpose: apps subclass App and react to
// events / draw via on_setup()/on_event()/on_update().
//
// The frame flow mirrors Dear ImGui's example_glfw_vulkan (rotating acquire/
// present semaphore sets, recreate-on-out-of-date), but the swapchain code is
// ours - the ImGui_ImplVulkanH_* helpers are example-only per the backend's
// own docs, so this file owns its own swapchain objects instead.
//
// Startup splits into two stages so the shell survives Android's surface
// loss (APP_CMD_TERM_WINDOW takes the ANativeWindow away at any pause):
// the context stage (platform host + VkInstance) runs once, while the
// window stage (surface, device, swapchain, renderer, ImGui backends)
// attaches and detaches with the window - on Android it re-runs whenever
// the OS hands a window back.

#include <ws/ui/app.hpp>
#include <ws/ui/event.hpp>
#include <ws/ui/render_command_queue.hpp>

#include <ws/core/log.hpp>

#include "gui.h"
#include "renderer2d.h"
#include "shader_provider.h"
#include "window.h"

#include <volk.h>  // meta-loader: every vk* symbol below is a volk global

#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <implot.h>

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

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
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

// The depth attachment's format: D32 -> D24S8 -> D16, first with
// DEPTH_STENCIL_ATTACHMENT support in optimal tiling. Queried ONCE at
// device selection and cached - every render pass the process ever creates
// is built against this format, so swapchain rebuilds stay render-pass
// compatible with the ImGui backend's baked pipeline.
VkFormat choose_depth_format(VkPhysicalDevice physical) {
    const VkFormat preferred[] = {VK_FORMAT_D32_SFLOAT,
                                  VK_FORMAT_D24_UNORM_S8_UINT,
                                  VK_FORMAT_D16_UNORM};
    for (VkFormat candidate : preferred) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physical, candidate, &properties);
        if (properties.optimalTilingFeatures &
            VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            return candidate;
        }
    }
    return VK_FORMAT_UNDEFINED;
}

// Every concrete event type. The per-frame queue holds events by value -
// no heap per event, and no vtable anywhere in the event system.
using AnyEvent = std::variant<WindowCloseEvent, WindowResizeEvent, KeyPressedEvent,
                             KeyReleasedEvent, MouseButtonPressedEvent,
                             MouseButtonReleasedEvent, MouseMovedEvent,
                             MouseScrolledEvent, TouchPressedEvent,
                             TouchMovedEvent, TouchReleasedEvent, TextInputEvent>;

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

    // The windowing platform behind the seam (src/window.h): GLFW on
    // desktop, native_app_glue on Android. Window interactions below go
    // through it; no GLFW/Android symbol appears in this file.
    std::unique_ptr<WindowHost> host_;

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
    // The depth attachment, one per swapchain (extent == color): VMA owns
    // the image (the color images are swapchain-owned), the view is ours.
    // The format is chosen ONCE at device selection, so every render pass
    // ever built uses the same depth format.
    VkImage depth_image = VK_NULL_HANDLE;
    VmaAllocation depth_allocation = VK_NULL_HANDLE;
    VkImageView depth_view = VK_NULL_HANDLE;
    VkFormat depth_format = VK_FORMAT_UNDEFINED;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<Frame> frames;
    std::vector<Sync> syncs;
    uint32_t image_index = 0;  // swapchain image being rendered this frame
    uint32_t sync_index = 0;   // rotating acquire/present semaphore set
    bool rebuild_swapchain = false;
    // The acquire returned VK_SUBOPTIMAL_KHR: the frame still runs and
    // presents (the image IS acquired, its semaphore IS signaled), and the
    // swapchain is recreated only after that frame has retired.
    bool swapchain_suboptimal = false;

    // Stage bookkeeping for the split startup (see the file comment): the
    // context stage (host + instance) runs once; the device stage (device,
    // allocator, ImGui pool) and the window stage (surface + swapchain +
    // backends) attach to the current window - and re-attach on Android
    // whenever the OS hands one back. The r2d renderer, the ImGui/ImPlot
    // contexts and the gui input publication (gpu_context_up) are created
    // at the FIRST attach and survive a surface loss: only the backends
    // and the swapchain ride the window, so a re-attach re-initializes
    // those alone (the drift rules), never the full GPU init again.
    bool device_stage_up = false;
    bool window_stage_up = false;
    bool gpu_context_up = false;  // renderer + ImGui contexts + gui publication
    bool imgui_platform_up = false;  // GLFW / Android platform backend
    bool imgui_vk_up = false;
    uint32_t imgui_image_count = 0;  // values the backend was initialized with;
    VkFormat imgui_format = VK_FORMAT_UNDEFINED;  // drift means it must be re-inited
    bool viewport_style_set = false;

    // --- event system -------------------------------------------------------
    // Platform callbacks (src/window_glfw.cpp / src/window_android.cpp)
    // funnel every event through the sink;
    // forward_event() pre-marks ImGui-consumed input, queues the rest and
    // coalesces mouse-move/scroll/touch-move per frame (move keeps the
    // latest position, scroll sums its deltas, each touch keeps its own
    // latest move). deliver_events() then drives App::on_event,
    // once per queued event, before on_update().
    WindowEventSink event_sink_;
    std::vector<AnyEvent> event_queue_;
    std::optional<MouseMovedEvent> pending_mouse_move_;  // coalesced: latest wins
    std::optional<MouseScrolledEvent> pending_scroll_;   // coalesced: deltas summed
    // One pending move per touch contact, latest wins - every moving finger
    // keeps its own slot, so no finger's final position is lost at a busy
    // frame boundary (pending_mouse_move_'s rule, per pointer id).
    std::array<std::optional<TouchMovedEvent>, gui::InputFrame::kMaxTouches>
        pending_touch_moves_{};
    bool touch_move_overflow_warned_ = false;

    // --- render command queue -----------------------------------------------
    // The app fills write_queue_ during on_update(); the render step swaps
    // and executes read_queue_ against the frame's command buffer. Kept as
    // two so moving app logic off the render thread later needs no API
    // change. Arena capacity survives the swap (the high-water mark).
    RenderCommandQueue write_queue_;
    RenderCommandQueue read_queue_;

    // --- 2D renderer ---------------------------------------------------------
    // The shell-owned batch renderer behind the r2d free functions. Apps
    // record draws through ws::ui::r2d during on_update(); the shell runs
    // frame_begin/frame_end around it and flushes the sorted batch inside
    // the render pass, before ImGui's draw data (2D world under widgets).
    r2d::Renderer2D renderer2d_;

    // --- gui library --------------------------------------------------------
    // One input snapshot per frame, filled by the platform callbacks' raw
    // tap (src/window_glfw.cpp / src/window_android.cpp, through
    // WindowEventSink::input). main_loop clears
    // the per-frame edges at the top of each iteration; on_update reads
    // the snapshot through ws::ui::gui::frame_input() for the duration of
    // the run.
    gui::InputFrame gui_input_;

    ~Impl() { shutdown(); }

    // --- event system --------------------------------------------------------

    // True when ImGui owns this input (pointer over a widget, text field
    // focused, ...). Deliberately evaluated at event time - the flags were
    // recomputed at the last NewFrame, which is the standard Hazel-style
    // approximation. Window close/resize never count as captured.
    bool imgui_wants(const Event& event) const {
        if (ImGui::GetCurrentContext() == nullptr) {
            return false;  // events before init_imgui(): nothing owns input yet
        }
        const ImGuiIO& io = ImGui::GetIO();
        switch (event.type()) {
            case EventType::KeyPressed:
            case EventType::KeyReleased:
            case EventType::TextInput:
                return io.WantCaptureKeyboard;
            case EventType::MouseButtonPressed:
            case EventType::MouseButtonReleased:
            case EventType::MouseMoved:
            case EventType::MouseScrolled:
                return io.WantCaptureMouse;
            case EventType::TouchPressed:
            case EventType::TouchMoved:
            case EventType::TouchReleased:
                // Never gated: ImGui is fed by the primary-touch bridge
                // (the platform mirrors it into io), not by these - so a
                // widget over the finger keeps its unfiltered stream.
                return false;
            default:
                return false;
        }
    }

    // Single funnel for every window/input event. ImGui-captured input is
    // pre-marked handled (the app's dispatcher skips it, but still observes
    // the event); resize also flags the swapchain rebuild here, before the
    // app gets a chance to consume the event.
    void forward_event(Event& event) {
        if (imgui_wants(event)) {
            event.handled = true;
        }
        // No default: /w44062 (cmake/WsWarnings.cmake; the -Wswitch
        // counterpart on gcc/clang) fails the build if a new EventType is
        // added without a case here, so no event can be silently dropped.
        // A new event also needs an AnyEvent alternative above - the
        // variant rejects types it cannot hold.
        switch (event.type()) {
            case EventType::WindowResize:
                rebuild_swapchain = true;
                event_queue_.push_back(static_cast<WindowResizeEvent&>(event));
                break;
            case EventType::MouseMoved:
                pending_mouse_move_ = static_cast<MouseMovedEvent&>(event);
                break;
            case EventType::MouseScrolled: {
                // Offsets are per-event deltas, not positions: sum them, or
                // ticks arriving between frames would be lost.
                const MouseScrolledEvent& scrolled =
                    static_cast<MouseScrolledEvent&>(event);
                if (pending_scroll_) {
                    pending_scroll_->x_offset += scrolled.x_offset;
                    pending_scroll_->y_offset += scrolled.y_offset;
                } else {
                    pending_scroll_ = scrolled;
                }
                break;
            }
            case EventType::WindowClose:
                event_queue_.push_back(static_cast<WindowCloseEvent&>(event));
                break;
            case EventType::KeyPressed:
                event_queue_.push_back(static_cast<KeyPressedEvent&>(event));
                break;
            case EventType::KeyReleased:
                event_queue_.push_back(static_cast<KeyReleasedEvent&>(event));
                break;
            case EventType::MouseButtonPressed:
                event_queue_.push_back(static_cast<MouseButtonPressedEvent&>(event));
                break;
            case EventType::MouseButtonReleased:
                event_queue_.push_back(static_cast<MouseButtonReleasedEvent&>(event));
                break;
            case EventType::TouchPressed:
                event_queue_.push_back(static_cast<TouchPressedEvent&>(event));
                break;
            case EventType::TouchMoved: {
                // Latest position per pointer id - pending_mouse_move_'s
                // rule, one slot per contact. A table with no id match and
                // no free slot means more distinct movers than the touch
                // cap ever admits (hand-rolled emitters only): drop + warn
                // once.
                const TouchMovedEvent& moved =
                    static_cast<TouchMovedEvent&>(event);
                std::optional<TouchMovedEvent>* slot = nullptr;
                for (auto& pending : pending_touch_moves_) {
                    if (pending && pending->pointer_id == moved.pointer_id) {
                        slot = &pending;  // this contact's coalescing slot
                        break;
                    }
                    if (!pending && slot == nullptr) {
                        slot = &pending;  // first free slot, if no id matches
                    }
                }
                if (slot != nullptr) {
                    *slot = moved;
                } else if (!touch_move_overflow_warned_) {
                    touch_move_overflow_warned_ = true;
                    core::warn("ui: touch-move coalescing overfull - a move "
                               "was dropped");
                }
                break;
            }
            case EventType::TouchReleased:
                event_queue_.push_back(static_cast<TouchReleasedEvent&>(event));
                break;
            case EventType::TextInput:
                // One event per glyph, arrival order - typing is low-rate
                // and order-preserving, so no coalescing.
                event_queue_.push_back(static_cast<TextInputEvent&>(event));
                break;
        }
    }

    // Coalesced events flush right before delivery, so the app sees exactly
    // one MouseMovedEvent / one MouseScrolledEvent per frame even when the
    // OS delivered dozens. Touch moves flush the same way - one per moving
    // contact, in first-move slot order (deterministic). Their order
    // relative to that frame's discrete events is not preserved.
    void flush_coalesced() {
        if (pending_mouse_move_) {
            event_queue_.push_back(std::move(*pending_mouse_move_));
            pending_mouse_move_.reset();
        }
        if (pending_scroll_) {
            event_queue_.push_back(std::move(*pending_scroll_));
            pending_scroll_.reset();
        }
        for (auto& pending : pending_touch_moves_) {
            if (pending) {
                event_queue_.push_back(std::move(*pending));
                pending.reset();
            }
        }
    }

    // App::on_event runs synchronously on the main thread, before this
    // frame's on_update(), once per queued event. Exceptions are caught,
    // logged, and close the window cleanly.
    void deliver_events(App& app) {
        flush_coalesced();
        for (AnyEvent& event : event_queue_) {
            try {
                std::visit([&app](auto& concrete) { app.on_event(concrete); },
                           event);
            } catch (const std::exception& e) {
                core::error("ui: exception in on_event(): {}", e.what());
                host_->request_stop();
            } catch (...) {
                core::error("ui: unknown exception in on_event()");
                host_->request_stop();
            }
        }
        event_queue_.clear();
    }

    bool create_instance() {
        const VkResult err = volkInitialize();
        if (err != VK_SUCCESS) {
            core::error("ui: Vulkan loader not found (volkInitialize = VkResult {})",
                        static_cast<int>(err));
            return false;
        }

        // WSI extensions per platform: whatever the host needs to build its
        // surface (GLFW's required set on desktop, surface + android_surface
        // on Android).
        const std::vector<const char*> extensions = host_->instance_extensions();

        // The shell needs nothing beyond WSI basics, so request the lowest API
        // version to run on the widest range of drivers.
        VkApplicationInfo app_info{};
        app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app_info.pApplicationName = config.title.c_str();
        app_info.apiVersion = VK_API_VERSION_1_0;

        VkInstanceCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        create_info.pApplicationInfo = &app_info;
        create_info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        create_info.ppEnabledExtensionNames = extensions.data();
        if (vkCreateInstance(&create_info, nullptr, &instance) != VK_SUCCESS) {
            core::error("ui: vkCreateInstance failed");
            return false;
        }
        volkLoadInstance(instance);
        return true;
    }

    bool create_surface() {
        if (!host_->create_surface(instance, &surface)) {
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
                // batch.frag's non-constant sampler-array index needs this;
                // skip a device that lacks it rather than fail later in
                // create_device() when another candidate might have it.
                VkPhysicalDeviceFeatures features{};
                vkGetPhysicalDeviceFeatures(physical, &features);
                if (!features.shaderSampledImageArrayDynamicIndexing) {
                    continue;
                }
                candidates.push_back({physical, i});
                break;
            }
        }
        if (candidates.empty()) {
            core::error(
                "ui: no device can present to the window surface and "
                "supports shaderSampledImageArrayDynamicIndexing");
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
        // Depth format for every swapchain this process will build, chosen
        // here because select_device runs exactly once and strictly before
        // the first create_swapchain - that ordering IS the ImGui-backend
        // compatibility guarantee (no pass ever exists with another
        // format). A GPU with no depth-attachable format cannot run the
        // renderer's depth-tested path - fail loudly, same rule as the
        // dynamic-indexing feature above.
        depth_format = choose_depth_format(physical_device);
        if (depth_format == VK_FORMAT_UNDEFINED) {
            core::error("ui: device supports no depth-attachable format");
            return false;
        }
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

        // batch.frag indexes u_textures[32] with a non-constant (runtime)
        // index, which the spec gates behind this feature - without it,
        // dynamic sampler-array indexing is undefined behavior even where
        // a given driver happens to tolerate it. Required, not optional:
        // fail loudly rather than ship a renderer that silently depends on
        // driver leniency.
        VkPhysicalDeviceFeatures available{};
        vkGetPhysicalDeviceFeatures(physical_device, &available);
        if (!available.shaderSampledImageArrayDynamicIndexing) {
            core::error(
                "ui: GPU lacks shaderSampledImageArrayDynamicIndexing, "
                "required by the 2D batch shader's texture array");
            return false;
        }
        VkPhysicalDeviceFeatures enabled{};
        enabled.shaderSampledImageArrayDynamicIndexing = VK_TRUE;

        const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo create_info{};
        create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        create_info.queueCreateInfoCount = 1;
        create_info.pQueueCreateInfos = &queue_info;
        create_info.enabledExtensionCount = 1;
        create_info.ppEnabledExtensionNames = extensions;
        create_info.pEnabledFeatures = &enabled;
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

        // Depth attachment, same extent as the color images. VMA owns the
        // image; the view takes the DEPTH aspect only (a D24S8 stencil half
        // stays unused, the standard depth-only view). No barrier needed -
        // the render pass's UNDEFINED initialLayout transition covers first
        // use, exactly like the color attachment.
        VkImageCreateInfo depth_info{};
        depth_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        depth_info.imageType = VK_IMAGE_TYPE_2D;
        depth_info.format = depth_format;
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
        if (vmaCreateImage(allocator, &depth_info, &depth_alloc, &depth_image,
                           &depth_allocation, nullptr) != VK_SUCCESS) {
            core::error("ui: vmaCreateImage (depth) failed");
            return false;
        }
        VkImageViewCreateInfo depth_view_info{};
        depth_view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        depth_view_info.image = depth_image;
        depth_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        depth_view_info.format = depth_format;
        depth_view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        depth_view_info.subresourceRange.levelCount = 1;
        depth_view_info.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device, &depth_view_info, nullptr, &depth_view) !=
            VK_SUCCESS) {
            core::error("ui: vkCreateImageView (depth) failed");
            return false;
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

        // Cleared to 1.0 each frame, never read after the pass (only the
        // subpass's own test/write touches it) - DONT_CARE keeps the store
        // free, and the pass's UNDEFINED transition puts it into layout.
        VkAttachmentDescription depth{};
        depth.format = depth_format;
        depth.samples = VK_SAMPLE_COUNT_1_BIT;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depth_ref{1,
                                       VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color_ref;
        subpass.pDepthStencilAttachment = &depth_ref;

        VkAttachmentDescription attachments[2] = {color, depth};
        VkRenderPassCreateInfo render_pass_info{};
        render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        render_pass_info.attachmentCount = 2;
        render_pass_info.pAttachments = attachments;
        render_pass_info.subpassCount = 1;
        render_pass_info.pSubpasses = &subpass;
        if (vkCreateRenderPass(device, &render_pass_info, nullptr, &render_pass) !=
            VK_SUCCESS) {
            core::error("ui: vkCreateRenderPass failed");
            return false;
        }

        framebuffers.resize(image_count);
        for (uint32_t i = 0; i < image_count; ++i) {
            // Every framebuffer pairs its color view with the one depth
            // view - the pass has two attachments, so each must supply both.
            VkImageView framebuffer_views[2] = {views[i], depth_view};
            VkFramebufferCreateInfo framebuffer_info{};
            framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebuffer_info.renderPass = render_pass;
            framebuffer_info.attachmentCount = 2;
            framebuffer_info.pAttachments = framebuffer_views;
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
        if (depth_view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, depth_view, nullptr);
        }
        if (depth_image != VK_NULL_HANDLE) {
            // The allocator is a device-stage object - it outlives every
            // window-stage teardown, including this one.
            vmaDestroyImage(allocator, depth_image, depth_allocation);
        }
        if (swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device, swapchain, nullptr);
        }

        framebuffers.clear();
        frames.clear();
        syncs.clear();
        views.clear();
        images.clear();  // swapchain-owned; freed with the swapchain above
        depth_view = VK_NULL_HANDLE;
        depth_image = VK_NULL_HANDLE;
        depth_allocation = VK_NULL_HANDLE;
        render_pass = VK_NULL_HANDLE;
        swapchain = VK_NULL_HANDLE;
        image_count = 0;
        image_index = 0;
        sync_index = 0;
    }

    // The GLSL sources' directory, for the dev-only shader hot reload
    // (WS_UI_DEV_SHADER_RELOAD): the committed spirv.h serves every default
    // build; a dev build polls these files' mtimes each frame. Repo-root
    // relative: apps already rely on CWD-relative asset paths, and the
    // reload option is desktop-debug-only (configure-time gated), so the
    // dev runs from the workspace root as always.
    static const char* shader_dir() { return "libs/ui/src/shaders"; }

    bool recreate_swapchain() {
        int fb_width = 0;
        int fb_height = 0;
        host_->framebuffer_size(&fb_width, &fb_height);
        destroy_swapchain_objects();
        if (!create_swapchain(fb_width, fb_height)) {
            return false;
        }
        rebuild_swapchain = false;
        swapchain_suboptimal = false;

        // The backend bakes MinImageCount/RenderPass/its buffer ring at Init.
        // An identical format keeps the render passes (hence its pipeline)
        // compatible; if the image count or format changed it must be
        // re-initialized. The depth format never drifts - chosen once at
        // device selection - so it never breaks compatibility either.
        if (image_count != imgui_image_count || format != imgui_format) {
            ImGui_ImplVulkan_Shutdown();
            imgui_vk_up = false;
            if (!init_imgui_backend()) {
                return false;
            }
        }
        // Same drift rules for the 2D renderer: format drift rebuilds its
        // pipeline, image-count drift its descriptor sets and vertex
        // regions. A plain resize is render-pass compatible - its viewport
        // and scissor are dynamic and arrive per flush.
        if (!renderer2d_.on_swapchain_recreated(render_pass, format, image_count)) {
            return false;
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

        // The Vulkan backend needs the swapchain (render pass + image
        // count), so it inits in init_imgui_backend() once the window
        // stage created one; the platform backend binds to the current
        // window right after this - both called from attach_window(),
        // which owns their lifecycle (Android re-runs them per attach).
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
        if (err == VK_ERROR_OUT_OF_DATE_KHR) {
            // This frame cannot run: the acquire failed and signaled
            // nothing. Drop the commands submitted for it instead of
            // letting them leak into a later frame (queue contract:
            // same-frame execution, render_command_queue.hpp; the 2D
            // renderer keeps the same contract).
            write_queue_.clear();
            renderer2d_.frame_drop();
            rebuild_swapchain = true;  // present_frame() skips; next loop recreates
            return;
        }
        // VK_SUBOPTIMAL_KHR is a success code: the image IS acquired and
        // sync.acquired IS signaled, so the frame must run and present -
        // only a submit's semaphore wait consumes that signal, and the
        // rebuild's destroy would otherwise take a live signaled semaphore
        // down with the swapchain. Presenting a suboptimal swapchain is
        // valid; present_frame() raises the rebuild flag once this frame
        // has fully retired.
        swapchain_suboptimal = (err == VK_SUBOPTIMAL_KHR);
        if (!swapchain_suboptimal) {
            vk_check(err, "vkAcquireNextImageKHR");
        }

        Frame& frame = frames[image_index];
        vk_check(vkWaitForFences(device, 1, &frame.fence, VK_TRUE, UINT64_MAX),
                 "vkWaitForFences");
        vk_check(vkResetFences(device, 1, &frame.fence), "vkResetFences");
        vk_check(vkResetCommandPool(device, frame.pool, 0), "vkResetCommandPool");

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk_check(vkBeginCommandBuffer(frame.buffer, &begin_info), "vkBeginCommandBuffer");

        // Dark slate background; apps draw their UI on top. The depth clear
        // is 1.0 - far plane, the LEQUAL convention: a 3D chunk's equal
        // depths keep the submission order the stable sort built.
        VkClearValue clears[2]{};
        clears[0].color.float32[0] = 0.08f;
        clears[0].color.float32[1] = 0.08f;
        clears[0].color.float32[2] = 0.10f;
        clears[0].color.float32[3] = 1.0f;
        clears[1].depthStencil.depth = 1.0f;
        clears[1].depthStencil.stencil = 0;

        VkRenderPassBeginInfo render_pass_info{};
        render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        render_pass_info.renderPass = render_pass;
        render_pass_info.framebuffer = framebuffers[image_index];
        render_pass_info.renderArea.extent = extent;
        render_pass_info.clearValueCount = 2;
        render_pass_info.pClearValues = clears;
        vkCmdBeginRenderPass(frame.buffer, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);

        // 2D world first, widgets on top. This image's fence is already
        // waited above, so the batch's vertex upload and the chunk descriptor
        // writes cannot race the submit that last used them.
        renderer2d_.flush(frame.buffer, image_index,
                          static_cast<float>(extent.width),
                          static_cast<float>(extent.height));

        ImGui_ImplVulkan_RenderDrawData(draw_data, frame.buffer);

        // App-submitted commands run here, inside the render pass, after the
        // UI draw data - the seam for non-ImGui GPU work (overlays, offscreen
        // passes). The swap turns the queue the app filled during on_update()
        // into the read queue; frames that cannot run (the bail path above,
        // a hidden window in main_loop) clear the write queue instead - a
        // command never outlives the frame it was submitted in.
        std::swap(write_queue_, read_queue_);
        try {
            read_queue_.execute();
        } catch (const std::exception& e) {
            core::error("ui: exception in render command queue: {}", e.what());
            host_->request_stop();
        } catch (...) {
            core::error("ui: unknown exception in render command queue");
            host_->request_stop();
        }

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
        if (swapchain_suboptimal) {
            // The acquire said the swapchain no longer matches the
            // surface; this frame presented fine, so recreate before the
            // next acquire - the semaphore pair is fully retired now.
            rebuild_swapchain = true;
            swapchain_suboptimal = false;
        }
        sync_index = (sync_index + 1) % static_cast<uint32_t>(syncs.size());
    }

    int main_loop(App& app) {
        for (;;) {
            // Fresh gui input edges for this pump: the previous frame's
            // pressed/released/scroll/text clear, held state and position
            // stay. Iconified and surface-loss iterations never reach
            // on_update, so their edges drop here - drop, never defer,
            // like every queue.
            gui_input_.next_frame();
            host_->pump();

            // Every queued event reaches on_event before this frame's
            // on_update() - including the WindowCloseEvent of the closing
            // click, so apps can react before the loop exits.
            deliver_events(app);
            if (host_->should_close()) {
                break;
            }

            // The window stage follows the platform's window state: attach
            // while a window exists, detach when the platform took it away
            // (Android surface loss; desktop stays ready for the whole
            // run), and block on the platform while there is nothing to
            // render instead of spinning.
            if (!host_->window_ready()) {
                if (window_stage_up) {
                    detach_window();
                }
                if (host_->should_close()) {
                    break;
                }
                host_->wait_for_window();
                continue;
            }
            if (!window_stage_up && !attach_window(app)) {
                return 1;
            }

            // Swapchain recreation is event-driven: a WindowResizeEvent (or
            // Vulkan itself, via out-of-date/suboptimal) raised the flag -
            // there is no per-frame size poll. Skip while the surface is
            // unusable (minimized); the flag survives until a good size
            // arrives.
            const bool window_hidden = host_->is_hidden();
            if (rebuild_swapchain && !window_hidden) {
                int fb_width = 0;
                int fb_height = 0;
                host_->framebuffer_size(&fb_width, &fb_height);
                if (fb_width > 0 && fb_height > 0 && !recreate_swapchain()) {
                    return 1;
                }
            }
            if (window_hidden) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

#ifdef WS_UI_DEV_SHADER_RELOAD
            // Dev-only hot reload: poll the GLSL mtimes at the frame
            // boundary (nothing recorded, nothing in flight) and rebake
            // the pipelines when they changed. A failed compile keeps the
            // last good words inside the provider, so the app keeps
            // rendering no matter what the editor did.
            if (r2d::spirv::poll_reload(shader_dir())) {
                // Until per-frame deletion queues exist (M6/M12), a hard
                // wait is the only correct drain: the GPU may still be
                // executing draws through the old pipelines. Dev-only
                // iteration cost, never in a default build.
                vkDeviceWaitIdle(device);
                if (renderer2d_.reload_pipelines()) {
                    core::info("ui: shader hot reload - pipelines rebaked");
                } else {
                    core::error("ui: shader hot reload - pipeline rebake failed");
                }
            }
#endif

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
            host_->imgui_platform_new_frame();
            ImGui::NewFrame();

            // One r2d frame per ImGui frame: begin before on_update() records
            // its draws, sort + replay once it returned.
            renderer2d_.frame_begin(static_cast<float>(extent.width),
                                    static_cast<float>(extent.height));
            bool frame_abandoned = false;
            try {
                app.on_update();
                renderer2d_.frame_end();
            } catch (const std::exception& e) {
                core::error("ui: exception in on_update(): {}", e.what());
                host_->request_stop();
                frame_abandoned = true;
            } catch (...) {
                core::error("ui: unknown exception in on_update()");
                host_->request_stop();
                frame_abandoned = true;
            }
            if (frame_abandoned) {
                // The app may have thrown between Begin and End, leaving
                // ImGui's window stack unbalanced. Calling ImGui::Render()
                // would trip its "Missing End()" recovery assert and abort
                // instead of closing cleanly, so abandon the frame: skip
                // Render and the queues (the context teardown tolerates a
                // half-open frame) and exit through the should-close flag.
                write_queue_.clear();
                renderer2d_.frame_drop();
                break;
            }

            ImGui::Render();
            ImDrawData* draw_data = ImGui::GetDrawData();
            const bool hidden =
                draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f;
            if (!hidden) {
                render_frame(draw_data);
            } else {
                // Zero-size (minimized) frame: same rule as the bail path in
                // render_frame() - drop, never defer.
                write_queue_.clear();
                renderer2d_.frame_drop();
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

    // Context stage: the platform host and the Vulkan instance - everything
    // that survives an Android surface loss. Runs once per process. The
    // sink wiring happens here, before host_->init(): the platform installs
    // its callbacks against that sink inside init().
    bool startup_context() {
        // The single funnel every platform callback reads (see the event
        // system block) plus the gui library's raw input tap.
        event_sink_.forward = [this](Event& event) { forward_event(event); };
        event_sink_.input = &gui_input_;  // gui tap: raw input, no filtering
        host_ = make_window_host();
        if (!host_->init(event_sink_, config)) {
            return false;
        }
        return create_instance();
    }

    // Window stage: attaches the render stack to the platform's current
    // window. Surface first, then (first attach only) the GPU objects and
    // the renderer/ImGui contexts, then the swapchain. Re-attaches on
    // Android after a surface loss: the drift rules re-initialize only
    // what the OS round-trip actually invalidated, so app state (fonts,
    // textures, ImGui contexts) survives untouched.
    bool attach_window(App& app) {
        if (!create_surface()) {
            return false;
        }
        if (!device_stage_up) {
            if (!select_device() || !create_device() || !create_allocator() ||
                !create_imgui_pool()) {
                return false;
            }
            device_stage_up = true;
        }
        int fb_width = 0;
        int fb_height = 0;
        host_->framebuffer_size(&fb_width, &fb_height);
        if (fb_width <= 0 || fb_height <= 0 || !create_swapchain(fb_width, fb_height)) {
            return false;
        }
        if (!gpu_context_up) {
            // First attach only. Before on_setup(): apps load textures
            // there, and the r2d free functions need an active renderer.
            // init() may fail mid-way; shutdown() doubles as the
            // (idempotent) failure-path cleanup.
            if (!renderer2d_.init(device, physical_device, allocator, queue,
                                  queue_family, render_pass, format, image_count)) {
                return false;
            }
            r2d::set_active(&renderer2d_);
            // The gui input snapshot publishes the same way (frame_input()).
            gui::set_active_input(&gui_input_);
            if (!init_imgui(app)) {
                return false;
            }
            gpu_context_up = true;
        } else {
            // Re-attach: the renderer and the ImGui/ImPlot contexts are
            // device-lifetime state and survived the detach (only the
            // backends and the swapchain ride the window), so only the
            // drift rules run - the same ones recreate_swapchain()
            // applies. The backend was shut down at the detach and
            // re-initializes here; identical format and image count keep
            // its render pass compatible, and the renderer re-creates
            // only what the OS round-trip actually invalidated.
            if (!imgui_vk_up || image_count != imgui_image_count ||
                format != imgui_format) {
                if (imgui_vk_up) {
                    ImGui_ImplVulkan_Shutdown();
                }
                if (!init_imgui_backend()) {
                    return false;
                }
            }
            if (!renderer2d_.on_swapchain_recreated(render_pass, format, image_count)) {
                return false;
            }
        }
        // The platform backend binds to the current window - Android hands
        // over a NEW ANativeWindow with every INIT_WINDOW, so this runs on
        // every attach, never only the first.
        if (!host_->imgui_platform_init()) {
            core::error("ui: ImGui platform backend failed to initialize");
            return false;
        }
        imgui_platform_up = true;
        window_stage_up = true;
        rebuild_swapchain = false;  // the swapchain above is the current one
        swapchain_suboptimal = false;
        core::info("ui: \"{}\" ready: {}x{}, {} swapchain images, {} present",
                   config.title, extent.width, extent.height, image_count,
                   present_mode_name(present_mode));
        return true;
    }

    // Tears the window stage down, in the same order final shutdown would:
    // backends, swapchain objects, surface. Idempotent - it is BOTH the
    // mid-run teardown for Android surface loss (APP_CMD_TERM_WINDOW) and
    // the first step of shutdown(); every destroy is guarded, so it is the
    // failure-path cleanup for a partially completed attach too.
    void detach_window() {
        if (imgui_vk_up) {
            ImGui_ImplVulkan_Shutdown();
            imgui_vk_up = false;
        }
        if (imgui_platform_up) {
            host_->imgui_platform_shutdown();
            imgui_platform_up = false;
        }
        destroy_swapchain_objects();
        if (surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance, surface, nullptr);
            surface = VK_NULL_HANDLE;
        }
        window_stage_up = false;
    }

    // Idempotent: every destroy is guarded, so it doubles as the failure-path
    // cleanup for a partially completed startup or attach.
    void shutdown() {
        // Window stage first, when it is up at all: backends, swapchain
        // objects, surface - the same detach an Android surface loss takes,
        // so final shutdown is simply "detach and never re-attach". A loop
        // that broke mid-frame (exception, close button) lands here with the
        // stage still up; a startup failure lands here with it already down.
        detach_window();

        // Backend shutdown touches ImGui::GetIO(), so the contexts go last.
        if (ImPlot::GetCurrentContext() != nullptr) {
            ImPlot::DestroyContext();
        }
        if (ImGui::GetCurrentContext() != nullptr) {
            ImGui::DestroyContext();
        }

        // 2D renderer before the allocator and device it allocates from; the
        // active pointer goes null so post-run r2d calls drop with a warn
        // instead of touching a dead renderer.
        r2d::set_active(nullptr);
        renderer2d_.shutdown();
        // Same for the gui input tap: post-run frame_input() reports an
        // empty frame, same drop-safe contract.
        gui::set_active_input(nullptr);

        if (allocator != VK_NULL_HANDLE) {
            vmaDestroyAllocator(allocator);
            allocator = VK_NULL_HANDLE;
        }
        if (imgui_pool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device, imgui_pool, nullptr);
            imgui_pool = VK_NULL_HANDLE;
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
        // The host goes down after the Vulkan objects its surface fed: on
        // desktop this is where the GLFW window and the library die (the
        // host destructor), with the loop already out of the pump.
        host_.reset();
        device_stage_up = false;
        gpu_context_up = false;
    }

    int run(App& app) {
        if (!startup_context()) {
            shutdown();  // failure-path cleanup; every destroy is guarded
            return 1;
        }
        const int result = main_loop(app);
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

RenderCommandQueue& App::command_queue() {
    return impl_->write_queue_;
}

float App::ui_scale() const {
    // The density factor through the platform seam; before run() (and
    // after a failed startup) there is no window platform, so the
    // unscaled 1.0 stands.
    return impl_->host_ ? impl_->host_->ui_scale() : 1.0f;
}

}  // namespace ws::ui
