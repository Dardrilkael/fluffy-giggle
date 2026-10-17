#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <string_view>

#include "gpu_choice.h"  // internal: the pure decision helpers (src/)

#include <ws/gfx/gpu.hpp>

// gfx's own suite: what runs without a GPU - the pure decision helpers
// (the descriptor-indexing tier table, the depth-format walk) and Gpu's
// null-state contract (getters before init, lifecycle misuse, idempotent
// teardown). No Vulkan device is ever faked: Gpu is never init()ed here,
// so no volk entry point runs; the cases only pin the guarded, headless
// paths.

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
