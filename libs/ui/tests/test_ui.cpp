#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstddef>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "atlas_packer.h"  // internal: pure-CPU atlas packer (src/ on the test include path)
#include "batch_core.h"     // internal: pure-CPU batch builder (same path)
#include "renderer2d.h"     // internal: command recording + sort/replay (same path)
#include <ws/ui/app.hpp>
#include <ws/ui/event.hpp>
#include <ws/ui/gui/gui_context.hpp>  // must stay includable without any third-party header
#include <ws/ui/gui/id_stack.hpp>      // (same - one include line per header proves each parses alone)
#include <ws/ui/gui/input_frame.hpp>
#include <ws/ui/gui/layout_cursor.hpp>
#include <ws/ui/gui/rect.hpp>
#include <ws/ui/gui/theme.hpp>  // (same - the stock chrome parses headless)
#include <ws/ui/gui/widgets.hpp>  // (same - proves the widget layer parses headless)
#include <ws/ui/render_command_queue.hpp>
#include <ws/ui/renderer2d.hpp>  // must stay includable without any third-party header
#include <ws/ui/widgets.hpp>  // must stay includable without any third-party header

// The shell must be constructible and destructible without a window, GPU or
// Vulkan loader - all of that only happens inside run(). The apps/playground
// app is the real end-to-end smoke test for those paths.

TEST_CASE("AppConfig defaults are sane") {
    ws::ui::AppConfig config;
    CHECK(config.title == "ws app");
    CHECK(config.width == 1280);
    CHECK(config.height == 720);
    CHECK(config.vsync == true);
}

TEST_CASE("App constructs and destructs without run()") {
    ws::ui::AppConfig config;
    config.title = "headless";
    {
        ws::ui::App app{config};
    }  // destroyed here - must not require run() to have been called
    CHECK(true);
}

TEST_CASE("App is neither copyable nor movable") {
    static_assert(!std::is_copy_constructible_v<ws::ui::App>);
    static_assert(!std::is_move_constructible_v<ws::ui::App>);
    CHECK(true);
}

// --- event system ---------------------------------------------------------

TEST_CASE("every event carries its own type and starts unhandled") {
    using ws::ui::EventType;
    CHECK(ws::ui::WindowCloseEvent{}.type() == EventType::WindowClose);
    CHECK(ws::ui::WindowCloseEvent{}.static_type() == EventType::WindowClose);
    CHECK(ws::ui::WindowResizeEvent{640, 480}.type() == EventType::WindowResize);
    CHECK(ws::ui::KeyPressedEvent{'D', false}.type() == EventType::KeyPressed);
    CHECK(ws::ui::KeyReleasedEvent{'D'}.type() == EventType::KeyReleased);
    CHECK(ws::ui::MouseButtonPressedEvent{0}.type() == EventType::MouseButtonPressed);
    CHECK(ws::ui::MouseButtonReleasedEvent{1}.type() == EventType::MouseButtonReleased);
    CHECK(ws::ui::MouseMovedEvent{1.0f, 2.0f}.type() == EventType::MouseMoved);
    CHECK(ws::ui::MouseScrolledEvent{0.0f, -1.0f}.type() == EventType::MouseScrolled);

    ws::ui::WindowResizeEvent resize{800, 600};
    CHECK(resize.handled == false);
    CHECK(resize.width == 800);
    CHECK(resize.height == 600);
    ws::ui::KeyPressedEvent key{'D', true};
    CHECK(key.keycode == 'D');
    CHECK(key.repeat == true);
}

TEST_CASE("dispatcher fires only on the matching type and honors the return") {
    ws::ui::WindowResizeEvent event{800, 600};
    ws::ui::EventDispatcher dispatcher{event};

    bool called = false;
    CHECK(dispatcher.dispatch<ws::ui::KeyPressedEvent>(
              [&](const ws::ui::KeyPressedEvent&) {
                  called = true;
                  return true;
              }) == false);
    CHECK(called == false);  // wrong type: handler must not fire

    CHECK(dispatcher.dispatch<ws::ui::WindowResizeEvent>(
              [&](const ws::ui::WindowResizeEvent& e) {
                  called = true;
                  CHECK(e.width == 800);
                  return false;  // declines: event stays unhandled
              }) == false);
    CHECK(called);
    CHECK(event.handled == false);

    CHECK(dispatcher.dispatch<ws::ui::WindowResizeEvent>(
              [](const ws::ui::WindowResizeEvent&) { return true; }) == true);
    CHECK(event.handled == true);
}

TEST_CASE("dispatcher skips events the shell pre-marked handled") {
    ws::ui::MouseMovedEvent event{3.0f, 4.0f};
    event.handled = true;  // e.g. ImGui captured the mouse over a widget
    ws::ui::EventDispatcher dispatcher{event};

    bool called = false;
    CHECK(dispatcher.dispatch<ws::ui::MouseMovedEvent>(
              [&](const ws::ui::MouseMovedEvent&) {
                  called = true;
                  return true;
              }) == false);
    CHECK(called == false);
}

// --- render command queue --------------------------------------------------

TEST_CASE("command queue runs in submission order and clears on execute") {
    ws::ui::RenderCommandQueue queue;
    CHECK(queue.empty());

    std::vector<int> order;
    const int captured = 100;  // trivially copyable captures only
    queue.submit([&order] { order.push_back(1); });
    queue.submit([&order, captured] { order.push_back(2 + captured); });
    CHECK(queue.empty() == false);

    queue.execute();
    REQUIRE(order.size() == 2);  // two commands ran
    CHECK(order[0] == 1);
    CHECK(order[1] == 102);
    CHECK(queue.empty());

    order.clear();
    queue.execute();  // cleared: nothing re-runs
    CHECK(order.empty());
}

TEST_CASE("a throwing command drops the rest of the queue") {
    ws::ui::RenderCommandQueue queue;
    bool second_ran = false;
    queue.submit([] { throw std::runtime_error{"boom"}; });
    queue.submit([&second_ran] { second_ran = true; });

    CHECK_THROWS(queue.execute());
    CHECK(second_ran == false);  // first command threw: second never ran
    CHECK(queue.empty());         // and the half-run frame must not re-run

    queue.execute();  // reusable after the throw
    CHECK(second_ran == false);
}

// --- 2D renderer: pure-CPU batch core ----------------------------------------

namespace {

using ws::ui::r2d::batch::BatchCore;
using ws::ui::r2d::batch::Mat4;

// A default-camera VP on a square frame - the exact screen a unit quad of
// the world would render against.
Mat4 default_vp() {
    return ws::ui::r2d::batch::camera_vp(ws::ui::r2d::Camera2D{}, 100.0f, 100.0f);
}

bool push_test_quad(BatchCore& core, std::uint32_t texture_id) {
    const float pos[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    const float uv[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    const float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    return core.push_quad(pos, uv, color, texture_id);
}

}  // namespace

TEST_CASE("rgba packs 0xRRGGBBAA and unpack_rgba normalizes") {
    using ws::ui::r2d::rgba;
    CHECK(rgba(255, 0, 0, 128) == 0xFF000080u);
    CHECK(rgba(0x12, 0x34, 0x56) == 0x123456FFu);  // alpha defaults to opaque

    float c[4];
    ws::ui::r2d::batch::unpack_rgba(0xFF000080u, c);
    CHECK(c[0] == doctest::Approx(1.0f));
    CHECK(c[1] == 0.0f);
    CHECK(c[2] == 0.0f);
    CHECK(c[3] == doctest::Approx(128.0f / 255.0f));
}

TEST_CASE("camera_vp default centers the origin and maps y-down") {
    using ws::ui::r2d::batch::camera_vp;
    const auto vp = camera_vp(ws::ui::r2d::Camera2D{}, 1000.0f, 500.0f);
    const auto project = [&](float px, float py) {
        return std::pair{vp.m[0] * px + vp.m[4] * py + vp.m[12],
                         vp.m[1] * px + vp.m[5] * py + vp.m[13]};
    };

    auto [x, y] = project(0.0f, 0.0f);  // the camera center -> clip origin
    CHECK(x == doctest::Approx(0.0f));
    CHECK(y == doctest::Approx(0.0f));

    std::tie(x, y) = project(500.0f, 250.0f);  // half-extent corner -> bottom-right
    CHECK(x == doctest::Approx(1.0f));
    CHECK(y == doctest::Approx(1.0f));  // +y world renders DOWN (Vulkan NDC)

    std::tie(x, y) = project(0.0f, 250.0f);  // straight down -> bottom edge
    CHECK(x == doctest::Approx(0.0f));
    CHECK(y == doctest::Approx(1.0f));
}

TEST_CASE("camera_vp honors camera center and zoom") {
    using ws::ui::r2d::batch::camera_vp;
    const ws::ui::r2d::Camera2D camera{100.0f, 50.0f, 0.0f, 2.0f};
    const auto vp = camera_vp(camera, 1000.0f, 500.0f);
    const auto project = [&](float px, float py) {
        return std::pair{vp.m[0] * px + vp.m[4] * py + vp.m[12],
                         vp.m[1] * px + vp.m[5] * py + vp.m[13]};
    };

    auto [x, y] = project(100.0f, 100.0f * 0.5f);  // center of the camera
    CHECK(x == doctest::Approx(0.0f));
    CHECK(y == doctest::Approx(0.0f));

    std::tie(x, y) = project(350.0f, 175.0f);  // a half-extent away at zoom 2
    CHECK(x == doctest::Approx(1.0f));
    CHECK(y == doctest::Approx(1.0f));
}

TEST_CASE("camera_vp rotation convention is locked") {
    using ws::ui::r2d::batch::camera_vp;
    // 90 degrees: a world point right of center must land TOP-center
    // (positive rotation rolls the view clockwise).
    const ws::ui::r2d::Camera2D camera{0.0f, 0.0f, 1.5707963f, 1.0f};
    const auto vp = camera_vp(camera, 20.0f, 20.0f);
    const float x = vp.m[0] * 10.0f + vp.m[4] * 0.0f + vp.m[12];
    const float y = vp.m[1] * 10.0f + vp.m[5] * 0.0f + vp.m[13];
    CHECK(x == doctest::Approx(0.0f));
    CHECK(y == doctest::Approx(-1.0f));
}

TEST_CASE("push_quad emits 4 vertices with corner/uv/color/tex order") {
    BatchCore core;
    core.new_frame();
    REQUIRE(core.open_segment(default_vp()));

    const float pos[4][2] = {{1.0f, 2.0f}, {11.0f, 2.0f}, {11.0f, 12.0f}, {1.0f, 12.0f}};
    const float uv[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    const float color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    REQUIRE(core.push_quad(pos, uv, color, 7u));
    REQUIRE(core.vertices().size() == 4);

    for (int i = 0; i < 4; ++i) {  // TL, TR, BR, BL
        const auto& v = core.vertices()[static_cast<std::size_t>(i)];
        CHECK(v.pos[0] == pos[i][0]);
        CHECK(v.pos[1] == pos[i][1]);
        CHECK(v.uv[0] == uv[i][0]);
        CHECK(v.uv[1] == uv[i][1]);
        CHECK(v.color[0] == color[0]);  // flat color rides on all corners
        CHECK(v.color[1] == color[1]);
        CHECK(v.color[2] == color[2]);
        CHECK(v.color[3] == color[3]);
        CHECK(v.tex_index == 0.0f);
    }
    CHECK(core.chunk_count() == 1);
    CHECK(core.chunk(0).quad_count == 1);
    CHECK(core.chunk(0).texture_ids[0] == 7u);
    CHECK(core.chunk(0).vertex_offset == 0);
}

TEST_CASE("texture slots assign per unique id and reuse repeats") {
    BatchCore core;
    core.new_frame();
    REQUIRE(core.open_segment(default_vp()));

    REQUIRE(push_test_quad(core, 1u));
    REQUIRE(push_test_quad(core, 1u));  // repeat: reuses slot 0
    REQUIRE(push_test_quad(core, 2u));  // new: takes slot 1

    CHECK(core.chunk_count() == 1);
    const auto& chunk = core.chunk(0);
    CHECK(chunk.texture_count == 2);
    CHECK(chunk.texture_ids[0] == 1u);
    CHECK(chunk.texture_ids[1] == 2u);
    CHECK(chunk.quad_count == 3);
    CHECK(core.vertices()[7].tex_index == 0.0f);   // 2nd quad: reused slot
    CHECK(core.vertices()[11].tex_index == 1.0f);  // 3rd quad: fresh slot
}

TEST_CASE("texture overflow splits chunks and preserves order") {
    BatchCore core;
    core.new_frame();
    REQUIRE(core.open_segment(default_vp()));

    const std::uint32_t unique_textures = 33;  // one past the chunk table
    for (std::uint32_t id = 0; id < unique_textures; ++id) {
        REQUIRE(push_test_quad(core, id));
    }

    REQUIRE(core.chunk_count() == 2);
    CHECK(core.chunk(0).texture_count == ws::ui::r2d::batch::kMaxTexturesPerChunk);
    CHECK(core.chunk(0).quad_count == ws::ui::r2d::batch::kMaxTexturesPerChunk);
    CHECK(core.chunk(1).texture_count == 1);
    CHECK(core.chunk(1).quad_count == 1);
    CHECK(core.chunk(1).texture_ids[0] == 32u);
    CHECK(core.chunk(1).vertex_offset == 32u * 4);  // contiguous: painter order
    CHECK(core.chunk(1).vp.m[0] == core.chunk(0).vp.m[0]);  // same camera
    REQUIRE(core.vertices().size() == std::size_t(unique_textures) * 4);
    CHECK(core.vertices().back().tex_index == 0.0f);  // fresh table, slot 0
}

TEST_CASE("open_segment closes the current chunk and carries the new VP") {
    BatchCore core;
    core.new_frame();
    REQUIRE(core.open_segment(default_vp()));
    REQUIRE(push_test_quad(core, 5u));

    const auto zoomed = ws::ui::r2d::batch::camera_vp(
        ws::ui::r2d::Camera2D{0.0f, 0.0f, 0.0f, 2.0f}, 100.0f, 100.0f);
    REQUIRE(core.open_segment(zoomed));
    REQUIRE(push_test_quad(core, 5u));

    REQUIRE(core.chunk_count() == 2);
    CHECK(core.chunk(0).quad_count == 1);
    CHECK(core.chunk(1).quad_count == 1);
    CHECK(core.chunk(1).vertex_offset == 4);  // 4 vertices before it
    CHECK(core.chunk(1).vp.m[0] == doctest::Approx(zoomed.m[0]));
}

TEST_CASE("vertex cap drops draws without appending") {
    BatchCore core;
    core.new_frame();
    REQUIRE(core.open_segment(default_vp()));

    const auto quads = ws::ui::r2d::batch::kMaxVerticesPerFrame / 4;
    for (std::size_t i = 0; i < quads; ++i) {
        if (!push_test_quad(core, 0u)) {
            FAIL("quad ", i, " dropped below the cap");
        }
    }
    CHECK(core.vertices().size() == ws::ui::r2d::batch::kMaxVerticesPerFrame);

    CHECK(push_test_quad(core, 0u) == false);  // one past the cap drops
    CHECK(core.vertices().size() == ws::ui::r2d::batch::kMaxVerticesPerFrame);
}

TEST_CASE("chunk cap drops further segments and their draws") {
    BatchCore core;
    core.new_frame();
    for (std::uint32_t i = 0; i < ws::ui::r2d::batch::kMaxChunksPerFrame; ++i) {
        REQUIRE(core.open_segment(default_vp()));
    }
    CHECK(core.open_segment(default_vp()) == false);  // 9th segment: no chunk
    CHECK(core.chunk_count() == ws::ui::r2d::batch::kMaxChunksPerFrame);
    CHECK(push_test_quad(core, 0u) == false);  // segment stayed closed

    core.close_segment();
    CHECK(push_test_quad(core, 0u) == false);  // still closed after end()
}

TEST_CASE("line expansion builds a thickness quad") {
    float pos[4][2];
    float uv[4][2];
    REQUIRE(ws::ui::r2d::batch::line_quad(0.0f, 0.0f, 10.0f, 0.0f, 2.0f, pos, uv));

    CHECK(pos[0][0] == doctest::Approx(0.0f));   // A: p0 + half normal
    CHECK(pos[0][1] == doctest::Approx(1.0f));
    CHECK(pos[1][0] == doctest::Approx(0.0f));   // B: p0 - half normal
    CHECK(pos[1][1] == doctest::Approx(-1.0f));
    CHECK(pos[2][0] == doctest::Approx(10.0f));  // C: p1 - half normal
    CHECK(pos[2][1] == doctest::Approx(-1.0f));
    CHECK(pos[3][0] == doctest::Approx(10.0f));  // D: p1 + half normal
    CHECK(pos[3][1] == doctest::Approx(1.0f));

    CHECK(uv[0][0] == 0.0f);  // u runs along the line, v across it
    CHECK(uv[1][1] == 1.0f);
    CHECK(uv[2][0] == 1.0f);
    CHECK(uv[3][1] == 0.0f);
}

TEST_CASE("degenerate lines are dropped") {
    float pos[4][2];
    float uv[4][2];
    CHECK(ws::ui::r2d::batch::line_quad(1.0f, 1.0f, 1.0f, 1.0f, 2.0f, pos,
                                        uv) == false);  // zero length
    CHECK(ws::ui::r2d::batch::line_quad(0.0f, 0.0f, 10.0f, 0.0f, 0.0f, pos,
                                        uv) == false);  // no thickness
}

// --- sprite atlas packer -------------------------------------------------------

namespace {

// A solid RGBA8 image for the packer to borrow.
std::vector<std::uint8_t> solid_pixels(std::uint32_t w, std::uint32_t h,
                                       std::uint8_t r, std::uint8_t g,
                                       std::uint8_t b) {
    std::vector<std::uint8_t> pixels(std::size_t(w) * h * 4);
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i + 0] = r;
        pixels[i + 1] = g;
        pixels[i + 2] = b;
        pixels[i + 3] = 255;
    }
    return pixels;
}

const std::uint8_t* page_px(const ws::ui::r2d::atlas::Page& page,
                            std::size_t col, std::size_t row) {
    return &page.pixels[(row * page.size + col) * 4];
}

bool is_color(const std::uint8_t* px, std::uint8_t r, std::uint8_t g,
              std::uint8_t b, std::uint8_t a = 255) {
    return px[0] == r && px[1] == g && px[2] == b && px[3] == a;
}

// Samples the canvas at a rect's center (exact: small ints over
// power-of-two page sizes).
const std::uint8_t* sample_center(const ws::ui::r2d::atlas::Page& page,
                                  const ws::ui::r2d::atlas::Rect& rect) {
    const auto to_px = [size = page.size](float t) {
        return static_cast<std::size_t>(t * static_cast<float>(size));
    };
    return page_px(page, to_px((rect.u0 + rect.u1) * 0.5f),
                   to_px((rect.v0 + rect.v1) * 0.5f));
}

}  // namespace

TEST_CASE("atlas packer places images and maps handles in input order") {
    using namespace ws::ui::r2d;
    const auto red = solid_pixels(8, 4, 255, 0, 0);
    const auto green = solid_pixels(4, 8, 0, 255, 0);
    const auto blue = solid_pixels(8, 4, 0, 0, 255);
    const atlas::SourceImage images[3] = {
        {red.data(), 8, 4}, {green.data(), 4, 8}, {blue.data(), 8, 4}};

    const atlas::Packing packed = atlas::pack(images, 3, 64);

    REQUIRE(packed.pages.size() == 1);
    REQUIRE(packed.assigned.size() == 3);
    CHECK(packed.pages[0].rects.size() == 3);

    // Rects carry the native size and each input's own pixels, whatever
    // order packing visited them in.
    const auto rect_of = [&](std::uint32_t input) -> const atlas::Rect& {
        const atlas::Placement& pl = packed.assigned[input];
        return packed.pages[pl.page].rects[pl.index];
    };
    const atlas::Rect& r0 = rect_of(0);
    const atlas::Rect& r1 = rect_of(1);
    const atlas::Rect& r2 = rect_of(2);
    CHECK(r0.w == 8);
    CHECK(r0.h == 4);
    CHECK(r1.w == 4);
    CHECK(r1.h == 8);
    CHECK(r2.w == 8);
    CHECK(r2.h == 4);
    CHECK(is_color(sample_center(packed.pages[0], r0), 255, 0, 0));
    CHECK(is_color(sample_center(packed.pages[0], r1), 0, 255, 0));
    CHECK(is_color(sample_center(packed.pages[0], r2), 0, 0, 255));

    // Tallest first: green (h=8) opens the shelf at the origin; the two
    // 4px-high images follow in input order (red before blue).
    CHECK(r1.u0 == doctest::Approx(1.0f / 64.0f));
    CHECK(r1.v0 == doctest::Approx(1.0f / 64.0f));
    CHECK(r0.u0 == doctest::Approx(7.0f / 64.0f));
    CHECK(r2.u0 == doctest::Approx(17.0f / 64.0f));
}

TEST_CASE("atlas packer wraps shelves and spills pages") {
    using namespace ws::ui::r2d;
    const auto red = solid_pixels(10, 6, 255, 0, 0);
    const auto green = solid_pixels(10, 6, 0, 255, 0);
    const auto blue = solid_pixels(10, 6, 0, 0, 255);
    const atlas::SourceImage images[3] = {
        {red.data(), 10, 6}, {green.data(), 10, 6}, {blue.data(), 10, 6}};

    // Cells are 12x8 on a 16px page: one per shelf, two shelves per page.
    const atlas::Packing packed = atlas::pack(images, 3, 16);

    REQUIRE(packed.pages.size() == 2);
    CHECK(packed.pages[0].rects.size() == 2);
    CHECK(packed.pages[1].rects.size() == 1);
    CHECK(packed.pages[0].pixels.size() == 16u * 16u * 4u);

    // Input order survives the spill: red and green share page 0 on
    // separate shelves, blue starts page 1.
    CHECK(packed.assigned[0].page == 0);
    CHECK(packed.assigned[0].index == 0);
    CHECK(packed.assigned[1].page == 0);
    CHECK(packed.assigned[1].index == 1);
    CHECK(packed.assigned[2].page == 1);
    CHECK(packed.assigned[2].index == 0);

    // Shelf geometry: wrapped x, next shelf one shelf-height down.
    const atlas::Rect& r0 = packed.pages[0].rects[0];
    const atlas::Rect& r1 = packed.pages[0].rects[1];
    CHECK(r0.u0 == doctest::Approx(1.0f / 16.0f));
    CHECK(r0.v0 == doctest::Approx(1.0f / 16.0f));
    CHECK(r1.u0 == doctest::Approx(1.0f / 16.0f));
    CHECK(r1.v0 == doctest::Approx(9.0f / 16.0f));
}

TEST_CASE("atlas packer degrades failed and oversized images to white cells") {
    using namespace ws::ui::r2d;
    const auto red = solid_pixels(4, 4, 255, 0, 0);
    std::vector<std::uint8_t> dummy(4);
    const atlas::SourceImage images[4] = {
        {red.data(), 4, 4},        // fine
        {nullptr, 0, 0},           // stb load failed
        {dummy.data(), 100, 100},  // taller than the page - pixels never read
        {dummy.data(), 0, 5},      // degenerate width
    };

    const atlas::Packing packed = atlas::pack(images, 4, 64);

    REQUIRE(packed.pages.size() == 1);
    REQUIRE(packed.assigned.size() == 4);
    for (const atlas::Placement& pl : packed.assigned) {  // all still valid
        CHECK(pl.page == 0);
        CHECK(pl.index < packed.pages[0].rects.size());
    }

    // The good image keeps its pixels and size; the failures are white.
    const atlas::Rect& good = packed.pages[0].rects[packed.assigned[0].index];
    CHECK(good.w == 4);
    CHECK(good.h == 4);
    CHECK(is_color(sample_center(packed.pages[0], good), 255, 0, 0));
    for (std::uint32_t i = 1; i < 4; ++i) {
        const atlas::Rect& rect =
            packed.pages[0].rects[packed.assigned[i].index];
        CHECK(rect.w == atlas::kFallbackSize);
        CHECK(rect.h == atlas::kFallbackSize);
        CHECK(is_color(sample_center(packed.pages[0], rect), 255, 255, 255));
    }
}

TEST_CASE("atlas packer extrudes sprite edges into the gutter") {
    using namespace ws::ui::r2d;
    const auto red = solid_pixels(4, 4, 255, 0, 0);
    const atlas::SourceImage images[1] = {{red.data(), 4, 4}};

    const atlas::Packing packed = atlas::pack(images, 1, 32);

    REQUIRE(packed.pages.size() == 1);
    const atlas::Rect& rect =
        packed.pages[packed.assigned[0].page].rects[packed.assigned[0].index];

    // Content occupies texels [1, 5) on both axes; the 1px ring around it
    // duplicates the sprite's edge pixels.
    CHECK(rect.u0 == doctest::Approx(1.0f / 32.0f));
    CHECK(rect.u1 == doctest::Approx(5.0f / 32.0f));
    CHECK(rect.v0 == doctest::Approx(1.0f / 32.0f));
    CHECK(rect.v1 == doctest::Approx(5.0f / 32.0f));
    CHECK(is_color(page_px(packed.pages[0], 0, 0), 255, 0, 0));  // near corner
    CHECK(is_color(page_px(packed.pages[0], 5, 5), 255, 0, 0));  // far corner
    CHECK(is_color(page_px(packed.pages[0], 6, 6), 0, 0, 0, 0));  // untouched
}

TEST_CASE("public r2d API is drop-safe without a running app") {
    using namespace ws::ui::r2d;
    begin();               // screen mode
    begin(Camera2D{});     // world mode
    set_layer(3);
    draw_quad(0.0f, 0.0f, 10.0f, 10.0f, rgba(255, 255, 255));
    draw_line(0.0f, 0.0f, 10.0f, 10.0f, 2.0f, rgba(255, 255, 255));
    draw_sprite(0.0f, 0.0f, 10.0f, 10.0f, rgba(255, 255, 255), Sprite{});
    end();
    load_texture("nope.png");            // warns, returns white, no crash
    const auto sprites = load_atlas({"nope.png"});  // warns, empty, no crash
    CHECK(sprites.empty());
    CHECK(sprite_size(Sprite{}).width == 0.0f);

    const FrameSize size = frame_size();
    CHECK(size.width == 0.0f);
    CHECK(size.height == 0.0f);
}

TEST_CASE("Camera2D and Texture defaults") {
    const ws::ui::r2d::Camera2D camera;
    CHECK(camera.x == 0.0f);
    CHECK(camera.y == 0.0f);
    CHECK(camera.rotation == 0.0f);
    CHECK(camera.zoom == 1.0f);  // pixel-perfect by default

    const ws::ui::r2d::Texture texture;
    CHECK(texture.id == 0);  // the white texture
}

// --- 2D renderer: recorded commands, sort, replay -----------------------------

TEST_CASE("frame_end sorts explicit layers over submission order") {
    using namespace ws::ui::r2d;
    Renderer2D r;
    r.frame_begin(100.0f, 100.0f);
    r.begin(Camera2D{});
    r.set_layer(1);
    r.draw_quad(10.0f, 0.0f, 1.0f, 1.0f, rgba(255, 255, 255));  // layer 1, first
    r.set_layer(0);
    r.draw_quad(0.0f, 0.0f, 1.0f, 1.0f, rgba(255, 255, 255));  // layer 0, second
    r.frame_end();

    // Same camera and texture: one chunk, but the layer-0 quad draws first.
    REQUIRE(r.batch().chunk_count() == 1);
    REQUIRE(r.batch().vertices().size() == 8);
    CHECK(r.batch().vertices()[0].pos[0] == 0.0f);   // layer-0 quad's TL
    CHECK(r.batch().vertices()[4].pos[0] == 10.0f);  // layer-1 quad's TL
}

TEST_CASE("same-material draws keep submission order (stable sort)") {
    using namespace ws::ui::r2d;
    Renderer2D r;
    r.frame_begin(100.0f, 100.0f);
    r.begin(Camera2D{});
    r.draw_quad(0.0f, 0.0f, 1.0f, 1.0f, rgba(255, 255, 255));
    r.draw_quad(10.0f, 0.0f, 1.0f, 1.0f, rgba(255, 255, 255));
    r.frame_end();
    REQUIRE(r.batch().vertices().size() == 8);
    CHECK(r.batch().vertices()[0].pos[0] == 0.0f);
    CHECK(r.batch().vertices()[4].pos[0] == 10.0f);
}

TEST_CASE("the material sort groups a texture's quads within a block") {
    using namespace ws::ui::r2d;
    Renderer2D r;
    r.frame_begin(100.0f, 100.0f);
    r.begin(Camera2D{});
    r.draw_quad(0.0f, 0.0f, 1.0f, 1.0f, rgba(255, 255, 255), Texture{1});
    r.draw_quad(0.0f, 20.0f, 1.0f, 1.0f, rgba(255, 255, 255), Texture{2});
    r.draw_quad(20.0f, 0.0f, 1.0f, 1.0f, rgba(255, 255, 255), Texture{1});
    r.draw_quad(20.0f, 20.0f, 1.0f, 1.0f, rgba(255, 255, 255), Texture{2});
    r.frame_end();

    // Grouped by texture (submission order kept within each group): both
    // texture-1 quads, then both texture-2 quads.
    REQUIRE(r.batch().chunk_count() == 1);
    const auto& v = r.batch().vertices();
    REQUIRE(v.size() == 16);
    CHECK(v[0].pos[0] == 0.0f);
    CHECK(v[0].tex_index == 0.0f);
    CHECK(v[4].pos[0] == 20.0f);
    CHECK(v[4].tex_index == 0.0f);
    CHECK(v[8].pos[0] == 0.0f);
    CHECK(v[8].tex_index == 1.0f);
    CHECK(v[12].pos[0] == 20.0f);
    CHECK(v[12].tex_index == 1.0f);
    CHECK(r.batch().chunk(0).texture_count == 2);
}

TEST_CASE("begin() blocks stay back to front; set_layer overrides that") {
    using namespace ws::ui::r2d;
    Renderer2D r;
    r.frame_begin(100.0f, 100.0f);
    r.begin(Camera2D{0.0f, 0.0f, 0.0f, 1.0f});
    r.draw_quad(0.0f, 0.0f, 1.0f, 1.0f, rgba(255, 255, 255));
    r.begin(Camera2D{0.0f, 0.0f, 0.0f, 2.0f});  // different zoom: different VP
    r.draw_quad(10.0f, 0.0f, 1.0f, 1.0f, rgba(255, 255, 255));
    r.frame_end();

    REQUIRE(r.batch().chunk_count() == 2);
    CHECK(r.batch().chunk(0).vertex_offset == 0);
    CHECK(r.batch().chunk(1).vertex_offset == 4);
    CHECK(r.batch().chunk(0).vp.m[0] != r.batch().chunk(1).vp.m[0]);  // zoom
    CHECK(r.batch().vertices()[0].pos[0] == 0.0f);   // block 1 first...
    CHECK(r.batch().vertices()[4].pos[0] == 10.0f);  // ...then block 2

    // A high layer in block 1 draws after block 2 regardless.
    r.frame_begin(100.0f, 100.0f);
    r.begin(Camera2D{0.0f, 0.0f, 0.0f, 1.0f});
    r.set_layer(5);
    r.draw_quad(0.0f, 0.0f, 1.0f, 1.0f, rgba(255, 255, 255));
    r.begin(Camera2D{0.0f, 0.0f, 0.0f, 2.0f});
    r.set_layer(0);
    r.draw_quad(10.0f, 0.0f, 1.0f, 1.0f, rgba(255, 255, 255));
    r.frame_end();
    REQUIRE(r.batch().chunk_count() == 2);
    CHECK(r.batch().vertices()[0].pos[0] == 10.0f);  // layer 0 (block 2) first
    CHECK(r.batch().vertices()[4].pos[0] == 0.0f);  // layer 5 (block 1) last
}

TEST_CASE("draws outside a begin()/end() block are dropped") {
    using namespace ws::ui::r2d;
    Renderer2D r;
    r.frame_begin(100.0f, 100.0f);
    r.draw_quad(0.0f, 0.0f, 1.0f, 1.0f, rgba(255, 255, 255));  // before begin()
    r.begin(Camera2D{});
    r.draw_quad(0.0f, 0.0f, 1.0f, 1.0f, rgba(255, 255, 255));
    r.end();
    r.draw_quad(50.0f, 0.0f, 1.0f, 1.0f, rgba(255, 255, 255));  // after end()
    r.frame_end();

    CHECK(r.batch().chunk_count() == 1);
    REQUIRE(r.batch().vertices().size() == 4);  // only the in-block draw
    CHECK(r.batch().vertices()[0].pos[0] == 0.0f);
}

TEST_CASE("frame_drop discards recorded commands, never defers them") {
    using namespace ws::ui::r2d;
    Renderer2D r;
    r.frame_begin(100.0f, 100.0f);
    r.begin(Camera2D{});
    r.draw_quad(0.0f, 0.0f, 1.0f, 1.0f, rgba(255, 255, 255));
    r.frame_drop();
    CHECK(r.batch().empty());

    r.frame_begin(100.0f, 100.0f);  // a fresh frame starts clean
    r.frame_end();
    CHECK(r.batch().empty());
    CHECK(r.batch().chunk_count() == 0);
}

TEST_CASE("lines record through the same sorted path") {
    using namespace ws::ui::r2d;
    Renderer2D r;
    r.frame_begin(100.0f, 100.0f);
    r.begin(Camera2D{});
    r.set_layer(1);
    r.draw_line(0.0f, 0.0f, 10.0f, 0.0f, 2.0f, rgba(255, 255, 255));
    r.set_layer(0);
    r.draw_quad(0.0f, 0.0f, 1.0f, 1.0f, rgba(255, 255, 255));
    r.frame_end();

    // The layer-0 quad wins despite being submitted second; its first
    // vertex sits at y = 0, the line's corners at y = +/- 1.
    REQUIRE(r.batch().vertices().size() == 8);
    CHECK(r.batch().vertices()[0].pos[1] == 0.0f);
    CHECK(r.batch().vertices()[4].pos[1] == 1.0f);  // line corner A
}

TEST_CASE("texture ids pass through unchanged without a GPU registry") {
    using namespace ws::ui::r2d;
    Renderer2D r;
    r.frame_begin(100.0f, 100.0f);
    r.begin_screen();
    r.draw_quad(0.0f, 0.0f, 10.0f, 10.0f, 0xFF0000FF);             // id 0
    r.draw_quad(20.0f, 0.0f, 10.0f, 10.0f, 0xFF0000FF, Texture{7});
    r.frame_end();

    // The unknown-id clamp guards the flush-time descriptor write, which
    // needs a GPU: without a registry there is nothing to validate against,
    // so synthetic ids flow through untouched (the CPU-side tests and any
    // non-GPU consumer rely on exactly that).
    REQUIRE(r.batch().chunk_count() == 1);
    const auto& chunk = r.batch().chunk(0);
    CHECK(chunk.texture_count == 2);
    CHECK(chunk.texture_ids[0] == 0u);  // id 0 (white) slots like any id
    CHECK(chunk.texture_ids[1] == 7u);
    CHECK(r.batch().vertices()[4].tex_index == 1.0f);  // id-7 quad: slot 1
}

TEST_CASE("load_texture without a GPU context returns the white texture") {
    using namespace ws::ui::r2d;
    Renderer2D r;
    CHECK(r.load_texture("no-such-file.png").id == 0);
}

// --- atlas sprites -------------------------------------------------------------

TEST_CASE("draw_sprite records the page texture and sub-rect UVs") {
    using namespace ws::ui::r2d;
    Renderer2D r;
    std::vector<atlas::Rect> rects(1);
    rects[0].u0 = 0.25f;
    rects[0].v0 = 0.5f;
    rects[0].u1 = 0.75f;
    rects[0].v1 = 1.0f;
    rects[0].w = 8;
    rects[0].h = 4;
    r.register_atlas_page(Texture{7}, std::move(rects));

    r.frame_begin(100.0f, 100.0f);
    r.begin_screen();
    r.draw_sprite(10.0f, 20.0f, 30.0f, 40.0f, rgba(255, 255, 255),
                  Sprite{7, 0});
    r.frame_end();

    REQUIRE(r.batch().vertices().size() == 4);
    const auto& v = r.batch().vertices();
    CHECK(v[0].uv[0] == 0.25f);  // TL
    CHECK(v[0].uv[1] == 0.5f);
    CHECK(v[1].uv[0] == 0.75f);  // TR
    CHECK(v[1].uv[1] == 0.5f);
    CHECK(v[2].uv[0] == 0.75f);  // BR
    CHECK(v[2].uv[1] == 1.0f);
    CHECK(v[3].uv[0] == 0.25f);  // BL
    CHECK(v[3].uv[1] == 1.0f);
    CHECK(r.batch().chunk(0).texture_count == 1);
    CHECK(r.batch().chunk(0).texture_ids[0] == 7u);
}

TEST_CASE("a page's sprites share one material key, whatever the draw order") {
    using namespace ws::ui::r2d;
    Renderer2D r;
    std::vector<atlas::Rect> rects(2);
    rects[0].u0 = 0.0f;
    rects[0].v0 = 0.0f;
    rects[0].u1 = 0.5f;
    rects[0].v1 = 0.5f;
    rects[1].u0 = 0.5f;
    rects[1].v0 = 0.5f;
    rects[1].u1 = 1.0f;
    rects[1].v1 = 1.0f;
    r.register_atlas_page(Texture{7}, std::move(rects));

    r.frame_begin(100.0f, 100.0f);
    r.begin_screen();
    r.draw_sprite(0.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFFFF, Sprite{7, 0});
    r.draw_quad(20.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFFFF, Texture{9});
    r.draw_sprite(0.0f, 20.0f, 1.0f, 1.0f, 0xFFFFFFFF, Sprite{7, 1});
    r.frame_end();

    // The material sort pulls both sprites (one page texture) ahead of the
    // raw-texture quad, all inside one chunk - the whole point of atlases.
    REQUIRE(r.batch().chunk_count() == 1);
    const auto& v = r.batch().vertices();
    REQUIRE(v.size() == 12);
    CHECK(v[0].pos[0] == 0.0f);   // sprite 0
    CHECK(v[4].pos[1] == 20.0f);  // sprite 1, reordered before the quad
    CHECK(v[8].pos[0] == 20.0f);  // the raw-texture quad last
    CHECK(v[0].tex_index == 0.0f);
    CHECK(v[4].tex_index == 0.0f);
    CHECK(v[8].tex_index == 1.0f);
    CHECK(r.batch().chunk(0).texture_count == 2);
    CHECK(r.batch().chunk(0).texture_ids[0] == 7u);
    CHECK(r.batch().chunk(0).texture_ids[1] == 9u);
    // Sub-rect UVs stayed per-sprite through the reorder.
    CHECK(v[0].uv[0] == 0.0f);
    CHECK(v[4].uv[0] == 0.5f);
}

TEST_CASE("unknown or sentinel sprites draw white, geometry intact") {
    using namespace ws::ui::r2d;
    Renderer2D r;
    std::vector<atlas::Rect> rects(1);
    r.register_atlas_page(Texture{7}, std::move(rects));

    r.frame_begin(100.0f, 100.0f);
    r.begin_screen();
    r.draw_sprite(0.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFFFF, Sprite{});  // sentinel
    r.draw_sprite(10.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFFFF, Sprite{7, 9});
    r.draw_sprite(20.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFFFF, Sprite{42, 0});
    r.frame_end();

    // All three degrade to the white texture with full-quad UVs; the
    // quads themselves still draw where they were placed.
    const auto& v = r.batch().vertices();
    REQUIRE(v.size() == 12);
    CHECK(r.batch().chunk(0).texture_count == 1);
    CHECK(r.batch().chunk(0).texture_ids[0] == 0u);
    CHECK(v[4].pos[0] == 10.0f);
    CHECK(v[8].pos[0] == 20.0f);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(v[i * 4].uv[0] == 0.0f);      // TL
        CHECK(v[i * 4].uv[1] == 0.0f);
        CHECK(v[i * 4 + 2].uv[0] == 1.0f);  // BR
        CHECK(v[i * 4 + 2].uv[1] == 1.0f);
    }
}

TEST_CASE("load_atlas without a GPU context returns white sprites") {
    using namespace ws::ui::r2d;
    Renderer2D r;
    const std::vector<Sprite> sprites = r.load_atlas({"a.png", "b.png"});
    REQUIRE(sprites.size() == 2);
    CHECK(sprites[0].page == 0u);
    CHECK(sprites[0].index == 0u);
    CHECK(sprites[1].page == 0u);
    CHECK(sprites[1].index == 0u);
}

TEST_CASE("sprite_size reports native pixels and {0,0} for unknown sprites") {
    using namespace ws::ui::r2d;
    Renderer2D r;
    std::vector<atlas::Rect> rects(1);
    rects[0].w = 8;
    rects[0].h = 4;
    r.register_atlas_page(Texture{7}, std::move(rects));

    const FrameSize native = r.sprite_size(Sprite{7, 0});
    CHECK(native.width == 8.0f);
    CHECK(native.height == 4.0f);

    const FrameSize unknown = r.sprite_size(Sprite{9, 0});
    CHECK(unknown.width == 0.0f);
    CHECK(unknown.height == 0.0f);
    const FrameSize sentinel = r.sprite_size(Sprite{});
    CHECK(sentinel.width == 0.0f);
    CHECK(sentinel.height == 0.0f);
}

// --- gui module (Phases 0-4: the core is real) ---------------------------------------

TEST_CASE("gui module: the public surface constructs, links and behaves safely") {
    using namespace ws::ui::gui;

    // Fresh objects honor the declared defaults.
    CHECK(IdStack{}.depth() == 0);
    CHECK(LayoutCursor{}.depth() == 0);
    CHECK(GuiContext{}.hot_id() == kInvalidWidgetId);
    CHECK(GuiContext{}.active_id() == kInvalidWidgetId);

    // A filled snapshot flows through the frame lifecycle: new_frame
    // adopts it, end_frame clears any press a vanished widget left held.
    InputFrame input;
    input.mouse_x = 100.0f;
    input.mouse_down[0] = true;
    input.key_pressed['D'] = true;
    input.scroll_dy = -2.0f;
    input.text_input.push_back(U'w');
    GuiContext gui;
    gui.new_frame(input);
    gui.end_frame();
    CHECK(gui.input().mouse_x == 100.0f);
    CHECK(gui.input().mouse_down[0] == true);
    CHECK(gui.input().key_pressed['D'] == true);
    CHECK(gui.input().text_input.size() == 1);

    // Ids hash for real (Phase 2), hit tests run for real (Phase 3),
    // layout flows for real (Phase 4); the invalid id claims nothing.
    // Underflow pops/closes are the documented no-ops - none of this may
    // crash.
    gui.ids().push("panel");
    gui.ids().push(7);
    CHECK(gui.ids().current("button") != kInvalidWidgetId);
    gui.ids().pop();
    gui.ids().pop();
    gui.ids().pop();

    LayoutCursor layout;
    const Rect content = layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 8.0f);
    CHECK(content.width == 384.0f);
    CHECK(layout.next(100.0f, 24.0f).height == 24.0f);
    layout.same_line();
    layout.end_panel();
    layout.end_panel();  // underflow close: documented no-op

    const HitTest hit = gui.hit_test(kInvalidWidgetId, {0.0f, 0.0f, 10.0f, 10.0f});
    CHECK(hit.hovered == false);
    CHECK(hit.pressed == false);
    CHECK(hit.clicked == false);
    CHECK(hit.released == false);
}

TEST_CASE("InputFrame::next_frame clears edges and keeps held state") {
    using namespace ws::ui::gui;
    InputFrame frame;
    frame.mouse_x = 100.0f;
    frame.mouse_y = 50.0f;
    frame.mouse_down[0] = true;
    frame.mouse_pressed[0] = true;
    frame.mouse_released[1] = true;
    frame.scroll_dx = 1.5f;
    frame.scroll_dy = -2.5f;
    frame.key_down['D'] = true;
    frame.key_pressed['D'] = true;
    frame.text_input.push_back(U'w');

    frame.next_frame();

    // Held state and position survive the frame boundary...
    CHECK(frame.mouse_x == 100.0f);
    CHECK(frame.mouse_y == 50.0f);
    CHECK(frame.mouse_down[0] == true);
    CHECK(frame.key_down['D'] == true);
    // ...the per-frame edges do not.
    CHECK(frame.mouse_pressed[0] == false);
    CHECK(frame.mouse_released[1] == false);
    CHECK(frame.scroll_dx == 0.0f);
    CHECK(frame.scroll_dy == 0.0f);
    CHECK(frame.key_pressed['D'] == false);
    CHECK(frame.text_input.empty());
}

TEST_CASE("frame_input is drop-safe without a running app") {
    // Headless: the shell never installed an accumulator, so the public
    // getter answers an empty snapshot instead of dangling.
    const ws::ui::gui::InputFrame& frame = ws::ui::gui::frame_input();
    CHECK(frame.mouse_x == 0.0f);
    CHECK(frame.mouse_y == 0.0f);
    CHECK(frame.mouse_down[0] == false);
    CHECK(frame.mouse_pressed[0] == false);
    CHECK(frame.key_pressed['D'] == false);
    CHECK(frame.scroll_dy == 0.0f);
    CHECK(frame.text_input.empty());
}

// --- gui ids (Phase 2) --------------------------------------------------------------

TEST_CASE("IdStack: same label under different scopes ids differently") {
    using namespace ws::ui::gui;
    IdStack ids;
    ids.push("file-dialog");
    const WidgetId file_ok = ids.current("ok");
    ids.pop();
    ids.push("error-dialog");
    const WidgetId error_ok = ids.current("ok");

    CHECK(file_ok != error_ok);
    CHECK(file_ok != kInvalidWidgetId);
    CHECK(error_ok != kInvalidWidgetId);

    // pop() restored the first scope: the same derivation reads the same.
    ids.pop();
    ids.push("file-dialog");
    CHECK(ids.current("ok") == file_ok);
}

TEST_CASE("IdStack: ids are stable across frames and fresh stacks") {
    using namespace ws::ui::gui;
    // Immediate mode re-describes the UI every frame: two independent
    // walks of the same code path must derive identical ids - that is
    // what lets per-widget state key on them.
    IdStack frame_one;
    frame_one.push("panel");
    const WidgetId one = frame_one.current("slider");

    IdStack frame_two;
    frame_two.push("panel");
    CHECK(frame_two.current("slider") == one);

    // ...through int scopes too (indexed list rows).
    frame_one.push(3);
    frame_two.push(3);
    const WidgetId cell_one = frame_one.current("cell");
    CHECK(cell_one == frame_two.current("cell"));
    CHECK(cell_one != one);

    frame_one.pop();
    frame_two.pop();
    CHECK(frame_one.current("slider") == one);
}

TEST_CASE("IdStack: int scopes separate siblings without stringifying") {
    using namespace ws::ui::gui;
    IdStack ids;
    ids.push("list");
    ids.push(0);
    const WidgetId row_zero = ids.current("row");
    ids.pop();
    ids.push(1);
    const WidgetId row_one = ids.current("row");
    ids.pop();

    CHECK(row_zero != row_one);

    // The VALUE 65 is not the LABEL "A" and not the LABEL "65": int
    // scopes hash the integer's bytes, never a stringified form.
    ids.push(65);
    const WidgetId via_value = ids.current("row");
    ids.pop();
    ids.push("A");
    CHECK(ids.current("row") != via_value);
    ids.pop();
    ids.push("65");
    CHECK(ids.current("row") != via_value);
}

TEST_CASE("IdStack: scope paths are delimited, not plain concatenations") {
    using namespace ws::ui::gui;
    IdStack ab;
    ab.push("ab");
    const WidgetId ab_then_c = ab.current("c");

    IdStack a;
    a.push("a");
    const WidgetId a_then_bc = a.current("bc");

    // Without a delimiter "ab"/"c" and "a"/"bc" would hash the same
    // byte stream; the delimiter makes them different paths.
    CHECK(ab_then_c != a_then_bc);
}

TEST_CASE("IdStack: pop restores scope and overflow clamps at max depth") {
    using namespace ws::ui::gui;
    IdStack ids;
    ids.pop();  // empty pop: documented no-op
    CHECK(ids.depth() == 0);

    ids.push("outer");
    CHECK(ids.depth() == 1);
    const WidgetId outer_widget = ids.current("widget");

    ids.push("inner");
    CHECK(ids.depth() == 2);
    CHECK(ids.current("widget") != outer_widget);

    ids.pop();
    CHECK(ids.depth() == 1);
    CHECK(ids.current("widget") == outer_widget);

    // Pushing past kMaxDepth clamps (with one warn): the remembered
    // scope stops deepening, but the depth KEEPS COUNTING, so the
    // matching pops stay balanced - they can never eat a real scope.
    for (int i = 0; i < IdStack::kMaxDepth + 4; ++i) {
        ids.push(i);
    }
    CHECK(ids.depth() == 1 + IdStack::kMaxDepth + 4);  // the count stays honest
    CHECK(ids.current("deep") != kInvalidWidgetId);  // still a valid id
    for (int i = 0; i < IdStack::kMaxDepth + 4; ++i) {
        ids.pop();
    }

    // The overflow round trip corrupted nothing above the clamp.
    CHECK(ids.depth() == 1);
    CHECK(ids.current("widget") == outer_widget);
}

// --- gui hit-testing (Phase 3) ------------------------------------------------------

namespace {

// A pointer snapshot at (x, y) with the primary button's held state and
// optional press/release edges set.
ws::ui::gui::InputFrame snap(float x, float y, bool down = false,
                             bool pressed = false, bool released = false) {
    ws::ui::gui::InputFrame frame;
    frame.mouse_x = x;
    frame.mouse_y = y;
    frame.mouse_down[0] = down;
    frame.mouse_pressed[0] = pressed;
    frame.mouse_released[0] = released;
    return frame;
}

}  // namespace

TEST_CASE("hit_test: hover claims hot; a full click is press-then-release inside") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const WidgetId id = IdStack{}.current("button");
    const Rect rect{100.0f, 100.0f, 80.0f, 24.0f};

    // Frame 1: pointer inside, nothing held - hover only.
    gui.new_frame(snap(120.0f, 110.0f));
    HitTest hit = gui.hit_test(id, rect);
    CHECK(hit.hovered);
    CHECK(!hit.pressed);
    CHECK(!hit.clicked);
    CHECK(!hit.released);
    CHECK(gui.hot_id() == id);
    CHECK(gui.active_id() == kInvalidWidgetId);
    gui.end_frame();

    // Frame 2: press inside - the widget takes the press.
    gui.new_frame(snap(120.0f, 110.0f, true, true));
    hit = gui.hit_test(id, rect);
    CHECK(hit.hovered);
    CHECK(hit.pressed);
    CHECK(!hit.clicked);
    CHECK(gui.active_id() == id);

    // Frame 3: still held, pointer dragged outside - the owner keeps the
    // press, hover is gone.
    gui.new_frame(snap(10.0f, 10.0f, true));
    hit = gui.hit_test(id, rect);
    CHECK(!hit.hovered);
    CHECK(hit.pressed);
    CHECK(gui.active_id() == id);

    // Frame 4: release outside - the drag ends, no click.
    gui.new_frame(snap(10.0f, 10.0f, false, false, true));
    hit = gui.hit_test(id, rect);
    CHECK(hit.released);
    CHECK(!hit.clicked);
    CHECK(gui.active_id() == kInvalidWidgetId);
    gui.end_frame();
}

TEST_CASE("hit_test: release inside is a click; press+release in one frame still clicks") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const WidgetId id = IdStack{}.current("button");
    const Rect rect{0.0f, 0.0f, 100.0f, 100.0f};

    gui.new_frame(snap(50.0f, 50.0f, true, true));
    (void)gui.hit_test(id, rect);
    gui.end_frame();

    gui.new_frame(snap(50.0f, 50.0f, false, false, true));
    const HitTest hit = gui.hit_test(id, rect);
    CHECK(hit.hovered);
    CHECK(hit.released);
    CHECK(hit.clicked);
    CHECK(gui.active_id() == kInvalidWidgetId);
    CHECK(gui.hot_id() == id);
    gui.end_frame();

    // A click entirely between two frames: both edges arrive together.
    GuiContext fast;
    fast.new_frame(snap(50.0f, 50.0f, false, true, true));
    const HitTest quick = fast.hit_test(id, rect);
    CHECK(quick.clicked);
    CHECK(quick.released);
    fast.end_frame();
}

TEST_CASE("hit_test: a held press captures the pointer - other widgets cannot take it") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const IdStack ids;
    // Two overlapping buttons; B draws later (on top) but A owns the press.
    const WidgetId a = ids.current("a");
    const WidgetId b = ids.current("b");
    const Rect rect{0.0f, 0.0f, 100.0f, 100.0f};

    gui.new_frame(snap(50.0f, 50.0f, true, true));
    (void)gui.hit_test(a, rect);  // A runs first: takes the press
    CHECK(gui.active_id() == a);

    const HitTest under = gui.hit_test(b, rect);  // B overlaps: no press for B
    CHECK(!under.pressed);
    CHECK(!under.clicked);
    CHECK(gui.hot_id() == a);  // while A owns the press, B cannot be hot
    CHECK(gui.active_id() == a);

    // Pressing again while A owns it changes nothing for B.
    gui.new_frame(snap(50.0f, 50.0f, true, true));
    const HitTest again = gui.hit_test(b, rect);
    CHECK(!again.pressed);
    CHECK(gui.active_id() == a);
    gui.end_frame();
}

TEST_CASE("hit_test: a vanished press owner is dropped at end_frame") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const WidgetId gone = IdStack{}.current("gone");
    const WidgetId other = IdStack{}.current("other");
    const Rect rect{0.0f, 0.0f, 100.0f, 100.0f};

    // Press on `gone`...
    gui.new_frame(snap(50.0f, 50.0f, true, true));
    (void)gui.hit_test(gone, rect);
    CHECK(gui.active_id() == gone);
    gui.end_frame();

    // ...then the UI changes: `gone` no longer runs, but the release
    // arrives. end_frame must drop the ownership, or no widget could be
    // hot while the stale id owns the captured press.
    gui.new_frame(snap(50.0f, 50.0f, false, false, true));
    gui.end_frame();
    CHECK(gui.active_id() == kInvalidWidgetId);

    gui.new_frame(snap(50.0f, 50.0f));
    const HitTest hit = gui.hit_test(other, rect);
    CHECK(hit.hovered);
    CHECK(gui.hot_id() == other);
}

TEST_CASE("hit_test: while another widget owns the drag, overlapped widgets do not hover") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const IdStack ids;
    const WidgetId a = ids.current("a");
    const WidgetId b = ids.current("b");
    const Rect a_rect{0.0f, 0.0f, 100.0f, 100.0f};
    const Rect b_rect{100.0f, 0.0f, 100.0f, 100.0f};

    gui.new_frame(snap(50.0f, 50.0f, true, true));
    (void)gui.hit_test(a, a_rect);  // A takes the press
    gui.end_frame();

    // The drag moves the pointer over B: B is inside, but A owns the
    // captured pointer - B must not report hover (hot, not merely
    // inside), and nobody is hot while the pointer is off the owner.
    gui.new_frame(snap(150.0f, 50.0f, true));
    const HitTest under = gui.hit_test(b, b_rect);
    CHECK(!under.hovered);
    CHECK(gui.hot_id() == kInvalidWidgetId);  // B could not claim
    const HitTest owner = gui.hit_test(a, a_rect);
    CHECK(owner.pressed);    // the drag lives on
    CHECK(!owner.hovered);   // the pointer left A's rect
    gui.end_frame();
}

TEST_CASE("hit_test: a duplicate id in one frame is flagged and tolerated") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const WidgetId id = IdStack{}.current("dupe");

    // Two widgets claiming the same id (same label, same scope, no
    // push(index) between them) share hot/active - a caller bug the
    // context flags (warns once per process, visible in this log) and
    // tolerates: both queries answer, nothing corrupts.
    gui.new_frame(snap(50.0f, 50.0f, true, true));
    const HitTest first = gui.hit_test(id, {0.0f, 0.0f, 100.0f, 100.0f});
    const HitTest second = gui.hit_test(id, {0.0f, 0.0f, 100.0f, 100.0f});
    CHECK(first.pressed);
    CHECK(second.pressed);
    gui.end_frame();

    // Distinct ids in one frame flag nothing (no false positives), and
    // the same id may reappear in the NEXT frame - the census is per
    // frame.
    gui.new_frame(snap(50.0f, 50.0f));
    const HitTest again = gui.hit_test(id, {0.0f, 0.0f, 100.0f, 100.0f});
    CHECK(again.hovered);  // the same id reappears next frame, unflagged
    const WidgetId other = IdStack{}.current("other");
    (void)gui.hit_test(other, {0.0f, 0.0f, 100.0f, 100.0f});  // distinct: fine
    gui.end_frame();
}

TEST_CASE("persistent_state: keys values by (type, id) for the context's lifetime") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const IdStack ids;
    const WidgetId a = ids.current("a");
    const WidgetId b = ids.current("b");

    // First touch default-constructs; entries survive frames.
    gui.persistent_state<float>(a) = 1.5f;
    gui.new_frame(snap(0.0f, 0.0f));
    gui.end_frame();
    CHECK(gui.persistent_state<float>(a) == 1.5f);

    // Different id = different value; different type at the same id is a
    // different table.
    gui.persistent_state<float>(b) = 2.5f;
    gui.persistent_state<int>(a) = 7;
    CHECK(gui.persistent_state<float>(a) == 1.5f);
    CHECK(gui.persistent_state<float>(b) == 2.5f);
    CHECK(gui.persistent_state<int>(a) == 7);

    // A second context owns independent state (no globals underneath).
    GuiContext fresh;
    CHECK(fresh.persistent_state<float>(a) == 0.0f);
}

// --- gui layout (Phase 4) ------------------------------------------------------------

TEST_CASE("LayoutCursor: widgets stack downward, advancing below the line's tallest") {
    using namespace ws::ui::gui;
    LayoutCursor layout;

    // Padding insets the content rect on every side.
    const Rect content = layout.begin_panel({10.0f, 20.0f, 200.0f, 300.0f}, 8.0f);
    CHECK(content.x == 18.0f);
    CHECK(content.y == 28.0f);
    CHECK(content.width == 184.0f);
    CHECK(content.height == 284.0f);
    CHECK(layout.depth() == 1);

    // The first widget sits at the content's top-left...
    const Rect first = layout.next(100.0f, 24.0f);
    CHECK(first.x == 18.0f);
    CHECK(first.y == 28.0f);
    CHECK(first.width == 100.0f);
    CHECK(first.height == 24.0f);

    // ...the next opens a fresh line below it.
    const Rect second = layout.next(60.0f, 30.0f);
    CHECK(second.x == 18.0f);
    CHECK(second.y == 52.0f);
}

TEST_CASE("LayoutCursor: same_line places beside; the line advances below its tallest") {
    using namespace ws::ui::gui;
    LayoutCursor layout;
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);

    const Rect a = layout.next(100.0f, 24.0f);  // (0, 0)
    layout.same_line();
    const Rect b = layout.next(50.0f, 40.0f);  // (100, 0): beside a
    CHECK(b.x == 100.0f);
    CHECK(b.y == 0.0f);

    // The next line starts below the taller b, not below the shorter a.
    const Rect c = layout.next(30.0f, 20.0f);
    CHECK(c.x == 0.0f);
    CHECK(c.y == 40.0f);

    // same_line with nothing placed yet: a no-op, placement still starts
    // at the content's top-left.
    LayoutCursor fresh;
    (void)fresh.begin_panel({0.0f, 0.0f, 100.0f, 100.0f}, 0.0f);
    fresh.same_line();
    const Rect d = fresh.next(10.0f, 10.0f);
    CHECK(d.x == 0.0f);
    CHECK(d.y == 0.0f);

    // A zero-size spacer still counts as placed: same_line resumes after
    // it onto its line (an empty-rect heuristic would swallow this and
    // open a new line instead).
    LayoutCursor spacers;
    (void)spacers.begin_panel({0.0f, 0.0f, 100.0f, 100.0f}, 0.0f);
    (void)spacers.next(50.0f, 20.0f);  // line 1: a 50x20 widget at (0, 0)
    (void)spacers.next(0.0f, 0.0f);    // a 0x0 spacer still ends line 1
    spacers.same_line();
    const Rect beside = spacers.next(10.0f, 10.0f);
    CHECK(beside.x == 0.0f);   // after the 0-wide spacer
    CHECK(beside.y == 20.0f);  // on the spacer's line, not a fresh one
}

TEST_CASE("LayoutCursor: panels nest and the parent's cursor resumes untouched") {
    using namespace ws::ui::gui;
    LayoutCursor layout;
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    (void)layout.next(100.0f, 20.0f);  // the outer cursor now sits at y = 20

    // A child panel lays out independently of the parent's cursor.
    const Rect inner_content = layout.begin_panel({100.0f, 50.0f, 100.0f, 60.0f}, 10.0f);
    CHECK(layout.depth() == 2);
    CHECK(inner_content.x == 110.0f);
    CHECK(inner_content.width == 80.0f);
    const Rect inner = layout.next(40.0f, 10.0f);
    CHECK(inner.x == 110.0f);
    CHECK(inner.y == 60.0f);
    layout.end_panel();
    CHECK(layout.depth() == 1);

    // The parent continues exactly where it was parked.
    const Rect after = layout.next(10.0f, 10.0f);
    CHECK(after.x == 0.0f);
    CHECK(after.y == 20.0f);

    layout.end_panel();
    CHECK(layout.depth() == 0);
    layout.end_panel();  // underflow close: documented no-op
    CHECK(layout.depth() == 0);
}

TEST_CASE("LayoutCursor: without an open panel next() is drop-safe; overflow clamps") {
    using namespace ws::ui::gui;

    // No panel open: an empty rect, not a crash or a phantom placement.
    LayoutCursor layout;
    const Rect nowhere = layout.next(50.0f, 50.0f);
    CHECK(nowhere.x == 0.0f);
    CHECK(nowhere.y == 0.0f);
    CHECK(nowhere.width == 0.0f);
    CHECK(nowhere.height == 0.0f);
    layout.same_line();  // no-op either; still must not crash

    // Overflow: past kMaxPanels the push clamps (logged once - visible
    // in this run's log) but still counts, so the matching end_panel()
    // calls stay balanced.
    for (int i = 0; i < LayoutCursor::kMaxPanels + 2; ++i) {
        (void)layout.begin_panel({0.0f, 0.0f, 100.0f, 100.0f}, 0.0f);
    }
    CHECK(layout.depth() == LayoutCursor::kMaxPanels + 2);  // honest count
    for (int i = 0; i < LayoutCursor::kMaxPanels + 2; ++i) {
        layout.end_panel();
    }
    CHECK(layout.depth() == 0);
}

TEST_CASE("LayoutCursor: overflow round trips never eat a real panel") {
    using namespace ws::ui::gui;
    LayoutCursor layout;
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    (void)layout.next(100.0f, 20.0f);  // the real cursor parks at y = 20

    // Deep nesting past the clamp: the overflowed panels are degenerate,
    // but every end_panel() pairs with its own begin_panel() - the real
    // panel must survive the round trip with its cursor untouched.
    for (int i = 0; i < LayoutCursor::kMaxPanels + 2; ++i) {
        (void)layout.begin_panel({0.0f, 0.0f, 100.0f, 100.0f}, 0.0f);
    }
    for (int i = 0; i < LayoutCursor::kMaxPanels + 2; ++i) {
        layout.end_panel();
    }
    CHECK(layout.depth() == 1);

    const Rect after = layout.next(10.0f, 10.0f);
    CHECK(after.x == 0.0f);
    CHECK(after.y == 20.0f);  // the parked cursor, uncorrupted
}

// --- gui widgets (Phase 5) ------------------------------------------------------------

// One frame's widget pass, the way an app runs it: the layout re-opens
// its panel each frame, so the sole button always sits at the content
// origin (0, 0, default width x 24). Headless, the draws drop - only the
// hit-test result and bookkeeping are observable.
TEST_CASE("button: a full click over the laid-out rect fires exactly once") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;
    const WidgetId id = IdStack{}.current("fire");

    // Frame 1: press inside the button's rect - pressed, not yet clicked.
    gui.new_frame(snap(60.0f, 12.0f, true, true));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(!button(gui, layout, kDefaultChrome, "fire"));
    layout.end_panel();
    CHECK(gui.active_id() == id);
    gui.end_frame();

    // Frame 2: the same pass again - the release lands on the same rect
    // and completes the click.
    gui.new_frame(snap(60.0f, 12.0f, false, false, true));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(button(gui, layout, kDefaultChrome, "fire"));
    layout.end_panel();
    CHECK(gui.active_id() == kInvalidWidgetId);
    gui.end_frame();

    // Frame 3: no edges - the click is over and does not repeat.
    gui.new_frame(snap(60.0f, 12.0f));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(!button(gui, layout, kDefaultChrome, "fire"));
    layout.end_panel();
    gui.end_frame();
}

TEST_CASE("button: stacked buttons hit-test their own laid-out rects") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;

    // A click that starts and ends between two frames, aimed at the
    // SECOND button (its rect is (0, 24, 120, 24) - the first stacked
    // above it). "first" must not fire.
    gui.new_frame(snap(60.0f, 36.0f, false, true, true));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(!button(gui, layout, kDefaultChrome, "first"));
    CHECK(button(gui, layout, kDefaultChrome, "second"));
    layout.end_panel();
    gui.end_frame();
}

TEST_CASE("button: hover alone claims hot but never clicks") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;

    gui.new_frame(snap(60.0f, 12.0f));  // pointer over "first", no press
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(!button(gui, layout, kDefaultChrome, "first"));
    CHECK(!button(gui, layout, kDefaultChrome, "second"));
    layout.end_panel();
    // The hover claimed hot for "first" (the second's rect misses the
    // pointer, so it claimed nothing); nothing became active.
    CHECK(gui.hot_id() == IdStack{}.current("first"));
    CHECK(gui.active_id() == kInvalidWidgetId);
    gui.end_frame();
}

TEST_CASE("button: the theme drives the widget's metrics") {
    using namespace ws::ui::gui;
    CHECK(kDefaultChrome.control_height == 24.0f);  // the layout-affecting default

    // A tall variant: with 40 px controls the second button's rect sits
    // at y = 40 - a click there must land on it, not on the first.
    ChromeTheme tall = kDefaultChrome;
    tall.control_height = 40.0f;

    GuiContext gui;
    LayoutCursor layout;
    gui.new_frame(snap(60.0f, 60.0f, false, true, true));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(!button(gui, layout, tall, "first"));
    CHECK(button(gui, layout, tall, "second"));
    layout.end_panel();
    gui.end_frame();
}
