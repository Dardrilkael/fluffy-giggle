#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <string_view>

#include "gpu_choice.h"        // internal: the pure decision helpers (src/)
#include "swapchain_choice.h"  // internal: the swapchain decision helpers (src/)

#include <ws/gfx/gpu.hpp>
#include <ws/gfx/swapchain.hpp>

// gfx's own suite: what runs without a GPU - the pure decision helpers
// (the descriptor-indexing tier table, the depth-format walk, the
// swapchain choices) and the null-state contracts (Gpu's getters before
// init, lifecycle misuse, idempotent teardown; Swapchain's lifecycle and
// frame flow before create; the phase-C memory/object guards - descriptor
// validation, the device-stage Status, null-tolerant destroys). No
// Vulkan device is ever faked: Gpu is never init()ed and Swapchain never
// created here, so no volk entry point runs; the cases only pin the
// guarded, headless paths.

namespace {

// pick_depth_format's predicate for the walk tests: approves exactly one
// format, as the format-properties query would.
bool supports_one(uint32_t format, void* user) {
    return format == *static_cast<const uint32_t*>(user);
}

}  // namespace

// --- the tier decision (gpu_choice.h) ---------------------------------------

TEST_CASE("decide_indexing maps every advertised capability to the tier") {
    const auto caps_with = [](bool loader, bool dialect, bool nonuniform,
                               bool partial, bool uab, bool limits) {
        ws::gfx::IndexingCaps caps{};
        caps.loader_1_1 = loader;
        caps.dialect = dialect;
        caps.nonuniform = nonuniform;
        caps.partially_bound = partial;
        caps.update_after_bind = uab;
        caps.limits_cover_request = limits;
        return caps;
    };

    SUBCASE("everything advertised: bindless, update-after-bind rides along") {
        const ws::gfx::DescriptorIndexing tier = ws::gfx::decide_indexing(
            caps_with(true, true, true, true, true, true), true);
        CHECK(tier.bindless == true);
        CHECK(tier.update_after_bind == true);
    }
    SUBCASE("no update-after-bind: bindless with idle-flush writes") {
        const ws::gfx::DescriptorIndexing tier = ws::gfx::decide_indexing(
            caps_with(true, true, true, true, false, true), true);
        CHECK(tier.bindless == true);
        CHECK(tier.update_after_bind == false);
    }
    SUBCASE("a 1.0 loader pins the chunked fallback") {
        CHECK(ws::gfx::decide_indexing(caps_with(false, true, true, true, true, true),
                                       true)
                  .bindless == false);
    }
    SUBCASE("no descriptor-indexing dialect: chunked") {
        CHECK(ws::gfx::decide_indexing(caps_with(true, false, true, true, true, true),
                                       true)
                  .bindless == false);
    }
    SUBCASE("missing nonuniform indexing: chunked") {
        CHECK(ws::gfx::decide_indexing(caps_with(true, true, false, true, true, true),
                                       true)
                  .bindless == false);
    }
    SUBCASE("missing partially-bound: chunked") {
        CHECK(ws::gfx::decide_indexing(caps_with(true, true, true, false, true, true),
                                       true)
                  .bindless == false);
    }
    SUBCASE("limits short of the requested table: chunked") {
        CHECK(ws::gfx::decide_indexing(caps_with(true, true, true, true, true, false),
                                       true)
                  .bindless == false);
    }
    SUBCASE("bindless not requested: chunked even on a fully capable device") {
        const ws::gfx::DescriptorIndexing tier =
            ws::gfx::decide_indexing(caps_with(true, true, true, true, true, true),
                                     false);
        CHECK(tier.bindless == false);
        CHECK(tier.update_after_bind == false);
    }
}

// --- the depth-format walk (gpu_choice.h) -----------------------------------

TEST_CASE("pick_depth_format takes the first supported candidate") {
    const uint32_t candidates[] = {10, 11, 12};
    constexpr uint32_t count = sizeof(candidates) / sizeof(candidates[0]);

    SUBCASE("first supported wins, order preserved") {
        uint32_t supported = 11;
        CHECK(ws::gfx::pick_depth_format(candidates, count, &supports_one,
                                         &supported) == 11);
        supported = 10;
        CHECK(ws::gfx::pick_depth_format(candidates, count, &supports_one,
                                         &supported) == 10);
    }
    SUBCASE("no candidate qualifies: VK_FORMAT_UNDEFINED (0)") {
        uint32_t supported = 99;
        CHECK(ws::gfx::pick_depth_format(candidates, count, &supports_one,
                                         &supported) == 0);
    }
    SUBCASE("empty candidate list: 0") {
        CHECK(ws::gfx::pick_depth_format(nullptr, 0, &supports_one, nullptr) == 0);
    }
}

// --- the swapchain choices (swapchain_choice.h) -------------------------------
// The decision tables swapchain.cpp feeds from the surface queries. The
// values below are plain numbers (the helpers take uint32 by design):
// real spec numbering where a table names real modes and bits, arbitrary
// stand-ins elsewhere - chosen to exercise the walk orders.

TEST_CASE("choose_surface_format takes the first preferred format offered") {
    using ws::gfx::SurfaceFormatChoice;
    const SurfaceFormatChoice offered[] = {{50, 7}, {44, 7}, {43, 9}};
    const uint32_t preferred[] = {43, 44};

    SUBCASE("an empty list is no restrictions: the caller's answer") {
        const SurfaceFormatChoice unrestricted{77, 7};
        const SurfaceFormatChoice choice = ws::gfx::choose_surface_format(
            nullptr, 0, preferred, 2, 7, unrestricted);
        CHECK(choice.format == 77u);
        CHECK(choice.color_space == 7u);
    }
    SUBCASE("the color space gates the walk: the next preference wins") {
        // 43 is the top preference but only offered in the wrong color
        // space, so 44 takes it.
        const SurfaceFormatChoice choice = ws::gfx::choose_surface_format(
            offered, 3, preferred, 2, 7, SurfaceFormatChoice{});
        CHECK(choice.format == 44u);
        CHECK(choice.color_space == 7u);
    }
    SUBCASE("preference order outranks the surface's list order") {
        const SurfaceFormatChoice list_order[] = {{37, 7}, {44, 7}};
        const uint32_t prefs[] = {44, 37};
        const SurfaceFormatChoice choice = ws::gfx::choose_surface_format(
            list_order, 2, prefs, 2, 7, SurfaceFormatChoice{});
        CHECK(choice.format == 44u);
        CHECK(choice.color_space == 7u);
    }
    SUBCASE("no preferred format offered: the surface's first entry") {
        const SurfaceFormatChoice only[] = {{99, 7}};
        const SurfaceFormatChoice choice = ws::gfx::choose_surface_format(
            only, 1, preferred, 2, 7, SurfaceFormatChoice{});
        CHECK(choice.format == 99u);
        CHECK(choice.color_space == 7u);
    }
}

TEST_CASE("choose_present_mode takes the first preference supported") {
    // Real VkPresentModeKHR numbering: immediate 0, mailbox 1, fifo 2,
    // fifo_relaxed 3. The vsync-on path never walks (FIFO is assigned
    // directly, the one mode the spec guarantees); this is the vsync-off
    // walk and its fallback.
    const uint32_t prefs[] = {1, 0};  // mailbox, then immediate
    const uint32_t fifo = 2;

    SUBCASE("first preference supported wins") {
        const uint32_t avail[] = {fifo, 1};
        CHECK(ws::gfx::choose_present_mode(prefs, 2, avail, 2, fifo) == 1u);
    }
    SUBCASE("an unsupported first preference walks to the next") {
        const uint32_t relaxed[] = {3, 0};  // fifo_relaxed, then immediate
        const uint32_t avail[] = {fifo, 0};
        CHECK(ws::gfx::choose_present_mode(relaxed, 2, avail, 2, fifo) == 0u);
    }
    SUBCASE("nothing preferred is supported: the FIFO fallback") {
        const uint32_t avail[] = {fifo};
        CHECK(ws::gfx::choose_present_mode(prefs, 2, avail, 1, fifo) == fifo);
    }
}

TEST_CASE("choose_extent answers the fixed extent, else the clamped size") {
    using ws::gfx::ExtentChoice;
    const ExtentChoice lo{320, 200};    // the surface's min
    const ExtentChoice hi{2560, 1440};  // the surface's max
    // The spec's "match the window" sentinel: the surface has no fixed
    // size, so the framebuffer size - clamped into the bounds above -
    // is the extent.
    const ExtentChoice match_window{0xFFFFFFFFu, 0xFFFFFFFFu};

    SUBCASE("a fixed current extent ignores the framebuffer size") {
        const ExtentChoice fixed{800, 600};
        ExtentChoice out{};
        CHECK(ws::gfx::choose_extent(fixed, lo, hi, 100, 50, &out));
        CHECK(out.width == 800u);
        CHECK(out.height == 600u);
    }
    SUBCASE("the sentinel clamps the framebuffer size into the bounds") {
        ExtentChoice out{};
        CHECK(ws::gfx::choose_extent(match_window, lo, hi, 1280, 720, &out));
        CHECK(out.width == 1280u);
        CHECK(out.height == 720u);
    }
    SUBCASE("clamped from below and from above") {
        ExtentChoice out{};
        CHECK(ws::gfx::choose_extent(match_window, lo, hi, 100, 100, &out));
        CHECK(out.width == 320u);
        CHECK(out.height == 200u);
        CHECK(ws::gfx::choose_extent(match_window, lo, hi, 4000, 3000, &out));
        CHECK(out.width == 2560u);
        CHECK(out.height == 1440u);
    }
    SUBCASE("a zero-sized extent answers false - no visible area") {
        const ExtentChoice fixed{0, 600};  // fixed, but no width
        ExtentChoice out{};
        CHECK_FALSE(ws::gfx::choose_extent(fixed, lo, hi, 1280, 720, &out));
        CHECK(out.width == 0u);
    }
}

TEST_CASE("choose_image_count asks for one above the minimum") {
    CHECK(ws::gfx::choose_image_count(2, 0) == 3u);  // max 0: unlimited
    CHECK(ws::gfx::choose_image_count(2, 8) == 3u);  // room to spare
    CHECK(ws::gfx::choose_image_count(2, 2) == 2u);  // min == max pinned
    CHECK(ws::gfx::choose_image_count(4, 0) == 5u);
}

TEST_CASE("choose_composite_alpha prefers opaque, else the lowest bit") {
    const uint32_t opaque = 1;  // VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
    CHECK(ws::gfx::choose_composite_alpha(0b111, opaque) == opaque);
    CHECK(ws::gfx::choose_composite_alpha(0b110, opaque) == 2u);
    CHECK(ws::gfx::choose_composite_alpha(0, opaque) == opaque);
}

// --- Gpu's null-state contract (no loader, no device) ------------------------

TEST_CASE("a default-constructed Gpu reports the null state") {
    ws::gfx::Gpu gpu;
    CHECK(gpu.instance() == nullptr);          // VkInstance
    CHECK(gpu.surface() == nullptr);          // VkSurfaceKHR
    CHECK(gpu.physical_device() == nullptr);  // VkPhysicalDevice
    CHECK(gpu.device() == nullptr);            // VkDevice
    CHECK(gpu.queue() == nullptr);             // VkQueue
    CHECK(gpu.allocator() == nullptr);         // VmaAllocator
    CHECK(gpu.queue_family() == 0);
    CHECK(gpu.depth_format() == 0);  // VK_FORMAT_UNDEFINED
    CHECK(gpu.descriptor_indexing().bindless == false);
    CHECK(gpu.descriptor_indexing().update_after_bind == false);
}

TEST_CASE("lifecycle misuse is a Status, never a crash") {
    ws::gfx::Gpu gpu;

    // attach before init: the guarded error path.
    const ws::core::Status attached = gpu.attach_surface();
    REQUIRE_FALSE(attached);
    CHECK(attached.error().category == std::string_view("gfx"));
    CHECK(attached.error().code != 0);
    CHECK_FALSE(attached.error().message.empty());

    // detach/shutdown on nothing: no-ops, idempotent.
    gpu.detach_surface();
    gpu.shutdown();
    gpu.shutdown();
    gpu.detach_surface();

    // The null state survives the teardown cycles.
    CHECK(gpu.device() == nullptr);
    CHECK(gpu.descriptor_indexing().bindless == false);
}

TEST_CASE("init rejects an unusable SurfaceSource before any loader call") {
    {
        ws::gfx::Gpu gpu;
        // A source with extensions but no pointer array.
        ws::gfx::SurfaceSource source{};
        source.instance_extension_count = 2;
        source.instance_extensions = nullptr;
        source.create_surface = [](void*, void**, void*) { return true; };
        const ws::core::Status status = gpu.init(source, ws::gfx::DeviceOptions{});
        REQUIRE_FALSE(status);
        CHECK(gpu.instance() == nullptr);
    }
    {
        ws::gfx::Gpu gpu;
        // No surface callback: no attach could ever run.
        ws::gfx::SurfaceSource source{};
        const ws::core::Status status = gpu.init(source, ws::gfx::DeviceOptions{});
        REQUIRE_FALSE(status);
        CHECK(gpu.instance() == nullptr);
    }
}

// --- the device-memory + object helpers (phase C; no device stage) ----------
// The desc/word values below are opaque stand-ins: the guards fire before
// any Vulkan call could read them, so their numeric meaning never matters.

TEST_CASE("the creates reject bad descriptors first") {
    ws::gfx::Gpu gpu;  // never init()ed: no device stage - the cheap
                       // descriptor validation fires before any stage check

    SUBCASE("a zero-size buffer desc") {
        ws::gfx::BufferOut out;
        out.buffer = reinterpret_cast<void*>(1);  // cleared on failure
        const ws::core::Status st =
            gpu.create_buffer(ws::gfx::BufferDesc{0, 1u}, out);
        REQUIRE_FALSE(st);
        CHECK(st.error().category == std::string_view("gfx"));
        CHECK(st.error().code != 0);
        CHECK(out.buffer == nullptr);
        CHECK(out.allocation == nullptr);
        CHECK(out.mapped == nullptr);
    }
    SUBCASE("a zero-extent image desc") {
        ws::gfx::ImageOut out;
        const ws::core::Status st =
            gpu.create_image(ws::gfx::ImageDesc{0, 64, 37u, 4u}, out);
        REQUIRE_FALSE(st);
        CHECK(st.error().code != 0);
        CHECK(out.image == nullptr);
        CHECK(out.allocation == nullptr);
    }
    SUBCASE("a zero-mip image desc") {
        ws::gfx::ImageOut out;
        const ws::core::Status st =
            gpu.create_image(ws::gfx::ImageDesc{64, 64, 37u, 4u, 0u}, out);
        REQUIRE_FALSE(st);
        CHECK(st.error().code != 0);
        CHECK(out.image == nullptr);
        CHECK(out.allocation == nullptr);
    }
    SUBCASE("empty shader words") {
        void* module = reinterpret_cast<void*>(1);
        const ws::core::Status st =
            gpu.create_shader_module(nullptr, 4, &module);
        REQUIRE_FALSE(st);
        CHECK(st.error().code != 0);
        CHECK(module == nullptr);
    }
}

TEST_CASE("the creates need the device stage - a Status, never a crash") {
    ws::gfx::Gpu gpu;
    const std::uint32_t words[4] = {0, 1, 2, 3};

    ws::gfx::BufferOut buffer;
    const ws::core::Status buffer_st =
        gpu.create_buffer(ws::gfx::BufferDesc{1024, 1u}, buffer);
    REQUIRE_FALSE(buffer_st);
    CHECK(buffer_st.error().category == std::string_view("gfx"));
    CHECK(buffer_st.error().code != 0);
    CHECK(buffer.buffer == nullptr);

    ws::gfx::ImageOut image;
    const ws::core::Status image_st =
        gpu.create_image(ws::gfx::ImageDesc{64, 64, 37u, 4u}, image);
    REQUIRE_FALSE(image_st);
    CHECK(image_st.error().code != 0);

    void* module = nullptr;
    const ws::core::Status module_st =
        gpu.create_shader_module(words, 4, &module);
    REQUIRE_FALSE(module_st);
    CHECK(module_st.error().code != 0);
    CHECK(module == nullptr);
}

TEST_CASE("destroy and flush are null-tolerant no-ops on a dead Gpu") {
    ws::gfx::Gpu gpu;  // no device stage: nothing to free through
    gpu.destroy_buffer(nullptr, nullptr);
    gpu.destroy_image(nullptr, nullptr);
    gpu.flush_buffer(nullptr);
    gpu.destroy_shader_module(nullptr);
    // Even non-null handles: without the device stage there is no
    // allocator to free through, so the destroys stay no-ops - failure
    // teardowns never need their own stage checks.
    gpu.destroy_buffer(reinterpret_cast<void*>(1), reinterpret_cast<void*>(2));
    gpu.destroy_image(reinterpret_cast<void*>(1), reinterpret_cast<void*>(2));
    gpu.destroy_shader_module(reinterpret_cast<void*>(1));
}

// --- Swapchain's null-state contract (no device, never created) ---------------

TEST_CASE("a default-constructed Swapchain reports the null state") {
    ws::gfx::Swapchain swapchain;
    CHECK(swapchain.render_pass() == nullptr);  // VkRenderPass
    CHECK(swapchain.format() == 0);             // VK_FORMAT_UNDEFINED
    CHECK(swapchain.present_mode() == 2);       // VK_PRESENT_MODE_FIFO_KHR
    CHECK(swapchain.image_count() == 0);
    CHECK(swapchain.width() == 0);
    CHECK(swapchain.height() == 0);
}

TEST_CASE("Swapchain lifecycle misuse is a Status, never a crash") {
    ws::gfx::Gpu gpu;  // never init()ed: no device stage, no surface
    ws::gfx::Swapchain swapchain;

    // create() against a dead Gpu: the guarded error path - build()
    // never runs, so no volk entry point is touched.
    const ws::core::Status created =
        swapchain.create(gpu, 1280, 720, ws::gfx::SwapchainOptions{});
    REQUIRE_FALSE(created);
    CHECK(created.error().category == std::string_view("gfx"));
    CHECK(created.error().code != 0);
    CHECK_FALSE(created.error().message.empty());

    // The failed create stored nothing: recreate() still answers the
    // never-created Status instead of walking a null Gpu.
    const ws::core::Status recreated = swapchain.recreate(1280, 720);
    REQUIRE_FALSE(recreated);
    CHECK(recreated.error().category == std::string_view("gfx"));
    CHECK(recreated.error().code != 0);

    // destroy() on nothing: safe, idempotent.
    swapchain.destroy();
    swapchain.destroy();

    // The null state survives the whole cycle.
    CHECK(swapchain.render_pass() == nullptr);
    CHECK(swapchain.image_count() == 0);
}

TEST_CASE("Swapchain's frame flow is guarded when empty") {
    ws::gfx::Swapchain swapchain;
    ws::gfx::Swapchain::Frame frame{};

    // The frame flow guards only the EMPTY state (the consumer frames
    // with its window stage up): acquire answers the out-of-date bail so
    // the caller's recreate path runs, present answers needs_rebuild,
    // and the recording middle is a no-op.
    CHECK(swapchain.acquire(&frame) == ws::gfx::AcquireStatus::out_of_date);
    CHECK(swapchain.present(frame) == ws::gfx::PresentStatus::needs_rebuild);
    const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    swapchain.begin_render_pass(frame, clear);
    swapchain.end_render_pass(frame);
    swapchain.submit(frame);
    swapchain.wait_frames();  // no fences were ever submitted
    CHECK(swapchain.render_pass() == nullptr);
}
