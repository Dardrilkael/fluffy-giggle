#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "atlas_packer.h"  // internal: pure-CPU atlas packer (src/ on the test include path)
#include "batch_core.h"     // internal: pure-CPU batch builder (same path)
#include "font.h"           // internal: pure-CPU font bake + utf8 decoder (same path)
#include "renderer2d.h"     // internal: command recording + sort/replay (same path)
#include "rounded_texture.h"  // internal: the builtin rounded-rect mask bake (same path)
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
    CHECK(ws::ui::TouchPressedEvent{7, 1.0f, 2.0f}.type() ==
          EventType::TouchPressed);
    CHECK(ws::ui::TouchMovedEvent{7, 1.0f, 2.0f}.type() == EventType::TouchMoved);
    CHECK(ws::ui::TouchReleasedEvent{7, 1.0f, 2.0f, true}.type() ==
          EventType::TouchReleased);
    CHECK(ws::ui::TextInputEvent{U'x'}.type() == EventType::TextInput);

    ws::ui::WindowResizeEvent resize{800, 600};
    CHECK(resize.handled == false);
    CHECK(resize.width == 800);
    CHECK(resize.height == 600);
    ws::ui::KeyPressedEvent key{'D', true};
    CHECK(key.keycode == 'D');
    CHECK(key.repeat == true);
    ws::ui::TouchReleasedEvent touch{7, 1.0f, 2.0f, true};
    CHECK(touch.pointer_id == 7);
    CHECK(touch.x == 1.0f);
    CHECK(touch.y == 2.0f);
    CHECK(touch.cancelled == true);
    ws::ui::TextInputEvent glyph{U'w'};
    CHECK(glyph.codepoint == U'w');
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
    const float pos[4][3] = {{0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f},
                             {1.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
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
    core.begin_frame();
    REQUIRE(core.open_segment(default_vp()));

    const float pos[4][3] = {{1.0f, 2.0f, 0.0f}, {11.0f, 2.0f, 0.0f},
                             {11.0f, 12.0f, 0.0f}, {1.0f, 12.0f, 0.0f}};
    const float uv[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    const float color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    REQUIRE(core.push_quad(pos, uv, color, 7u));
    REQUIRE(core.vertices().size() == 4);

    for (int i = 0; i < 4; ++i) {  // TL, TR, BR, BL
        const auto& v = core.vertices()[static_cast<std::size_t>(i)];
        CHECK(v.pos[0] == pos[i][0]);
        CHECK(v.pos[1] == pos[i][1]);
        CHECK(v.pos[2] == pos[i][2]);  // z rides along - the z=0 plane for 2D
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
    core.begin_frame();
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
    core.begin_frame();
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
    core.begin_frame();
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
    core.begin_frame();
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

TEST_CASE("chunk cap drops further segments and their draws, never silently") {
    BatchCore core;
    core.begin_frame();
    for (std::uint32_t i = 0; i < ws::ui::r2d::batch::kMaxChunksPerFrame; ++i) {
        REQUIRE(core.open_segment(default_vp()));
    }
    CHECK(core.open_segment(default_vp()) == false);  // one past: no chunk
    CHECK(core.chunk_count() == ws::ui::r2d::batch::kMaxChunksPerFrame);
    CHECK(push_test_quad(core, 0u) == false);  // segment stayed closed

    core.close_segment();
    CHECK(push_test_quad(core, 0u) == false);  // still closed after end()

    // The array cannot grow (descriptor sets are pre-allocated per chunk
    // slot at init), so over-cap is COUNTED, not silent (M5): the failed
    // open is one dropped segment, the two unopenable-segment quads are
    // two dropped quads. The renderer turns these into its once-per-frame
    // warn; the stats surface carries the numbers.
    CHECK(core.dropped_chunks() == 1);
    CHECK(core.dropped_quads() == 2);

    // begin_frame resets the accounting along with everything else.
    core.begin_frame();
    CHECK(core.dropped_chunks() == 0);
    CHECK(core.dropped_quads() == 0);
}

TEST_CASE("quad_corners_pivot rotates about the pivot; own-center is the default") {
    using namespace ws::ui::r2d::batch;
    const float half_pi = 1.5707963f;

    // Rotation about a pivot OUTSIDE the rect: the whole rect orbits. At
    // +90 degrees (clockwise, y-down) the map is (x, y) -> (-y, x): the
    // rect centered at (12, 12) pivoting the origin swings to (-12, -12),
    // so its corners land in the third quadrant around it.
    float out[4][3];
    quad_corners_pivot(10.0f, 10.0f, 4.0f, 4.0f, 0.0f, 0.0f, half_pi, out);
    CHECK(out[0][0] == doctest::Approx(-10.0f));  // TL
    CHECK(out[0][1] == doctest::Approx(10.0f));
    CHECK(out[1][0] == doctest::Approx(-10.0f));  // TR
    CHECK(out[1][1] == doctest::Approx(14.0f));
    CHECK(out[2][0] == doctest::Approx(-14.0f));  // BR
    CHECK(out[2][1] == doctest::Approx(14.0f));
    CHECK(out[3][0] == doctest::Approx(-14.0f));  // BL
    CHECK(out[3][1] == doctest::Approx(10.0f));
    CHECK(out[0][2] == 0.0f);  // the z=0 plane every 2D draw lands on

    // Zero rotation lays the corners out plainly, pivot or not.
    quad_corners_pivot(10.0f, 10.0f, 4.0f, 4.0f, -50.0f, 7.0f, 0.0f, out);
    CHECK(out[0][0] == doctest::Approx(10.0f));
    CHECK(out[0][1] == doctest::Approx(10.0f));
    CHECK(out[2][0] == doctest::Approx(14.0f));
    CHECK(out[2][1] == doctest::Approx(14.0f));

    // quad_corners IS quad_corners_pivot at the rect's own center - the
    // equivalence draw_rounded_quad's nine slices each break by pivoting
    // the whole rect's center instead.
    float own[4][3];
    quad_corners(10.0f, 20.0f, 30.0f, 40.0f, 0.7f, own);
    quad_corners_pivot(10.0f, 20.0f, 30.0f, 40.0f, 25.0f, 40.0f, 0.7f, out);
    for (int i = 0; i < 4; ++i) {
        CHECK(out[i][0] == doctest::Approx(own[i][0]));
        CHECK(out[i][1] == doctest::Approx(own[i][1]));
    }
}

TEST_CASE("open_segment carries the clip; texture splits inherit it") {
    BatchCore core;
    core.begin_frame();
    const float clip[4] = {5.0f, 5.0f, 50.0f, 50.0f};
    REQUIRE(core.open_segment(default_vp(), clip));
    REQUIRE(push_test_quad(core, 0u));
    for (int i = 0; i < 4; ++i) {
        CHECK(core.chunk(0).clip[i] == clip[i]);
    }

    // The default argument is the full-frame sentinel: an unclipped segment.
    REQUIRE(core.open_segment(default_vp()));
    REQUIRE(push_test_quad(core, 0u));
    for (int i = 0; i < 4; ++i) {
        CHECK(core.chunk(1).clip[i] == ws::ui::r2d::batch::kFullClip[i]);
    }

    // A texture-overflow split continues the SAME segment: the fresh chunk
    // inherits both the VP and the clip.
    core.begin_frame();
    REQUIRE(core.open_segment(default_vp(), clip));
    for (std::uint32_t id = 0;
         id <= ws::ui::r2d::batch::kMaxTexturesPerChunk; ++id) {
        // 33 unique textures: one past the 32-slot table forces the split.
        REQUIRE(push_test_quad(core, id));
    }
    REQUIRE(core.chunk_count() == 2);
    CHECK(core.chunk(1).vp.m[0] == core.chunk(0).vp.m[0]);
    for (int i = 0; i < 4; ++i) {
        CHECK(core.chunk(1).clip[i] == clip[i]);
    }
}

TEST_CASE("line expansion builds a thickness quad") {
    float pos[4][3];
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
    CHECK(pos[0][2] == 0.0f);  // the z=0 plane every 2D draw lands on

    CHECK(uv[0][0] == 0.0f);  // u runs along the line, v across it
    CHECK(uv[1][1] == 1.0f);
    CHECK(uv[2][0] == 1.0f);
    CHECK(uv[3][1] == 0.0f);
}

TEST_CASE("degenerate lines are dropped") {
    float pos[4][3];
    float uv[4][2];
    CHECK(ws::ui::r2d::batch::line_quad(1.0f, 1.0f, 1.0f, 1.0f, 2.0f, pos,
                                        uv) == false);  // zero length
    CHECK(ws::ui::r2d::batch::line_quad(0.0f, 0.0f, 10.0f, 0.0f, 0.0f, pos,
                                        uv) == false);  // no thickness
}

// --- 3D camera math (basis, perspective VP, billboards, world lines) -----------

TEST_CASE("camera_vp3d canonical camera maps axes to the Vulkan frustum") {
    using ws::ui::r2d::Camera3D;
    using ws::ui::r2d::batch::camera_vp3d;
    // Eye on -z looking at the origin, default up, fov 90 (half-tan 1), a
    // square frame, near 1 / far 99: the target sits at view depth 4.
    Camera3D camera;
    camera.eye[2] = -4.0f;
    camera.fov_y = 1.5707963f;  // 90 degrees
    camera.near_z = 1.0f;
    camera.far_z = 99.0f;
    const auto vp = camera_vp3d(camera, 100.0f, 100.0f);
    const auto project = [&](float px, float py, float pz) {
        const float x = vp.m[0] * px + vp.m[4] * py + vp.m[8] * pz + vp.m[12];
        const float y = vp.m[1] * px + vp.m[5] * py + vp.m[9] * pz + vp.m[13];
        const float z = vp.m[2] * px + vp.m[6] * py + vp.m[10] * pz + vp.m[14];
        const float w = vp.m[3] * px + vp.m[7] * py + vp.m[11] * pz + vp.m[15];
        return std::tuple{x / w, y / w, z / w};
    };

    auto [x, y, z] = project(0.0f, 0.0f, 0.0f);  // the target -> clip origin
    CHECK(x == doctest::Approx(0.0f));
    CHECK(y == doctest::Approx(0.0f));
    CHECK(z == doctest::Approx(99.0f * 3.0f / (4.0f * 98.0f)));

    std::tie(x, y, z) = project(4.0f, 0.0f, 0.0f);  // D*tan along +x: right edge
    CHECK(x == doctest::Approx(1.0f));  // (a wrong cross order mirrors this)

    std::tie(x, y, z) = project(0.0f, -4.0f, 0.0f);  // world -y (screen-up): top
    CHECK(y == doctest::Approx(-1.0f));  // (a y-flip lands this at +1)

    std::tie(x, y, z) = project(0.0f, 4.0f, 0.0f);  // world +y: bottom edge
    CHECK(y == doctest::Approx(1.0f));

    std::tie(x, y, z) = project(0.0f, 0.0f, -3.0f);  // eye + near: the near plane
    CHECK(z == doctest::Approx(0.0f));

    std::tie(x, y, z) = project(0.0f, 0.0f, 95.0f);  // eye + far: the far plane
    CHECK(z == doctest::Approx(1.0f));
}

TEST_CASE("camera_vp3d fov and aspect scale the frustum edges") {
    using ws::ui::r2d::Camera3D;
    using ws::ui::r2d::batch::camera_vp3d;
    // fov 53.13deg -> half-tan 0.5: at view depth 4 the frustum's half-height
    // is 2 and, on a 2:1 frame, its half-width is 4.
    Camera3D camera;
    camera.eye[2] = -4.0f;
    camera.fov_y = 0.9272952f;  // 2 * atan(0.5)
    camera.near_z = 1.0f;
    camera.far_z = 99.0f;
    const auto vp = camera_vp3d(camera, 200.0f, 100.0f);
    const auto project = [&](float px, float py, float pz) {
        const float x = vp.m[0] * px + vp.m[4] * py + vp.m[8] * pz + vp.m[12];
        const float y = vp.m[1] * px + vp.m[5] * py + vp.m[9] * pz + vp.m[13];
        const float w = vp.m[3] * px + vp.m[7] * py + vp.m[11] * pz + vp.m[15];
        return std::pair{x / w, y / w};
    };

    auto [x, y] = project(4.0f, 0.0f, 0.0f);  // half-width: aspect widens x
    CHECK(x == doctest::Approx(1.0f));
    std::tie(x, y) = project(2.0f, 0.0f, 0.0f);  // half-height's worth along x
    CHECK(x == doctest::Approx(0.5f));
    std::tie(x, y) = project(0.0f, -2.0f, 0.0f);  // half-height: vertical edge
    CHECK(y == doctest::Approx(-1.0f));
}

TEST_CASE("camera_vp3d off-axis eye keeps the target at the frustum center") {
    using ws::ui::r2d::Camera3D;
    namespace batch = ws::ui::r2d::batch;
    Camera3D camera;
    camera.eye[0] = 10.0f; camera.eye[1] = -20.0f; camera.eye[2] = -7.0f;
    camera.target[0] = 3.0f; camera.target[1] = 4.0f; camera.target[2] = 5.0f;
    camera.up[0] = -0.3f; camera.up[1] = -0.8f; camera.up[2] = 0.52f;  // unnormalized
    camera.fov_y = 1.5707963f;
    camera.near_z = 1.0f;
    camera.far_z = 99.0f;

    const batch::CamBasis basis = batch::camera_basis(camera);
    const auto vp = batch::camera_vp3d(camera, 100.0f, 100.0f);
    const auto project = [&](const float p[3]) {
        const float x = vp.m[0] * p[0] + vp.m[4] * p[1] + vp.m[8] * p[2] + vp.m[12];
        const float y = vp.m[1] * p[0] + vp.m[5] * p[1] + vp.m[9] * p[2] + vp.m[13];
        const float w = vp.m[3] * p[0] + vp.m[7] * p[1] + vp.m[11] * p[2] + vp.m[15];
        return std::pair{x / w, y / w};
    };
    // The frustum edges sit half a fov-tan away from the TARGET, along the
    // basis axes (the shared basis is exactly what production records with).
    const float depth = (camera.target[0] - camera.eye[0]) * basis.forward[0] +
                        (camera.target[1] - camera.eye[1]) * basis.forward[1] +
                        (camera.target[2] - camera.eye[2]) * basis.forward[2];
    const float s = depth * std::tan(camera.fov_y * 0.5f);

    auto [x, y] = project(camera.target);  // the target pins the view center -
    CHECK(x == doctest::Approx(0.0f));  // which only a wrong translation sign
    CHECK(y == doctest::Approx(0.0f));  // (m[12]/m[13]) can break: the canonical
                                        // camera's eye sits on both axes' zero
    float p[3];
    for (int a = 0; a < 3; ++a) p[a] = camera.target[a] + s * basis.right[a];
    std::tie(x, y) = project(p);  // right edge of the frustum
    CHECK(x == doctest::Approx(1.0f));
    for (int a = 0; a < 3; ++a) p[a] = camera.target[a] + s * basis.up[a];
    std::tie(x, y) = project(p);  // screen-up edge: TOP
    CHECK(y == doctest::Approx(-1.0f));
    for (int a = 0; a < 3; ++a) p[a] = camera.target[a] - s * basis.up[a];
    std::tie(x, y) = project(p);  // screen-down edge: BOTTOM
    CHECK(y == doctest::Approx(1.0f));
}

TEST_CASE("camera_vp3d degenerate inputs stay finite") {
    using ws::ui::r2d::Camera3D;
    using ws::ui::r2d::batch::Mat4;
    using ws::ui::r2d::batch::camera_vp3d;
    const auto all_finite = [](const Mat4& vp) {
        for (float e : vp.m) {
            if (!std::isfinite(e)) {
                return false;
            }
        }
        return true;
    };
    const auto assert_finite = [&](const Camera3D& camera, float w, float h) {
        CHECK(all_finite(camera_vp3d(camera, w, h)));
    };

    const float nan = std::numeric_limits<float>::quiet_NaN();
    Camera3D camera;

    camera.fov_y = 0.0f;         assert_finite(camera, 100.0f, 100.0f);
    camera.fov_y = -0.5f;        assert_finite(camera, 100.0f, 100.0f);
    camera.fov_y = nan;           assert_finite(camera, 100.0f, 100.0f);
    camera.fov_y = 3.14159265f;  assert_finite(camera, 100.0f, 100.0f);  // >pi/2 half-tan
    camera.fov_y = 0.7853982f;
    camera.near_z = 0.0f;        assert_finite(camera, 100.0f, 100.0f);
    camera.near_z = -1.0f;       assert_finite(camera, 100.0f, 100.0f);
    camera.near_z = nan;          assert_finite(camera, 100.0f, 100.0f);
    camera.near_z = 0.1f;
    camera.far_z = 0.1f;         assert_finite(camera, 100.0f, 100.0f);  // == near
    camera.far_z = -5.0f;        assert_finite(camera, 100.0f, 100.0f);  // < near
    camera.far_z = 1000.0f;
    camera.target[0] = camera.eye[0];  // target == eye: forward degenerates
    camera.target[1] = camera.eye[1];
    camera.target[2] = camera.eye[2];
    assert_finite(camera, 100.0f, 100.0f);
    camera.up[0] = 0.0f; camera.up[1] = 0.0f; camera.up[2] = 0.0f;  // zero hint
    assert_finite(camera, 100.0f, 100.0f);
    camera.up[0] = 0.0f; camera.up[1] = 0.0f; camera.up[2] = 1.0f;  // parallel to
    assert_finite(camera, 100.0f, 100.0f);                         // the fallback forward
    assert_finite(camera, 0.0f, 0.0f);  // zero extents
}

TEST_CASE("camera_basis is orthonormal and honors the up hint") {
    using ws::ui::r2d::Camera3D;
    using ws::ui::r2d::batch::CamBasis;
    using ws::ui::r2d::batch::camera_basis;
    using ws::math::dot;  // the workspace vecmath - what camera_basis itself uses

    // The default camera looks down +z; the y-down hint makes world -y the
    // screen-up and +x the screen-right - the same axes the 2D camera maps.
    const CamBasis b = camera_basis(Camera3D{});
    CHECK(b.right[0] == doctest::Approx(1.0f));
    CHECK(b.right[1] == doctest::Approx(0.0f));
    CHECK(b.right[2] == doctest::Approx(0.0f));
    CHECK(b.up[0] == doctest::Approx(0.0f));
    CHECK(b.up[1] == doctest::Approx(-1.0f));
    CHECK(b.up[2] == doctest::Approx(0.0f));
    CHECK(b.forward[0] == doctest::Approx(0.0f));
    CHECK(b.forward[1] == doctest::Approx(0.0f));
    CHECK(b.forward[2] == doctest::Approx(1.0f));

    CHECK(dot(b.right, b.right) == doctest::Approx(1.0f));  // orthonormal:
    CHECK(dot(b.up, b.up) == doctest::Approx(1.0f));       // unit axes,
    CHECK(dot(b.forward, b.forward) == doctest::Approx(1.0f));
    CHECK(dot(b.right, b.up) == doctest::Approx(0.0f));  // mutually perpendicular
    CHECK(dot(b.right, b.forward) == doctest::Approx(0.0f));
    CHECK(dot(b.up, b.forward) == doctest::Approx(0.0f));

    // The hint defines the view handedness: flipping it mirrors the basis.
    Camera3D flipped;
    flipped.up[1] = 1.0f;  // y-up hint
    const CamBasis f = camera_basis(flipped);
    CHECK(f.right[0] == doctest::Approx(-1.0f));  // +x now renders LEFT
    CHECK(f.up[1] == doctest::Approx(1.0f));      // +y renders up, as hinted
    CHECK(f.forward[2] == doctest::Approx(1.0f));
}

TEST_CASE("billboard_corners centers on (x,y,z), faces the camera, spreads w/h") {
    namespace batch = ws::ui::r2d::batch;
    const batch::CamBasis basis = batch::camera_basis(ws::ui::r2d::Camera3D{});

    float out[4][3];
    batch::billboard_corners(100.0f, 200.0f, -50.0f, 4.0f, 6.0f, basis, out);

    // Every corner sits on the plane through the center facing the camera,
    // and the corners average to the center (a symmetric spread).
    const float center[3] = {100.0f, 200.0f, -50.0f};
    float mean[3] = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 4; ++i) {
        const float off[3] = {out[i][0] - center[0], out[i][1] - center[1],
                              out[i][2] - center[2]};
        CHECK(off[0] * basis.forward[0] + off[1] * basis.forward[1] +
              off[2] * basis.forward[2] == doctest::Approx(0.0f));
        for (int a = 0; a < 3; ++a) {
            mean[a] += out[i][a] * 0.25f;
        }
    }
    CHECK(mean[0] == doctest::Approx(center[0]));
    CHECK(mean[1] == doctest::Approx(center[1]));
    CHECK(mean[2] == doctest::Approx(center[2]));

    // TL, TR, BR, BL in the canonical basis: a center-anchored y-down quad.
    CHECK(out[0][0] == doctest::Approx(98.0f));   // TL: x - w/2, y - h/2
    CHECK(out[0][1] == doctest::Approx(197.0f));
    CHECK(out[1][0] == doctest::Approx(102.0f));  // TR
    CHECK(out[1][1] == doctest::Approx(197.0f));
    CHECK(out[2][0] == doctest::Approx(102.0f));  // BR
    CHECK(out[2][1] == doctest::Approx(203.0f));
    CHECK(out[3][0] == doctest::Approx(98.0f));   // BL
    CHECK(out[3][1] == doctest::Approx(203.0f));
    CHECK(out[0][2] == doctest::Approx(-50.0f));  // all on the center's plane
    CHECK(out[2][2] == doctest::Approx(-50.0f));
}

TEST_CASE("line_quad3d expands perpendicular to segment and view; degenerates drop") {
    namespace batch = ws::ui::r2d::batch;
    const batch::CamBasis basis = batch::camera_basis(ws::ui::r2d::Camera3D{});

    float pos[4][3];
    float uv[4][2];
    REQUIRE(batch::line_quad3d(0.0f, 0.0f, 0.0f, 10.0f, 0.0f, 0.0f, 2.0f, basis,
                               pos, uv));
    // n = cross(dir, forward) = (0,-1,0): the ribbon spans world y, A,B at p0
    // and C,D at p1 - line_quad's structure.
    CHECK(pos[0][0] == doctest::Approx(0.0f));   // A: p0 + n*half
    CHECK(pos[0][1] == doctest::Approx(-1.0f));
    CHECK(pos[0][2] == doctest::Approx(0.0f));
    CHECK(pos[1][0] == doctest::Approx(0.0f));   // B: p0 - n*half
    CHECK(pos[1][1] == doctest::Approx(1.0f));
    CHECK(pos[2][0] == doctest::Approx(10.0f));  // C: p1 - n*half
    CHECK(pos[2][1] == doctest::Approx(1.0f));
    CHECK(pos[3][0] == doctest::Approx(10.0f));  // D: p1 + n*half
    CHECK(pos[3][1] == doctest::Approx(-1.0f));
    CHECK(uv[0][0] == 0.0f);  // line_quad's UVs: u along, v across
    CHECK(uv[1][1] == 1.0f);
    CHECK(uv[2][0] == 1.0f);
    CHECK(uv[3][1] == 0.0f);

    // Degenerate input drops, the 2D contract.
    CHECK(batch::line_quad3d(1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 2.0f, basis,
                             pos, uv) == false);  // zero length
    CHECK(batch::line_quad3d(0.0f, 0.0f, 0.0f, 10.0f, 0.0f, 0.0f, 0.0f, basis,
                             pos, uv) == false);  // no thickness

    // A segment along the view axis degenerates the cross - the screen-up
    // fallback keeps the ribbon finite and spread across the screen.
    REQUIRE(batch::line_quad3d(0.0f, 0.0f, -10.0f, 0.0f, 0.0f, 10.0f, 2.0f,
                               basis, pos, uv));
    for (int i = 0; i < 4; ++i) {
        CHECK(std::isfinite(pos[i][0]));
        CHECK(std::isfinite(pos[i][1]));
        CHECK(std::isfinite(pos[i][2]));
    }
    CHECK(pos[0][1] == doctest::Approx(-1.0f));  // spread along screen-up
    CHECK(pos[1][1] == doctest::Approx(1.0f));
    CHECK(pos[0][2] == doctest::Approx(-10.0f));  // and ALONG the segment
    CHECK(pos[3][2] == doctest::Approx(10.0f));
}

TEST_CASE("open_segment carries depth_test; texture splits inherit it") {
    using ws::ui::r2d::batch::kFullClip;
    BatchCore core;
    core.begin_frame();

    // The default is the 2D kind: chunks render through the painter's
    // pipeline, depth untouched.
    REQUIRE(core.open_segment(default_vp()));
    REQUIRE(push_test_quad(core, 0u));
    CHECK(core.chunk(0).depth_test == false);

    // A perspective segment flags its chunks - the flush's per-chunk
    // pipeline bind keys on this bit. The clip can stay the full-frame
    // sentinel while the flag is set: the defaulted parameter keeps the
    // existing callers' calls unchanged.
    REQUIRE(core.open_segment(default_vp(), kFullClip, true));
    REQUIRE(push_test_quad(core, 0u));
    CHECK(core.chunk(1).depth_test == true);

    // A texture-overflow split continues the SAME segment: the fresh
    // chunk inherits the depth mode alongside the VP and clip.
    core.begin_frame();
    REQUIRE(core.open_segment(default_vp(), kFullClip, true));
    for (std::uint32_t id = 0;
         id <= ws::ui::r2d::batch::kMaxTexturesPerChunk; ++id) {
        // 33 unique textures: one past the 32-slot table forces the split.
        REQUIRE(push_test_quad(core, id));
    }
    REQUIRE(core.chunk_count() == 2);
    CHECK(core.chunk(0).depth_test == true);
    CHECK(core.chunk(1).depth_test == true);
    CHECK(core.chunk(1).vp.m[0] == core.chunk(0).vp.m[0]);
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

TEST_CASE("the rounded-rect mask bakes arcs with one-texel coverage") {
    using namespace ws::ui::r2d;
    const auto mask = rounded::bake();
    using rounded::kTexSize;
    using rounded::kTexRadius;

    // RGBA8, row-major from the top-left.
    REQUIRE(mask.size() ==
            std::size_t(kTexSize) * std::size_t(kTexSize) * 4);
    const auto px = [&](int x, int y, int c) {
        return mask[(std::size_t(y) * kTexSize + x) * 4 + c];
    };

    // Shape rides in alpha only: RGB is white everywhere, even outside the
    // rect (a tinted draw needs the color to come from the vertex, not the
    // mask).
    const int probes[][2] = {{0, 0}, {31, 0}, {16, 16}, {0, 31}};
    for (const auto& p : probes) {
        CHECK(px(p[0], p[1], 0) == 255);
        CHECK(px(p[0], p[1], 1) == 255);
        CHECK(px(p[0], p[1], 2) == 255);
    }

    // The flat interior is solid: this texel sits inside the [r, n-r] box,
    // so no arc ever touches it.
    CHECK(px(16, 16, 3) == 255);
    // The corner is fully outside the quarter-circle at radius 8.
    CHECK(px(0, 0, 3) == 0);
    CHECK(px(31, 31, 3) == 0);
    // And a texel on the arc is partial - the one-texel coverage ramp that
    // antialiases the corners. Its exact byte depends on sqrt rounding,
    // but a full 0 or 255 here would mean the ramp is missing.
    const int arc = px(0, 5, 3);  // dist to the arc center (8, 8) ~ 7.91
    CHECK(arc > 0);
    CHECK(arc < 255);
    CHECK(arc > 100);  // ~60% coverage, not a sliver or a near-solid
    CHECK(arc < 200);
    // The baked radius is a whole number of texels: at (r, r) the box
    // clamp starts, so the arc's flat edges begin exactly there.
    CHECK(px(kTexRadius, 0, 3) == 255);  // top edge, right of the corner: solid
    CHECK(px(0, kTexRadius, 3) == 255);  // left edge, below the corner
}

TEST_CASE("public r2d API is drop-safe without a running app") {
    using namespace ws::ui::r2d;
    begin();               // screen mode
    begin(Camera2D{});     // world mode
    set_layer(3);
    draw_quad(0.0f, 0.0f, 10.0f, 10.0f, rgba(255, 255, 255));
    draw_line(0.0f, 0.0f, 10.0f, 10.0f, 2.0f, rgba(255, 255, 255));
    draw_sprite(0.0f, 0.0f, 10.0f, 10.0f, rgba(255, 255, 255), Sprite{});
    push_clip(0.0f, 0.0f, 10.0f, 10.0f);  // warns, then ignored: no app
    pop_clip();
    draw_rounded_quad(0.0f, 0.0f, 10.0f, 10.0f, 3.0f,
                      rgba(255, 255, 255));  // same warn-once, same ignore
    end();
    load_texture("nope.png");            // warns, returns white, no crash
    const auto sprites = load_atlas({"nope.png"});  // warns, empty, no crash
    CHECK(sprites.empty());
    CHECK(sprite_size(Sprite{}).width == 0.0f);
    const Font font = load_font("nope.ttf", 16.0f);  // warns, empty, no crash
    CHECK(font.empty());
    // Headless draw_text drops the quads but keeps the honest width - the
    // font's own measure - so layout math survives a missing renderer.
    CHECK(draw_text(0.0f, 0.0f, "hi", font) == 0.0f);
    Font::Glyph synth[95]{};
    synth['h' - 32].advance = 4.0f;
    const Font filled(16.0f, 20.0f, synth, 95);
    CHECK(draw_text(0.0f, 0.0f, "hh", filled) == 8.0f);

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

TEST_CASE("mixed textures keep painter order; one chunk absorbs them all") {
    using namespace ws::ui::r2d;
    Renderer2D r;
    r.frame_begin(100.0f, 100.0f);
    r.begin(Camera2D{});
    r.draw_quad(0.0f, 0.0f, 1.0f, 1.0f, rgba(255, 255, 255), Texture{1});
    r.draw_quad(0.0f, 20.0f, 1.0f, 1.0f, rgba(255, 255, 255), Texture{2});
    r.draw_quad(20.0f, 0.0f, 1.0f, 1.0f, rgba(255, 255, 255), Texture{1});
    r.draw_quad(20.0f, 20.0f, 1.0f, 1.0f, rgba(255, 255, 255), Texture{2});
    r.frame_end();

    // Texture is not a sort key: the replay is the exact submission order,
    // and the per-chunk texture table holds both textures in ONE chunk.
    // (Sorting by material would have pulled the texture-1 quads together,
    // flipping overlapping draws that differ only in texture - the exact
    // bug that put a rounded face over its label bar.)
    REQUIRE(r.batch().chunk_count() == 1);
    const auto& v = r.batch().vertices();
    REQUIRE(v.size() == 16);
    CHECK(v[0].pos[0] == 0.0f);
    CHECK(v[0].pos[1] == 0.0f);
    CHECK(v[0].tex_index == 0.0f);
    CHECK(v[4].pos[0] == 0.0f);
    CHECK(v[4].pos[1] == 20.0f);
    CHECK(v[4].tex_index == 1.0f);
    CHECK(v[8].pos[0] == 20.0f);
    CHECK(v[8].tex_index == 0.0f);   // texture 1 again: reuses its table slot
    CHECK(v[12].pos[0] == 20.0f);
    CHECK(v[12].pos[1] == 20.0f);
    CHECK(v[12].tex_index == 1.0f);  // texture 2's slot again
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

// --- 3D camera blocks: record/replay through the same sort ---------------------

TEST_CASE("begin(Camera3D) records depth-tested perspective chunks") {
    using namespace ws::ui::r2d;
    Renderer2D r;
    r.frame_begin(100.0f, 100.0f);
    r.begin(Camera3D{});
    r.draw_quad(0.0f, 0.0f, 100.0f, 10.0f, 10.0f, rgba(255, 255, 255));
    r.frame_end();

    REQUIRE(r.batch().chunk_count() == 1);
    CHECK(r.batch().chunk(0).depth_test == true);
    // The chunk VP is the recorded camera's perspective matrix, entry for
    // entry (same inputs as the expected: deterministic math).
    const auto want = batch::camera_vp3d(Camera3D{}, 100.0f, 100.0f);
    for (int i = 0; i < 16; ++i) {
        CHECK(r.batch().chunk(0).vp.m[i] == want.m[i]);
    }
    // The billboard's z is real world data now, not the 2D z=0 plane.
    CHECK(r.batch().vertices()[0].pos[2] == 100.0f);

    // A 2D block stays the 2D kind: orthographic VP, flag off.
    r.frame_begin(100.0f, 100.0f);
    r.begin(Camera2D{});
    r.draw_quad(0.0f, 0.0f, 10.0f, 10.0f, rgba(255, 255, 255));
    r.frame_end();
    REQUIRE(r.batch().chunk_count() == 1);
    CHECK(r.batch().chunk(0).depth_test == false);
    const auto want2d = batch::camera_vp(Camera2D{}, 100.0f, 100.0f);
    CHECK(r.batch().chunk(0).vp.m[0] == want2d.m[0]);
}

TEST_CASE("3D overloads in a 2D block drop; 2D draws in a 3D block land on z=0") {
    using namespace ws::ui::r2d;
    Renderer2D r;
    r.frame_begin(100.0f, 100.0f);
    r.begin(Camera2D{});
    r.draw_quad(0.0f, 0.0f, 50.0f, 10.0f, 10.0f, rgba(255, 255, 255));
    r.draw_line(0.0f, 0.0f, 0.0f, 10.0f, 10.0f, 10.0f, 2.0f,
                rgba(255, 255, 255));
    r.draw_sprite(0.0f, 0.0f, 50.0f, 10.0f, 10.0f, rgba(255, 255, 255),
                 Sprite{});
    r.frame_end();
    // Warned once per frame, not asserted here (the log path is the same
    // warn_drop plumbing the block tests above pin); the CONTRACT is that
    // nothing records.
    CHECK(r.batch().chunk_count() == 0);
    CHECK(r.batch().vertices().empty());

    // The reverse is legal: a 2D draw inside a 3D block records on the
    // world's z=0 plane and replays through the perspective VP - a
    // depth-tested chunk, the documented surprise.
    r.frame_begin(100.0f, 100.0f);
    r.begin(Camera3D{});
    r.draw_quad(0.0f, 0.0f, 10.0f, 10.0f, rgba(255, 255, 255));  // 2D form
    r.frame_end();
    REQUIRE(r.batch().vertices().size() == 4);
    for (const auto& v : r.batch().vertices()) {
        CHECK(v.pos[2] == 0.0f);  // the world's z=0 plane, by construction
    }
    CHECK(r.batch().chunk(0).depth_test == true);
}

TEST_CASE("3D and 2D blocks sort by epoch beside each other") {
    using namespace ws::ui::r2d;
    Renderer2D r;
    r.frame_begin(100.0f, 100.0f);
    r.begin(Camera2D{});
    r.draw_quad(0.0f, 0.0f, 1.0f, 1.0f, rgba(255, 255, 255));  // block 1: 2D
    r.begin(Camera3D{});
    r.draw_quad(10.0f, 0.0f, 50.0f, 1.0f, 1.0f, rgba(255, 255, 255));  // 3D
    r.frame_end();

    REQUIRE(r.batch().chunk_count() == 2);
    CHECK(r.batch().chunk(0).depth_test == false);  // 2D block first
    CHECK(r.batch().chunk(1).depth_test == true);   // then the 3D block
    CHECK(r.batch().vertices()[0].pos[0] == 0.0f);
    // The 3D quad is CENTER-anchored at x=10, w=1: its TL corner is 9.5
    // (the 2D form's x=10 would be the corner itself - this pins the
    // anchoring difference between the overloads).
    CHECK(r.batch().vertices()[4].pos[0] == doctest::Approx(9.5f));

    // set_layer outranks the block, across kinds: a high-layer 2D block
    // draws after the 3D block regardless of submission order.
    r.frame_begin(100.0f, 100.0f);
    r.begin(Camera2D{});
    r.set_layer(5);
    r.draw_quad(0.0f, 0.0f, 1.0f, 1.0f, rgba(255, 255, 255));
    r.begin(Camera3D{});
    r.set_layer(0);
    r.draw_quad(10.0f, 0.0f, 50.0f, 1.0f, 1.0f, rgba(255, 255, 255));
    r.frame_end();
    REQUIRE(r.batch().chunk_count() == 2);
    CHECK(r.batch().chunk(0).depth_test == true);    // layer 0: the 3D block
    CHECK(r.batch().chunk(1).depth_test == false);   // layer 5: the 2D block
    CHECK(r.batch().vertices()[0].pos[0] == doctest::Approx(9.5f));  // 3D TL
}

TEST_CASE("3D draw_sprite resolves the page rect onto a billboard") {
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
    r.begin(Camera3D{});
    r.draw_sprite(0.0f, 0.0f, 100.0f, 8.0f, 4.0f, rgba(255, 255, 255),
                  Sprite{7, 0});
    r.frame_end();

    REQUIRE(r.batch().vertices().size() == 4);
    const auto& v = r.batch().vertices();
    // The default camera's basis: right (1,0,0), up (0,-1,0) - world -y is
    // screen up, the 2D convention. TL = center - right*w/2 + up*h/2.
    CHECK(v[0].pos[0] == doctest::Approx(-4.0f));  // TL
    CHECK(v[0].pos[1] == doctest::Approx(-2.0f));
    CHECK(v[0].pos[2] == doctest::Approx(100.0f));
    CHECK(v[1].pos[0] == doctest::Approx(4.0f));   // TR
    CHECK(v[2].pos[1] == doctest::Approx(2.0f));   // BR
    CHECK(v[3].pos[0] == doctest::Approx(-4.0f));  // BL
    // Per-corner sub-rect UVs, the 2D draw_sprite contract.
    CHECK(v[0].uv[0] == 0.25f);  // TL
    CHECK(v[0].uv[1] == 0.5f);
    CHECK(v[2].uv[0] == 0.75f);  // BR
    CHECK(v[2].uv[1] == 1.0f);
    CHECK(r.batch().chunk(0).texture_ids[0] == 7u);  // the page texture
    CHECK(r.batch().chunk(0).depth_test == true);

    // An unknown sprite degrades to white on the same billboard geometry.
    r.frame_begin(100.0f, 100.0f);
    r.begin(Camera3D{});
    r.draw_sprite(0.0f, 0.0f, 100.0f, 8.0f, 4.0f, rgba(255, 255, 255),
                  Sprite{9, 0});
    r.frame_end();
    REQUIRE(r.batch().vertices().size() == 4);
    CHECK(r.batch().vertices()[0].uv[0] == 0.0f);  // full white UVs
    CHECK(r.batch().vertices()[0].pos[2] == doctest::Approx(100.0f));
    CHECK(r.batch().chunk(0).texture_ids[0] == 0u);
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

TEST_CASE("sprites and raw textures interleave; the page never splits the chunk") {
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

    // The whole point of atlases is the chunk structure, not the draw
    // order: one page's sprites ride one texture-table slot, so however
    // the calls interleave with other textures everything stays in one
    // chunk - and, texture not being a sort key, the app's painter order
    // survives the batch intact.
    REQUIRE(r.batch().chunk_count() == 1);
    const auto& v = r.batch().vertices();
    REQUIRE(v.size() == 12);
    CHECK(v[0].pos[0] == 0.0f);   // sprite 0, first submitted
    CHECK(v[4].pos[0] == 20.0f);  // the raw-texture quad, in its place
    CHECK(v[8].pos[1] == 20.0f);  // sprite 1, last submitted
    CHECK(v[0].tex_index == 0.0f);  // page 7's slot...
    CHECK(v[4].tex_index == 1.0f);  // ...the quad's own slot...
    CHECK(v[8].tex_index == 0.0f);  // ...page 7 again
    CHECK(r.batch().chunk(0).texture_count == 2);
    CHECK(r.batch().chunk(0).texture_ids[0] == 7u);
    CHECK(r.batch().chunk(0).texture_ids[1] == 9u);
    // Sub-rect UVs stayed per-sprite through the batch.
    CHECK(v[0].uv[0] == 0.0f);
    CHECK(v[8].uv[0] == 0.5f);
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

TEST_CASE("push_clip batches clip runs apart; pop restores the full frame") {
    using namespace ws::ui::r2d;
    Renderer2D r;
    r.frame_begin(800.0f, 600.0f);
    r.begin(Camera2D{});
    r.draw_quad(0.0f, 0.0f, 10.0f, 10.0f, rgba(255, 255, 255));
    r.push_clip(10.0f, 10.0f, 100.0f, 100.0f);
    r.draw_quad(20.0f, 20.0f, 10.0f, 10.0f, rgba(255, 255, 255));
    r.pop_clip();
    r.draw_quad(200.0f, 200.0f, 10.0f, 10.0f, rgba(255, 255, 255));
    r.end();
    r.frame_end();

    // Clip changes split chunks in RECORD order: full-clip quad, clipped
    // quad, full-clip quad again - three chunks. (The old clip sort key
    // would have merged the two full-clip draws into one chunk ahead of
    // the clipped one - the grouping was never the contract, painter
    // order is.)
    REQUIRE(r.batch().chunk_count() == 3);
    const auto& first = r.batch().chunk(0);
    CHECK(first.quad_count == 1);
    for (int i = 0; i < 4; ++i) {
        CHECK(first.clip[i] == ws::ui::r2d::batch::kFullClip[i]);
    }
    const auto& clipped = r.batch().chunk(1);
    CHECK(clipped.quad_count == 1);
    CHECK(clipped.clip[0] == 10.0f);
    CHECK(clipped.clip[1] == 10.0f);
    CHECK(clipped.clip[2] == 100.0f);
    CHECK(clipped.clip[3] == 100.0f);
    const auto& last = r.batch().chunk(2);
    CHECK(last.quad_count == 1);
    for (int i = 0; i < 4; ++i) {
        CHECK(last.clip[i] == ws::ui::r2d::batch::kFullClip[i]);
    }
    // Painter order survives: first submitted first, everywhere.
    CHECK(r.batch().vertices()[0].pos[0] == 0.0f);
    CHECK(r.batch().vertices()[4].pos[0] == 20.0f);
    CHECK(r.batch().vertices()[8].pos[0] == 200.0f);
}

TEST_CASE("overlapping clipped panels keep record order (clip is not a sort key)") {
    using namespace ws::ui::r2d;
    Renderer2D r;
    r.frame_begin(800.0f, 600.0f);
    r.begin(Camera2D{});
    // Panel A first, its clip origin sorts LATER (100,100)...
    r.push_clip(100.0f, 100.0f, 200.0f, 200.0f);
    r.draw_quad(80.0f, 80.0f, 200.0f, 200.0f, rgba(255, 0, 0));
    r.pop_clip();
    // ...panel B second, clip origin sorts EARLIER (0,0). The old sort
    // key moved B's chunk before A's, so A painted over B in the
    // overlap - the z swap this test pins down.
    r.push_clip(0.0f, 0.0f, 150.0f, 150.0f);
    r.draw_quad(80.0f, 80.0f, 200.0f, 200.0f, rgba(0, 255, 0));
    r.pop_clip();
    r.end();
    r.frame_end();

    // Record order: chunk 0 is panel A (red), chunk 1 panel B (green) -
    // B paints OVER A where the panels overlap, exactly as recorded.
    REQUIRE(r.batch().chunk_count() == 2);
    CHECK(r.batch().chunk(0).clip[0] == 100.0f);
    CHECK(r.batch().chunk(1).clip[0] == 0.0f);
    CHECK(r.batch().vertices()[0].color[1] == 0.0f);  // red quad first
    CHECK(r.batch().vertices()[4].color[1] == 1.0f);  // green quad second
}

TEST_CASE("more than 16 clip runs overflow loudly, not silently") {
    using namespace ws::ui::r2d;
    Renderer2D r;
    r.frame_begin(800.0f, 600.0f);
    r.begin(Camera2D{});
    // One more distinct clip than the cap: 17 single-draw runs, each its
    // own chunk (a clip change always splits).
    for (std::uint32_t i = 0; i < batch::kMaxChunksPerFrame + 1; ++i) {
        r.push_clip(1.0f + static_cast<float>(i), 2.0f, 10.0f, 10.0f);
        r.draw_quad(0.0f, 0.0f, 1.0f, 1.0f, rgba(255, 255, 255));
        r.pop_clip();
    }
    r.end();
    r.frame_end();

    // The array does NOT grow: every chunk slot owns a pre-allocated
    // descriptor set per swapchain image, so growth would mean mid-frame
    // GPU allocation - forbidden. The overflow is counted instead (the
    // renderer logs it once per frame; the stats surface carries it).
    REQUIRE(r.batch().chunk_count() == batch::kMaxChunksPerFrame);
    CHECK(r.batch().dropped_chunks() == 1);   // the 17th run never opened
    CHECK(r.batch().dropped_quads() == 1);   // ...and its draw dropped
    // The 16 surviving chunks replay in exact record order.
    REQUIRE(r.batch().vertices().size() == 16 * 4);
    for (std::uint32_t i = 0; i < batch::kMaxChunksPerFrame; ++i) {
        CHECK(r.batch().chunk(i).clip[0] == 1.0f + static_cast<float>(i));
    }
}

TEST_CASE("nested clips intersect at push time; disjoint ones go empty") {
    using namespace ws::ui::r2d;
    Renderer2D r;
    r.frame_begin(800.0f, 600.0f);
    r.begin(Camera2D{});
    r.push_clip(0.0f, 0.0f, 200.0f, 200.0f);
    r.push_clip(50.0f, 50.0f, 300.0f, 300.0f);  // intersects to (50,50,150,150)
    r.draw_quad(60.0f, 60.0f, 10.0f, 10.0f, rgba(255, 255, 255));
    r.pop_clip();
    r.draw_quad(70.0f, 70.0f, 10.0f, 10.0f, rgba(255, 255, 255));
    r.pop_clip();
    r.end();
    r.frame_end();

    // One chunk per clip value; nesting intersected, never unioned. The
    // inner-clipped draw was RECORDED first, so it is chunk 0 in record
    // order - the opposite of the clip-value sort, and the point of M5:
    // record order is the contract. (Chunk order and z-order both follow
    // record here; the nested draw simply paints first.)
    REQUIRE(r.batch().chunk_count() == 2);
    const auto& inner = r.batch().chunk(0);
    CHECK(inner.clip[0] == 50.0f);
    CHECK(inner.clip[1] == 50.0f);
    CHECK(inner.clip[2] == 150.0f);
    CHECK(inner.clip[3] == 150.0f);
    const auto& outer = r.batch().chunk(1);
    CHECK(outer.clip[0] == 0.0f);
    CHECK(outer.clip[1] == 0.0f);
    CHECK(outer.clip[2] == 200.0f);
    CHECK(outer.clip[3] == 200.0f);

    // A disjoint push intersects to the empty rect - normalized, never
    // negative, never NaN - and still batches under that value.
    r.frame_begin(800.0f, 600.0f);
    r.begin(Camera2D{});
    r.push_clip(10.0f, 10.0f, 50.0f, 50.0f);
    r.push_clip(200.0f, 200.0f, 50.0f, 50.0f);  // no overlap with the first
    r.draw_quad(0.0f, 0.0f, 10.0f, 10.0f, rgba(255, 255, 255));
    r.end();
    r.frame_end();
    REQUIRE(r.batch().chunk_count() == 1);
    const auto& empty = r.batch().chunk(0);
    CHECK(empty.clip[0] == 200.0f);  // the intersection's left/top survive
    CHECK(empty.clip[1] == 200.0f);
    CHECK(empty.clip[2] == 0.0f);    // but the extents clamp to nothing
    CHECK(empty.clip[3] == 0.0f);
}

TEST_CASE("the clip stack balances overflow, resets per frame, pops empty safely") {
    using namespace ws::ui::r2d;
    Renderer2D r;
    r.frame_begin(800.0f, 600.0f);
    r.begin(Camera2D{});
    // Overflow: depth counts past the cap so pops stay balanced - the
    // warn fires once per renderer, deeper clips stop shrinking.
    for (int i = 0; i < Renderer2D::kMaxClipDepth + 3; ++i) {
        r.push_clip(5.0f, 5.0f, 10.0f, 10.0f);
    }
    r.draw_quad(0.0f, 0.0f, 1.0f, 1.0f, rgba(255, 255, 255));  // deepest real clip
    for (int i = 0; i < Renderer2D::kMaxClipDepth + 3; ++i) {
        r.pop_clip();
    }
    r.draw_quad(0.0f, 20.0f, 1.0f, 1.0f, rgba(255, 255, 255));  // back to full
    r.end();
    r.frame_end();
    // Two clip values, so two chunks in record order: the overflowed draw
    // first, the post-pop draw second. Chunk 0 kept the deepest REAL clip,
    // not a stack-past-the-end one.
    REQUIRE(r.batch().chunk_count() == 2);
    CHECK(r.batch().chunk(0).quad_count == 1);
    CHECK(r.batch().chunk(1).quad_count == 1);
    CHECK(r.batch().chunk(0).clip[0] == 5.0f);
    CHECK(r.batch().chunk(0).clip[1] == 5.0f);
    CHECK(r.batch().chunk(0).clip[2] == 10.0f);
    CHECK(r.batch().chunk(0).clip[3] == 10.0f);
    for (int i = 0; i < 4; ++i) {
        CHECK(r.batch().chunk(1).clip[i] == ws::ui::r2d::batch::kFullClip[i]);
    }

    // A clip left pushed never leaks: frame_begin empties the stack. (The
    // frame below records nothing, so frame_end is a no-op - the stack
    // reset is what frame_begin does, not what frame_end undoes.)
    r.frame_begin(800.0f, 600.0f);
    r.push_clip(5.0f, 5.0f, 5.0f, 5.0f);  // never popped
    r.frame_end();

    r.frame_begin(800.0f, 600.0f);
    r.begin(Camera2D{});
    r.pop_clip();  // nothing pushed this frame: documented no-op
    r.draw_quad(0.0f, 0.0f, 1.0f, 1.0f, rgba(255, 255, 255));
    r.end();
    r.frame_end();
    REQUIRE(r.batch().chunk_count() == 1);
    for (int i = 0; i < 4; ++i) {
        CHECK(r.batch().chunk(0).clip[i] == ws::ui::r2d::batch::kFullClip[i]);
    }
}

TEST_CASE("draw_rounded_quad 9-slices: one segment, corners sample the arcs") {
    using namespace ws::ui::r2d;
    Renderer2D r;
    r.frame_begin(100.0f, 100.0f);
    r.begin(Camera2D{});
    r.draw_rounded_quad(0.0f, 0.0f, 100.0f, 40.0f, 8.0f, rgba(90, 168, 110));
    r.end();
    r.frame_end();

    // Nine slices, one material (the builtin mask - white headless), so one
    // batch segment: a rounded quad costs no more chunk budget than a flat
    // one. Slice order is row-major: 0=TL corner, 1=top strip, 2=TR corner,
    // 3=left strip, 4=center, ..., 8=BR corner.
    const auto& v = r.batch().vertices();
    REQUIRE(r.batch().chunk_count() == 1);
    REQUIRE(r.batch().chunk(0).quad_count == 9);
    REQUIRE(v.size() == 36);
    const float lo = float(rounded::kTexRadius) / float(rounded::kTexSize);

    // TL corner slice: sits at the rect's corner, samples the mask's arc.
    CHECK(v[0].pos[0] == doctest::Approx(0.0f));
    CHECK(v[0].pos[1] == doctest::Approx(0.0f));
    CHECK(v[0].uv[0] == doctest::Approx(0.0f));
    CHECK(v[0].uv[1] == doctest::Approx(0.0f));
    // Top strip: starts at x = radius, stretches the mask's solid edge band.
    CHECK(v[4].pos[0] == doctest::Approx(8.0f));
    CHECK(v[4].pos[1] == doctest::Approx(0.0f));
    CHECK(v[4].uv[0] == doctest::Approx(lo));
    CHECK(v[4].uv[1] == doctest::Approx(0.0f));
    // TR corner: mirrored slice, mirrored UVs.
    CHECK(v[8].pos[0] == doctest::Approx(92.0f));
    CHECK(v[8].uv[0] == doctest::Approx(1.0f - lo));
    // Center: inset by the radius on both axes, samples the solid middle.
    CHECK(v[16].pos[0] == doctest::Approx(8.0f));
    CHECK(v[16].pos[1] == doctest::Approx(8.0f));
    CHECK(v[16].uv[0] == doctest::Approx(lo));
    CHECK(v[16].uv[1] == doctest::Approx(lo));
    // BR corner: the deepest slice, full-depth UVs.
    CHECK(v[32].pos[0] == doctest::Approx(92.0f));
    CHECK(v[32].pos[1] == doctest::Approx(32.0f));
    CHECK(v[32].uv[0] == doctest::Approx(1.0f - lo));
    CHECK(v[32].uv[1] == doctest::Approx(1.0f - lo));
}

TEST_CASE("draw_rounded_quad clamps oversize radii, squares sub-pixel, drops empty") {
    using namespace ws::ui::r2d;
    Renderer2D r;
    r.frame_begin(100.0f, 100.0f);
    r.begin(Camera2D{});
    // Radius 8 > half of the 4px side: clamps to 2. Still nine slices - the
    // horizontal strips degenerate to zero height but record, keeping the
    // slice count a constant (tests and batching stay simple).
    r.draw_rounded_quad(0.0f, 0.0f, 10.0f, 4.0f, 8.0f, rgba(255, 255, 255));
    // Sub-pixel and zero radii: honest square quads, one record each.
    r.draw_rounded_quad(0.0f, 30.0f, 10.0f, 4.0f, 0.25f, rgba(255, 255, 255));
    r.draw_rounded_quad(0.0f, 40.0f, 10.0f, 4.0f, 0.0f, rgba(255, 255, 255));
    r.end();
    r.frame_end();

    const auto& v = r.batch().vertices();
    REQUIRE(r.batch().chunk_count() == 1);  // all white-texture: one segment
    REQUIRE(r.batch().chunk(0).quad_count == 11);  // 9 + 1 + 1
    REQUIRE(v.size() == 44);
    CHECK(v[4].pos[0] == doctest::Approx(2.0f));  // top strip starts at the
    CHECK(v[4].pos[1] == doctest::Approx(0.0f)); // clamped radius
    CHECK(v[12].pos[1] == doctest::Approx(2.0f));  // zero-height strip records
    CHECK(v[36].uv[0] == doctest::Approx(0.0f));   // plain quad: kQuadUV
    CHECK(v[36].uv[1] == doctest::Approx(0.0f));
    CHECK(v[36].pos[1] == doctest::Approx(30.0f));  // and it is the y=30 quad

    // Degenerate extents drop silently - live app data, not worth a warn.
    r.frame_begin(100.0f, 100.0f);
    r.begin(Camera2D{});
    r.draw_rounded_quad(0.0f, 0.0f, 0.0f, 10.0f, 3.0f, rgba(255, 255, 255));
    r.draw_rounded_quad(0.0f, 0.0f, 10.0f, -1.0f, 3.0f, rgba(255, 255, 255));
    r.end();
    r.frame_end();
    CHECK(r.batch().chunk_count() == 0);
    CHECK(r.batch().vertices().empty());
}

TEST_CASE("draw_rounded_quad rotation pivots the whole rect, slices rigid") {
    using namespace ws::ui::r2d;
    Renderer2D r;
    r.frame_begin(100.0f, 100.0f);
    r.begin(Camera2D{});
    r.draw_rounded_quad(0.0f, 0.0f, 100.0f, 100.0f, 10.0f, rgba(255, 255, 255),
                        1.5707963f);
    r.end();
    r.frame_end();

    // +90 degrees, clockwise on screen: the whole square orbits its center
    // as one rigid shape - the TL corner swings to the top-right, the
    // center slice's TL lands 10px in along the new top edge, the BR
    // corner slice to the bottom-left. Nine pivoted slices, one shape.
    const auto& v = r.batch().vertices();
    REQUIRE(r.batch().chunk(0).quad_count == 9);
    CHECK(v[0].pos[0] == doctest::Approx(100.0f));
    CHECK(v[0].pos[1] == doctest::Approx(0.0f));
    CHECK(v[16].pos[0] == doctest::Approx(90.0f));
    CHECK(v[16].pos[1] == doctest::Approx(10.0f));
    CHECK(v[32].pos[0] == doctest::Approx(10.0f));
    CHECK(v[32].pos[1] == doctest::Approx(90.0f));
    // The UVs did not rotate with the shape - the BR slice still samples
    // the mask's BR arc, wherever the rotation carries it.
    CHECK(v[32].uv[0] == doctest::Approx(0.75f));
    CHECK(v[32].uv[1] == doctest::Approx(0.75f));
}

// --- gui module (Phases 0-4: the core is real) ---------------------------------------

TEST_CASE("gui module: the public surface constructs, links and behaves safely") {
    using namespace ws::ui::gui;

    // Fresh objects honor the declared defaults.
    CHECK(IdStack{}.depth() == 0);
    CHECK(LayoutCursor{}.depth() == 0);
    CHECK(GuiContext{}.hot_id() == kInvalidWidgetId);
    CHECK(GuiContext{}.active_id() == kInvalidWidgetId);

    // A filled snapshot flows through the frame lifecycle: begin_frame
    // adopts it, end_frame clears any press a vanished widget left held.
    InputFrame input;
    input.mouse_x = 100.0f;
    input.mouse_down[0] = true;
    input.key_pressed['D'] = true;
    input.scroll_dy = -2.0f;
    input.text_input.push_back(U'w');
    GuiContext gui;
    gui.begin_frame(input);
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
    CHECK(gui.ids().id("button") != kInvalidWidgetId);
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
    CHECK(frame.touch_count == 0);
    CHECK(frame.bridged_touch() == ws::ui::gui::kNoPointer);
    // An empty frame still lists the mouse: pointer slot 0, at rest.
    std::array<ws::ui::gui::PointerView,
               ws::ui::gui::InputFrame::kMaxPointers>
        pointers{};
    CHECK(frame.gather_pointers(pointers) == 1);
    CHECK(pointers[0].id == ws::ui::gui::kMousePointerId);
    CHECK(!pointers[0].down);
}

// --- InputFrame touch table (multi-touch groundwork) -------------------------

TEST_CASE("touch table: down/move/up track a contact through its frames") {
    using namespace ws::ui::gui;
    InputFrame frame;

    // Contact 17 lands: entry 0, held with a press edge.
    CHECK(frame.touch_down(PointerId{17}, 30.0f, 40.0f) == 0);
    CHECK(frame.touch_count == 1);
    CHECK(frame.touches[0].id == PointerId{17});
    CHECK(frame.touches[0].down);
    CHECK(frame.touches[0].pressed);
    CHECK(!frame.touches[0].released);
    CHECK(frame.touches[0].x == 30.0f);
    CHECK(frame.touches[0].y == 40.0f);

    // ...moves: position only, no new edges (the press edge from the down
    // above still belongs to this frame - edges clear at next_frame()).
    frame.touch_move(PointerId{17}, 55.0f, 65.0f);
    CHECK(frame.touches[0].x == 55.0f);
    CHECK(frame.touches[0].y == 65.0f);
    CHECK(frame.touches[0].down);
    CHECK(!frame.touches[0].released);

    // ...lifts: release edge, but the entry stays for this frame - a widget
    // must still be able to see who lifted.
    frame.touch_up(PointerId{17}, 60.0f, 70.0f, false);
    CHECK(frame.touch_count == 1);
    CHECK(!frame.touches[0].down);
    CHECK(frame.touches[0].released);
    CHECK(!frame.touches[0].cancelled);
    CHECK(frame.touches[0].x == 60.0f);

    // ...and leaves at the frame boundary.
    frame.next_frame();
    CHECK(frame.touch_count == 0);
}

TEST_CASE("touch table: insertion order survives releases and frame edges") {
    using namespace ws::ui::gui;
    InputFrame frame;
    frame.touch_down(PointerId{7}, 0.0f, 0.0f);
    frame.touch_down(PointerId{3}, 1.0f, 1.0f);
    frame.touch_down(PointerId{9}, 2.0f, 2.0f);
    CHECK(frame.touch_count == 3);
    CHECK(frame.touches[1].id == PointerId{3});

    // The middle contact lifts; the frame boundary compacts the table
    // without reordering the survivors.
    frame.touch_up(PointerId{3}, 1.0f, 1.0f, false);
    frame.next_frame();
    CHECK(frame.touch_count == 2);
    CHECK(frame.touches[0].id == PointerId{7});
    CHECK(frame.touches[1].id == PointerId{9});
    CHECK(!frame.touches[0].pressed);  // edges cleared on the survivors
    CHECK(!frame.touches[1].pressed);

    // A new contact appends after them.
    CHECK(frame.touch_down(PointerId{5}, 3.0f, 3.0f) == 2);
    CHECK(frame.touches[2].id == PointerId{5});
}

TEST_CASE("touch table: a repeated down for the same id updates in place") {
    using namespace ws::ui::gui;
    InputFrame frame;
    CHECK(frame.touch_down(PointerId{7}, 10.0f, 10.0f) == 0);
    // One down per contact is the platform contract; a stray repeat must
    // not duplicate the entry.
    CHECK(frame.touch_down(PointerId{7}, 20.0f, 20.0f) == 0);
    CHECK(frame.touch_count == 1);
    CHECK(frame.touches[0].x == 20.0f);
    CHECK(frame.touches[0].pressed);
}

TEST_CASE("primary-touch bridge: the first contact mirrors the mouse") {
    using namespace ws::ui::gui;
    InputFrame frame;

    // The first contact of the gesture is the primary: it mirrors the mouse
    // so single-finger use and ImGui keep working unchanged.
    frame.touch_down(PointerId{7}, 10.0f, 20.0f);
    CHECK(frame.bridged_touch() == PointerId{7});
    CHECK(frame.mouse_x == 10.0f);
    CHECK(frame.mouse_y == 20.0f);
    CHECK(frame.mouse_down[0]);
    CHECK(frame.mouse_pressed[0]);

    // A second finger joins: tracked, never mirrored.
    frame.touch_down(PointerId{9}, 80.0f, 90.0f);
    CHECK(frame.bridged_touch() == PointerId{7});
    CHECK(frame.mouse_x == 10.0f);

    // Each finger's moves mirror only its own.
    frame.touch_move(PointerId{9}, 85.0f, 95.0f);
    CHECK(frame.mouse_x == 10.0f);
    frame.touch_move(PointerId{7}, 15.0f, 25.0f);
    CHECK(frame.mouse_x == 15.0f);
    CHECK(frame.mouse_y == 25.0f);

    // The secondary lifting changes nothing for the mouse.
    frame.touch_up(PointerId{9}, 85.0f, 95.0f, false);
    CHECK(frame.mouse_down[0]);
    CHECK(!frame.mouse_released[0]);
    CHECK(frame.bridged_touch() == PointerId{7});

    // The primary lifting releases the mouse.
    frame.touch_up(PointerId{7}, 15.0f, 25.0f, false);
    CHECK(!frame.mouse_down[0]);
    CHECK(frame.mouse_released[0]);
    CHECK(frame.bridged_touch() == kNoPointer);
}

TEST_CASE("bridge: no promotion - a freed mouse stays free until a new gesture") {
    using namespace ws::ui::gui;
    InputFrame frame;
    frame.touch_down(PointerId{7}, 10.0f, 20.0f);
    frame.touch_down(PointerId{9}, 30.0f, 40.0f);

    // The primary lifts while the secondary holds.
    frame.touch_up(PointerId{7}, 10.0f, 20.0f, false);
    CHECK(frame.bridged_touch() == kNoPointer);
    CHECK(!frame.mouse_down[0]);

    // The surviving finger neither owns nor moves the mouse.
    frame.touch_move(PointerId{9}, 35.0f, 45.0f);
    CHECK(frame.mouse_x == 10.0f);
    CHECK(frame.mouse_released[0]);  // still released - no re-press

    // The NEXT contact starts a fresh gesture and bridges.
    frame.touch_down(PointerId{4}, 60.0f, 70.0f);
    CHECK(frame.bridged_touch() == PointerId{4});
    CHECK(frame.mouse_down[0]);
    CHECK(frame.mouse_pressed[0]);
    CHECK(frame.mouse_x == 60.0f);
}

TEST_CASE("cancelled contact: release edge with cancelled set, bridge lifts") {
    using namespace ws::ui::gui;
    InputFrame frame;
    frame.touch_down(PointerId{2}, 5.0f, 6.0f);
    frame.touch_up(PointerId{2}, 5.0f, 6.0f, true);

    CHECK(frame.touches[0].cancelled);
    CHECK(frame.touches[0].released);
    CHECK(!frame.touches[0].down);
    // A cancelled gesture is "never happened": the mouse reads as released,
    // and the bridge frees.
    CHECK(!frame.mouse_down[0]);
    CHECK(frame.mouse_released[0]);
    CHECK(frame.bridged_touch() == kNoPointer);
}

TEST_CASE("touch cap: the ninth contact drops, the table stays intact") {
    using namespace ws::ui::gui;
    InputFrame frame;
    for (std::uint32_t id = 1; id <= static_cast<std::uint32_t>(
                                     InputFrame::kMaxTouches);
         ++id) {
        CHECK(frame.touch_down(PointerId{id}, static_cast<float>(id), 0.0f) >=
              0);
    }
    CHECK(frame.touch_count == InputFrame::kMaxTouches);
    CHECK(frame.bridged_touch() == PointerId{1});  // the first contact bridges

    // The 9th finger: rejected with no side effects anywhere.
    CHECK(frame.touch_down(PointerId{9}, 99.0f, 99.0f) == -1);
    CHECK(frame.touch_count == InputFrame::kMaxTouches);
    CHECK(frame.bridged_touch() == PointerId{1});
    CHECK(frame.mouse_x == 1.0f);       // the dropped contact moved nothing
    CHECK(!frame.mouse_released[0]);   // ...and lifted nothing
    CHECK(frame.touches[InputFrame::kMaxTouches - 1].id ==
          PointerId{InputFrame::kMaxTouches});
}

TEST_CASE("gather_pointers: mouse is slot 0, touches follow in table order") {
    using namespace ws::ui::gui;
    InputFrame frame;
    frame.touch_down(PointerId{7}, 40.0f, 50.0f);
    frame.touch_down(PointerId{9}, 60.0f, 70.0f);
    frame.touch_up(PointerId{9}, 65.0f, 75.0f, false);  // 9 is in its release frame
    // The platform's bridge would have overwritten the mirrored mouse with
    // the touch; writing the mouse fields directly is the desktop shape.
    // Either way slot 0 reports whatever the mouse state says.
    frame.mouse_x = 12.0f;
    frame.mouse_y = 34.0f;
    frame.mouse_down[0] = true;
    frame.mouse_pressed[0] = true;

    std::array<PointerView, InputFrame::kMaxPointers> pointers{};
    const int count = frame.gather_pointers(pointers);
    CHECK(count == 3);
    CHECK(pointers[0].id == kMousePointerId);
    CHECK(pointers[0].x == 12.0f);
    CHECK(pointers[0].y == 34.0f);
    CHECK(pointers[0].down);
    CHECK(pointers[0].pressed);
    CHECK(pointers[1].id == PointerId{7});
    CHECK(pointers[1].x == 40.0f);
    CHECK(pointers[1].down);
    CHECK(pointers[2].id == PointerId{9});
    CHECK(!pointers[2].down);
    CHECK(pointers[2].released);
    CHECK(pointers[2].x == 65.0f);
    CHECK(pointers[3].id == kNoPointer);  // past the count: the sentinel

    // pointer_pos reaches the mouse and each touch - including one in its
    // release frame - and misses ids the frame never held.
    float x = 0.0f;
    float y = 0.0f;
    CHECK(frame.pointer_pos(kMousePointerId, &x, &y));
    CHECK(x == 12.0f);
    CHECK(y == 34.0f);
    CHECK(frame.pointer_pos(PointerId{7}, &x, &y));
    CHECK(x == 40.0f);
    CHECK(y == 50.0f);
    CHECK(frame.pointer_pos(PointerId{9}, &x, &y));
    CHECK(x == 65.0f);
    CHECK(y == 75.0f);
    CHECK(frame.pointer_pos(PointerId{7}, nullptr, nullptr));  // pure existence check
    CHECK(!frame.pointer_pos(PointerId{1234}, &x, &y));
}

TEST_CASE("Rect helpers: edges, centers and symmetric insets") {
    using namespace ws::ui::gui;
    const Rect r{10.0f, 20.0f, 100.0f, 40.0f};

    CHECK(r.right() == 110.0f);
    CHECK(r.bottom() == 60.0f);
    CHECK(r.center_x() == 60.0f);
    CHECK(r.center_y() == 40.0f);

    const Rect big = r.expanded(2.0f);
    CHECK(big.x == 8.0f);
    CHECK(big.y == 18.0f);
    CHECK(big.width == 104.0f);
    CHECK(big.height == 44.0f);
    const Rect small = r.shrunk(2.0f);
    CHECK(small.x == 12.0f);
    CHECK(small.y == 22.0f);
    CHECK(small.width == 96.0f);
    CHECK(small.height == 36.0f);

    // The half-open test: the inside and the top-left corner in, the
    // far edges out.
    CHECK(r.contains(10.0f, 20.0f));
    CHECK(r.contains(55.0f, 40.0f));
    CHECK(!r.contains(110.0f, 40.0f));
    CHECK(!r.contains(10.0f, 60.0f));
    CHECK(!Rect{}.contains(0.0f, 0.0f));  // empty rects contain nothing

    // Shrunk past empty keeps the raw arithmetic; the half-open test
    // then answers false everywhere - an over-inset rect hits nothing.
    const Rect gone = r.shrunk(60.0f);
    CHECK(gone.width < 0.0f);
    CHECK(!gone.contains(60.0f, 40.0f));
}

// --- gui ids (Phase 2) --------------------------------------------------------------

TEST_CASE("IdStack: same label under different scopes ids differently") {
    using namespace ws::ui::gui;
    IdStack ids;
    ids.push("file-dialog");
    const WidgetId file_ok = ids.id("ok");
    ids.pop();
    ids.push("error-dialog");
    const WidgetId error_ok = ids.id("ok");

    CHECK(file_ok != error_ok);
    CHECK(file_ok != kInvalidWidgetId);
    CHECK(error_ok != kInvalidWidgetId);

    // pop() restored the first scope: the same derivation reads the same.
    ids.pop();
    ids.push("file-dialog");
    CHECK(ids.id("ok") == file_ok);
}

TEST_CASE("IdStack: ids are stable across frames and fresh stacks") {
    using namespace ws::ui::gui;
    // Immediate mode re-describes the UI every frame: two independent
    // walks of the same code path must derive identical ids - that is
    // what lets per-widget state key on them.
    IdStack frame_one;
    frame_one.push("panel");
    const WidgetId one = frame_one.id("slider");

    IdStack frame_two;
    frame_two.push("panel");
    CHECK(frame_two.id("slider") == one);

    // ...through int scopes too (indexed list rows).
    frame_one.push(3);
    frame_two.push(3);
    const WidgetId cell_one = frame_one.id("cell");
    CHECK(cell_one == frame_two.id("cell"));
    CHECK(cell_one != one);

    frame_one.pop();
    frame_two.pop();
    CHECK(frame_one.id("slider") == one);
}

TEST_CASE("IdStack: int scopes separate siblings without stringifying") {
    using namespace ws::ui::gui;
    IdStack ids;
    ids.push("list");
    ids.push(0);
    const WidgetId row_zero = ids.id("row");
    ids.pop();
    ids.push(1);
    const WidgetId row_one = ids.id("row");
    ids.pop();

    CHECK(row_zero != row_one);

    // The VALUE 65 is not the LABEL "A" and not the LABEL "65": int
    // scopes hash the integer's bytes, never a stringified form.
    ids.push(65);
    const WidgetId via_value = ids.id("row");
    ids.pop();
    ids.push("A");
    CHECK(ids.id("row") != via_value);
    ids.pop();
    ids.push("65");
    CHECK(ids.id("row") != via_value);
}

TEST_CASE("IdStack: scope paths are delimited, not plain concatenations") {
    using namespace ws::ui::gui;
    IdStack ab;
    ab.push("ab");
    const WidgetId ab_then_c = ab.id("c");

    IdStack a;
    a.push("a");
    const WidgetId a_then_bc = a.id("bc");

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
    const WidgetId outer_widget = ids.id("widget");

    ids.push("inner");
    CHECK(ids.depth() == 2);
    CHECK(ids.id("widget") != outer_widget);

    ids.pop();
    CHECK(ids.depth() == 1);
    CHECK(ids.id("widget") == outer_widget);

    // Pushing past kMaxDepth clamps (with one warn): the remembered
    // scope stops deepening, but the depth KEEPS COUNTING, so the
    // matching pops stay balanced - they can never eat a real scope.
    for (int i = 0; i < IdStack::kMaxDepth + 4; ++i) {
        ids.push(i);
    }
    CHECK(ids.depth() == 1 + IdStack::kMaxDepth + 4);  // the count stays honest
    CHECK(ids.id("deep") != kInvalidWidgetId);  // still a valid id
    for (int i = 0; i < IdStack::kMaxDepth + 4; ++i) {
        ids.pop();
    }

    // The overflow round trip corrupted nothing above the clamp.
    CHECK(ids.depth() == 1);
    CHECK(ids.id("widget") == outer_widget);
}

TEST_CASE("ScopedId: the destructor pops, even through an early return") {
    using namespace ws::ui::gui;
    IdStack ids;
    const WidgetId outside = ids.id("w");

    const auto early = [&ids]() {
        ScopedId scope(ids, "panel");
        return ids.id("w");  // returns mid-scope: the guard pops anyway
    };
    CHECK(early() != outside);       // same label, different scope
    CHECK(ids.depth() == 0);         // popped by the guard, not by hand
    CHECK(ids.id("w") == outside);  // the parent scope answers again

    {
        ScopedId index_scope(ids, 3);
        CHECK(ids.depth() == 1);
    }
    CHECK(ids.depth() == 0);

    // A copy or a move would pop the same scope twice: the guard's
    // lifetime IS its correctness, so neither may exist.
    static_assert(!std::is_copy_constructible_v<ScopedId>);
    static_assert(!std::is_copy_assignable_v<ScopedId>);
    static_assert(!std::is_move_constructible_v<ScopedId>);
    static_assert(!std::is_move_assignable_v<ScopedId>);
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

// One touch contact for touch_frame(): where it is and which edges fired
// this frame. A contact with neither edge is held (mid-drag); pressed +
// released together model a tap that began and ended within one frame.
struct TouchSnap {
    std::uint32_t id;
    float x;
    float y;
    bool pressed = false;
    bool released = false;
};

// snap()'s multi-touch counterpart: a frame of ONLY these contacts, built
// through the public touch_* writers. The first touch_down bridges into
// the mouse by design (the primary-touch -> mouse mirror), and writing the
// mouse fields afterwards is the either-writer rule the Phase A gather
// test set - so the mouse parks far away and the frame's pointer list is
// exactly these contacts, in order.
ws::ui::gui::InputFrame touch_frame(std::initializer_list<TouchSnap> contacts) {
    ws::ui::gui::InputFrame frame;
    for (const TouchSnap& contact : contacts) {
        const ws::ui::gui::PointerId id{contact.id};
        const int slot = frame.touch_down(id, contact.x, contact.y);
        if (slot >= 0 && !contact.pressed) {
            frame.touches[static_cast<std::size_t>(slot)].pressed = false;
        }
        if (contact.released) {
            frame.touch_up(id, contact.x, contact.y, false);
        }
    }
    frame.mouse_x = 900.0f;  // parked: nothing in the test rects is there
    frame.mouse_y = 900.0f;
    frame.mouse_down[0] = false;
    frame.mouse_pressed[0] = false;
    frame.mouse_released[0] = false;
    return frame;
}

}  // namespace

TEST_CASE("is_hot/is_active: the am-I spelling, invalid pinned false") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const WidgetId w = IdStack{}.id("w");
    const Rect rect{0.0f, 0.0f, 100.0f, 100.0f};

    // Hover: hot, not active.
    gui.begin_frame(snap(50.0f, 50.0f));
    (void)gui.hit_test(w, rect);
    CHECK(gui.is_hot(w));
    CHECK(!gui.is_active(w));
    CHECK(!gui.is_hot(kInvalidWidgetId));  // "no widget" never matches
    CHECK(!gui.is_active(kInvalidWidgetId));
    CHECK(!gui.is_hot(w, PointerId{12345u}));  // an unknown pointer answers false
    gui.end_frame();

    // Press: the press frame claims active on the spot.
    gui.begin_frame(snap(50.0f, 50.0f, true, true));
    (void)gui.hit_test(w, rect);
    CHECK(gui.is_hot(w));
    CHECK(gui.is_active(w));
    CHECK(!gui.is_active(w, PointerId{12345u}));
    gui.end_frame();

    // Idle: hot_id() itself answers the invalid sentinel - is_hot() must
    // not fall into matching it.
    GuiContext idle;
    idle.begin_frame(snap(50.0f, 50.0f));
    CHECK(idle.hot_id() == kInvalidWidgetId);
    CHECK(!idle.is_hot(kInvalidWidgetId));
    idle.end_frame();
}

TEST_CASE("hit_test: hover claims hot; a full click is press-then-release inside") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const WidgetId id = IdStack{}.id("button");
    const Rect rect{100.0f, 100.0f, 80.0f, 24.0f};

    // Frame 1: pointer inside, nothing held - hover only.
    gui.begin_frame(snap(120.0f, 110.0f));
    HitTest hit = gui.hit_test(id, rect);
    CHECK(hit.hovered);
    CHECK(!hit.pressed);
    CHECK(!hit.clicked);
    CHECK(!hit.released);
    CHECK(gui.hot_id() == id);
    CHECK(gui.active_id() == kInvalidWidgetId);
    gui.end_frame();

    // Frame 2: press inside - the widget takes the press.
    gui.begin_frame(snap(120.0f, 110.0f, true, true));
    hit = gui.hit_test(id, rect);
    CHECK(hit.hovered);
    CHECK(hit.pressed);
    CHECK(!hit.clicked);
    CHECK(gui.active_id() == id);

    // Frame 3: still held, pointer dragged outside - the owner keeps the
    // press, hover is gone.
    gui.begin_frame(snap(10.0f, 10.0f, true));
    hit = gui.hit_test(id, rect);
    CHECK(!hit.hovered);
    CHECK(hit.pressed);
    CHECK(gui.active_id() == id);

    // Frame 4: release outside - the drag ends, no click.
    gui.begin_frame(snap(10.0f, 10.0f, false, false, true));
    hit = gui.hit_test(id, rect);
    CHECK(hit.released);
    CHECK(!hit.clicked);
    CHECK(gui.active_id() == kInvalidWidgetId);
    gui.end_frame();
}

TEST_CASE("hit_test: release inside is a click; press+release in one frame still clicks") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const WidgetId id = IdStack{}.id("button");
    const Rect rect{0.0f, 0.0f, 100.0f, 100.0f};

    gui.begin_frame(snap(50.0f, 50.0f, true, true));
    (void)gui.hit_test(id, rect);
    gui.end_frame();

    gui.begin_frame(snap(50.0f, 50.0f, false, false, true));
    const HitTest hit = gui.hit_test(id, rect);
    CHECK(hit.hovered);
    CHECK(hit.released);
    CHECK(hit.clicked);
    CHECK(gui.active_id() == kInvalidWidgetId);
    CHECK(gui.hot_id() == id);
    gui.end_frame();

    // A click entirely between two frames: both edges arrive together.
    GuiContext fast;
    fast.begin_frame(snap(50.0f, 50.0f, false, true, true));
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
    const WidgetId a = ids.id("a");
    const WidgetId b = ids.id("b");
    const Rect rect{0.0f, 0.0f, 100.0f, 100.0f};

    gui.begin_frame(snap(50.0f, 50.0f, true, true));
    (void)gui.hit_test(a, rect);  // A runs first: takes the press
    CHECK(gui.active_id() == a);

    const HitTest under = gui.hit_test(b, rect);  // B overlaps: no press for B
    CHECK(!under.pressed);
    CHECK(!under.clicked);
    CHECK(gui.hot_id() == a);  // while A owns the press, B cannot be hot
    CHECK(gui.active_id() == a);

    // Pressing again while A owns it changes nothing for B.
    gui.begin_frame(snap(50.0f, 50.0f, true, true));
    const HitTest again = gui.hit_test(b, rect);
    CHECK(!again.pressed);
    CHECK(gui.active_id() == a);
    gui.end_frame();
}

TEST_CASE("hit_test: a vanished press owner is dropped at end_frame") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const WidgetId gone = IdStack{}.id("gone");
    const WidgetId other = IdStack{}.id("other");
    const Rect rect{0.0f, 0.0f, 100.0f, 100.0f};

    // Press on `gone`...
    gui.begin_frame(snap(50.0f, 50.0f, true, true));
    (void)gui.hit_test(gone, rect);
    CHECK(gui.active_id() == gone);
    gui.end_frame();

    // ...then the UI changes: `gone` no longer runs, but the release
    // arrives. end_frame must drop the ownership, or no widget could be
    // hot while the stale id owns the captured press.
    gui.begin_frame(snap(50.0f, 50.0f, false, false, true));
    gui.end_frame();
    CHECK(gui.active_id() == kInvalidWidgetId);

    gui.begin_frame(snap(50.0f, 50.0f));
    const HitTest hit = gui.hit_test(other, rect);
    CHECK(hit.hovered);
    CHECK(gui.hot_id() == other);
}

TEST_CASE("hit_test: while another widget owns the drag, overlapped widgets do not hover") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const IdStack ids;
    const WidgetId a = ids.id("a");
    const WidgetId b = ids.id("b");
    const Rect a_rect{0.0f, 0.0f, 100.0f, 100.0f};
    const Rect b_rect{100.0f, 0.0f, 100.0f, 100.0f};

    gui.begin_frame(snap(50.0f, 50.0f, true, true));
    (void)gui.hit_test(a, a_rect);  // A takes the press
    gui.end_frame();

    // The drag moves the pointer over B: B is inside, but A owns the
    // captured pointer - B must not report hover (hot, not merely
    // inside), and nobody is hot while the pointer is off the owner.
    gui.begin_frame(snap(150.0f, 50.0f, true));
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
    const WidgetId id = IdStack{}.id("dupe");

    // Two widgets claiming the same id (same label, same scope, no
    // push(index) between them) share hot/active - a caller bug the
    // context flags (warns once per process, visible in this log) and
    // tolerates: both queries answer, nothing corrupts.
    gui.begin_frame(snap(50.0f, 50.0f, true, true));
    const HitTest first = gui.hit_test(id, {0.0f, 0.0f, 100.0f, 100.0f});
    const HitTest second = gui.hit_test(id, {0.0f, 0.0f, 100.0f, 100.0f});
    CHECK(first.pressed);
    CHECK(second.pressed);
    gui.end_frame();

    // Distinct ids in one frame flag nothing (no false positives), and
    // the same id may reappear in the NEXT frame - the census is per
    // frame.
    gui.begin_frame(snap(50.0f, 50.0f));
    const HitTest again = gui.hit_test(id, {0.0f, 0.0f, 100.0f, 100.0f});
    CHECK(again.hovered);  // the same id reappears next frame, unflagged
    const WidgetId other = IdStack{}.id("other");
    (void)gui.hit_test(other, {0.0f, 0.0f, 100.0f, 100.0f});  // distinct: fine
    gui.end_frame();
}

// --- multi-pointer hit-testing (a finger each) -------------------------------------

TEST_CASE("hit_test: two pointers can press two widgets in the same frame") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const WidgetId a = IdStack{}.id("a");
    const WidgetId b = IdStack{}.id("b");
    const Rect a_rect{0.0f, 0.0f, 100.0f, 100.0f};
    const Rect b_rect{100.0f, 0.0f, 100.0f, 100.0f};

    // Two fingers, two widgets, one frame: each press is owned by the
    // finger that made it - the single-pointer rules, run per pointer.
    gui.begin_frame(touch_frame({{1, 50.0f, 50.0f, /*pressed=*/true},
                              {2, 150.0f, 50.0f, /*pressed=*/true}}));
    const HitTest hit_a = gui.hit_test(a, a_rect);
    const HitTest hit_b = gui.hit_test(b, b_rect);
    CHECK(hit_a.pressed);
    CHECK(hit_a.pointer == PointerId{1});
    CHECK(hit_b.pressed);
    CHECK(hit_b.pointer == PointerId{2});
    CHECK(gui.active_id(PointerId{1}) == a);
    CHECK(gui.active_id(PointerId{2}) == b);
    CHECK(gui.owning_pointer(a) == PointerId{1});
    CHECK(gui.owning_pointer(b) == PointerId{2});
    CHECK(gui.hot_id(PointerId{1}) == a);
    CHECK(gui.hot_id(PointerId{2}) == b);
    // The parked mouse claims nothing, but it is still a pointer in the
    // frame's list - and each pointer reports its own position.
    CHECK(gui.hot_id() == kInvalidWidgetId);
    CHECK(gui.pointer(PointerId{1}) != nullptr);
    CHECK(gui.pointer(PointerId{1})->x == 50.0f);
    CHECK(gui.pointer(PointerId{2})->x == 150.0f);
    CHECK(gui.pointer(PointerId{99}) == nullptr);  // no such pointer this frame
    CHECK(gui.pointer(kMousePointerId) != nullptr);
    gui.end_frame();

    // Both lift inside: both click, in one frame.
    gui.begin_frame(touch_frame({{1, 50.0f, 50.0f, false, true},
                              {2, 150.0f, 50.0f, false, true}}));
    CHECK(gui.hit_test(a, a_rect).clicked);
    CHECK(gui.hit_test(b, b_rect).clicked);
    gui.end_frame();
}

TEST_CASE("hit_test: two pointers on one widget - one press, one click") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const WidgetId id = IdStack{}.id("both");
    const Rect rect{0.0f, 0.0f, 100.0f, 100.0f};

    // Two fingers land on the same widget: ONE press (the fold), and the
    // first-contact finger is the deterministic owner.
    gui.begin_frame(touch_frame({{5, 30.0f, 30.0f, /*pressed=*/true},
                              {9, 70.0f, 70.0f, /*pressed=*/true}}));
    const HitTest hit = gui.hit_test(id, rect);
    CHECK(hit.pressed);
    CHECK(!hit.clicked);
    CHECK(hit.pointer == PointerId{5});  // the lowest slot with a claim
    CHECK(gui.owning_pointer(id) == PointerId{5});
    CHECK(gui.active_id(PointerId{5}) == id);  // both fingers hold it - same fold,
    CHECK(gui.active_id(PointerId{9}) == id);  // one press, benign by design
    gui.end_frame();

    // Both lift inside: the fold makes it one click - `clicked` is one
    // bool per frame no matter how many pointers resolved.
    gui.begin_frame(touch_frame({{5, 30.0f, 30.0f, false, true},
                              {9, 70.0f, 70.0f, false, true}}));
    CHECK(gui.hit_test(id, rect).clicked);
    CHECK(gui.active_id(PointerId{5}) == kInvalidWidgetId);  // both presses resolved
    CHECK(gui.active_id(PointerId{9}) == kInvalidWidgetId);
    gui.end_frame();
}

TEST_CASE("hit_test: capture is per pointer - one drag blocks no other finger") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const WidgetId a = IdStack{}.id("a");
    const WidgetId b = IdStack{}.id("b");
    const Rect a_rect{0.0f, 0.0f, 100.0f, 100.0f};
    const Rect b_rect{100.0f, 0.0f, 100.0f, 100.0f};

    // Finger 1 presses A; finger 2 presses B - same frame.
    gui.begin_frame(touch_frame({{1, 50.0f, 50.0f, /*pressed=*/true},
                              {2, 150.0f, 50.0f, /*pressed=*/true}}));
    CHECK(gui.hit_test(a, a_rect).pressed);
    CHECK(gui.hit_test(b, b_rect).pressed);
    gui.end_frame();

    // Finger 1 drags off A (still holding); finger 2 rests on B. A keeps
    // its captured press; B stays hovered - hot is per finger now, so a
    // foreign drag no longer darkens the whole frame.
    gui.begin_frame(touch_frame({{1, 10.0f, 250.0f},  // held, dragged off A
                              {2, 150.0f, 50.0f}}));  // held, on B
    const HitTest dragged = gui.hit_test(a, a_rect);
    const HitTest resting = gui.hit_test(b, b_rect);
    CHECK(dragged.pressed);  // capture: still owns finger 1's press
    CHECK(!dragged.hovered);  // off-rect: no hover
    CHECK(resting.pressed);  // B keeps finger 2's press
    CHECK(resting.hovered);  // ...and finger 2 still hovers it
    gui.end_frame();
}

TEST_CASE("end_frame: a vanished owner's release is dropped per pointer") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const WidgetId a = IdStack{}.id("a");
    const WidgetId b = IdStack{}.id("b");
    const Rect a_rect{0.0f, 0.0f, 100.0f, 100.0f};
    const Rect b_rect{100.0f, 0.0f, 100.0f, 100.0f};

    gui.begin_frame(touch_frame({{1, 50.0f, 50.0f, /*pressed=*/true},
                              {2, 150.0f, 50.0f, /*pressed=*/true}}));
    CHECK(gui.hit_test(a, a_rect).pressed);
    CHECK(gui.hit_test(b, b_rect).pressed);
    gui.end_frame();

    // The UI changed: A no longer runs, so finger 1's release arrives
    // with no owner. end_frame drops it without touching finger 2, whose
    // drag of B is still live and still consuming its own frames.
    gui.begin_frame(touch_frame({{1, 50.0f, 50.0f, false, true},
                              {2, 180.0f, 60.0f}}));
    CHECK(gui.hit_test(b, b_rect).pressed);  // B runs: finger 2 drags on
    gui.end_frame();                         // drops finger 1's orphan release
    CHECK(gui.active_id(PointerId{1}) == kInvalidWidgetId);
    CHECK(gui.active_id(PointerId{2}) == b);
    CHECK(gui.owning_pointer(b) == PointerId{2});
    gui.end_frame();
}

TEST_CASE("hit_test: the bridged finger is mouse and touch - one click") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const WidgetId id = IdStack{}.id("one");
    const Rect rect{0.0f, 0.0f, 100.0f, 100.0f};

    // Built through touch_down only - the bridge mirrors the mouse into
    // the contact. No parking here: the duplicate IS the test.
    InputFrame press;
    press.touch_down(PointerId{7}, 50.0f, 50.0f);
    gui.begin_frame(press);
    const HitTest hit = gui.hit_test(id, rect);
    CHECK(hit.pressed);
    CHECK(hit.pointer == kMousePointerId);  // the mouse slot outranks the touch
    CHECK(gui.active_id() == id);           // the mouse's track claimed...
    CHECK(gui.active_id(PointerId{7}) == id);  // ...and so did the touch's: the
                                              // documented duplicate, benign
    CHECK(gui.owning_pointer(id) == kMousePointerId);  // lowest slot resolves
    gui.end_frame();

    // The lift mirrors too: one click, not two.
    InputFrame lift;
    lift.touch_down(PointerId{7}, 50.0f, 50.0f);  // the entry, back for its release frame
    lift.touch_up(PointerId{7}, 50.0f, 50.0f, false);  // mirrors the mouse's release
    lift.touches[0].pressed = false;  // down and up in one frame: the press
    lift.mouse_pressed[0] = false;    // edge is not this frame's fact
    gui.begin_frame(lift);
    const HitTest released = gui.hit_test(id, rect);
    CHECK(released.released);
    CHECK(released.clicked);
    CHECK(released.pointer == kMousePointerId);
    gui.end_frame();
}

TEST_CASE("persistent_state: keys values by (type, id) for the context's lifetime") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const IdStack ids;
    const WidgetId a = ids.id("a");
    const WidgetId b = ids.id("b");

    // First touch default-constructs; entries survive frames.
    gui.persistent_state<float>(a) = 1.5f;
    gui.begin_frame(snap(0.0f, 0.0f));
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
    const WidgetId id = IdStack{}.id("fire");

    // Frame 1: press inside the button's rect - pressed, not yet clicked.
    gui.begin_frame(snap(60.0f, 12.0f, true, true));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(!button(gui, layout, kDefaultChrome, "fire"));
    layout.end_panel();
    CHECK(gui.active_id() == id);
    gui.end_frame();

    // Frame 2: the same pass again - the release lands on the same rect
    // and completes the click.
    gui.begin_frame(snap(60.0f, 12.0f, false, false, true));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(button(gui, layout, kDefaultChrome, "fire"));
    layout.end_panel();
    CHECK(gui.active_id() == kInvalidWidgetId);
    gui.end_frame();

    // Frame 3: no edges - the click is over and does not repeat.
    gui.begin_frame(snap(60.0f, 12.0f));
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
    gui.begin_frame(snap(60.0f, 36.0f, false, true, true));
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

    gui.begin_frame(snap(60.0f, 12.0f));  // pointer over "first", no press
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(!button(gui, layout, kDefaultChrome, "first"));
    CHECK(!button(gui, layout, kDefaultChrome, "second"));
    layout.end_panel();
    // The hover claimed hot for "first" (the second's rect misses the
    // pointer, so it claimed nothing); nothing became active.
    CHECK(gui.hot_id() == IdStack{}.id("first"));
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
    gui.begin_frame(snap(60.0f, 60.0f, false, true, true));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(!button(gui, layout, tall, "first"));
    CHECK(button(gui, layout, tall, "second"));
    layout.end_panel();
    gui.end_frame();
}

TEST_CASE("theme scaled: metrics multiply, faces and font carry") {
    using namespace ws::ui::gui;
    const ws::ui::r2d::Font font{};
    ChromeTheme base = kDefaultChrome;
    base.font = &font;

    const ChromeTheme big = base.scaled(2.0f);
    CHECK(big.control_height == base.control_height * 2.0f);
    CHECK(big.corner_radius == base.corner_radius * 2.0f);
    CHECK(big.padding == base.padding * 2.0f);
    CHECK(big.spacing == base.spacing * 2.0f);
    CHECK(big.scroll_speed == base.scroll_speed * 2.0f);
    CHECK(big.drag_threshold == base.drag_threshold * 2.0f);
    // The look does not change with the density: every face and the
    // borrowed font pointer carry as-is.
    CHECK(big.panel_bg == base.panel_bg);
    CHECK(big.control_on == base.control_on);
    CHECK(big.border == base.border);
    CHECK(big.font == &font);

    // Scale 1 is the identity - the 100%-scaling desktop.
    const ChromeTheme same = base.scaled(1.0f);
    CHECK(same.control_height == base.control_height);
    CHECK(same.drag_threshold == base.drag_threshold);
    CHECK(same.font == &font);
}

TEST_CASE("checkbox: a full click toggles the bool exactly once") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;
    bool value = false;

    // Idle: pointer away, no edges - nothing toggles.
    gui.begin_frame(snap(300.0f, 300.0f));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(!checkbox(gui, layout, kDefaultChrome, "signal", value));
    layout.end_panel();
    CHECK(value == false);
    gui.end_frame();

    // Press inside (the (0,0) row is 68x24): owned, but no flip yet.
    gui.begin_frame(snap(4.0f, 4.0f, true, true));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(!checkbox(gui, layout, kDefaultChrome, "signal", value));
    layout.end_panel();
    CHECK(value == false);
    gui.end_frame();

    // Release inside: the click lands - toggled true, reported once.
    gui.begin_frame(snap(4.0f, 4.0f, false, false, true));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(checkbox(gui, layout, kDefaultChrome, "signal", value));
    layout.end_panel();
    CHECK(value == true);
    gui.end_frame();

    // Next frame: the click is over; the value holds.
    gui.begin_frame(snap(4.0f, 4.0f));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(!checkbox(gui, layout, kDefaultChrome, "signal", value));
    layout.end_panel();
    CHECK(value == true);
    gui.end_frame();
}

TEST_CASE("slider: a press jumps to the pointer and the drag tracks it") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;
    float value = 0.0f;

    // Press at the middle of the (0,0,120,24) track: click-to-set jumps
    // the value there on the press frame itself.
    gui.begin_frame(snap(60.0f, 12.0f, true, true));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(slider_float(gui, layout, kDefaultChrome, "amp", value, 0.0f, 1.0f));
    layout.end_panel();
    CHECK(value == doctest::Approx(0.5f));
    gui.end_frame();

    // Hold and move to 3/4: the drag tracks the pointer.
    gui.begin_frame(snap(90.0f, 12.0f, true));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(slider_float(gui, layout, kDefaultChrome, "amp", value, 0.0f, 1.0f));
    layout.end_panel();
    CHECK(value == doctest::Approx(0.75f));
    gui.end_frame();

    // Release OUTSIDE the track: no click, the drag is over, and the
    // value parks wherever it landed - nothing maps on the release frame.
    gui.begin_frame(snap(200.0f, 12.0f, false, false, true));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(!slider_float(gui, layout, kDefaultChrome, "amp", value, 0.0f, 1.0f));
    layout.end_panel();
    CHECK(value == doctest::Approx(0.75f));
    gui.end_frame();
}

TEST_CASE("slider: two sliders drag under two pointers at once") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;
    float amp = 0.0f;
    float freq = 1.0f;

    // Two fingers press two sliders in the same frame (rows are 24 tall:
    // amp's track spans y 0..24, freq's y 24..48, both 120 wide). Each
    // press jump-sets its OWN slider - before multi-pointer, both values
    // mapped the mouse, and a second finger was invisible.
    gui.begin_frame(touch_frame({{1, 60.0f, 12.0f, /*pressed=*/true},
                              {2, 30.0f, 36.0f, /*pressed=*/true}}));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(slider_float(gui, layout, kDefaultChrome, "amp", amp, 0.0f, 1.0f));
    CHECK(slider_float(gui, layout, kDefaultChrome, "freq", freq, 0.0f, 1.0f));
    layout.end_panel();
    CHECK(amp == doctest::Approx(0.5f));    // finger 1 pressed at 60/120
    CHECK(freq == doctest::Approx(0.25f));  // finger 2 pressed at 30/120
    gui.end_frame();

    // Both held, both moved - each value follows ITS finger: never the
    // other pointer, never the parked mouse.
    gui.begin_frame(touch_frame({{1, 90.0f, 12.0f},
                              {2, 96.0f, 36.0f}}));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(slider_float(gui, layout, kDefaultChrome, "amp", amp, 0.0f, 1.0f));
    CHECK(slider_float(gui, layout, kDefaultChrome, "freq", freq, 0.0f, 1.0f));
    layout.end_panel();
    CHECK(amp == doctest::Approx(0.75f));  // finger 1 dragged to 90/120
    CHECK(freq == doctest::Approx(0.8f));  // finger 2 dragged to 96/120
    gui.end_frame();
}

TEST_CASE("slider: clamps and a degenerate range") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;

    // min == max: no division, no crash; a same-frame click (press+release
    // before the frame saw the button held) still click-to-sets via the
    // clicked path.
    float flat = 5.0f;
    gui.begin_frame(snap(10.0f, 12.0f, false, true, true));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(slider_float(gui, layout, kDefaultChrome, "flat", flat, 3.0f, 3.0f));
    layout.end_panel();
    CHECK(flat == doctest::Approx(3.0f));
    gui.end_frame();

    // An out-of-range value snaps in on the first interaction: pressing at
    // x=10 on a 0..1 track maps to 1/12 - the 2.0 never renders or sticks.
    float value = 2.0f;
    gui.begin_frame(snap(10.0f, 12.0f, true, true));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(slider_float(gui, layout, kDefaultChrome, "amp", value, 0.0f, 1.0f));
    layout.end_panel();
    CHECK(value == doctest::Approx(1.0f / 12.0f));
    gui.end_frame();
}

TEST_CASE("slider_int: float math, integer write-back") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;
    int samples = 128;

    // Mid-track press on 4..512: 4 + 0.5 * 508 = 258, exactly.
    gui.begin_frame(snap(60.0f, 12.0f, true, true));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(slider_int(gui, layout, kDefaultChrome, "samples", samples, 4, 512));
    layout.end_panel();
    CHECK(samples == 258);
    gui.end_frame();

    // Release inside: the clicked path re-maps the same pointer - same
    // value, so no change is reported and the int is not rewritten.
    gui.begin_frame(snap(60.0f, 12.0f, false, false, true));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(!slider_int(gui, layout, kDefaultChrome, "samples", samples, 4, 512));
    layout.end_panel();
    CHECK(samples == 258);
    gui.end_frame();
}

TEST_CASE("panel: begin scopes ids and flows children; end restores") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;
    const ChromeTheme& theme = kDefaultChrome;

    const WidgetId outside = IdStack{}.id("btn");

    gui.begin_frame(snap(400.0f, 400.0f));  // pointer away: no interactions
    const Rect content =
        begin_panel(gui, layout, theme, "demo", {10.0f, 10.0f, 200.0f, 100.0f});
    // Content = bounds inset by the theme padding.
    CHECK(content.x == doctest::Approx(10.0f + theme.padding));
    CHECK(content.y == doctest::Approx(10.0f + theme.padding));
    CHECK(content.width == doctest::Approx(200.0f - 2.0f * theme.padding));
    CHECK(content.height == doctest::Approx(100.0f - 2.0f * theme.padding));
    // Children id under the panel's scope.
    REQUIRE(gui.ids().depth() == 1);
    CHECK(gui.ids().id("btn") != outside);
    // A child button lays out inside the content rect and behaves.
    CHECK(!button(gui, layout, theme, "btn"));
    end_panel(gui, layout);
    // The id scope is back to the root: the same label means the same id.
    CHECK(gui.ids().depth() == 0);
    CHECK(gui.ids().id("btn") == outside);

    // The layout stack unwound too: a fresh panel flows from its own
    // bounds, not from where the last one left the cursor.
    const Rect after =
        begin_panel(gui, layout, theme, "demo2", {0.0f, 0.0f, 50.0f, 50.0f});
    CHECK(after.x == doctest::Approx(theme.padding));
    end_panel(gui, layout);
    gui.end_frame();
}

TEST_CASE("panel: nesting stacks; each end pops exactly one level") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;

    gui.begin_frame(snap(400.0f, 400.0f));
    const Rect outer =
        begin_panel(gui, layout, kDefaultChrome, "outer", {0.0f, 0.0f, 300.0f, 300.0f});
    REQUIRE(gui.ids().depth() == 1);
    const Rect inner = begin_panel(gui, layout, kDefaultChrome, "inner",
                                   {outer.x + 20.0f, outer.y + 20.0f, 100.0f, 100.0f});
    CHECK(inner.x == doctest::Approx(outer.x + 20.0f + kDefaultChrome.padding));
    CHECK(inner.y == doctest::Approx(outer.y + 20.0f + kDefaultChrome.padding));
    REQUIRE(gui.ids().depth() == 2);

    // A widget in the innermost panel ids under outer;inner - distinct
    // from the same label at the root, so the two can never fight over
    // hot/active.
    const WidgetId root_view = IdStack{}.id("signal");
    CHECK(gui.ids().id("signal") != root_view);
    bool value = false;
    CHECK(!checkbox(gui, layout, kDefaultChrome, "signal", value));
    CHECK(value == false);

    end_panel(gui, layout);  // inner: back in the outer scope
    CHECK(gui.ids().depth() == 1);
    end_panel(gui, layout);  // outer: back at the root
    CHECK(gui.ids().depth() == 0);
    gui.end_frame();
}

TEST_CASE("button: an icon rides along; corner variants keep the click contract") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;

    // An icon button (any sprite handle - headless, the draw drops like
    // every other widget draw) still lays out, hit-tests and clicks exactly
    // like a plain one: the icon is presentation, not a second widget.
    const ws::ui::r2d::Sprite icon{7, 3};  // never dereferenced here
    gui.begin_frame(snap(60.0f, 12.0f, false, true, true));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(button(gui, layout, kDefaultChrome, "fire", 120.0f, icon));
    layout.end_panel();
    gui.end_frame();

    // The sentinel sprite (the default argument) is the no-icon button.
    gui.begin_frame(snap(60.0f, 12.0f, false, true, true));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(button(gui, layout, kDefaultChrome, "plain"));
    layout.end_panel();
    gui.end_frame();

    // Square corners (radius 0) stay a live widget: the theme shapes the
    // drawing, never the interaction contract.
    ChromeTheme square = kDefaultChrome;
    square.corner_radius = 0.0f;
    gui.begin_frame(snap(60.0f, 12.0f, false, true, true));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(button(gui, layout, square, "square"));
    layout.end_panel();
    gui.end_frame();

    // An absurd radius exercises the guards: the button clamps it to half
    // the control height, the checkbox's fill inset clamps at 5px so the
    // square fill never pokes outside the arcs (here it draws nothing at
    // all) - and both still toggle on a full click. The pointer aims at
    // the checkbox's box, first row of this fresh frame.
    ChromeTheme round = kDefaultChrome;
    round.corner_radius = 100.0f;
    bool value = false;
    gui.begin_frame(snap(4.0f, 4.0f, false, true, true));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(checkbox(gui, layout, round, "marked", value));
    layout.end_panel();
    gui.end_frame();
    CHECK(value == true);
}

// --- gui tabs + scroll regions (Phase 9) ---------------------------------------------

TEST_CASE("tab_bar: a click moves the selection; the strip is one line") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;
    static const char* kTabs[] = {"alpha", "beta", "gamma"};
    int active = 0;

    // Tabs of 60 px on a 24-tall line: alpha (0,0,60,24), beta (60,0,..),
    // gamma (120,0,..). A same-frame click on beta's rect selects it and
    // reports exactly once.
    gui.begin_frame(snap(90.0f, 12.0f, false, true, true));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(tab_bar(gui, layout, kDefaultChrome, "views", kTabs, 3, active,
                  60.0f));
    layout.end_panel();
    gui.end_frame();
    CHECK(active == 1);

    // Clicking the already-selected tab: no move, no report.
    gui.begin_frame(snap(90.0f, 12.0f, false, true, true));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(!tab_bar(gui, layout, kDefaultChrome, "views", kTabs, 3, active,
                   60.0f));
    layout.end_panel();
    gui.end_frame();
    CHECK(active == 1);

    // The strip occupied ONE line: the widget placed after it starts
    // below the 24-tall tabs, not 72 down.
    gui.begin_frame(snap(400.0f, 400.0f));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    (void)tab_bar(gui, layout, kDefaultChrome, "views", kTabs, 3, active,
                  60.0f);
    const Rect after = layout.next(100.0f, 10.0f);
    CHECK(after.y == 24.0f);
    layout.end_panel();
    gui.end_frame();

    // A two-frame click back onto alpha: press frame selects nothing...
    gui.begin_frame(snap(30.0f, 12.0f, true, true));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(!tab_bar(gui, layout, kDefaultChrome, "views", kTabs, 3, active,
                   60.0f));
    layout.end_panel();
    gui.end_frame();
    CHECK(active == 1);
    // ...release frame completes it.
    gui.begin_frame(snap(30.0f, 12.0f, false, false, true));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(tab_bar(gui, layout, kDefaultChrome, "views", kTabs, 3, active,
                  60.0f));
    layout.end_panel();
    gui.end_frame();
    CHECK(active == 0);

    // An out-of-range active selects nothing - until a click brings it
    // back in range.
    active = 7;
    gui.begin_frame(snap(150.0f, 12.0f, false, true, true));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(tab_bar(gui, layout, kDefaultChrome, "views", kTabs, 3, active,
                  60.0f));
    layout.end_panel();
    gui.end_frame();
    CHECK(active == 2);

    // count <= 0: nothing placed, nothing selected, the state untouched.
    int none = 5;
    gui.begin_frame(snap(400.0f, 400.0f));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(!tab_bar(gui, layout, kDefaultChrome, "empty", nullptr, 0, none));
    const Rect empty_after = layout.next(10.0f, 10.0f);
    CHECK(empty_after.y == 0.0f);  // the strip claimed no line
    layout.end_panel();
    gui.end_frame();
    CHECK(none == 5);
}

TEST_CASE("tab_bar: index scopes keep duplicate tab labels distinct") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;
    static const char* kSame[] = {"tab", "tab", "tab"};
    int active = 0;

    // Three tabs with ONE label: the index scopes are what separate them.
    // During the press frame the pressed tab's id is active - the
    // probe-derived id of views;2;tab, which collided ids (all three
    // hashing to views;0;tab) could never produce.
    IdStack probe;
    probe.push("views");
    probe.push(2);
    const WidgetId tab_two = probe.id("tab");

    gui.begin_frame(snap(150.0f, 12.0f, true, true));  // press on gamma
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(!tab_bar(gui, layout, kDefaultChrome, "views", kSame, 3, active,
                   60.0f));
    layout.end_panel();
    CHECK(gui.active_id() == tab_two);
    gui.end_frame();

    // Release on the same tab: the click lands on gamma only.
    gui.begin_frame(snap(150.0f, 12.0f, false, false, true));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(tab_bar(gui, layout, kDefaultChrome, "views", kSame, 3, active,
                  60.0f));
    layout.end_panel();
    gui.end_frame();
    CHECK(active == 2);
}

TEST_CASE("scroll: the wheel moves the flow - clamps at both ends and persists") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;

    // One frame's pass over a 200x100 scroll panel (padding 12 → the
    // viewport is (12, 12, 176, 76)) with `rows` 24-tall buttons, all
    // sharing one label under per-index id scopes - the loop-children
    // discipline. Returns the viewport and the row a full click landed
    // on (-1 = none); headless, the draws drop and only layout,
    // hit-testing and the persistent scroll state act.
    const auto pass = [&](const InputFrame& frame, int rows) {
        gui.begin_frame(frame);
        const Rect view = begin_scroll_panel(
            gui, layout, kDefaultChrome, "list", {0.0f, 0.0f, 200.0f, 100.0f});
        int clicked = -1;
        for (int i = 0; i < rows; ++i) {
            gui.ids().push(i);
            if (button(gui, layout, kDefaultChrome, "row", 120.0f)) {
                clicked = i;
            }
            gui.ids().pop();
        }
        end_scroll_panel(gui, layout, kDefaultChrome);
        gui.end_frame();
        return std::pair{view, clicked};
    };

    // Ten rows = 240 px of content; the scroll range is 240 - 76 = 164.
    // Two click snapshots recur through the frames: a full click at the
    // unscrolled row 0's center, and one at row 3's center at offset 32.
    const InputFrame click_origin = snap(60.0f, 24.0f, false, true, true);
    const InputFrame click_row3 = snap(60.0f, 64.0f, false, true, true);

    // Frame 1: a wheel-down unit aims to scroll, but a fresh panel has no
    // measured range yet - the immediate-mode first frame cannot scroll.
    // The end of this frame measures the range for the next.
    InputFrame f = snap(100.0f, 50.0f);  // pointer inside the viewport
    f.scroll_dy = -1.0f;
    {
        const auto [view, clicked] = pass(f, 10);
        CHECK(view.x == 12.0f);  // the viewport is unshifted, always
        CHECK(view.y == 12.0f);
        CHECK(view.width == 176.0f);
        CHECK(view.height == 76.0f);
        CHECK(clicked == -1);
    }

    // Frame 2: a click at the UNSCROLLED row 0 proves the frame-1 wheel
    // moved nothing (had it, row 0 would sit at y -20..4 and this click
    // would land on row 1).
    {
        const auto [view, clicked] = pass(click_origin, 10);
        CHECK(view.y == 12.0f);
        CHECK(clicked == 0);
    }

    // Frame 3: a wheel-down unit now moves the flow up 32 px
    // (kDefaultChrome.scroll_speed) - and row 3, laid out at y 52..76,
    // takes the same frame's click at its scrolled position.
    f = click_row3;
    f.scroll_dy = -1.0f;
    {
        const auto [view, clicked] = pass(f, 10);
        CHECK(view.y == 12.0f);  // the viewport never shifts with the scroll
        CHECK(clicked == 3);
    }

    // Frame 4: no wheel - the offset persisted across the frame boundary.
    {
        const auto [view, clicked] = pass(click_row3, 10);
        CHECK(view.y == 12.0f);
        CHECK(clicked == 3);
    }

    // Frame 5: a huge wheel-up clamps at the top (offset 0); row 0 is
    // back at the unscrolled origin and takes the click.
    f = click_origin;
    f.scroll_dy = 100.0f;
    {
        const auto [view, clicked] = pass(f, 10);
        CHECK(view.height == 76.0f);  // the viewport is still the window
        CHECK(clicked == 0);
    }

    // Frame 6: a huge wheel-down clamps at the bottom (offset 164): row 9
    // spans y 64..88, its bottom edge EXACTLY the viewport's bottom.
    f = snap(60.0f, 87.0f, false, true, true);
    f.scroll_dy = -100.0f;
    {
        const auto [view, clicked] = pass(f, 10);
        CHECK(view.y == 12.0f);
        CHECK(clicked == 9);
    }

    // Frame 7: idle - the offset held at the clamp; a click below row 9's
    // bottom edge hits nothing at all.
    {
        const auto [view, clicked] =
            pass(snap(60.0f, 90.0f, false, true, true), 10);
        CHECK(view.y == 12.0f);
        CHECK(clicked == -1);
    }

    // Frames 8-9: the content shrinks to two rows (48 px - shorter than
    // the viewport). Frame 8 is the documented one-frame lag: the offset
    // still holds 164 against last frame's range, so the two rows flow far
    // above and the click at the unscrolled origin hits nothing...
    {
        const auto [view, clicked] = pass(click_origin, 2);
        CHECK(view.height == 76.0f);
        CHECK(clicked == -1);
    }
    // ...frame 9 clamps back to 0 and row 0 takes the click.
    {
        const auto [view, clicked] = pass(click_origin, 2);
        CHECK(view.height == 76.0f);
        CHECK(clicked == 0);
    }
}

TEST_CASE("scroll: two regions keep independent offsets") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;

    // Two side-by-side panels, five 24-tall rows each (120 px of content,
    // range 120 - 76 = 44). Returns which row a click landed on per panel.
    const auto pass = [&](const InputFrame& frame, int& a_click, int& b_click) {
        gui.begin_frame(frame);
        (void)begin_scroll_panel(gui, layout, kDefaultChrome, "left",
                                 {0.0f, 0.0f, 120.0f, 100.0f});
        a_click = -1;
        for (int i = 0; i < 5; ++i) {
            gui.ids().push(i);
            if (button(gui, layout, kDefaultChrome, "row", 100.0f)) {
                a_click = i;
            }
            gui.ids().pop();
        }
        end_scroll_panel(gui, layout, kDefaultChrome);
        (void)begin_scroll_panel(gui, layout, kDefaultChrome, "right",
                                 {140.0f, 0.0f, 120.0f, 100.0f});
        b_click = -1;
        for (int i = 0; i < 5; ++i) {
            gui.ids().push(i);
            if (button(gui, layout, kDefaultChrome, "row", 100.0f)) {
                b_click = i;
            }
            gui.ids().pop();
        }
        end_scroll_panel(gui, layout, kDefaultChrome);
        gui.end_frame();
    };

    // Frame 1: idle - both panels measure their range this frame.
    int a = -2, b = -2;
    pass(snap(400.0f, 400.0f), a, b);
    CHECK(a == -1);
    CHECK(b == -1);

    // Frame 2: the pointer sits inside the LEFT viewport only; its wheel
    // scrolls the left panel 32 px and must not touch the right one.
    InputFrame f = snap(60.0f, 50.0f);
    f.scroll_dy = -1.0f;
    pass(f, a, b);
    CHECK(a == -1);
    CHECK(b == -1);

    // Frame 3: the left panel's row 3 (y 52..76 at offset 32) takes the
    // click - the right panel would put row 3 at y 84..108.
    pass(snap(60.0f, 64.0f, false, true, true), a, b);
    CHECK(a == 3);
    CHECK(b == -1);

    // Frame 4: the RIGHT panel's row 0 still sits at its unscrolled y
    // (12..36) - had it scrolled 32, this click (row 0's spot) would land
    // on row 1 (y 4..28) instead.
    pass(snap(200.0f, 24.0f, false, true, true), a, b);
    CHECK(a == -1);
    CHECK(b == 0);
}

TEST_CASE("scroll: nests inside a panel and measures its own flow") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;
    const ChromeTheme& theme = kDefaultChrome;

    // The outer panel flows a header, then a nested scroll region, then
    // resumes - the scroll panel pushes on top of the outer panel's
    // pushes, and its extent must measure ITS rows, not the outer's.
    const auto pass = [&](const InputFrame& frame, int& clicked) {
        gui.begin_frame(frame);
        (void)begin_panel(gui, layout, theme, "page",
                          {0.0f, 0.0f, 200.0f, 200.0f});
        (void)button(gui, layout, theme, "header", 120.0f);  // at (12, 12)
        (void)begin_scroll_panel(gui, layout, theme, "list",
                                 {12.0f, 12.0f, 100.0f, 100.0f});
        clicked = -1;
        for (int i = 0; i < 5; ++i) {
            gui.ids().push(i);
            if (button(gui, layout, theme, "row", 120.0f)) {
                clicked = i;
            }
            gui.ids().pop();
        }
        end_scroll_panel(gui, layout, theme);
        // The outer cursor resumes below the header, parked while the
        // scroll region flowed its own rows.
        const Rect after = layout.next(50.0f, 10.0f);
        CHECK(after.y == 36.0f);
        end_panel(gui, layout);
        gui.end_frame();
    };

    // The depths stack: the scroll region's scope rides on the outer
    // panel's, and each end pops exactly its own push.
    gui.begin_frame(snap(300.0f, 300.0f));
    (void)begin_panel(gui, layout, theme, "page", {0.0f, 0.0f, 200.0f, 200.0f});
    CHECK(gui.ids().depth() == 1);
    (void)begin_scroll_panel(gui, layout, theme, "list",
                             {12.0f, 12.0f, 100.0f, 100.0f});
    CHECK(gui.ids().depth() == 2);  // page;list - children id under both
    end_scroll_panel(gui, layout, theme);
    CHECK(gui.ids().depth() == 1);
    end_panel(gui, layout);
    CHECK(gui.ids().depth() == 0);
    gui.end_frame();

    // An idle pass: five rows measure the inner range - 120 px of content
    // against the viewport (24, 24, 76, 76) => 44.
    int clicked = -2;
    pass(snap(300.0f, 300.0f), clicked);
    CHECK(clicked == -1);

    // A wheel unit scrolls the inner region 32 - row 3, laid out at y
    // 64..88, takes the click; the header, laid out by the OUTER panel,
    // never moved.
    InputFrame f = snap(60.0f, 76.0f, false, true, true);
    f.scroll_dy = -1.0f;
    pass(f, clicked);
    CHECK(clicked == 3);
}

TEST_CASE("scroll: a bounds-too-small viewport is empty and inert") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;

    // Bounds smaller than the padding: the viewport clamps empty (never
    // backwards), an empty rect contains no pointer, so the wheel has
    // nothing to gate on - and the pushes still balance.
    gui.begin_frame(snap(50.0f, 50.0f));
    const Rect view = begin_scroll_panel(gui, layout, kDefaultChrome, "tiny",
                                         {0.0f, 0.0f, 4.0f, 4.0f});
    CHECK(view.x == 12.0f);  // the inset origin stays honest
    CHECK(view.y == 12.0f);
    CHECK(view.width == 0.0f);
    CHECK(view.height == 0.0f);
    CHECK(gui.ids().depth() == 1);
    end_scroll_panel(gui, layout, kDefaultChrome);
    CHECK(gui.ids().depth() == 0);
    CHECK(layout.depth() == 0);
    gui.end_frame();

    // An end without its begin decays to harmless no-ops, like every
    // other underflow pop in the module.
    gui.begin_frame(snap(50.0f, 50.0f));
    end_scroll_panel(gui, layout, kDefaultChrome);
    CHECK(gui.ids().depth() == 0);
    CHECK(layout.depth() == 0);
    gui.end_frame();
}

// --- scroll by touch-drag (the threshold cancels the press) ------------------------

TEST_CASE("scroll: a touch drag scrolls 1:1 and cancels the press it started on") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;

    // The drag seam, end to end: 10 rows of 24 px in a 200x100 panel
    // (padding 12 - the viewport is (12, 12, 176, 76), rows flow from
    // y 12-offset). A probe contact (99) taps a fixed screen point each
    // frame: the row IT clicks tells the offset, without reading the
    // side table (ScrollState is the lib's private type; the clicks ARE
    // the observable).
    const auto pass = [&](const InputFrame& frame) {
        gui.begin_frame(frame);
        (void)begin_scroll_panel(gui, layout, kDefaultChrome, "list",
                                 {0.0f, 0.0f, 200.0f, 100.0f});
        int clicked = -1;
        for (int i = 0; i < 10; ++i) {
            gui.ids().push(i);
            if (button(gui, layout, kDefaultChrome, "row", 120.0f)) {
                clicked = i;
            }
            gui.ids().pop();
        }
        end_scroll_panel(gui, layout, kDefaultChrome);
        gui.end_frame();
        return clicked;
    };
    const auto row_id = [](int row) {
        IdStack ids;
        ids.push("list");
        ids.push(row);
        return ids.id("row");
    };

    // Frame 1: finger 7 presses row 2 (y 60..84). The child claims the
    // press - the tap is the CHILD's so far, the panel only watches.
    CHECK(pass(touch_frame({{7, 60.0f, 60.0f, /*pressed=*/true}})) == -1);
    CHECK(gui.active_id(PointerId{7}) == row_id(2));

    // Frame 2: 20 px up - past the 8 px threshold. The crossing frame
    // cancels the child's press (finger 7 will never click now) and
    // applies NO jump: the offset is still 0, so the probe tap at
    // (60, 24) still lands on row 0.
    CHECK(pass(touch_frame({{7, 60.0f, 40.0f},
                            {99, 60.0f, 24.0f, true, true}})) == 0);
    CHECK(gui.active_id(PointerId{7}) == kInvalidWidgetId);  // the scroll owns the finger

    // Frame 3: 30 px more of drag - the offset follows 1:1. At offset 30
    // row 1 spans y 6..30, so the same probe point now clicks row 1.
    CHECK(pass(touch_frame({{7, 60.0f, 10.0f},
                            {99, 60.0f, 24.0f, true, true}})) == 1);

    // Frame 4: finger 7 lifts at (60, 10) - INSIDE row 1's scrolled rect
    // (y 6..30). The cancelled press means its release clicks nothing.
    CHECK(pass(touch_frame({{7, 60.0f, 10.0f, false, true}})) == -1);

    // Frame 5: the offset held after the lift; the same probe still hits
    // row 1 - the content stayed put.
    CHECK(pass(touch_frame({{99, 60.0f, 24.0f, true, true}})) == 1);
}

TEST_CASE("scroll: a tap inside the threshold stays the child's click") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;

    const auto pass = [&](const InputFrame& frame) {
        gui.begin_frame(frame);
        (void)begin_scroll_panel(gui, layout, kDefaultChrome, "list",
                                 {0.0f, 0.0f, 200.0f, 100.0f});
        int clicked = -1;
        for (int i = 0; i < 10; ++i) {
            gui.ids().push(i);
            if (button(gui, layout, kDefaultChrome, "row", 120.0f)) {
                clicked = i;
            }
            gui.ids().pop();
        }
        end_scroll_panel(gui, layout, kDefaultChrome);
        gui.end_frame();
        return clicked;
    };
    const auto row_id = [](int row) {
        IdStack ids;
        ids.push("list");
        ids.push(row);
        return ids.id("row");
    };

    // Press row 0, wobble 4 px - under the threshold, the press stays
    // the child's and the offset never moves.
    CHECK(pass(touch_frame({{5, 60.0f, 24.0f, /*pressed=*/true}})) == -1);
    CHECK(gui.active_id(PointerId{5}) == row_id(0));
    CHECK(pass(touch_frame({{5, 60.0f, 20.0f}})) == -1);
    CHECK(gui.active_id(PointerId{5}) == row_id(0));

    // Lift back inside row 0 (y 12..36): a plain click - the tap was a
    // tap, never a drag.
    CHECK(pass(touch_frame({{5, 60.0f, 20.0f, false, true}})) == 0);
}

TEST_CASE("scroll: the bridged finger's drag cancels mouse and touch both") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;

    // Hand-built frames, the mouse NOT parked: the first contact bridges
    // and the mouse mirrors it - the real Android shape, where a dragging
    // finger is two pointers at once. Without cancel_pointer's mirror
    // rule, the mouse's copy of the press would survive the cancel and
    // click the row on release.
    const auto pass = [&](const InputFrame& frame) {
        gui.begin_frame(frame);
        (void)begin_scroll_panel(gui, layout, kDefaultChrome, "list",
                                 {0.0f, 0.0f, 200.0f, 100.0f});
        int clicked = -1;
        for (int i = 0; i < 10; ++i) {
            gui.ids().push(i);
            if (button(gui, layout, kDefaultChrome, "row", 120.0f)) {
                clicked = i;
            }
            gui.ids().pop();
        }
        end_scroll_panel(gui, layout, kDefaultChrome);
        gui.end_frame();
        return clicked;
    };
    const auto row_id = [](int row) {
        IdStack ids;
        ids.push("list");
        ids.push(row);
        return ids.id("row");
    };

    // Frame 1: contact 7 presses row 2 - the mouse mirrors the press, so
    // BOTH tracks claim the row (the documented duplicate).
    InputFrame press;
    press.touch_down(PointerId{7}, 60.0f, 60.0f);
    CHECK(pass(press) == -1);
    CHECK(gui.active_id() == row_id(2));  // the mouse's track pressed...
    CHECK(gui.active_id(PointerId{7}) == row_id(2));  // ...and so did the touch's

    // Frame 2: 20 px up - the crossing. cancel_pointer(7) must take the
    // mouse's claim down with the touch's.
    InputFrame cross;
    cross.touch_down(PointerId{7}, 60.0f, 60.0f);  // the entry; bridges again
    cross.touch_move(PointerId{7}, 60.0f, 40.0f);  // the bridge mirrors the position
    cross.touches[0].pressed = false;  // held, not pressed: clear the
    cross.mouse_pressed[0] = false;    // edges both sides of the bridge
    CHECK(pass(cross) == -1);
    CHECK(gui.active_id() == kInvalidWidgetId);  // BOTH claims cancelled
    CHECK(gui.active_id(PointerId{7}) == kInvalidWidgetId);

    // Frame 3: drag back down to the press point - the offset would go
    // negative and clamps at 0, so a probe at (60, 24) still clicks row 0.
    // The probe is a same-frame tap: its press edge STAYS (a click is
    // press-then-release; a bare release edge clicks nothing).
    InputFrame back;
    back.touch_down(PointerId{7}, 60.0f, 60.0f);
    back.touch_down(PointerId{99}, 60.0f, 24.0f);  // the probe (not the bridge - 7 is)
    back.touch_up(PointerId{99}, 60.0f, 24.0f, false);
    back.touches[0].pressed = false;
    back.mouse_pressed[0] = false;
    CHECK(pass(back) == 0);

    // Frame 4: release exactly where it pressed - row 2 still spans
    // y 60..84, so the lift is INSIDE the row. The cancelled press clicks
    // nothing; had the mouse's claim survived, this would click row 2.
    InputFrame lift;
    lift.touch_down(PointerId{7}, 60.0f, 60.0f);  // the entry, back for its lift
    lift.touch_up(PointerId{7}, 60.0f, 60.0f, false);  // mirrors the mouse's release
    lift.touches[0].pressed = false;  // down+up in one frame: the press
    lift.mouse_pressed[0] = false;    // edge is not this frame's fact
    CHECK(pass(lift) == -1);
}

TEST_CASE("scroll: two fingers drag one region, one lift confuses nothing") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;

    const auto pass = [&](const InputFrame& frame) {
        gui.begin_frame(frame);
        (void)begin_scroll_panel(gui, layout, kDefaultChrome, "list",
                                 {0.0f, 0.0f, 200.0f, 100.0f});
        int clicked = -1;
        for (int i = 0; i < 10; ++i) {
            gui.ids().push(i);
            if (button(gui, layout, kDefaultChrome, "row", 120.0f)) {
                clicked = i;
            }
            gui.ids().pop();
        }
        end_scroll_panel(gui, layout, kDefaultChrome);
        gui.end_frame();
        return clicked;
    };

    // Frame 1: two presses inside the viewport, rows 0 and 2.
    CHECK(pass(touch_frame({{1, 60.0f, 30.0f, /*pressed=*/true},
                            {2, 60.0f, 60.0f, /*pressed=*/true}})) == -1);

    // Frame 2: both move 20 px up - both cross, both cancel; no jump.
    CHECK(pass(touch_frame({{1, 60.0f, 10.0f},
                            {2, 60.0f, 40.0f}})) == -1);

    // Frame 3: both drag 10 px more - BOTH deltas apply (offset 20): at
    // offset 20 row 1 spans y 16..40 and the probe at (60, 24) clicks
    // row 1. A single-track drag would sit at offset 10 and click row 0.
    CHECK(pass(touch_frame({{1, 60.0f, 0.0f},
                            {2, 60.0f, 30.0f},
                            {99, 60.0f, 24.0f, true, true}})) == 1);

    // Frame 4: finger 1 lifts; finger 2 drags 10 more (offset 30) - the
    // survivor's bookkeeping is its own. At offset 30 row 1 spans
    // y 6..30 and the probe at (60, 14) clicks row 1 (at offset 20 it
    // would still be row 0).
    CHECK(pass(touch_frame({{1, 60.0f, 0.0f, false, true},
                            {2, 60.0f, 20.0f},
                            {99, 60.0f, 14.0f, true, true}})) == 1);
}

TEST_CASE("scroll: drag_threshold is the theme's metric") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;
    ChromeTheme sticky = kDefaultChrome;
    sticky.drag_threshold = 100.0f;

    const auto pass = [&](const InputFrame& frame) {
        gui.begin_frame(frame);
        (void)begin_scroll_panel(gui, layout, sticky, "list",
                                 {0.0f, 0.0f, 200.0f, 100.0f});
        int clicked = -1;
        for (int i = 0; i < 10; ++i) {
            gui.ids().push(i);
            if (button(gui, layout, sticky, "row", 120.0f)) {
                clicked = i;
            }
            gui.ids().pop();
        }
        end_scroll_panel(gui, layout, sticky);
        gui.end_frame();
        return clicked;
    };
    const auto row_id = [](int row) {
        IdStack ids;
        ids.push("list");
        ids.push(row);
        return ids.id("row");
    };

    // A 20 px excursion - past kDefaultChrome.drag_threshold, well under
    // this theme's: the press stays the child's, never cancelled.
    CHECK(pass(touch_frame({{5, 60.0f, 24.0f, /*pressed=*/true}})) == -1);
    CHECK(gui.active_id(PointerId{5}) == row_id(0));
    CHECK(pass(touch_frame({{5, 60.0f, 4.0f}})) == -1);
    CHECK(gui.active_id(PointerId{5}) == row_id(0));

    // Lift inside row 0 (y 12..36 - the offset never moved, or this
    // release would fall outside the shifted rect): a plain click.
    CHECK(pass(touch_frame({{5, 60.0f, 30.0f, false, true}})) == 0);
}

TEST_CASE("r2d free functions route through set_active") {
    using namespace ws::ui::r2d;
    // The seam the gui widgets draw through: the free functions are the
    // public API, set_active is the app.cpp install point (and this
    // test's). Everything else tests the members directly, so this is
    // the one case that would catch a broken g_active hand-off.
    Renderer2D r;
    struct ActiveRenderer {
        explicit ActiveRenderer(Renderer2D* renderer) { set_active(renderer); }
        ~ActiveRenderer() { set_active(nullptr); }
    };
    const ActiveRenderer active{&r};  // RAII: null again on ANY exit
    (void)active;

    r.frame_begin(800.0f, 600.0f);
    REQUIRE(frame_size().width == 800.0f);  // the free getter routes to r
    begin();
    draw_quad(0.0f, 0.0f, 10.0f, 10.0f, rgba(255, 255, 255));
    end();
    r.frame_end();
    REQUIRE(r.batch().chunk_count() == 1);
    REQUIRE(r.batch().vertices().size() == 4);
}

// --- fonts + text ----------------------------------------------------------------
//
// Two halves, like the rest of r2d: the CPU half (utf8 decode, bake,
// Font measure) needs no renderer; the draw half runs headless against a
// synthetic page - a glyph IS a Sprite, so draw_text is draw_sprite with
// a pen loop. The one host dependency is the system face, resolved
// through this section's ONE path constant (never inlined at a call
// site): the bake test asserts STRUCTURE, not exact Segoe metrics, so
// any real face keeps it green.

namespace {

constexpr const char* kDefaultTestFontPath = "C:/Windows/Fonts/segoeui.ttf";

// A hand-built font for the draw tests: 'A' and 'B' advance 10 and 5 with
// known offsets and sprites on a synthetic page; the space advances 3
// without a bitmap. Every other codepoint is baked but BLANK (advance 0,
// no sprite) - which is not the same as unbaked: blank still answers
// glyph(), draws nothing and advances nothing, while unbaked (outside
// ASCII 32..126) skips entirely.
ws::ui::r2d::Font ab_font() {
    using ws::ui::r2d::Font;
    Font::Glyph glyphs[95]{};
    glyphs[0].advance = 3.0f;  // ' '
    Font::Glyph& a = glyphs['A' - 32];
    a.advance = 10.0f;
    a.offset_x = 1.0f;
    a.offset_y = 2.0f;
    a.width = 4.0f;
    a.height = 5.0f;
    a.sprite = ws::ui::r2d::Sprite{7, 0};
    Font::Glyph& b = glyphs['B' - 32];
    b.advance = 5.0f;
    b.offset_x = 0.0f;
    b.offset_y = 3.0f;
    b.width = 2.0f;
    b.height = 3.0f;
    b.sprite = ws::ui::r2d::Sprite{7, 1};
    return Font(24.0f, 30.0f, glyphs, 95);
}

}  // namespace

TEST_CASE("utf8 decode covers multibyte codepoints and forgives damage") {
    using ws::ui::r2d::font::utf8_next;
    const char* s = "";
    CHECK(utf8_next(s) == 0u);  // the terminator is the end sentinel
    s = "a";
    CHECK(utf8_next(s) == 'a');
    CHECK(*s == '\0');  // one byte consumed
    s = "\xC3\xA9";  // U+00E9
    CHECK(utf8_next(s) == 0xE9u);
    s = "\xE2\x82\xAC";  // U+20AC
    CHECK(utf8_next(s) == 0x20ACu);
    s = "\xF0\x9F\x99\x82";  // U+1F642
    CHECK(utf8_next(s) == 0x1F642u);

    // Damage never stalls the decode: each bad spot yields U+FFFD and
    // eats exactly one byte.
    s = "\xFF";  // not a lead byte
    CHECK(utf8_next(s) == 0xFFFDu);
    CHECK(*s == '\0');
    s = "a\xC3";  // 'a', then a 2-byte lead cut short by the terminator
    CHECK(utf8_next(s) == 'a');
    CHECK(utf8_next(s) == 0xFFFDu);  // the lead alone is eaten...
    CHECK(utf8_next(s) == 0u);       // ...the NUL still ends the string
    s = "\xC0\x80";  // overlong NUL: rejected, never decoded as U+0000
    CHECK(utf8_next(s) == 0xFFFDu);
    CHECK(*s == '\0');
}

TEST_CASE("font bake is structural - never exact face metrics") {
    const auto baked =
        ws::ui::r2d::font::bake_file(kDefaultTestFontPath, 24.0f);
    if (!baked.ok) {
        MESSAGE("segoeui.ttf not present - bake checks skipped");
        return;
    }
    REQUIRE(baked.glyphs.size() == 95u);  // the whole ASCII range, in order
    CHECK(baked.glyphs.front().codepoint == ' ');
    CHECK(baked.glyphs.back().codepoint == '~');
    CHECK(baked.pixel_height == 24.0f);
    CHECK(baked.line_height > 0.0f);

    // The space: an advance to honor, no bitmap to draw.
    const auto& space = baked.glyphs[' ' - 32];
    CHECK(space.w == 0u);
    CHECK(space.rgba.empty());
    CHECK(space.advance > 0.0f);

    // Shape invariants that hold for any sane face, not Segoe numbers.
    CHECK(baked.glyphs['W' - 32].advance > baked.glyphs['i' - 32].advance);
    const auto& a = baked.glyphs['A' - 32];
    REQUIRE(a.w > 0u);
    REQUIRE(a.h > 0u);
    REQUIRE(a.rgba.size() == static_cast<std::size_t>(a.w) * a.h * 4u);

    // The tint convention: RGB solid white, the coverage mask in alpha -
    // the vertex tint supplies the text color, like the rounded mask.
    bool ink = false;
    for (std::size_t i = 0; i < a.rgba.size() / 4u; ++i) {
        if (a.rgba[i * 4u + 3u] > 0) {
            ink = true;
            CHECK(a.rgba[i * 4u] == 255);
            CHECK(a.rgba[i * 4u + 1u] == 255);
            CHECK(a.rgba[i * 4u + 2u] == 255);
            break;
        }
    }
    CHECK(ink);
}

TEST_CASE("Font measures the baked advance sum and skips the unbaked") {
    using namespace ws::ui::r2d;
    const Font font = ab_font();
    CHECK(font.pixel_height() == 24.0f);
    CHECK(font.line_height() == 30.0f);
    CHECK(!font.empty());

    CHECK(font.measure_text("AB") == 15.0f);    // 10 + 5
    CHECK(font.measure_text("A B") == 18.0f);  // ...plus the space's 3
    // 'z' is baked but blank: present in the table, contributes nothing -
    // NOT the same as the skipped codepoint below.
    CHECK(font.measure_text("AzA") == 20.0f);
    // U+00E9 is outside the bake range: skipped, no advance. (Adjacent
    // literals: a hex escape absorbs following hex-digit characters.)
    CHECK(font.measure_text("A" "\xC3\xA9" "A") == 20.0f);
    CHECK(font.measure_text(nullptr) == 0.0f);
    CHECK(font.measure_text("") == 0.0f);

    // glyph(): in-range always answers - blank stays blank, by data;
    // out-of-range is null.
    const Font::Glyph* a = font.glyph('A');
    REQUIRE(a != nullptr);
    CHECK(a->advance == 10.0f);
    REQUIRE(font.glyph('z') != nullptr);
    CHECK(font.glyph('z')->advance == 0.0f);
    CHECK(font.glyph(31) == nullptr);   // below the bake range
    CHECK(font.glyph(127) == nullptr);  // above it

    // The blank Font (a failed load, the headless fallback): inert, but
    // still answers in-range glyph() with zeroed data.
    const Font blank;
    CHECK(blank.empty());
    CHECK(blank.measure_text("A") == 0.0f);
    REQUIRE(blank.glyph('A') != nullptr);
    CHECK(blank.glyph('A')->advance == 0.0f);
}

TEST_CASE("draw_text records one contiguous glyph run on the font's own page") {
    using namespace ws::ui::r2d;
    Renderer2D r;
    r.register_atlas_page(Texture{7}, {{0.0f, 0.0f, 0.5f, 0.5f, 4, 5},
                                       {0.5f, 0.0f, 1.0f, 0.5f, 2, 3}});
    const Font font = ab_font();

    r.frame_begin(800.0f, 600.0f);
    r.begin_screen();
    const float drawn =
        r.draw_text(100.0f, 40.0f, "A B", font, rgba(235, 238, 242));
    r.end();
    r.frame_end();

    // draw == measure, by construction: draw_text's loop is measure_text's
    // loop plus the quads. The pinned invariant - assert it first.
    REQUIRE(drawn == 18.0f);
    CHECK(font.measure_text("A B") == 18.0f);

    // One contiguous run: both glyphs share the page texture, so both
    // quads land in ONE chunk under ONE texture-table slot - the batching
    // sanity check from the design. Fonts own their pages BY DESIGN: a
    // font's glyphs never share a page with load_atlas sprites (packing
    // is one-shot; a packed atlas never grows), so this test deliberately
    // does NOT assert that text merges with icon sprites. Text batching
    // apart from icons is the contract, not a regression.
    REQUIRE(r.batch().chunk_count() == 1);
    const auto& chunk = r.batch().chunk(0);
    REQUIRE(chunk.texture_count == 1);
    CHECK(chunk.texture_ids[0] == 7u);
    REQUIRE(r.batch().vertices().size() == 8);
    for (const auto& v : r.batch().vertices()) {
        CHECK(v.tex_index == 0.0f);  // one page, one slot
    }
    // 'A': pen 100 + offset (1, 2) -> the quad's TL lands at (101, 42).
    CHECK(r.batch().vertices()[0].pos[0] == 101.0f);
    CHECK(r.batch().vertices()[0].pos[1] == 42.0f);
    // 'B': the pen advanced past 'A' (10) and the space (3) -> 113.
    CHECK(r.batch().vertices()[4].pos[0] == 113.0f);
    CHECK(r.batch().vertices()[4].pos[1] == 43.0f);
}

TEST_CASE("draw_text skips the unbaked and stays inert on empty fonts") {
    using namespace ws::ui::r2d;
    Renderer2D r;
    r.register_atlas_page(Texture{7}, {{0.0f, 0.0f, 1.0f, 1.0f, 4, 5}});
    const Font font = ab_font();

    r.frame_begin(800.0f, 600.0f);
    r.begin_screen();
    // 'z' is baked but blank (no bitmap, no advance); U+00E9 is outside
    // the bake range. Neither draws - and only 'z' even exists in the
    // table. The two 'A's carry the width.
    const float drawn = r.draw_text(50.0f, 20.0f, "Az" "\xC3\xA9" "A", font);
    r.end();
    r.frame_end();
    REQUIRE(drawn == font.measure_text("Az" "\xC3\xA9" "A"));
    REQUIRE(drawn == 20.0f);
    REQUIRE(r.batch().chunk_count() == 1);
    REQUIRE(r.batch().vertices().size() == 8);
    CHECK(r.batch().vertices()[4].pos[0] == 61.0f);  // 'z' advanced nothing

    // A glyph whose page failed to upload (Sprite{} = page 0) keeps its
    // advance but drops the quad: a white box is never useful text. The
    // draw==measure identity survives even this font.
    Font::Glyph degraded[95]{};
    degraded['A' - 32].advance = 10.0f;
    degraded['A' - 32].width = 4.0f;
    degraded['A' - 32].height = 5.0f;  // sprite stays Sprite{} (page 0)
    const Font degraded_font(24.0f, 30.0f, degraded, 95);
    r.frame_begin(800.0f, 600.0f);
    r.begin_screen();
    CHECK(r.draw_text(0.0f, 0.0f, "AA", degraded_font) == 20.0f);
    r.end();
    r.frame_end();
    CHECK(degraded_font.measure_text("AA") == 20.0f);
    CHECK(r.batch().chunk_count() == 0);  // nothing recorded at all

    // An empty font and a null string are inert; the default color
    // argument needs no special-casing to exercise.
    r.frame_begin(800.0f, 600.0f);
    r.begin_screen();
    CHECK(r.draw_text(0.0f, 0.0f, "text", Font{}) == 0.0f);
    CHECK(r.draw_text(0.0f, 0.0f, nullptr, font) == 0.0f);
    CHECK(r.draw_text(0.0f, 0.0f, "A", font) == 10.0f);  // default color
    r.end();
    r.frame_end();
    REQUIRE(r.batch().vertices().size() == 4);  // just the one 'A'
}

TEST_CASE("gui labels draw through the theme font") {
    using namespace ws::ui::gui;
    // The stock chrome stays fontless: byte-for-byte the pre-font look.
    CHECK(kDefaultChrome.font == nullptr);

    ws::ui::r2d::Renderer2D r;
    // Point the r2d free functions (the widgets draw through them) at
    // this test's renderer for exactly this scope - and back to null on
    // ANY exit: a failed REQUIRE throws, and a g_active left dangling at
    // a destroyed stack renderer would poison every later test.
    struct ActiveRenderer {
        explicit ActiveRenderer(ws::ui::r2d::Renderer2D* renderer) {
            ws::ui::r2d::set_active(renderer);
        }
        ~ActiveRenderer() { ws::ui::r2d::set_active(nullptr); }
    };
    const ActiveRenderer active{&r};
    (void)active;  // a scope guard: its lifetime is the point

    r.register_atlas_page(ws::ui::r2d::Texture{7},
                          {{0.0f, 0.0f, 0.5f, 0.5f, 4, 5},
                           {0.5f, 0.0f, 1.0f, 0.5f, 2, 3}});
    const ws::ui::r2d::Font font = ab_font();
    ChromeTheme theme = kDefaultChrome;
    theme.font = &font;  // the theme points; it never owns

    // Widgets flow inside a panel - that is the production shape. A bare
    // LayoutCursor hands next() an empty rect (drop-safe, by design), and
    // an empty-rect button has nothing to draw: no face, no label.
    r.frame_begin(800.0f, 600.0f);
    r.begin_screen();
    GuiContext gui;
    LayoutCursor layout;
    gui.begin_frame(snap(500.0f, 500.0f));
    (void)begin_panel(gui, layout, theme, "root",
                      {0.0f, 0.0f, 200.0f, 100.0f});
    button(gui, layout, theme, "AB", 120.0f);  // advance 15, fits the face
    end_panel(gui, layout);
    gui.end_frame();
    r.end();
    r.frame_end();

    // The panel's chrome recorded before its clip push (full clip), its
    // button after it (under the panel's content clip) - the clip change
    // splits a chunk in record order, so the frame replays as two runs:
    // chrome, then widgets.
    REQUIRE(r.batch().chunk_count() == 2);
    const auto& widgets = r.batch().chunk(1);
    REQUIRE(widgets.texture_count == 2);  // the white face + the font's page
    int glyph_slot = -1;
    for (std::uint32_t s = 0; s < widgets.texture_count; ++s) {
        if (widgets.texture_ids[s] == 7u) {
            glyph_slot = static_cast<int>(s);
        }
    }
    // Painter order inside the run: the face recorded first, so white
    // owns slot 0 and the font's page slot 1. The two-slot table IS the
    // "fonts own their pages" contract: same run (painter order), two
    // textures - glyphs do NOT merge with icon sprites by design.
    REQUIRE(glyph_slot == 1);
    int glyph_verts = 0;
    for (const auto& v : r.batch().vertices()) {
        if (static_cast<int>(v.tex_index) == glyph_slot) {
            ++glyph_verts;
        }
    }
    CHECK(glyph_verts == 8);  // 'A' and 'B': two glyph quads, nothing else
}
