// ws::ui::App - the application shell (window + Vulkan + Dear ImGui).
//
// All platform/Vulkan state lives behind App::Impl. This file is
// windowing-agnostic: it talks to the OS only through the WindowHost seam
// (src/platform/window.h - window_glfw.cpp on desktop, window_android.cpp on
// Android), and no GLFW/Android symbol appears here. The public header
// exposes no third-party types on purpose: apps subclass App and react to
// events / draw via on_setup()/on_event()/on_update().
//
// The Vulkan device layer (loader, instance, surface, device selection,
// queue, VMA allocator) lives in ws::gfx since stage 3b, and the
// presentation layer (swapchain, depth attachment, render pass,
// framebuffers, frame sync) since its phase B - Impl holds a gfx::Gpu
// plus a gfx::Swapchain and unwraps their opaque void* handles through
// the accessors just below the member block (this file includes volk.h,
// so it is the one legit cast point). The frame loop, its recording
// middle (the r2d flush, ImGui draw data, the command queue) and the
// backends stay here.
//
// The frame flow is the classic frames-in-flight model, owned by
// gfx::Swapchain: gfx::kMaxFramesInFlight frame slots (pinned equal to
// r2d's constant below) each own a fence, command pool/buffer and
// acquire semaphore, while each swapchain image owns its render-finished
// semaphore - the slot count, not the image count, bounds how many
// frames may be in flight. Recreate-on-out-of-date as in Dear ImGui's
// examples, but the swapchain code is ours - the ImGui_ImplVulkanH_*
// helpers are example-only per the backend's own docs.
//
// Startup splits into two stages so the shell survives Android's surface
// loss (APP_CMD_TERM_WINDOW takes the ANativeWindow away at any pause):
// the context stage (platform host + the gfx instance) runs once, while the
// window stage (surface, device, swapchain, renderer, ImGui backends)
// attaches and detaches with the window - on Android it re-runs whenever
// the OS hands a window back.

#include <ws/gfx/gpu.hpp>
#include <ws/gfx/swapchain.hpp>
#include <ws/input/event.hpp>
#include <ws/input/input.hpp>
#include <ws/jobs/job_system.hpp>
#include <ws/ui/app.hpp>
#include <ws/ui/render_command_queue.hpp>

#include <ws/core/log.hpp>
#include <ws/core/result.hpp>

#include "render/renderer2d.h"
#include "render/shader_provider.h"
#include "platform/window.h"

#include <volk.h>  // meta-loader: every vk* symbol below is a volk global

#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <implot.h>

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

// Descriptor sets for the font atlas plus a handful of app textures
// registered through ImGui_ImplVulkan_AddTexture().
constexpr uint32_t kDescriptorSetCount = 16;

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

// Env probe for the WS_UI_VALIDATION opt-in: read once, at instance
// creation. MSVC deprecates getenv (C4996) over CRT-internal mutation
// concerns; a one-shot startup read is the portable POSIX spelling -
// suppressed scoped, not blanket-banned via _CRT_SECURE_NO_WARNINGS
// (shader_provider.cpp stays the one Win32-spelling holdout because its
// whole TU is desktop-only).
const char* env_probe(const char* name) {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    return std::getenv(name);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}

// Every concrete event type. The per-frame queue holds events by value -
// no vtable anywhere in the event system, and no heap per event but the
// one exception: FileDroppedEvent carries its paths vector (a drop is
// human-rare, so a copy there is nothing the loop ever notices).
using AnyEvent = std::variant<ws::input::WindowCloseEvent, ws::input::WindowResizeEvent, ws::input::KeyPressedEvent,
                             ws::input::KeyReleasedEvent, ws::input::MouseButtonPressedEvent,
                             ws::input::MouseButtonReleasedEvent, ws::input::MouseMovedEvent,
                             ws::input::MouseScrolledEvent, ws::input::TouchPressedEvent,
                             ws::input::TouchMovedEvent, ws::input::TouchReleasedEvent,
                             ws::input::TextInputEvent, ws::input::FileDroppedEvent>;

// The gfx::SurfaceSource adapter: WindowHost speaks Vulkan handles (it
// includes volk.h internally), gfx speaks opaque void* - this fn-ptr is
// one of this file's two cast points for that boundary (the other is the
// Impl accessors below). Wired once at init_gpu(); called by gfx at
// every attach_surface().
bool ui_gfx_create_surface(void* instance, void** surface_out, void* user) {
    auto* host = static_cast<WindowHost*>(user);
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!host->create_surface(static_cast<VkInstance>(instance), &surface)) {
        return false;
    }
    *surface_out = surface;
    return true;
}

}  // namespace

// The frame-slot count has two homes since stage 3b phase B:
// gfx::Swapchain owns the slots, and r2d's arenas + chunk sets key on
// it. One number - pinned equal so a change on either side fails the
// build right here.
static_assert(gfx::kMaxFramesInFlight == r2d::kMaxFramesInFlight,
              "gfx::Swapchain and r2d must agree on the frame-slot count");

struct App::Impl {

    AppConfig config;

    // The windowing platform behind the seam (src/platform/window.h): GLFW on
    // desktop, native_app_glue on Android. Window interactions below go
    // through it; no GLFW/Android symbol appears in this file.
    std::unique_ptr<WindowHost> host_;

    // The installed job system (stage 8), caller-owned: null until
    // install_job_system() hands one in, and the loop drains this
    // pointer's marshal queue once per frame - between the event
    // delivery and on_update(), the between-frames seam the gui's job
    // helpers (<ws/ui/gui/job_progress.hpp>) build on.
    ws::jobs::JobSystem* jobs_ = nullptr;

    // The Vulkan device layer (stage 3b): loader, instance, surface,
    // physical device, device, queue, VMA allocator - everything up to and
    // including the allocator, behind gfx::Gpu's staged lifecycle
    // (init once / attach+detach per window / shutdown guarded). Handles
    // cross the gfx boundary as opaque void*; the accessors below are
    // this file's one unwrap point.
    gfx::Gpu gpu_;
    VkDescriptorPool imgui_pool = VK_NULL_HANDLE;

    // The presentation layer (stage 3b phase B): swapchain, image views,
    // depth attachment, render pass, framebuffers and the whole frame
    // sync - the frame slots and the per-image render-finished
    // semaphores - behind gfx::Swapchain's opaque handles; the accessors
    // below are this file's unwrap point for its per-swapchain state.
    gfx::Swapchain swapchain_;
    // A resize (the WindowResizeEvent path) or Vulkan itself
    // (out-of-date/suboptimal at acquire or present) raised this: the
    // loop top recreates the swapchain before the next frame. There is
    // no per-frame size poll.
    bool rebuild_swapchain = false;

    // Stage bookkeeping for the split startup (see the file comment): the
    // context stage (host + gfx instance) runs once; the device stage
    // (device, queue, allocator - tracked inside gfx::Gpu) and the window
    // stage (surface + swapchain + backends) attach to the current window
    // - and re-attach on Android whenever the OS hands one back. The
    // ImGui pool (imgui_pool above) rides the device stage with them: it
    // is created at the first attach and outlives every surface loss,
    // which is exactly what imgui_pool == VK_NULL_HANDLE tracks here.
    // The r2d renderer, the ImGui/ImPlot contexts and the gui input
    // publication (gpu_context_up) are created at the FIRST attach and
    // survive a surface loss: only the backends and the swapchain ride
    // the window, so a re-attach re-initializes those alone (the drift
    // rules), never the full GPU init again.
    bool window_stage_up = false;
    bool gpu_context_up = false;  // renderer + ImGui contexts + gui publication
    bool imgui_platform_up = false;  // GLFW / Android platform backend
    bool imgui_vk_up = false;
    uint32_t imgui_image_count = 0;  // values the backend was initialized with;
    VkFormat imgui_format = VK_FORMAT_UNDEFINED;  // drift means it must be re-inited
    bool viewport_style_set = false;

    // --- event system -------------------------------------------------------
    // Platform callbacks (src/platform/window_glfw.cpp / src/platform/window_android.cpp)
    // funnel every event through the sink;
    // forward_event() pre-marks ImGui-consumed input, queues the rest and
    // coalesces mouse-move/scroll/touch-move per frame (move keeps the
    // latest position, scroll sums its deltas, each touch keeps its own
    // latest move). deliver_events() then drives App::on_event,
    // once per queued event, before on_update().
    WindowEventSink event_sink_;
    std::vector<AnyEvent> event_queue_;
    std::optional<ws::input::MouseMovedEvent> pending_mouse_move_;  // coalesced: latest wins
    std::optional<ws::input::MouseScrolledEvent> pending_scroll_;   // coalesced: deltas summed
    // One pending move per touch contact, latest wins - every moving finger
    // keeps its own slot, so no finger's final position is lost at a busy
    // frame boundary (pending_mouse_move_'s rule, per pointer id).
    std::array<std::optional<ws::input::TouchMovedEvent>, ws::input::InputFrame::kMaxTouches>
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
    // The texture tier gfx's device probe picked (M9): bindless (one global
    // descriptor-indexed table, nonuniformEXT in the shader) when the
    // device has descriptor indexing with the needed limits, the chunked
    // 32-slot-table fallback otherwise. Fixed once at the device stage;
    // the renderer init consumes it and it never changes after.
    r2d::TextureTier texture_tier_{};

    // --- gui library --------------------------------------------------------
    // One input snapshot per frame, filled by the platform callbacks' raw
    // tap (src/platform/window_glfw.cpp / src/platform/window_android.cpp, through
    // WindowEventSink::input). main_loop clears
    // the per-frame edges at the top of each iteration; on_update reads
    // the snapshot through ws::input::frame_input() for the duration of
    // the run.
    ws::input::InputFrame gui_input_;

    ~Impl() { shutdown(); }

    // Raw-handle views over gpu_ and swapchain_ - the shell's backend and
    // renderer code speaks volk types, so each use site casts once, here,
    // instead of sprinkling reinterpret_casts (the gfx public API is
    // opaque void* on purpose: no Vulkan vocabulary may cross a lib
    // boundary). Every one is null/0 until its stage is up - gfx's
    // null-state contract.
    VkInstance instance() const { return static_cast<VkInstance>(gpu_.instance()); }
    VkPhysicalDevice physical_device() const {
        return static_cast<VkPhysicalDevice>(gpu_.physical_device());
    }
    VkDevice device() const { return static_cast<VkDevice>(gpu_.device()); }
    VkQueue queue() const { return static_cast<VkQueue>(gpu_.queue()); }
    uint32_t queue_family() const { return gpu_.queue_family(); }
    // The current swapchain's render pass and format - what the backend
    // init and the renderer's drift checks read; null / UNDEFINED before
    // the window stage creates a swapchain.
    VkRenderPass render_pass() const {
        return static_cast<VkRenderPass>(swapchain_.render_pass());
    }
    VkFormat format() const { return static_cast<VkFormat>(swapchain_.format()); }

    // --- event system --------------------------------------------------------

    // True when ImGui owns this input (pointer over a widget, text field
    // focused, ...). Deliberately evaluated at event time - the flags were
    // recomputed at the last NewFrame, which is the standard Hazel-style
    // approximation. Window close/resize never count as captured.
    bool imgui_wants(const ws::input::Event& event) const {
        if (ImGui::GetCurrentContext() == nullptr) {
            return false;  // events before init_imgui(): nothing owns input yet
        }
        const ImGuiIO& io = ImGui::GetIO();
        switch (event.type()) {
            case ws::input::EventType::KeyPressed:
            case ws::input::EventType::KeyReleased:
            case ws::input::EventType::TextInput:
                return io.WantCaptureKeyboard;
            case ws::input::EventType::MouseButtonPressed:
            case ws::input::EventType::MouseButtonReleased:
            case ws::input::EventType::MouseMoved:
            case ws::input::EventType::MouseScrolled:
                return io.WantCaptureMouse;
            case ws::input::EventType::TouchPressed:
            case ws::input::EventType::TouchMoved:
            case ws::input::EventType::TouchReleased:
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
    void forward_event(ws::input::Event& event) {
        if (imgui_wants(event)) {
            event.handled = true;
        }
        // No default: /w44062 (cmake/WsWarnings.cmake; the -Wswitch
        // counterpart on gcc/clang) fails the build if a new ws::input::EventType is
        // added without a case here, so no event can be silently dropped.
        // A new event also needs an AnyEvent alternative above - the
        // variant rejects types it cannot hold.
        switch (event.type()) {
            case ws::input::EventType::WindowResize:
                rebuild_swapchain = true;
                event_queue_.push_back(static_cast<ws::input::WindowResizeEvent&>(event));
                break;
            case ws::input::EventType::MouseMoved:
                pending_mouse_move_ = static_cast<ws::input::MouseMovedEvent&>(event);
                break;
            case ws::input::EventType::MouseScrolled: {
                // Offsets are per-event deltas, not positions: sum them, or
                // ticks arriving between frames would be lost.
                const ws::input::MouseScrolledEvent& scrolled =
                    static_cast<ws::input::MouseScrolledEvent&>(event);
                if (pending_scroll_) {
                    pending_scroll_->x_offset += scrolled.x_offset;
                    pending_scroll_->y_offset += scrolled.y_offset;
                } else {
                    pending_scroll_ = scrolled;
                }
                break;
            }
            case ws::input::EventType::WindowClose:
                event_queue_.push_back(static_cast<ws::input::WindowCloseEvent&>(event));
                break;
            case ws::input::EventType::KeyPressed:
                event_queue_.push_back(static_cast<ws::input::KeyPressedEvent&>(event));
                break;
            case ws::input::EventType::KeyReleased:
                event_queue_.push_back(static_cast<ws::input::KeyReleasedEvent&>(event));
                break;
            case ws::input::EventType::MouseButtonPressed:
                event_queue_.push_back(static_cast<ws::input::MouseButtonPressedEvent&>(event));
                break;
            case ws::input::EventType::MouseButtonReleased:
                event_queue_.push_back(static_cast<ws::input::MouseButtonReleasedEvent&>(event));
                break;
            case ws::input::EventType::TouchPressed:
                event_queue_.push_back(static_cast<ws::input::TouchPressedEvent&>(event));
                break;
            case ws::input::EventType::TouchMoved: {
                // Latest position per pointer id - pending_mouse_move_'s
                // rule, one slot per contact. A table with no id match and
                // no free slot means more distinct movers than the touch
                // cap ever admits (hand-rolled emitters only): drop + warn
                // once.
                const ws::input::TouchMovedEvent& moved =
                    static_cast<ws::input::TouchMovedEvent&>(event);
                std::optional<ws::input::TouchMovedEvent>* slot = nullptr;
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
            case ws::input::EventType::TouchReleased:
                event_queue_.push_back(static_cast<ws::input::TouchReleasedEvent&>(event));
                break;
            case ws::input::EventType::TextInput:
                // One event per glyph, arrival order - typing is low-rate
                // and order-preserving, so no coalescing.
                event_queue_.push_back(static_cast<ws::input::TextInputEvent&>(event));
                break;
            case ws::input::EventType::FileDropped:
                // One event per drop, arrival order - the TextInput rule
                // again. The paths ride the event (copied at the platform
                // edge before the OS callback returned), so the queue's
                // by-value push moves them, nothing dangles.
                event_queue_.push_back(static_cast<ws::input::FileDroppedEvent&>(event));
                break;
        }
    }

    // Coalesced events flush right before delivery, so the app sees exactly
    // one ws::input::MouseMovedEvent / one ws::input::MouseScrolledEvent per frame even when the
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

    // The context stage's gfx half: init() builds the instance from the
    // host's WSI extension list - SurfaceSource adapts WindowHost to the
    // gfx seam (the extension pointers now, the surface callback at every
    // attach). The options carry the shell's requests:
    // - enable_validation: the WS_UI_VALIDATION opt-in (M6's sync-model
    //   test vehicle), read once right here - deliberately not gfx's
    //   business. Validation costs frames and needs the SDK; a machine
    //   without the layer warns once (inside gfx) and runs without it.
    // - want_bindless + the batch's own table size: the renderer's M9
    //   texture tier, probed once at the first attach.
    // The device stage itself (physical device, device, queue,
    // allocator) comes up at the first attach_surface().
    bool init_gpu() {
        const std::vector<const char*> extensions = host_->instance_extensions();
        gfx::SurfaceSource source{};
        source.instance_extensions = extensions.data();
        source.instance_extension_count = static_cast<uint32_t>(extensions.size());
        source.create_surface = &ui_gfx_create_surface;
        source.user = host_.get();

        gfx::DeviceOptions options{};
        options.application_name = config.title.c_str();
        const char* validation_env = env_probe("WS_UI_VALIDATION");
        options.enable_validation =
            validation_env != nullptr && std::string_view(validation_env) != "0";
        options.want_bindless = true;
        options.bindless_descriptor_limit = r2d::batch::kMaxBindlessTextures;

        const core::Status started = gpu_.init(source, options);
        if (!started) {
            core::log_error(started.error());
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
        if (vkCreateDescriptorPool(device(), &create_info, nullptr, &imgui_pool) !=
            VK_SUCCESS) {
            core::error("ui: failed to create the ImGui descriptor pool");
            return false;
        }
        return true;
    }

    // The GLSL sources' directory, for the dev-only shader hot reload
    // (WS_UI_DEV_SHADER_RELOAD): the committed spirv.h serves every default
    // build; a dev build polls these files' mtimes each frame. Repo-root
    // relative: apps already rely on CWD-relative asset paths, and the
    // reload option is desktop-debug-only (configure-time gated), so the
    // dev runs from the workspace root as always.
    static const char* shader_dir() { return "libs/ui/src/render/shaders"; }

    bool recreate_swapchain() {
        int fb_width = 0;
        int fb_height = 0;
        host_->framebuffer_size(&fb_width, &fb_height);
        // gfx::Swapchain owns the destroy+rebuild pair; its Status carries
        // the diagnostic. A failed recreate leaves it torn down cleanly
        // (create/recreate clean up after themselves) - shutdown() finishes
        // through the same guarded path.
        const core::Status recreated = swapchain_.recreate(fb_width, fb_height);
        if (!recreated) {
            core::log_error(recreated.error());
            return false;
        }
        rebuild_swapchain = false;

        // The backend bakes MinImageCount/RenderPass/its buffer ring at Init.
        // An identical format keeps the render passes (hence its pipeline)
        // compatible; if the image count or format changed it must be
        // re-initialized. The depth format never drifts - chosen once at
        // device selection - so it never breaks compatibility either.
        if (swapchain_.image_count() != imgui_image_count ||
            format() != imgui_format) {
            ImGui_ImplVulkan_Shutdown();
            imgui_vk_up = false;
            if (!init_imgui_backend()) {
                return false;
            }
        }
        // Same drift rules for the 2D renderer, minus the image count: its
        // vertex arenas and chunk descriptor sets are per frame slot (fixed
        // at init), so only format drift matters - it rebuilds the pipeline.
        // A plain resize is render-pass compatible - the viewport and
        // scissor are dynamic and arrive per flush.
        if (!renderer2d_.on_swapchain_recreated(render_pass(), format())) {
            return false;
        }
        return true;
    }

    // --- Dear ImGui ---------------------------------------------------------

    bool init_imgui_backend() {
        ImGui_ImplVulkan_InitInfo info{};
        info.Instance = instance();
        info.PhysicalDevice = physical_device();
        info.Device = device();
        info.QueueFamily = queue_family();
        info.Queue = queue();
        info.DescriptorPool = imgui_pool;
        info.RenderPass = render_pass();
        info.MinImageCount = kMinImageCount;  // never changes: the backend cannot
        info.ImageCount = swapchain_.image_count();  // update it after init
        info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        info.CheckVkResultFn = &imgui_vk_check;
        if (!ImGui_ImplVulkan_Init(&info)) {
            core::error("ui: ImGui Vulkan backend failed to initialize");
            return false;
        }
        imgui_image_count = swapchain_.image_count();
        imgui_format = format();
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

    int main_loop(App& app) {
        // The frame clock's "previous": the pump's own start, so the
        // first frame's dt measures real time and not a default
        // epoch's.
        std::chrono::steady_clock::time_point previous =
            std::chrono::steady_clock::now();
        for (;;) {
            // The frame clock: seconds from the previous pump top -
            // steady_clock, immune to wall-clock moves. One write per
            // iteration, before next_frame (which keeps the field: this
            // write IS its refresh).
            const auto now = std::chrono::steady_clock::now();
            gui_input_.dt_seconds =
                std::chrono::duration<float>(now - previous).count();
            previous = now;
            // Fresh gui input edges for this pump: the previous frame's
            // pressed/released/scroll/text clear, held state and position
            // stay. Iconified and surface-loss iterations never reach
            // on_update, so their edges drop here - drop, never defer,
            // like every queue.
            gui_input_.next_frame();
            host_->pump();

            // Every queued event reaches on_event before this frame's
            // on_update() - including the ws::input::WindowCloseEvent of the closing
            // click, so apps can react before the loop exits.
            deliver_events(app);

            // The installed job system's frame drain (stage 8):
            // everything its workers marshalled since the last frame -
            // progress deliveries, completions - runs HERE, on the
            // main thread, before this frame's on_update() and outside
            // any gui frame, so the callbacks may touch gui state
            // directly (the Toasts store, an app-owned JobProgress
            // mirror) with no locks. Draining before the close check
            // also delivers everything posted before the window close
            // before the loop exits; the hidden-window continue paths
            // below ride the same drain, so backgrounded completions
            // still land.
            if (jobs_ != nullptr) {
                jobs_->drain();
            }
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

            // Swapchain recreation is event-driven: a ws::input::WindowResizeEvent (or
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
                // Retire every frame slot instead of idling the whole
                // device: slot fences cover every submitted draw (all GPU
                // work lives in slot submissions; texture uploads are
                // fence-waited one-offs), which is exactly the drain
                // destroying the old pipelines needs. Deferred per-frame
                // deletion queues (M12) can move this off the iteration
                // loop later. Dev-only cost, never in a default build.
                swapchain_.wait_frames();
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
            renderer2d_.frame_begin(static_cast<float>(swapchain_.width()),
                                    static_cast<float>(swapchain_.height()));
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
            bool frame_rendered = false;
            gfx::Swapchain::Frame frame{};
            if (!hidden) {
                // Dark slate background; apps draw their UI on top. The
                // depth clear (1.0 - far plane, the LEQUAL convention) is
                // gfx's half of begin_render_pass.
                static constexpr float kClearColor[4] = {0.08f, 0.08f, 0.10f,
                                                         1.0f};
                if (swapchain_.acquire(&frame) == gfx::AcquireStatus::out_of_date) {
                    // This frame cannot run: nothing was acquired or
                    // signaled, and gfx kept the slot signaled and
                    // pristine. Drop the commands submitted for it instead
                    // of letting them leak into a later frame (queue
                    // contract: same-frame execution,
                    // render_command_queue.hpp; the 2D renderer keeps the
                    // same contract).
                    write_queue_.clear();
                    renderer2d_.frame_drop();
                    rebuild_swapchain = true;  // the loop top recreates
                } else {
                    swapchain_.begin_render_pass(frame, kClearColor);
                    VkCommandBuffer cmd =
                        static_cast<VkCommandBuffer>(frame.command_buffer);

                    // 2D world first, widgets on top. This slot's fence was
                    // waited inside acquire, so the batch's vertex upload
                    // and the chunk descriptor writes cannot race the
                    // submission that last used them.
                    renderer2d_.flush(cmd, frame.frame_index,
                                      static_cast<float>(swapchain_.width()),
                                      static_cast<float>(swapchain_.height()));

                    ImGui_ImplVulkan_RenderDrawData(draw_data, cmd);

                    // App-submitted commands run here, inside the render
                    // pass, after the UI draw data - the seam for
                    // non-ImGui GPU work (overlays, offscreen passes).
                    // The swap turns the queue the app filled during
                    // on_update() into the read queue; frames that cannot
                    // run (the bail path above, a hidden window) clear the
                    // write queue instead - a command never outlives the
                    // frame it was submitted in.
                    std::swap(write_queue_, read_queue_);
                    try {
                        read_queue_.execute();
                    } catch (const std::exception& e) {
                        core::error("ui: exception in render command queue: {}",
                                    e.what());
                        host_->request_stop();
                    } catch (...) {
                        core::error("ui: unknown exception in render command queue");
                        host_->request_stop();
                    }

                    swapchain_.end_render_pass(frame);
                    swapchain_.submit(frame);
                    frame_rendered = true;
                }
            } else {
                // Zero-size (minimized) frame: same rule as the bail path
                // above - drop, never defer.
                write_queue_.clear();
                renderer2d_.frame_drop();
            }

            if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
                ImGui::UpdatePlatformWindows();
                ImGui::RenderPlatformWindowsDefault();
            }
            if (frame_rendered) {
                if (swapchain_.present(frame) == gfx::PresentStatus::needs_rebuild) {
                    rebuild_swapchain = true;
                }
            }
        }
        return 0;
    }

    // --- lifecycle ----------------------------------------------------------

    // Context stage: the platform host and the Vulkan instance (inside
    // gfx::Gpu, since stage 3b) - everything that survives an Android
    // surface loss. Runs once per process. The
    // sink wiring happens here, before host_->init(): the platform installs
    // its callbacks against that sink inside init().
    bool startup_context() {
        // The single funnel every platform callback reads (see the event
        // system block) plus the gui library's raw input tap.
        event_sink_.forward = [this](ws::input::Event& event) { forward_event(event); };
        event_sink_.input = &gui_input_;  // gui tap: raw input, no filtering
        host_ = make_window_host();
        if (!host_->init(event_sink_, config)) {
            return false;
        }
        return init_gpu();
    }

    // Window stage: attaches the render stack to the platform's current
    // window. Surface first (through gfx::Gpu - the FIRST attach also
    // brings the device stage up inside it), then the ImGui pool and the
    // renderer/ImGui contexts (first attach only), then the swapchain.
    // Re-attaches on Android after a surface loss: the drift rules
    // re-initialize only what the OS round-trip actually invalidated, so
    // app state (fonts, textures, ImGui contexts) survives untouched.
    bool attach_window(App& app) {
        const core::Status attached = gpu_.attach_surface();
        if (!attached) {
            core::log_error(attached.error());
            return false;
        }
        // The ImGui pool rides the device stage: created at the first
        // attach, reused by every re-attach - a null pool IS the "not up
        // yet" marker (the device/queue/allocator half is gfx's own
        // tracking).
        if (imgui_pool == VK_NULL_HANDLE && !create_imgui_pool()) {
            return false;
        }
        int fb_width = 0;
        int fb_height = 0;
        host_->framebuffer_size(&fb_width, &fb_height);
        if (fb_width <= 0 || fb_height <= 0) {
            return false;
        }
        // The window stage's presentation half: the swapchain (with the
        // render pass + framebuffers it carries) against the Gpu the
        // surface attach above brought up. A failure cleans up after
        // itself inside gfx - the Status carries the diagnostic.
        const core::Status created =
            swapchain_.create(gpu_, fb_width, fb_height, {config.vsync});
        if (!created) {
            core::log_error(created.error());
            return false;
        }
        if (!gpu_context_up) {
            // First attach only. Before on_setup(): apps load textures
            // there, and the r2d free functions need an active renderer.
            // init() may fail mid-way; shutdown() doubles as the
            // (idempotent) failure-path cleanup.
            // The tier gfx's probe picked (parameterized by this very
            // table size at init_gpu()) maps onto the renderer's own
            // spelling.
            const gfx::DescriptorIndexing indexing = gpu_.descriptor_indexing();
            texture_tier_ = r2d::TextureTier{.bindless = indexing.bindless,
                                            .update_after_bind =
                                                indexing.update_after_bind};
            if (!renderer2d_.init(device(), physical_device(), gpu_, queue(),
                                  queue_family(), render_pass(), format(),
                                  texture_tier_)) {
                return false;
            }
            r2d::set_active(&renderer2d_);
            // The gui input snapshot publishes the same way (ws::input::frame_input()).
            ws::input::set_active_input(&gui_input_);
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
            if (!imgui_vk_up || swapchain_.image_count() != imgui_image_count ||
                format() != imgui_format) {
                if (imgui_vk_up) {
                    ImGui_ImplVulkan_Shutdown();
                }
                if (!init_imgui_backend()) {
                    return false;
                }
            }
            if (!renderer2d_.on_swapchain_recreated(render_pass(), format())) {
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
        core::info("ui: \"{}\" ready: {}x{}, {} swapchain images, "
                   "{} frames in flight, {} present",
                   config.title, swapchain_.width(), swapchain_.height(),
                   swapchain_.image_count(), gfx::kMaxFramesInFlight,
                   present_mode_name(static_cast<VkPresentModeKHR>(
                       swapchain_.present_mode())));
        return true;
    }

    // Tears the window stage down, in the same order final shutdown would:
    // backends, the swapchain (through gfx::Swapchain), surface (through
    // gfx::Gpu).
    // Idempotent - it is BOTH the
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
        swapchain_.destroy();
        gpu_.detach_surface();
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
        // Same for the gui input tap: post-run ws::input::frame_input() reports an
        // empty frame, same drop-safe contract.
        ws::input::set_active_input(nullptr);

        // The ImGui pool is a device object: destroy it while the device
        // still exists, BEFORE gfx tears the device stage down (the old
        // order destroyed the allocator first - legal, but pool-first is
        // the cleaner dependency direction).
        if (imgui_pool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device(), imgui_pool, nullptr);
            imgui_pool = VK_NULL_HANDLE;
        }
        // The whole Vulkan device layer: surface, allocator, device,
        // validation messenger, instance, the volk globals - in that
        // order, every step guarded and idempotent inside
        // gfx::Gpu::shutdown() (volkFinalize is last there: no vk* call
        // may happen after it).
        gpu_.shutdown();
        // The host goes down after the Vulkan objects its surface fed: on
        // desktop this is where the GLFW window and the library die (the
        // host destructor), with the loop already out of the pump.
        host_.reset();
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

bool App::prefers_dark() const {
    // The OS theme through the platform seam; before run() (and after a
    // failed startup) there is no window platform, so the light default
    // stands - the same answer a host without a probe gives.
    return impl_->host_ != nullptr && impl_->host_->prefers_dark();
}

void App::set_title(std::string_view title) {
    // The composed string handed to the platform window; before run()
    // (and after a failed startup) there is no window to retitle, and
    // a host whose platform owns the label no-ops inside the seam.
    if (impl_->host_) {
        impl_->host_->set_title(title);
    }
}

void App::set_cursor(CursorShape shape) {
    // Same seam story as set_title: no window before run(), and a host
    // without a cursor API (Android's glue) no-ops inside the seam.
    if (impl_->host_) {
        impl_->host_->set_cursor(shape);
    }
}

void App::install_clipboard(gui::GuiContext& gui) {
    // Nothing to wire before run() (or after a failed startup) - the
    // unset seam is the no-clipboard answer, the same one a host
    // without a clipboard gives.
    if (impl_->host_) {
        impl_->host_->install_clipboard(gui);
    }
}

void App::install_job_system(ws::jobs::JobSystem& jobs) {
    // A pointer store, nothing more: the drain site in the loop reads
    // the member every iteration, so installing any time - on_setup()
    // is the natural point, mid-run works too - takes effect on the
    // next pump. The system stays caller-owned; the shell never drains
    // after the loop, which is what the run()-outlives-it contract in
    // the header rests on.
    impl_->jobs_ = &jobs;
}

}  // namespace ws::ui
