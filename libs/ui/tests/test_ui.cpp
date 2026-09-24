#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstddef>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "batch_core.h"   // internal: pure-CPU batch builder (src/ on the test include path)
#include "renderer2d.h"   // internal: command recording + sort/replay (same path)
#include <ws/ui/app.hpp>
#include <ws/ui/event.hpp>
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

TEST_CASE("public r2d API is drop-safe without a running app") {
    using namespace ws::ui::r2d;
    begin();               // screen mode
    begin(Camera2D{});     // world mode
    set_layer(3);
    draw_quad(0.0f, 0.0f, 10.0f, 10.0f, rgba(255, 255, 255));
    draw_line(0.0f, 0.0f, 10.0f, 10.0f, 2.0f, rgba(255, 255, 255));
    end();
    load_texture("nope.png");  // warns, returns white, no crash

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
