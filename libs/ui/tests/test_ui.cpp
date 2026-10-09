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
    core.open_segment(default_vp());

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
    core.open_segment(default_vp());

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
    core.open_segment(default_vp());

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

TEST_CASE("global texture ids skip the chunk table (bindless mode)") {
    BatchCore core;
    core.begin_frame();
    // The bindless tier's shader table IS the texture registry: tex_index
    // carries the global id verbatim, so texture variety never splits a
    // chunk and the chunk's own table stays empty.
    core.use_global_texture_ids(true);
    core.open_segment(default_vp());

    const std::uint32_t unique_textures = 33;  // one past the CHUNK table
    for (std::uint32_t id = 0; id < unique_textures; ++id) {
        REQUIRE(push_test_quad(core, id));
    }

    REQUIRE(core.chunk_count() == 1);  // no splits on texture variety
    const auto& chunk = core.chunk(0);
    CHECK(chunk.texture_count == 0);  // the chunk's own table stays empty
    CHECK(chunk.quad_count == unique_textures);
    // tex_index is the GLOBAL registry id, verbatim - no slot remap.
    CHECK(core.vertices()[3].tex_index == 0.0f);
    CHECK(core.vertices()[7].tex_index == 1.0f);
    CHECK(core.vertices().back().tex_index ==
          static_cast<float>(unique_textures - 1));
}

TEST_CASE("open_segment closes the current chunk and carries the new VP") {
    BatchCore core;
    core.begin_frame();
    core.open_segment(default_vp());
    REQUIRE(push_test_quad(core, 5u));

    const auto zoomed = ws::ui::r2d::batch::camera_vp(
        ws::ui::r2d::Camera2D{0.0f, 0.0f, 0.0f, 2.0f}, 100.0f, 100.0f);
    core.open_segment(zoomed);
    REQUIRE(push_test_quad(core, 5u));

    REQUIRE(core.chunk_count() == 2);
    CHECK(core.chunk(0).quad_count == 1);
    CHECK(core.chunk(1).quad_count == 1);
    CHECK(core.chunk(1).vertex_offset == 4);  // 4 vertices before it
    CHECK(core.chunk(1).vp.m[0] == doctest::Approx(zoomed.m[0]));
}

TEST_CASE("vertex capacity grows instead of dropping (M9)") {
    BatchCore core;
    core.begin_frame();
    core.open_segment(default_vp());

    // One quad past the old hard cap (kInitialVertexCapacity WAS the cap,
    // M5-M8): nothing drops, the vertex array simply grew.
    const auto quads = ws::ui::r2d::batch::kInitialVertexCapacity / 4 + 1;
    for (std::size_t i = 0; i < quads; ++i) {
        if (!push_test_quad(core, 0u)) {
            FAIL("quad ", i, " dropped - capacity must grow, never drop");
        }
    }
    CHECK(core.vertices().size() == quads * 4);
    CHECK(core.chunk_count() == 1);
    CHECK(core.chunk(0).quad_count == quads);
    CHECK(core.dropped_quads() == 0);
}

TEST_CASE("chunk capacity grows instead of dropping (M9)") {
    BatchCore core;
    core.begin_frame();
    // One segment past the old hard cap (kInitialChunkCapacity WAS the
    // cap, M5-M8): every segment opens, every draw lands.
    for (std::uint32_t i = 0;
         i < ws::ui::r2d::batch::kInitialChunkCapacity + 1; ++i) {
        core.open_segment(default_vp());
        REQUIRE(push_test_quad(core, 0u));
    }
    CHECK(core.chunk_count() == ws::ui::r2d::batch::kInitialChunkCapacity + 1);
    CHECK(core.dropped_quads() == 0);

    // The ONE drop that remains (M9): a quad pushed with no open segment
    // - counted, never silent. The renderer warns once per frame.
    core.close_segment();
    CHECK(push_test_quad(core, 0u) == false);
    CHECK(core.dropped_quads() == 1);

    // begin_frame resets the accounting along with everything else.
    core.begin_frame();
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
    core.open_segment(default_vp(), clip);
    REQUIRE(push_test_quad(core, 0u));
    for (int i = 0; i < 4; ++i) {
        CHECK(core.chunk(0).clip[i] == clip[i]);
    }

    // The default argument is the full-frame sentinel: an unclipped segment.
    core.open_segment(default_vp());
    REQUIRE(push_test_quad(core, 0u));
    for (int i = 0; i < 4; ++i) {
        CHECK(core.chunk(1).clip[i] == ws::ui::r2d::batch::kFullClip[i]);
    }

    // A texture-overflow split continues the SAME segment: the fresh chunk
    // inherits both the VP and the clip.
    core.begin_frame();
    core.open_segment(default_vp(), clip);
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
    core.open_segment(default_vp());
    REQUIRE(push_test_quad(core, 0u));
    CHECK(core.chunk(0).depth_test == false);

    // A perspective segment flags its chunks - the flush's per-chunk
    // pipeline bind keys on this bit. The clip can stay the full-frame
    // sentinel while the flag is set: the defaulted parameter keeps the
    // existing callers' calls unchanged.
    core.open_segment(default_vp(), kFullClip, true);
    REQUIRE(push_test_quad(core, 0u));
    CHECK(core.chunk(1).depth_test == true);

    // A texture-overflow split continues the SAME segment: the fresh
    // chunk inherits the depth mode alongside the VP and clip.
    core.begin_frame();
    core.open_segment(default_vp(), kFullClip, true);
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

TEST_CASE("more clip runs than the initial chunk capacity all land (M9)") {
    using namespace ws::ui::r2d;
    Renderer2D r;
    r.frame_begin(800.0f, 600.0f);
    r.begin(Camera2D{});
    // One more distinct clip than the initial reservation: 17 single-draw
    // runs, each its own chunk (a clip change always splits). The chunk
    // array grows on demand now - nothing drops.
    for (std::uint32_t i = 0;
         i < batch::kInitialChunkCapacity + 1; ++i) {
        r.push_clip(1.0f + static_cast<float>(i), 2.0f, 10.0f, 10.0f);
        r.draw_quad(0.0f, 0.0f, 1.0f, 1.0f, rgba(255, 255, 255));
        r.pop_clip();
    }
    r.end();
    r.frame_end();

    // All 17 runs recorded, replayed in exact record order, zero drops.
    REQUIRE(r.batch().chunk_count() == batch::kInitialChunkCapacity + 1);
    CHECK(r.batch().dropped_quads() == 0);
    REQUIRE(r.batch().vertices().size() ==
            std::size_t(batch::kInitialChunkCapacity + 1) * 4);
    for (std::uint32_t i = 0; i < batch::kInitialChunkCapacity + 1; ++i) {
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
    frame.mouse_present = true;
    frame.mouse_down[0] = true;
    frame.mouse_pressed[0] = true;
    frame.mouse_released[1] = true;
    frame.scroll_dx = 1.5f;
    frame.scroll_dy = -2.5f;
    frame.key_down['D'] = true;
    frame.key_pressed['D'] = true;
    frame.text_input.push_back(U'w');

    frame.next_frame();

    // Held state and position survive the frame boundary... presence is
    // held state too - the platform re-establishes it per event, and a
    // frame boundary must not flicker the mouse out of the pointer list.
    CHECK(frame.mouse_x == 100.0f);
    CHECK(frame.mouse_y == 50.0f);
    CHECK(frame.mouse_present == true);
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

TEST_CASE("InputFrame: the frame clock survives next_frame - the shell's write is the refresh") {
    using namespace ws::ui::gui;
    InputFrame frame;
    CHECK(frame.dt_seconds == 0.0f);  // a hand-built frame starts at 0

    frame.dt_seconds = 1.0f / 120.0f;
    frame.next_frame();
    // next_frame clears EDGES; the clock is the SHELL's to refresh (its
    // next write, once per pump) - a frame boundary between writes must
    // not zero it.
    CHECK(frame.dt_seconds == doctest::Approx(1.0f / 120.0f));

    // And the context hands the shell's clock to widgets whole:
    // begin_frame copies the snapshot, dt included.
    GuiContext gui;
    frame.dt_seconds = 0.25f;
    gui.begin_frame(frame);
    CHECK(gui.input().dt_seconds == doctest::Approx(0.25f));
    gui.end_frame();
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
    CHECK(frame.dt_seconds == 0.0f);  // no shell ran: the clock reads 0
    CHECK(frame.text_input.empty());
    CHECK(frame.touch_count == 0);
    CHECK(frame.bridged_touch() == ws::ui::gui::kNoPointer);
    // An empty frame never saw a mouse: presence defaults false, so the
    // pointer list is EMPTY - the touch-only shape. The mouse joins only
    // when the platform says one exists.
    std::array<ws::ui::gui::PointerView,
               ws::ui::gui::InputFrame::kMaxPointers>
        pointers{};
    CHECK(frame.gather_pointers(pointers) == 0);
    CHECK(pointers[0].id == ws::ui::gui::kNoPointer);
    CHECK(!frame.pointer_pos(ws::ui::gui::kMousePointerId, nullptr, nullptr));
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

TEST_CASE("touch table: a same-frame id reuse keeps the lift's release edge") {
    using namespace ws::ui::gui;
    InputFrame frame;
    CHECK(frame.touch_down(PointerId{4}, 10.0f, 10.0f) == 0);
    frame.touch_up(PointerId{4}, 12.0f, 12.0f, false);
    // The platform hands id 4 to a new contact before next_frame() -
    // Android recycles low ids fast. The lift's edge must survive the
    // arrival: the slot reads pressed+released with down, up-then-down,
    // the flags the gui needs to report the lift while the new contact
    // claims fresh. (The defect: the arrival erased the release.)
    CHECK(frame.touch_down(PointerId{4}, 30.0f, 30.0f) == 0);
    CHECK(frame.touch_count == 1);
    CHECK(frame.touches[0].id == PointerId{4});
    CHECK(frame.touches[0].down);
    CHECK(frame.touches[0].pressed);
    CHECK(frame.touches[0].released);
    CHECK(frame.touches[0].x == 30.0f);  // position is the new contact's:
    CHECK(frame.touches[0].y == 30.0f);  // last-write, like the mouse's

    // The bridge mirrors the reuse order-exactly: the lift mirrored its
    // release, the new contact re-bridges and mirrors a fresh press - and
    // never cleared the mirrored release (the mouse's own rule, which the
    // touch side now matches).
    CHECK(frame.bridged_touch() == PointerId{4});
    CHECK(frame.mouse_down[0]);
    CHECK(frame.mouse_pressed[0]);
    CHECK(frame.mouse_released[0]);

    // The frame boundary clears every edge; the survivor stays.
    frame.next_frame();
    CHECK(frame.touch_count == 1);
    CHECK(frame.touches[0].down);
    CHECK(!frame.touches[0].pressed);
    CHECK(!frame.touches[0].released);
}

TEST_CASE("touch table: a recycled slot carries no stale release into a new contact") {
    using namespace ws::ui::gui;
    InputFrame frame;
    frame.touch_down(PointerId{1}, 0.0f, 0.0f);
    frame.touch_down(PointerId{2}, 1.0f, 1.0f);
    frame.touch_down(PointerId{3}, 2.0f, 2.0f);
    // 1 and 2 lift (2 cancelled); 3 survives. The boundary compacts 3
    // into slot 0 and leaves 2's released+cancelled bytes sitting in
    // slot 1, above the count - the next arrival is created straight on
    // top of them, and must not inherit either edge.
    frame.touch_up(PointerId{1}, 0.0f, 0.0f, false);
    frame.touch_up(PointerId{2}, 1.0f, 1.0f, true);
    frame.next_frame();
    CHECK(frame.touch_count == 1);

    CHECK(frame.touch_down(PointerId{9}, 5.0f, 5.0f) == 1);
    CHECK(frame.touches[1].id == PointerId{9});
    CHECK(frame.touches[1].down);
    CHECK(frame.touches[1].pressed);
    CHECK(!frame.touches[1].released);   // slot 1 held 2's released bytes
    CHECK(!frame.touches[1].cancelled);  // ...and its cancelled bytes
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
    // Either way slot 0 reports whatever the mouse state says - and the
    // desktop shape means a present mouse.
    frame.mouse_x = 12.0f;
    frame.mouse_y = 34.0f;
    frame.mouse_present = true;
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

TEST_CASE("gather_pointers: an absent mouse is not a pointer - touches only") {
    using namespace ws::ui::gui;
    // The touch-only shape: the bridge mirrors the finger into the raw
    // mouse fields, but presence is false, so the mouse claims nothing.
    // Whatever mouse_x/y hold - a finger's position, or the (0,0) default
    // - the pointer list is exactly the touches.
    InputFrame frame;
    frame.touch_down(PointerId{7}, 40.0f, 50.0f);
    CHECK(frame.mouse_x == 40.0f);  // the bridge mirrored the finger...
    CHECK(!frame.mouse_present);    // ...but the platform never saw a mouse

    std::array<PointerView, InputFrame::kMaxPointers> pointers{};
    CHECK(frame.gather_pointers(pointers) == 1);
    CHECK(pointers[0].id == PointerId{7});  // slot 0 is the TOUCH
    CHECK(pointers[0].x == 40.0f);
    CHECK(pointers[0].pressed);
    CHECK(pointers[1].id == kNoPointer);
    float x = 0.0f;
    float y = 0.0f;
    CHECK(!frame.pointer_pos(kMousePointerId, &x, &y));
    CHECK(!frame.pointer_pos(kMousePointerId, nullptr, nullptr));
    CHECK(frame.pointer_pos(PointerId{7}, &x, &y));
    CHECK(x == 40.0f);
    CHECK(y == 50.0f);

    // The mouse joins the list the moment the platform reports it - the
    // Bluetooth-mouse-plugged-in-while-touching shape: the raw fields
    // still mirror the finger (either-writer), and now they count.
    frame.mouse_present = true;
    CHECK(frame.gather_pointers(pointers) == 2);
    CHECK(pointers[0].id == kMousePointerId);
    CHECK(pointers[0].x == 40.0f);  // the finger's mirror, as written
    CHECK(pointers[1].id == PointerId{7});
    CHECK(frame.pointer_pos(kMousePointerId, &x, &y));
    CHECK(x == 40.0f);
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

TEST_CASE("IdStack: label markers split id from display (## ids whole, ### redirects)") {
    using namespace ws::ui::gui;
    IdStack ids;

    // "##": the FULL string ids - the suffix keys the widget's state
    // without ever drawing - so each suffixed spelling is its own
    // widget, distinct from the plain label and from other suffixes.
    CHECK(ids.id("Save##one") != ids.id("Save"));
    CHECK(ids.id("Save##one") != ids.id("Save##two"));
    CHECK(ids.id("Save##one") == ids.id("Save##one"));

    // The display half: the bytes before the first marker (which is
    // also how "###" displays), the label itself when no marker rode
    // it, empty for a null or a marker-only label.
    CHECK(label_display("Save##one") == "Save");
    CHECK(label_display("Pause###pause") == "Pause");
    CHECK(label_display("plain") == "plain");
    CHECK(label_display("###only") == "");
    CHECK(label_display("##only") == "");
    CHECK(label_display(nullptr) == "");

    // "###": the id REDIRECTS to the marker onward - one identity
    // behind two displays, so a widget can change its label without
    // orphaning its state.
    CHECK(ids.id("Play###pause") == ids.id("Pause###pause"));
    CHECK(ids.id("Play###pause") != ids.id("Play###resume"));
    CHECK(ids.id("Play###pause") != ids.id("Play"));
    // ...and it stays a scoped id like any other.
    const WidgetId at_root = ids.id("Play###pause");
    ids.push("panel");
    CHECK(ids.id("Play###pause") != at_root);
    ids.pop();
    CHECK(ids.id("Play###pause") == at_root);
}

TEST_CASE("IdStack: internal_id is the reserved family no label can spell") {
    using namespace ws::ui::gui;
    IdStack ids;

    // The gui's own state keys never collide with the user widgets that
    // happen to share their spelling - the families mix no hash stream.
    CHECK(ids.internal_id("scroll") != ids.id("scroll"));
    CHECK(ids.internal_id("scroll") != ids.id("##scroll"));
    CHECK(ids.internal_id("scroll") != ids.id("###scroll"));
    CHECK(ids.internal_id("scroll") != ids.internal_id("drag"));
    CHECK(ids.internal_id(nullptr) != ids.id(""));

    // Scoped like id(): re-derivation is stable, the scope moves it.
    const WidgetId root_key = ids.internal_id("scroll");
    ids.push("panel");
    const WidgetId scoped_key = ids.internal_id("scroll");
    CHECK(scoped_key != root_key);
    CHECK(ids.internal_id("scroll") == scoped_key);
    ids.pop();
    CHECK(ids.internal_id("scroll") == root_key);
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
// optional press/release edges set. The mouse is PRESENT: snap() models a
// device the platform has seen, so every pre-presence test keeps today's
// meaning (the flag defaults false - see the mouse_present block below).
ws::ui::gui::InputFrame snap(float x, float y, bool down = false,
                             bool pressed = false, bool released = false) {
    ws::ui::gui::InputFrame frame;
    frame.mouse_x = x;
    frame.mouse_y = y;
    frame.mouse_present = true;
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
// the mouse by design (the primary-touch -> mouse mirror), but the bridge
// writes raw fields only - mouse_present stays false, so the gui's
// pointer list is exactly these contacts, in order: the touch-only shape.
// The parked mouse fields below are the either-writer rule the Phase A
// gather test set, made inert by the absent flag: they feed raw-field
// readers (ImGui, apps) and can never claim anything.
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

    // Frame 0: the pointer arrives - the claim is live this frame, the
    // settled visual lands next frame (hovered reports last frame's
    // resolved hot, so the first frame on a widget shows nothing yet).
    gui.begin_frame(snap(120.0f, 110.0f));
    (void)gui.hit_test(id, rect);
    CHECK(gui.hot_id() == id);
    CHECK(gui.active_id() == kInvalidWidgetId);
    gui.end_frame();

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
    // Two overlapping buttons; B draws later (on top). A takes the press
    // first and the CARRIED gesture is capture - B cannot take it. (A FRESH
    // press on an overlap is the later widget's to steal - the M10
    // hot/press agreement rule, tested in the overlap suite below.)
    const WidgetId a = ids.id("a");
    const WidgetId b = ids.id("b");
    const Rect rect{0.0f, 0.0f, 100.0f, 100.0f};

    gui.begin_frame(snap(50.0f, 50.0f, true, true));
    (void)gui.hit_test(a, rect);  // A runs first: takes the press
    CHECK(gui.active_id() == a);
    gui.end_frame();

    // B appears on the overlap while the press is HELD (no new edge):
    // capture - B gets nothing, not even hot.
    gui.begin_frame(snap(50.0f, 50.0f, true));
    (void)gui.hit_test(a, rect);  // the owner keeps its own hot
    const HitTest under = gui.hit_test(b, rect);  // B overlaps: no press for B
    CHECK(!under.pressed);
    CHECK(!under.clicked);
    CHECK(gui.hot_id() == a);  // while A owns the press, B cannot be hot
    CHECK(gui.active_id() == a);

    // Even a fresh edge while A owns it changes nothing for B: the
    // gesture belongs to its owner until it ends.
    gui.begin_frame(snap(50.0f, 50.0f, true, true));
    (void)gui.hit_test(a, rect);
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
    CHECK(!hit.hovered);  // the claim is live this frame...
    CHECK(gui.hot_id() == other);
    gui.end_frame();

    // ...and the settled visual lands one frame later, once the frame's
    // resolved hot has become hot_prev.
    gui.begin_frame(snap(50.0f, 50.0f));
    CHECK(gui.hit_test(other, rect).hovered);
    gui.end_frame();
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

TEST_CASE("hit_test: overlapping widgets - only the topmost reports hovered") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const IdStack ids;
    const WidgetId bottom = ids.id("bottom");
    const WidgetId top = ids.id("top");
    const Rect rect{0.0f, 0.0f, 100.0f, 100.0f};

    // Frame 1: the pointer sits in the overlap. The covered widget runs
    // first and claims hot at its call (the live claim is call-ordered,
    // as ever) - the covering widget steals the claim when it runs. The
    // REPORTED hover must not light the covered one up, not even for
    // this covering frame: hovered reads last frame's resolution, so
    // neither shows hover yet and the claim is already decided.
    gui.begin_frame(snap(50.0f, 50.0f));
    const HitTest first_bottom = gui.hit_test(bottom, rect);
    const HitTest first_top = gui.hit_test(top, rect);
    CHECK(!first_bottom.hovered);  // covered: dark even on the covering frame
    CHECK(!first_top.hovered);     // settling: nobody shows hover yet
    CHECK(gui.hot_id() == top);    // ...but the live claim already resolved
    gui.end_frame();

    // Frame 2: settled - only the topmost reports hovered.
    gui.begin_frame(snap(50.0f, 50.0f));
    CHECK(!gui.hit_test(bottom, rect).hovered);
    CHECK(gui.hit_test(top, rect).hovered);
    CHECK(gui.hot_id() == top);
    gui.end_frame();

    // The covering widget vanishes: the covered one's hover is not
    // stranded - with the pointer alone on it, it claims unopposed and
    // settles the frame after.
    gui.begin_frame(snap(50.0f, 50.0f));
    CHECK(!gui.hit_test(bottom, rect).hovered);  // top held last frame's hot
    CHECK(gui.hot_id() == bottom);               // bottom claims unopposed
    gui.end_frame();
    gui.begin_frame(snap(50.0f, 50.0f));
    CHECK(gui.hit_test(bottom, rect).hovered);   // settled on bottom
    gui.end_frame();
}

TEST_CASE("hit_test: a frame past the census's slots still runs") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const Rect rect{0.0f, 0.0f, 100.0f, 100.0f};

    // 600 unique ids hit-tested in one frame: past the 512-slot census.
    // Nothing may crash or corrupt - the ids past the table simply stop
    // being duplicate-checked (warned once per process, visible in the
    // log). This is the degraded-but-alive contract the fixed table
    // promises: no allocation, no silent breakage.
    gui.begin_frame(snap(50.0f, 50.0f));
    for (int i = 0; i < 600; ++i) {
        gui.ids().push(i);
        (void)gui.hit_test(gui.ids().id("w"), rect);
        gui.ids().pop();
    }
    CHECK(gui.hot_id() != kInvalidWidgetId);  // the last claim stands
    gui.end_frame();

    // The next frame is a fresh census - the very same ids hit-test
    // cleanly again, and a widget can claim hot after the crush.
    gui.begin_frame(snap(50.0f, 50.0f));
    gui.ids().push(0);
    const WidgetId again_id = gui.ids().id("w");
    const HitTest again = gui.hit_test(again_id, rect);
    gui.ids().pop();
    CHECK(gui.hot_id() == again_id);
    CHECK(!again.hovered);  // the claim is live; the visual settles next frame
    gui.end_frame();
}

// --- layer scopes (begin_layer/end_layer) -------------------------------------------

TEST_CASE("layers: a popup wins the pointer over base, whatever the record order") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const IdStack ids;
    const WidgetId popup_w = ids.id("popup");
    const WidgetId base_w = ids.id("base");
    const Rect rect{0.0f, 0.0f, 100.0f, 100.0f};

    // Base content hit-tests AFTER the popup this frame - the same-layer
    // rule would let the later widget win. The layer rule overrides
    // record order: a strictly lower stratum never unseats a higher one.
    gui.begin_frame(snap(50.0f, 50.0f));
    gui.begin_layer(Layer::popup);
    (void)gui.hit_test(popup_w, rect);
    gui.end_layer();
    (void)gui.hit_test(base_w, rect);  // later, on top by record order - and losing
    CHECK(gui.hot_id() == popup_w);
    gui.end_frame();

    // Settled: the popup hovers, the covered base widget does not.
    gui.begin_frame(snap(50.0f, 50.0f));
    gui.begin_layer(Layer::popup);
    const HitTest popup_hit = gui.hit_test(popup_w, rect);
    gui.end_layer();
    const HitTest base_hit = gui.hit_test(base_w, rect);
    CHECK(popup_hit.hovered);
    CHECK(!base_hit.hovered);
    gui.end_frame();
}

TEST_CASE("layers: a higher layer steals a fresh press; a carried drag is capture") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const IdStack ids;
    const WidgetId base_w = ids.id("base");
    const WidgetId popup_w = ids.id("popup");
    const Rect rect{0.0f, 0.0f, 100.0f, 100.0f};

    // The press edge arrives with base content hit-testing first: base
    // claims at its call, and the popup scope - hit-testing later, at a
    // higher stratum - steals the THIS-FRAME claim. A click under a
    // popup belongs to the popup.
    gui.begin_frame(snap(50.0f, 50.0f, true, true));
    (void)gui.hit_test(base_w, rect);
    gui.begin_layer(Layer::popup);
    (void)gui.hit_test(popup_w, rect);
    gui.end_layer();
    CHECK(gui.active_id() == popup_w);  // stolen, fresh claim and all
    CHECK(gui.hot_id() == popup_w);    // hot moved with the press
    gui.end_frame();

    // Release inside: the popup clicks; base never did.
    gui.begin_frame(snap(50.0f, 50.0f, false, false, true));
    gui.begin_layer(Layer::popup);
    const HitTest popup_hit = gui.hit_test(popup_w, rect);
    gui.end_layer();
    const HitTest base_hit = gui.hit_test(base_w, rect);
    CHECK(popup_hit.released);
    CHECK(popup_hit.clicked);
    CHECK(!base_hit.released);  // the release folded into the popup's press
    CHECK(!base_hit.clicked);
    CHECK(gui.active_id() == kInvalidWidgetId);
    gui.end_frame();

    // The same steal refused the other way: base hit-tests after the
    // popup claimed, and a strictly lower layer never steals.
    GuiContext reverse;
    reverse.begin_frame(snap(50.0f, 50.0f, true, true));
    reverse.begin_layer(Layer::popup);
    (void)reverse.hit_test(popup_w, rect);
    reverse.end_layer();
    (void)reverse.hit_test(base_w, rect);  // later, lower: no steal
    CHECK(reverse.active_id() == popup_w);
    reverse.end_frame();
}

TEST_CASE("layers: a modal opening mid-drag does not steal the carried press") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const IdStack ids;
    const WidgetId base_w = ids.id("base");
    const WidgetId chrome_w = ids.id("chrome");
    const Rect rect{0.0f, 0.0f, 100.0f, 100.0f};

    // A drag starts on base content in an ordinary frame.
    gui.begin_frame(snap(50.0f, 50.0f, true, true));
    (void)gui.hit_test(base_w, rect);
    CHECK(gui.active_id() == base_w);
    gui.end_frame();

    // The next frame a modal veil opens OVER the in-flight drag. The
    // carried press is capture: not fresh, so no layer may steal it -
    // the base widget still owns its drag, finishes it, and the modal
    // blocks only its hover, not the gesture it interrupted.
    gui.begin_frame(snap(50.0f, 50.0f, true));
    gui.begin_layer(Layer::modal);
    (void)gui.hit_test(chrome_w, rect);  // the modal's chrome, over the drag
    gui.end_layer();
    const HitTest drag = gui.hit_test(base_w, rect);
    CHECK(drag.pressed);   // capture: the carried press is still base's
    CHECK(!drag.hovered);  // ...but the modal veil darkens its hover
    CHECK(gui.active_id() == base_w);  // the modal chrome could not steal
    gui.end_frame();

    // The drag releases under the veil: the gesture that started on base
    // finishes on base. Capture outlives the scope change.
    gui.begin_frame(snap(50.0f, 50.0f, false, false, true));
    gui.begin_layer(Layer::modal);
    (void)gui.hit_test(chrome_w, rect);
    gui.end_layer();
    const HitTest done = gui.hit_test(base_w, rect);
    CHECK(done.released);
    CHECK(done.clicked);
    gui.end_frame();
}

TEST_CASE("layers: a modal scope blocks lower strata - no click-through") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const IdStack ids;
    const WidgetId modal_w = ids.id("modal");
    const WidgetId base_w = ids.id("base");
    const Rect rect{0.0f, 0.0f, 100.0f, 100.0f};

    // The modal veil is open and the press lands where a base button
    // lives. The modal's own chrome claims it; the base widget is
    // starved of everything - no press, no hover, no hot claim.
    gui.begin_frame(snap(50.0f, 50.0f, true, true));
    gui.begin_layer(Layer::modal);
    const HitTest modal_hit = gui.hit_test(modal_w, rect);
    gui.end_layer();
    const HitTest base_hit = gui.hit_test(base_w, rect);
    CHECK(modal_hit.pressed);   // the modal stratum takes the press
    CHECK(!base_hit.pressed);   // ...and nothing clicks through the veil
    CHECK(!base_hit.hovered);   // no hover under the veil either
    CHECK(gui.active_id() == modal_w);
    CHECK(gui.hot_id() == modal_w);  // base could not unseat, nor claim
    gui.end_frame();

    // Release under the veil: the modal's chrome clicks; base stays dark.
    gui.begin_frame(snap(50.0f, 50.0f, false, false, true));
    gui.begin_layer(Layer::modal);
    const HitTest modal_up = gui.hit_test(modal_w, rect);
    gui.end_layer();
    const HitTest base_up = gui.hit_test(base_w, rect);
    CHECK(modal_up.clicked);
    CHECK(!base_up.released);
    CHECK(!base_up.clicked);
    gui.end_frame();

    // The veil lifts (the scope closed; a new frame): base is alive
    // again - the block was the scope, not a stuck flag.
    gui.begin_frame(snap(50.0f, 50.0f));
    const HitTest base_alive = gui.hit_test(base_w, rect);
    CHECK(gui.hot_id() == base_w);
    CHECK(!base_alive.hovered);  // live claim this frame...
    gui.end_frame();
    gui.begin_frame(snap(50.0f, 50.0f));
    CHECK(gui.hit_test(base_w, rect).hovered);  // ...settled after
    gui.end_frame();
}

TEST_CASE("layers: scopes nest, restore, and reset with the frame") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const WidgetId w = IdStack{}.id("w");
    const Rect rect{0.0f, 0.0f, 100.0f, 100.0f};

    CHECK(gui.current_layer() == Layer::base);  // no scope yet

    // Nest and unwind: each end_layer restores the enclosing stratum.
    gui.begin_frame(snap(50.0f, 50.0f));
    gui.begin_layer(Layer::popup);
    CHECK(gui.current_layer() == Layer::popup);
    gui.begin_layer(Layer::tooltip);
    CHECK(gui.current_layer() == Layer::tooltip);
    gui.end_layer();
    CHECK(gui.current_layer() == Layer::popup);
    gui.end_layer();
    CHECK(gui.current_layer() == Layer::base);

    // An end_layer with no open scope is a drop-safe no-op.
    gui.end_layer();
    CHECK(gui.current_layer() == Layer::base);
    gui.end_frame();

    // A scope left open by an early return cannot stick: the next
    // begin_frame resets the stack - and with it the modal veil.
    gui.begin_frame(snap(50.0f, 50.0f));
    gui.begin_layer(Layer::modal);
    gui.end_frame();
    gui.begin_frame(snap(50.0f, 50.0f));
    CHECK(gui.current_layer() == Layer::base);
    const HitTest alive = gui.hit_test(w, rect);
    CHECK(gui.hot_id() == w);  // no veil: base claims freely again
    CHECK(!alive.hovered);     // first frame on the widget: settling
    gui.end_frame();
    gui.begin_frame(snap(50.0f, 50.0f));
    CHECK(gui.hit_test(w, rect).hovered);
    gui.end_frame();
}

// --- widget state + keyboard focus (M8) ---------------------------------------------

TEST_CASE("state: derives Normal, Hovered, Pressed off one hit_test") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const WidgetId w = IdStack{}.id("w");
    const Rect rect{0.0f, 0.0f, 100.0f, 100.0f};

    // Idle: nothing doing anything.
    gui.begin_frame(snap(200.0f, 200.0f));
    const WidgetResult idle = gui.state(w, rect);
    CHECK(idle.hit.hovered == false);
    CHECK(idle.state == WidgetState::Normal);
    gui.end_frame();

    // Hover needs its settle frame (M7): the claim frame stays calm...
    gui.begin_frame(snap(50.0f, 50.0f));
    CHECK(gui.state(w, rect).state == WidgetState::Normal);
    gui.end_frame();
    // ...and the next frame reports the settled visual.
    gui.begin_frame(snap(50.0f, 50.0f));
    const WidgetResult hover = gui.state(w, rect);
    CHECK(hover.hit.hovered);
    CHECK(hover.state == WidgetState::Hovered);
    gui.end_frame();

    // Press outranks hover, wherever the pointer is.
    gui.begin_frame(snap(50.0f, 50.0f, true, true));
    const WidgetResult press = gui.state(w, rect);
    CHECK(press.hit.pressed);
    CHECK(press.state == WidgetState::Pressed);
    gui.end_frame();
}

TEST_CASE("state: Focused is the resting keyboard state, under the pointer's") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const WidgetId w = IdStack{}.id("w");
    const Rect rect{0.0f, 0.0f, 100.0f, 100.0f};

    // Focus with the pointer far away: the widget wears Focused.
    gui.begin_frame(snap(200.0f, 200.0f));
    gui.set_focus(w);
    CHECK(gui.is_focused(w));
    CHECK(gui.state(w, rect).state == WidgetState::Focused);
    gui.end_frame();

    // Focus persists across frames - state, not a claim.
    gui.begin_frame(snap(200.0f, 200.0f));
    CHECK(gui.state(w, rect).state == WidgetState::Focused);
    gui.end_frame();

    // But the pointer's transient states outrank it...
    gui.begin_frame(snap(50.0f, 50.0f));
    (void)gui.state(w, rect);  // the claim frame
    gui.end_frame();
    gui.begin_frame(snap(50.0f, 50.0f));
    const WidgetResult hovered = gui.state(w, rect);
    CHECK(hovered.state == WidgetState::Hovered);
    CHECK(gui.is_focused(w));  // ...without stealing the focus itself
    gui.end_frame();

    // And clearing the focus returns the widget to Normal.
    gui.begin_frame(snap(200.0f, 200.0f));
    gui.set_focus(kInvalidWidgetId);
    CHECK(!gui.is_focused(w));
    CHECK(gui.state(w, rect).state == WidgetState::Normal);
    gui.end_frame();
}

TEST_CASE("state: disabled widgets claim nothing and hold no focus") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const WidgetId w = IdStack{}.id("w");
    const WidgetId above = IdStack{}.id("above");
    const Rect rect{0.0f, 0.0f, 100.0f, 100.0f};

    // A fresh press inside BOTH rects: the disabled one runs first but
    // must not claim - the enabled one above takes the press, hot and
    // all. (The disabled id is not even censused: it never hit-tests.)
    gui.begin_frame(snap(50.0f, 50.0f, true, true));
    const WidgetResult off = gui.state(w, rect, true);
    CHECK(off.state == WidgetState::Disabled);
    CHECK(off.hit.pressed == false);
    CHECK(off.hit.hovered == false);
    const WidgetResult on = gui.state(above, rect);
    CHECK(on.hit.pressed);
    CHECK(on.state == WidgetState::Pressed);
    CHECK(gui.active_id() == above);
    CHECK(gui.hot_id() == above);
    gui.end_frame();

    // A focused widget that goes disabled drops the focus: it cannot
    // hold what it cannot use.
    gui.begin_frame(snap(200.0f, 200.0f));
    gui.set_focus(w);
    gui.end_frame();
    gui.begin_frame(snap(200.0f, 200.0f));
    const WidgetResult dropped = gui.state(w, rect, true);
    CHECK(dropped.state == WidgetState::Disabled);
    CHECK(!gui.is_focused(w));
    gui.end_frame();
}

TEST_CASE("focus: a press on a focusable takes focus; a press elsewhere clears it") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const WidgetId a = IdStack{}.id("a");
    const WidgetId plain = IdStack{}.id("plain");
    const Rect a_rect{0.0f, 0.0f, 100.0f, 100.0f};
    const Rect plain_rect{200.0f, 0.0f, 100.0f, 100.0f};

    // Press the focusable: focus lands at the press edge, the same frame.
    gui.begin_frame(snap(50.0f, 50.0f, true, true));
    gui.register_focusable(a);
    CHECK(gui.state(a, a_rect).state == WidgetState::Pressed);
    CHECK(gui.is_focused(a));
    gui.end_frame();

    // The release completes the click; focus stays (no new press edge).
    gui.begin_frame(snap(50.0f, 50.0f, false, false, true));
    (void)gui.state(a, a_rect);
    CHECK(gui.is_focused(a));
    gui.end_frame();

    // An idle frame changes nothing.
    gui.begin_frame(snap(50.0f, 50.0f));
    (void)gui.state(a, a_rect);
    CHECK(gui.is_focused(a));
    gui.end_frame();

    // A press on a plain (unregistered) widget is "elsewhere": the clear
    // lands at that frame's end_frame. (Each press gets its lift frame:
    // a held button cannot press again, and capture would rightly own
    // the pointer through any fabricated second edge.)
    gui.begin_frame(snap(250.0f, 50.0f, true, true));
    (void)gui.hit_test(plain, plain_rect);
    CHECK(gui.is_focused(a));  // until end_frame rules on it
    gui.end_frame();
    CHECK(!gui.is_focused(a));
    gui.begin_frame(snap(250.0f, 50.0f, false, false, true));  // lift
    gui.end_frame();

    // Refocus, then a press on empty space (inside no widget at all)
    // clears it the same way.
    gui.begin_frame(snap(50.0f, 50.0f, true, true));
    gui.register_focusable(a);
    (void)gui.state(a, a_rect);
    gui.end_frame();
    gui.begin_frame(snap(50.0f, 50.0f, false, false, true));  // the click lands
    (void)gui.state(a, a_rect);
    gui.end_frame();
    gui.begin_frame(snap(500.0f, 500.0f, true, true));
    gui.end_frame();
    CHECK(!gui.is_focused(a));
    gui.begin_frame(snap(500.0f, 500.0f, false, false, true));  // lift
    gui.end_frame();

    // But a press on the focusable that slips off and never clicks KEEPS
    // the focus it took at the edge: the press edge decides, not the
    // completed click.
    gui.begin_frame(snap(50.0f, 50.0f, true, true));
    gui.register_focusable(a);
    (void)gui.state(a, a_rect);
    gui.end_frame();
    gui.begin_frame(snap(400.0f, 400.0f, true));  // dragged away, held
    (void)gui.state(a, a_rect);  // capture: still the press owner
    CHECK(gui.is_focused(a));
    gui.end_frame();
    gui.begin_frame(snap(400.0f, 400.0f, false, false, true));  // release out
    (void)gui.state(a, a_rect);  // no click - and the focus held anyway
    CHECK(gui.is_focused(a));
    gui.end_frame();
}

TEST_CASE("focus: Tab and Shift+Tab cycle registration order, wrapping") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const WidgetId a = IdStack{}.id("a");
    const WidgetId b = IdStack{}.id("b");
    const WidgetId c = IdStack{}.id("c");

    const auto frame = [&](bool tab, bool shift) {
        InputFrame input = snap(200.0f, 200.0f);
        input.key_pressed[keys::kTab] = tab;
        input.key_down[keys::kLeftShift] = shift;
        gui.begin_frame(input);
        gui.register_focusable(a);  // registration order IS the Tab order
        gui.register_focusable(b);
        gui.register_focusable(c);
        gui.end_frame();
    };

    // From nothing, Tab starts at the first registration.
    frame(true, false);
    CHECK(gui.is_focused(a));
    frame(true, false);
    CHECK(gui.is_focused(b));
    frame(true, false);
    CHECK(gui.is_focused(c));
    frame(true, false);  // wraps forward
    CHECK(gui.is_focused(a));

    frame(true, true);  // wraps backward: a's predecessor is the last
    CHECK(gui.is_focused(c));
    frame(true, true);  // and walks back from there
    CHECK(gui.is_focused(b));

    frame(false, false);  // no Tab: nothing moves
    CHECK(gui.is_focused(b));

    // Focus outside this frame's registrations: forward restarts at the
    // first, backward at the last.
    gui.begin_frame(snap(200.0f, 200.0f));
    gui.set_focus(kInvalidWidgetId);
    gui.end_frame();
    frame(true, false);
    CHECK(gui.is_focused(a));
    gui.begin_frame(snap(200.0f, 200.0f));
    gui.set_focus(IdStack{}.id("gone"));  // never registered
    gui.end_frame();
    frame(true, true);
    CHECK(gui.is_focused(c));
}

TEST_CASE("focus: nav_next/nav_previous queue moves resolved at end_frame") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const WidgetId a = IdStack{}.id("a");
    const WidgetId b = IdStack{}.id("b");
    const WidgetId c = IdStack{}.id("c");

    const auto frame = [&](int steps) {
        gui.begin_frame(snap(200.0f, 200.0f));
        gui.register_focusable(a);
        gui.register_focusable(b);
        gui.register_focusable(c);
        for (int i = 0; i < steps; ++i) {
            gui.nav_next();
        }
        for (int i = 0; i > steps; --i) {
            gui.nav_previous();
        }
        CHECK(!gui.is_focused(b));  // mid-frame: nothing resolved yet
        gui.end_frame();
    };

    frame(1);  // from nothing: the first registration
    CHECK(gui.is_focused(a));
    frame(2);  // two queued steps in one frame
    CHECK(gui.is_focused(c));
    frame(-1);
    CHECK(gui.is_focused(b));

    // No focusables registered: the move drops, focus survives.
    gui.begin_frame(snap(200.0f, 200.0f));
    gui.nav_next();
    gui.end_frame();
    CHECK(gui.is_focused(b));
}

TEST_CASE("focus: registration past the cap degrades loudly, never crashes") {
    using namespace ws::ui::gui;
    GuiContext gui;
    IdStack ids;

    // The first registration survives; 200 more push the frame's list
    // well past its cap - the overflow drops with one warn, and the
    // move still lands on the first.
    gui.begin_frame(snap(200.0f, 200.0f));
    ids.push("cap");
    const WidgetId first = ids.id("w");
    gui.register_focusable(first);
    for (int i = 0; i < 200; ++i) {
        ids.push(i);
        gui.register_focusable(ids.id("w"));
        ids.pop();
    }
    gui.nav_next();
    gui.end_frame();
    CHECK(gui.is_focused(first));
}

TEST_CASE("layers: a modal veil clears focus under it") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const WidgetId base_w = IdStack{}.id("base");
    const Rect rect{0.0f, 0.0f, 100.0f, 100.0f};

    gui.begin_frame(snap(200.0f, 200.0f));
    gui.set_focus(base_w);
    gui.register_focusable(base_w);
    CHECK(gui.is_focused(base_w));
    gui.end_frame();

    // Chrome-first: the modal scope opens and closes at the top of the
    // frame, and the veil stays up (M7). The base widget state()s later
    // in the frame, under the veil - and drops the focus: a modal blocks
    // the keyboard like it blocks the pointer.
    gui.begin_frame(snap(200.0f, 200.0f));
    gui.begin_layer(Layer::modal);
    gui.end_layer();
    const WidgetResult veiled = gui.state(base_w, rect);
    CHECK(veiled.hit.hovered == false);  // M7's pointer block
    CHECK(!gui.is_focused(base_w));      // M8's keyboard block
    CHECK(veiled.state == WidgetState::Normal);
    gui.end_frame();
}

TEST_CASE("button: Enter and Space activate the focused button") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;

    // Click the button first: the press takes the focus (buttons register
    // focusable), the release completes the click.
    gui.begin_frame(snap(60.0f, 12.0f, true, true));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(!button(gui, layout, kDefaultChrome, "fire"));
    layout.end_panel();
    gui.end_frame();
    gui.begin_frame(snap(60.0f, 12.0f, false, false, true));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(button(gui, layout, kDefaultChrome, "fire"));
    layout.end_panel();
    gui.end_frame();

    // Pointer idle, focus held: Enter fires the button - and only it.
    InputFrame key_enter = snap(200.0f, 200.0f);
    key_enter.key_pressed[keys::kEnter] = true;
    gui.begin_frame(key_enter);
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(button(gui, layout, kDefaultChrome, "fire"));
    CHECK(!button(gui, layout, kDefaultChrome, "other"));  // unfocused sibling
    layout.end_panel();
    gui.end_frame();

    // Space does the same.
    InputFrame key_space = snap(200.0f, 200.0f);
    key_space.key_pressed[keys::kSpace] = true;
    gui.begin_frame(key_space);
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(button(gui, layout, kDefaultChrome, "fire"));
    layout.end_panel();
    gui.end_frame();

    // Clearing the focus kills the key path: the same Space does nothing.
    InputFrame key_none = snap(200.0f, 200.0f);
    key_none.key_pressed[keys::kSpace] = true;
    gui.begin_frame(key_none);
    gui.set_focus(kInvalidWidgetId);
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(!button(gui, layout, kDefaultChrome, "fire"));
    layout.end_panel();
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
    // The parked mouse claims nothing - and being absent (present=false,
    // the touch-only shape) it is not a pointer in the frame's list at
    // all; each real pointer still reports its own position.
    CHECK(gui.hot_id() == kInvalidWidgetId);
    CHECK(gui.pointer(PointerId{1}) != nullptr);
    CHECK(gui.pointer(PointerId{1})->x == 50.0f);
    CHECK(gui.pointer(PointerId{2})->x == 150.0f);
    CHECK(gui.pointer(PointerId{99}) == nullptr);  // no such pointer this frame
    CHECK(gui.pointer(kMousePointerId) == nullptr);  // no mouse this frame
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
    // the contact. No parking here: the duplicate IS the test. The mouse
    // must also be PRESENT: the duplicate shape is a real mouse plus a
    // finger (Android + Bluetooth). A touch-only device lists the finger
    // alone - the pointer_pos/gather tests above pin that shape.
    InputFrame press;
    press.touch_down(PointerId{7}, 50.0f, 50.0f);
    press.mouse_present = true;
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
    lift.mouse_present = true;
    gui.begin_frame(lift);
    const HitTest released = gui.hit_test(id, rect);
    CHECK(released.released);
    CHECK(released.clicked);
    CHECK(released.pointer == kMousePointerId);
    gui.end_frame();
}

// --- same-frame touch id reuse (the ordered-input defect) --------------------------

TEST_CASE("id reuse: lift and landing on one widget - the click fires, the press re-claims") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const WidgetId id = IdStack{}.id("w");
    const Rect rect{0.0f, 0.0f, 100.0f, 100.0f};

    // The finger presses in its own frame; the claim carries.
    gui.begin_frame(touch_frame({{4, 50.0f, 50.0f, /*pressed=*/true}}));
    CHECK(gui.hit_test(id, rect).pressed);
    CHECK(gui.active_id(PointerId{4}) == id);
    gui.end_frame();

    // One pump, one id: the finger lifts at (50,50) and the platform
    // hands its id to a new contact at (60,60) before next_frame(). The
    // slot reads pressed+released with down - the widget must report the
    // old lifetime's click AND hold the new contact's press: the release
    // not lost to the arrival, the claim not lost to the release.
    InputFrame reuse;
    reuse.touch_down(PointerId{4}, 50.0f, 50.0f);  // the carried press,
    reuse.touches[0].pressed = false;               // no edge of its own
    reuse.touch_up(PointerId{4}, 50.0f, 50.0f, false);  // the lift, inside
    reuse.touch_down(PointerId{4}, 60.0f, 60.0f);      // the landing, inside
    gui.begin_frame(reuse);  // the bridge's raw mouse writes stay inert:
                             // mouse_present false, touch is the pointer
    const HitTest both = gui.hit_test(id, rect);
    CHECK(both.released);  // the old lifetime's lift reported
    CHECK(both.clicked);   // the lift was inside: the click fires
    CHECK(both.pressed);   // the new contact's press is live on the widget
    CHECK(gui.active_id(PointerId{4}) == id);  // re-claimed fresh
    gui.end_frame();
    CHECK(gui.active_id(PointerId{4}) == id);  // survived its own release
                                               // edge (end_frame's gate)

    // The re-claim carried - and the new contact's own lift clicks: two
    // lifetimes, two clicks, neither stolen by the other.
    gui.begin_frame(touch_frame({{4, 60.0f, 60.0f}}));
    CHECK(gui.hit_test(id, rect).pressed);
    CHECK(gui.active_id(PointerId{4}) == id);
    gui.end_frame();
    gui.begin_frame(touch_frame({{4, 60.0f, 60.0f, false, /*released=*/true}}));
    const HitTest lift = gui.hit_test(id, rect);
    CHECK(lift.released);
    CHECK(lift.clicked);
    CHECK(gui.active_id(PointerId{4}) == kInvalidWidgetId);
    gui.end_frame();
}

TEST_CASE("id reuse: a landing on a different widget claims it fresh, no phantom click") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const WidgetId held = IdStack{}.id("held");
    const WidgetId tapped = IdStack{}.id("tapped");
    const Rect held_rect{0.0f, 0.0f, 100.0f, 100.0f};
    const Rect tapped_rect{200.0f, 0.0f, 100.0f, 100.0f};

    gui.begin_frame(touch_frame({{4, 50.0f, 50.0f, /*pressed=*/true}}));
    CHECK(gui.hit_test(held, held_rect).pressed);
    gui.end_frame();

    // The drag slid off `held`, the finger lifted there, and the new
    // contact lands inside `tapped` - one pump, one id. The old owner
    // reports the lift (no click: the pointer is off it at the lift and
    // at the landing both); `tapped` takes the new press through its own
    // press-claim block - and the lift's release edge must NOT read as
    // tapped's click, the contact is still down.
    InputFrame reuse;
    reuse.touch_down(PointerId{4}, 150.0f, 50.0f);  // carried, slid off held
    reuse.touches[0].pressed = false;
    reuse.touch_up(PointerId{4}, 150.0f, 50.0f, false);  // the lift, off held
    reuse.touch_down(PointerId{4}, 250.0f, 50.0f);      // lands inside tapped
    gui.begin_frame(reuse);
    const HitTest owner = gui.hit_test(held, held_rect);  // records first
    CHECK(owner.released);   // the drag it owned ended
    CHECK(!owner.clicked);  // off it: no click either way
    CHECK(gui.active_id(PointerId{4}) == kInvalidWidgetId);  // freed
    const HitTest newcomer = gui.hit_test(tapped, tapped_rect);
    CHECK(newcomer.pressed);    // the new press claimed, fresh
    CHECK(!newcomer.released);  // the lift was not THIS press's end
    CHECK(!newcomer.clicked);   // ...so no phantom click on a held contact
    CHECK(gui.active_id(PointerId{4}) == tapped);
    gui.end_frame();
    CHECK(gui.active_id(PointerId{4}) == tapped);  // survived end_frame

    // The fresh claim carries; its lift is the click that counts.
    gui.begin_frame(touch_frame({{4, 250.0f, 50.0f}}));
    CHECK(gui.hit_test(tapped, tapped_rect).pressed);
    CHECK(gui.active_id(PointerId{4}) == tapped);
    gui.end_frame();
    gui.begin_frame(touch_frame({{4, 250.0f, 50.0f, false, /*released=*/true}}));
    const HitTest lift = gui.hit_test(tapped, tapped_rect);
    CHECK(lift.released);
    CHECK(lift.clicked);
    CHECK(gui.active_id(PointerId{4}) == kInvalidWidgetId);
    gui.end_frame();
}

TEST_CASE("id reuse: a same-frame tap is still a tap - no re-claim, no carried press") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const WidgetId id = IdStack{}.id("tap");
    const Rect rect{0.0f, 0.0f, 100.0f, 100.0f};

    // pressed+released with down false - the down-then-up order. The
    // reuse machinery must leave the tap alone: claim, click, and clear
    // in one frame, nothing carried into the next.
    gui.begin_frame(touch_frame({{4, 50.0f, 50.0f, /*pressed=*/true,
                                  /*released=*/true}}));
    const HitTest tap = gui.hit_test(id, rect);
    CHECK(tap.pressed);
    CHECK(tap.released);
    CHECK(tap.clicked);
    CHECK(gui.active_id(PointerId{4}) == kInvalidWidgetId);  // ended in-frame
    gui.end_frame();
    CHECK(gui.active_id(PointerId{4}) == kInvalidWidgetId);  // stays ended

    // A new contact with the same id presses next frame and claims
    // cleanly - the tap's claim did not leak through the release gate.
    gui.begin_frame(touch_frame({{4, 50.0f, 50.0f, /*pressed=*/true}}));
    CHECK(gui.hit_test(id, rect).pressed);
    gui.end_frame();
}

TEST_CASE("id reuse: the owner vanished - the stale claim still lifts with the release") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const WidgetId gone = IdStack{}.id("gone");
    const WidgetId next = IdStack{}.id("next");
    const Rect gone_rect{0.0f, 0.0f, 100.0f, 100.0f};
    const Rect next_rect{0.0f, 0.0f, 100.0f, 100.0f};  // the same pixels:
                                                       // a rebuilt widget

    // The press claims; the UI then swaps the widget out from under it.
    gui.begin_frame(touch_frame({{4, 50.0f, 50.0f, /*pressed=*/true}}));
    CHECK(gui.hit_test(gone, gone_rect).pressed);
    gui.end_frame();

    // The reuse frame with the owner GONE (a tab flip, a rebuild): no
    // hit_test consumes the edge, so end_frame's unconsumed-release clear
    // must see it. Before the fix the arrival erased the edge and the
    // dead owner's claim lived on, blocking this pointer's hot and press
    // claims for the new contact's whole lifetime.
    InputFrame reuse;
    reuse.touch_down(PointerId{4}, 50.0f, 50.0f);
    reuse.touches[0].pressed = false;
    reuse.touch_up(PointerId{4}, 50.0f, 50.0f, false);
    reuse.touch_down(PointerId{4}, 50.0f, 50.0f);
    gui.begin_frame(reuse);
    const HitTest rebuilt = gui.hit_test(next, next_rect);  // the replacement
    CHECK(!rebuilt.pressed);   // the carried claim is capture: not fresh,
                               // not stealable - the new contact cannot
                               // take what the vanished owner still holds
    CHECK(!rebuilt.hovered);   // (and the settled hover was the old
                               // owner's: hot_prev is not this widget)
    CHECK(gui.active_id(PointerId{4}) == gone);  // ...and still holds
    gui.end_frame();
    CHECK(gui.active_id(PointerId{4}) == kInvalidWidgetId);  // the release
                                                             // lifted it

    // The new contact is claimable again: hover settles on it. Its press
    // edge was spent in the reuse frame, though - it holds, but presses
    // nothing (the one win a full ordered event queue would buy).
    gui.begin_frame(touch_frame({{4, 50.0f, 50.0f}}));
    const HitTest settled = gui.hit_test(next, next_rect);
    CHECK(!settled.pressed);
    CHECK(gui.is_hot(next, PointerId{4}));  // the claim the stale owner
                                            // used to block (the mouse is
                                            // absent: touch is the pointer)
    gui.end_frame();
    gui.begin_frame(touch_frame({{4, 50.0f, 50.0f}}));
    CHECK(gui.state(next, next_rect).state == WidgetState::Hovered);
    gui.end_frame();
}

// --- mouse presence (the touch-only/cursor-leave defect) ---------------------------

TEST_CASE("mouse_present: an absent mouse claims nothing - no hover at (0,0)") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const WidgetId id = IdStack{}.id("corner");
    const Rect rect{0.0f, 0.0f, 100.0f, 100.0f};

    // The defect's shape: a touch-only device where mouse_x/y sit at the
    // (0,0) default - dead on the top-left widget. No platform ever said
    // a mouse exists, so the mouse is not a pointer at all and no hover
    // may ever settle there - not on any frame, settled or not.
    InputFrame absent;
    absent.mouse_x = 0.0f;  // explicit: the top-left default IS the trap
    absent.mouse_y = 0.0f;
    for (int settle = 0; settle < 3; ++settle) {
        gui.begin_frame(absent);
        const WidgetResult result = gui.state(id, rect);
        CHECK(result.state == WidgetState::Normal);
        CHECK(!result.hit.hovered);
        CHECK(!result.hit.pressed);
        CHECK(result.hit.pointer == kNoPointer);
        CHECK(gui.hot_id() == kInvalidWidgetId);
        CHECK(gui.active_id() == kInvalidWidgetId);
        gui.end_frame();
    }

    // A ghost press edge (mouse_down written, device still absent) is
    // nobody's press - and a ghost release later clicks nothing.
    InputFrame ghost_press = absent;
    ghost_press.mouse_down[0] = true;
    ghost_press.mouse_pressed[0] = true;
    gui.begin_frame(ghost_press);
    CHECK(!gui.hit_test(id, rect).pressed);
    CHECK(gui.active_id() == kInvalidWidgetId);
    gui.end_frame();
    InputFrame ghost_lift = absent;
    ghost_lift.mouse_released[0] = true;
    gui.begin_frame(ghost_lift);
    CHECK(!gui.hit_test(id, rect).clicked);
    gui.end_frame();

    // Presence flips the whole device on: the same (0,0) position now
    // settles hover (one frame, as always) and a press claims. The widget
    // must RUN to claim - the settle frame hit-tests like any widget
    // frame does.
    InputFrame present = absent;
    present.mouse_present = true;
    gui.begin_frame(present);
    (void)gui.state(id, rect);  // hot claims this frame
    gui.end_frame();
    gui.begin_frame(present);
    CHECK(gui.state(id, rect).state == WidgetState::Hovered);
    CHECK(gui.is_hot(id));
    gui.end_frame();
    gui.begin_frame(snap(0.0f, 0.0f, true, true));
    CHECK(gui.hit_test(id, rect).pressed);
    gui.end_frame();
}

TEST_CASE("mouse_present: a cursor leave kills hover and evaporates a held press") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const WidgetId id = IdStack{}.id("leave");
    const Rect rect{0.0f, 0.0f, 100.0f, 100.0f};

    // Hover settles while the mouse is there (the widget runs both
    // frames - hot is frame-scoped, so the settle needs the claim)...
    gui.begin_frame(snap(50.0f, 50.0f));
    (void)gui.state(id, rect);
    gui.end_frame();
    gui.begin_frame(snap(50.0f, 50.0f));
    CHECK(gui.state(id, rect).state == WidgetState::Hovered);
    gui.end_frame();

    // ...the cursor leaves the window: the platform clears ONLY the flag
    // (position and button state stay as they were). The pointer drops
    // out of the frame and its track with it - hover dies the SAME frame,
    // not one frame later, because no track survives to carry hot_prev.
    InputFrame left = snap(50.0f, 50.0f);
    left.mouse_present = false;
    gui.begin_frame(left);
    const WidgetResult gone = gui.state(id, rect);
    CHECK(gone.state == WidgetState::Normal);
    CHECK(!gone.hit.hovered);
    CHECK(gui.hot_id() == kInvalidWidgetId);
    gui.end_frame();
    gui.begin_frame(left);
    CHECK(gui.state(id, rect).state == WidgetState::Normal);  // nothing settles back
    gui.end_frame();

    // A press in flight when the cursor leaves evaporates: the raw fields
    // still read button-down, but the pointer is gone, so the drag is
    // nobody's - and the release, whenever it lands, reports nowhere.
    gui.begin_frame(snap(50.0f, 50.0f, true, true));
    CHECK(gui.hit_test(id, rect).pressed);
    CHECK(gui.active_id() == id);
    gui.end_frame();
    InputFrame carried = snap(50.0f, 50.0f, true);  // raw fields say held...
    carried.mouse_present = false;                  // ...the device is gone
    gui.begin_frame(carried);
    CHECK(gui.active_id() == kInvalidWidgetId);  // the press evaporated
    CHECK(gui.state(id, rect).state == WidgetState::Normal);
    gui.end_frame();
    InputFrame lift = snap(50.0f, 50.0f, false, false, true);
    lift.mouse_present = false;
    gui.begin_frame(lift);
    CHECK(!gui.hit_test(id, rect).clicked);  // the release lands nowhere
    gui.end_frame();

    // The cursor returns: presence re-establishes the device, and the
    // hover settles again like clockwork (the widget runs both frames).
    gui.begin_frame(snap(50.0f, 50.0f));
    (void)gui.state(id, rect);
    gui.end_frame();
    gui.begin_frame(snap(50.0f, 50.0f));
    CHECK(gui.state(id, rect).state == WidgetState::Hovered);
    gui.end_frame();
}

TEST_CASE("mouse_present: a lifted finger leaves no hover at the tap point") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const WidgetId id = IdStack{}.id("tap");
    const Rect rect{0.0f, 0.0f, 100.0f, 100.0f};

    // The touch-only shape, built exactly as the platform would: a tap
    // through the touch_* writers, the flag never set (the bridge mirrors
    // raw fields only - it never speaks for presence). The finger's own
    // touch slot drives the press.
    InputFrame press;
    press.touch_down(PointerId{7}, 50.0f, 50.0f);
    gui.begin_frame(press);
    const HitTest hit = gui.hit_test(id, rect);
    CHECK(hit.pressed);
    CHECK(hit.pointer == PointerId{7});  // the finger, not a mouse
    CHECK(gui.active_id(PointerId{7}) == id);
    CHECK(gui.active_id() == kInvalidWidgetId);  // no mouse to mirror onto
    gui.end_frame();

    InputFrame lift;
    lift.touch_down(PointerId{7}, 50.0f, 50.0f);  // the entry, back for its lift
    lift.touch_up(PointerId{7}, 50.0f, 50.0f, false);
    lift.touches[0].pressed = false;  // down+up in one frame: the press edge
    lift.mouse_pressed[0] = false;    // is not this frame's fact
    gui.begin_frame(lift);
    CHECK(gui.hit_test(id, rect).clicked);  // the one click, off the touch
    gui.end_frame();

    // The finger is gone, but mouse_x/y still read the tap point - the
    // bridge's mirror, frozen at the lift. Under the old always-mouse
    // semantics that position hovered the widget forever; absent, it must
    // never settle.
    CHECK(lift.mouse_x == 50.0f);
    for (int settle = 0; settle < 3; ++settle) {
        gui.begin_frame(InputFrame{});  // no touches, no mouse: the raw
                                        // mirror is all that remains
        const WidgetResult calm = gui.state(id, rect);
        CHECK(calm.state == WidgetState::Normal);
        CHECK(!calm.hit.hovered);
        CHECK(gui.hot_id() == kInvalidWidgetId);
        gui.end_frame();
    }
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

TEST_CASE("clear_persistent_state: the explicit eviction - by id, then wholesale") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const IdStack ids;
    const WidgetId a = ids.id("a");
    const WidgetId b = ids.id("b");

    gui.persistent_state<float>(a) = 1.5f;
    gui.persistent_state<float>(b) = 2.5f;
    gui.persistent_state<int>(b) = 7;

    // One widget's entries leave EVERY table; the neighbor keeps its
    // place, type by type.
    gui.clear_persistent_state(a);
    CHECK(gui.persistent_state<float>(a) == 0.0f);  // re-created fresh
    CHECK(gui.persistent_state<float>(b) == 2.5f);
    CHECK(gui.persistent_state<int>(b) == 7);

    // Wholesale: every table empties - the app's "this screen is never
    // coming back". There is deliberately no automatic eviction; this
    // call is the whole eviction story.
    gui.clear_persistent_state();
    CHECK(gui.persistent_state<float>(b) == 0.0f);
    CHECK(gui.persistent_state<int>(b) == 0);
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

TEST_CASE("LayoutCursor: spacing gaps lines and same_line items; no trailing gap") {
    using namespace ws::ui::gui;
    LayoutCursor layout;
    (void)layout.begin_panel({0.0f, 0.0f, 200.0f, 200.0f}, 0.0f, 8.0f);

    // One spacing below a line's bottom is the next line's top.
    (void)layout.next(30.0f, 24.0f);  // line 1: (0, 0) .. (30, 24)
    const Rect b = layout.next(30.0f, 24.0f);
    CHECK(b.y == 32.0f);  // 24 + the 8-px gap

    // same_line resumes onto b's line, one spacing to its right. The
    // extent mid-line counts the line's tallest SO FAR - b's 24.
    layout.same_line();
    CHECK(layout.flow_extent() == 56.0f);  // 32 + 24
    const Rect c = layout.next(30.0f, 40.0f);
    CHECK(c.x == 38.0f);  // b.x + b.width + the gap
    CHECK(c.y == b.y);    // flush with b; c's height never lifts the line's top
    CHECK(layout.flow_extent() == 72.0f);  // the line's tallest now: 32 + 40

    // The next line starts below the line's TALLEST plus one gap...
    const Rect d = layout.next(30.0f, 10.0f);
    CHECK(d.y == 80.0f);  // 72 + 8

    // ...and the extent stops at the last placed line's bottom: the gap
    // before a line that never came is not content.
    CHECK(layout.flow_extent() == 90.0f);

    // An explicit same_line gap overrides the panel's spacing (tab_bar's
    // flush strip seam); the default negative gap is the panel's own.
    layout.same_line(0.0f);
    CHECK(layout.next(30.0f, 10.0f).x == d.x + d.width);
    layout.end_panel();

    // The default spacing is 0 - the unspaced look, today's exact pitch.
    LayoutCursor raw;
    (void)raw.begin_panel({0.0f, 0.0f, 100.0f, 100.0f}, 0.0f);
    (void)raw.next(30.0f, 24.0f);
    CHECK(raw.next(30.0f, 24.0f).y == 24.0f);  // flush: no gap

    // A spacing panel that placed nothing flows nothing.
    LayoutCursor empty;
    (void)empty.begin_panel({0.0f, 0.0f, 100.0f, 100.0f}, 0.0f, 8.0f);
    CHECK(empty.flow_extent() == 0.0f);
}

TEST_CASE("LayoutCursor: remaining_width is the room left on the line") {
    using namespace ws::ui::gui;
    LayoutCursor layout;

    // No panel open: no line to fill.
    CHECK(layout.remaining_width() == 0.0f);

    // content (22, 22, 176, 76): at a line start the whole content width
    // remains - before AND after a placement rewound the cursor.
    (void)layout.begin_panel({10.0f, 10.0f, 200.0f, 100.0f}, 12.0f);
    CHECK(layout.remaining_width() == 176.0f);
    (void)layout.next(60.0f, 24.0f);
    CHECK(layout.remaining_width() == 176.0f);

    // After same_line the cursor sits 60 in: 176 - 60 remain, and handing
    // the remainder to next() spans exactly to the content's right edge.
    layout.same_line();
    CHECK(layout.remaining_width() == 116.0f);
    const Rect fill = layout.next(layout.remaining_width(), 24.0f);
    CHECK(fill.x == 82.0f);
    CHECK(fill.width == 116.0f);
    CHECK(fill.x + fill.width == 198.0f);  // the content's right edge

    // The next line starts over at full width again.
    CHECK(layout.remaining_width() == 176.0f);
    layout.end_panel();
}

TEST_CASE("LayoutCursor: a bounds too small for its padding is an empty panel") {
    using namespace ws::ui::gui;
    LayoutCursor layout;

    // Padding 12 on a 10x10 bounds: the content clamps at 0 - never
    // backwards - and degenerates to its top-left point. Children still
    // place there (the scroll viewport has clamped this way all along).
    const Rect content = layout.begin_panel({10.0f, 10.0f, 10.0f, 10.0f}, 12.0f);
    CHECK(content.x == 22.0f);
    CHECK(content.y == 22.0f);
    CHECK(content.width == 0.0f);
    CHECK(content.height == 0.0f);
    const Rect child = layout.next(20.0f, 20.0f);
    CHECK(child.x == 22.0f);
    CHECK(child.y == 22.0f);  // at the degenerate content origin
    CHECK(layout.remaining_width() == 0.0f);  // nothing beyond the edge
    layout.end_panel();
}

TEST_CASE("LayoutCursor: alignment places within the line height so far") {
    using namespace ws::ui::gui;
    LayoutCursor layout;
    (void)layout.begin_panel({0.0f, 0.0f, 200.0f, 200.0f}, 0.0f);

    // The tallest first: later widgets on the line center and bottom
    // against it. The line's height is what stands to their LEFT.
    (void)layout.next(30.0f, 48.0f);  // the line runs 0..48
    layout.same_line();
    CHECK(layout.next(30.0f, 24.0f, Align::center).y == 12.0f);  // (48-24)/2
    layout.same_line();
    CHECK(layout.next(30.0f, 24.0f, Align::bottom).y == 24.0f);  // 48 - 24

    // The line advances below its TALLEST (48), not below the aligned
    // placements' bottoms.
    CHECK(layout.next(30.0f, 24.0f).y == 48.0f);
    layout.end_panel();

    // A tall LATECOMER tops out at the line's top: against the line so
    // far it IS the height, so it never pokes above into the previous
    // line (earlier widgets never re-center - place the tallest first).
    LayoutCursor late;
    (void)late.begin_panel({0.0f, 0.0f, 200.0f, 200.0f}, 0.0f);
    (void)late.next(30.0f, 24.0f);  // a short line so far
    late.same_line();
    const Rect big = late.next(30.0f, 96.0f, Align::center);
    CHECK(big.y == 0.0f);  // centered against max(24, 96) = itself: the top
    CHECK(late.flow_extent() == 96.0f);  // the line grew to its tallest
    CHECK(late.next(30.0f, 24.0f).y == 96.0f);  // the next line follows it
    late.end_panel();
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
    // The M14 label and scrollbar metrics ride the same train.
    CHECK(big.label_char_width == base.label_char_width * 2.0f);
    CHECK(big.label_inset == base.label_inset * 2.0f);
    CHECK(big.check_inset == base.check_inset * 2.0f);
    CHECK(big.slider_handle_width == base.slider_handle_width * 2.0f);
    CHECK(big.scrollbar_thumb_width == base.scrollbar_thumb_width * 2.0f);
    CHECK(big.scrollbar_band_width == base.scrollbar_band_width * 2.0f);
    CHECK(big.scrollbar_min_thumb == base.scrollbar_min_thumb * 2.0f);
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

TEST_CASE("theme: the label and scrollbar metrics pin the v1 look") {
    using namespace ws::ui::gui;
    const ChromeTheme& t = kDefaultChrome;
    // The fontless estimate (6 px a character) and the insets the
    // widgets fit labels against.
    CHECK(t.label_char_width == 6.0f);
    CHECK(t.label_inset == 2.0f);
    CHECK(t.check_inset == 5.0f);
    // The slider handle and the scrollbar's thumb, band and floor.
    CHECK(t.slider_handle_width == 10.0f);
    CHECK(t.scrollbar_thumb_width == 4.0f);
    CHECK(t.scrollbar_band_width == 12.0f);
    CHECK(t.scrollbar_min_thumb == 12.0f);
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

// --- layout semantics (M13): the gui look carries spacing and fill-width ------------

TEST_CASE("panel: children flow spaced theme.spacing; 0 restores the old pitch") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;
    const ChromeTheme& theme = kDefaultChrome;

    // The gui wrapper threads theme.spacing into its layout panel: rows
    // pitch control_height + spacing. A probe placement after two buttons
    // reads the pitch back without hit-testing.
    gui.begin_frame(snap(400.0f, 400.0f));
    (void)begin_panel(gui, layout, theme, "page", {0.0f, 0.0f, 200.0f, 200.0f});
    (void)button(gui, layout, theme, "one");
    (void)button(gui, layout, theme, "two");
    const Rect probe = layout.next(10.0f, 10.0f);
    CHECK(probe.y ==
          doctest::Approx(12.0f + 2.0f * (theme.control_height + theme.spacing)));
    end_panel(gui, layout);
    gui.end_frame();

    // A click in the 8-px seam between the rows (y 40 sits in 36..44)
    // hits nothing at all.
    bool hit = false;
    gui.begin_frame(snap(60.0f, 40.0f, false, true, true));
    (void)begin_panel(gui, layout, theme, "page", {0.0f, 0.0f, 200.0f, 200.0f});
    if (button(gui, layout, theme, "one")) { hit = true; }
    if (button(gui, layout, theme, "two")) { hit = true; }
    end_panel(gui, layout);
    gui.end_frame();
    CHECK(!hit);

    // theme.spacing = 0 is the unspaced look: flush rows - and the same
    // click now lands on the second row.
    ChromeTheme flush = kDefaultChrome;
    flush.spacing = 0.0f;
    gui.begin_frame(snap(400.0f, 400.0f));
    (void)begin_panel(gui, layout, flush, "page", {0.0f, 0.0f, 200.0f, 200.0f});
    (void)button(gui, layout, flush, "one");
    (void)button(gui, layout, flush, "two");
    const Rect tight = layout.next(10.0f, 10.0f);
    CHECK(tight.y == doctest::Approx(12.0f + 2.0f * flush.control_height));
    end_panel(gui, layout);
    gui.end_frame();

    hit = false;
    gui.begin_frame(snap(60.0f, 40.0f, false, true, true));  // now ON row two
    (void)begin_panel(gui, layout, flush, "page", {0.0f, 0.0f, 200.0f, 200.0f});
    if (button(gui, layout, flush, "one")) { hit = true; }
    if (button(gui, layout, flush, "two")) { hit = true; }
    end_panel(gui, layout);
    gui.end_frame();
    CHECK(hit);
}

TEST_CASE("button and slider: a negative width fills the rest of the line") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;
    const ChromeTheme& theme = kDefaultChrome;
    float gain = 0.0f;

    // Panel {0,0,200,100} -> content (12, 12, 176, 76). A fill button
    // spans the full content width (x 12..188); the fill slider takes
    // the next row (y 44) and fills it too.
    gui.begin_frame(snap(400.0f, 400.0f));
    (void)begin_panel(gui, layout, theme, "page", {0.0f, 0.0f, 200.0f, 100.0f});
    (void)button(gui, layout, theme, "fill", -1.0f);
    (void)slider_float(gui, layout, theme, "gain", gain, 0.0f, 1.0f, -1.0f);
    const Rect probe = layout.next(10.0f, 10.0f);
    CHECK(probe.y ==
          doctest::Approx(44.0f + theme.control_height + theme.spacing));
    end_panel(gui, layout);
    gui.end_frame();

    // A click near the fill button's right end (x 180, inside 12..188)
    // lands on it - a default-width button would have stopped at x 132.
    gui.begin_frame(snap(180.0f, 24.0f, false, true, true));
    (void)begin_panel(gui, layout, theme, "page", {0.0f, 0.0f, 200.0f, 100.0f});
    CHECK(button(gui, layout, theme, "fill", -1.0f));
    (void)slider_float(gui, layout, theme, "gain", gain, 0.0f, 1.0f, -1.0f);
    end_panel(gui, layout);
    gui.end_frame();

    // Past the content's right edge there is nothing to claim.
    gui.begin_frame(snap(190.0f, 24.0f, false, true, true));
    (void)begin_panel(gui, layout, theme, "page", {0.0f, 0.0f, 200.0f, 100.0f});
    CHECK(!button(gui, layout, theme, "fill", -1.0f));
    (void)slider_float(gui, layout, theme, "gain", gain, 0.0f, 1.0f, -1.0f);
    end_panel(gui, layout);
    gui.end_frame();

    // The fill slider maps its value over the FULL filled width: a press
    // at the content's horizontal center (x 100 = 12 + 88) reads 0.5.
    gui.begin_frame(snap(100.0f, 56.0f, true, true));
    (void)begin_panel(gui, layout, theme, "page", {0.0f, 0.0f, 200.0f, 100.0f});
    (void)button(gui, layout, theme, "fill", -1.0f);
    CHECK(slider_float(gui, layout, theme, "gain", gain, 0.0f, 1.0f, -1.0f));
    end_panel(gui, layout);
    gui.end_frame();
    CHECK(gain == doctest::Approx(0.5f));
}

// --- label fitting (M14): fit_text - the marker cut, the truncating width pass ------

TEST_CASE("fit_text: the display cut - a ## suffix ids but never draws") {
    using namespace ws::ui::gui;
    char scratch[128];
    const ChromeTheme& theme = kDefaultChrome;  // fontless: the estimate

    // No marker: the label's own bytes and the per-char estimate
    // (label_char_width = 6) - scratch never written.
    const LabelFit whole =
        fit_text("Save", 100.0f, theme, scratch, sizeof(scratch));
    CHECK(std::string_view(whole.text) == "Save");
    CHECK(whole.text != scratch);  // nothing needed changing: no copy
    CHECK(whole.advance == doctest::Approx(24.0f));

    // The marker cut moves the display into scratch (the view over the
    // label is not null-terminated at the cut) - the display advances,
    // the suffix never does.
    const LabelFit cut =
        fit_text("Save##one", 100.0f, theme, scratch, sizeof(scratch));
    CHECK(cut.text == scratch);
    CHECK(std::string_view(cut.text) == "Save");
    CHECK(cut.advance == doctest::Approx(24.0f));

    // A negative width skips the pass - the checkbox's sizing spelling.
    const LabelFit nosize =
        fit_text("Save##one", -1.0f, theme, scratch, sizeof(scratch));
    CHECK(nosize.text == scratch);
    CHECK(nosize.advance == doctest::Approx(24.0f));

    // Empty answers: no label, an empty one, a marker-only one.
    CHECK(fit_text(nullptr, 100.0f, theme, scratch,
                   sizeof(scratch)).advance == 0.0f);
    CHECK(std::string_view(fit_text("", 100.0f, theme, scratch,
                                    sizeof(scratch)).text).empty());
    CHECK(fit_text("###only", 100.0f, theme, scratch,
                   sizeof(scratch)).advance == 0.0f);
}

TEST_CASE("fit_text: a width miss truncates to the longest fitting prefix + '...'") {
    using namespace ws::ui::gui;
    char scratch[128];
    const ChromeTheme& theme = kDefaultChrome;  // 6 px per byte

    // "Saving" estimates 36 px whole. It fits exactly: the label's own
    // bytes, no copy.
    const LabelFit whole =
        fit_text("Saving", 36.0f, theme, scratch, sizeof(scratch));
    CHECK(whole.text != scratch);
    CHECK(std::string_view(whole.text) == "Saving");
    CHECK(whole.advance == doctest::Approx(36.0f));

    // One pixel short: the longest prefix + dots ("Sa..." = 30) - the
    // truncation lands on a character boundary and never overflows the
    // budget.
    const LabelFit prefix =
        fit_text("Saving", 35.0f, theme, scratch, sizeof(scratch));
    CHECK(prefix.text == scratch);
    CHECK(std::string_view(prefix.text) == "Sa...");
    CHECK(prefix.advance == doctest::Approx(30.0f));

    // The boundary fits exactly: 30 at 30, then one shorter step each
    // time the budget drops past a candidate.
    CHECK(std::string_view(fit_text("Saving", 30.0f, theme, scratch,
                                    sizeof(scratch)).text) == "Sa...");
    CHECK(std::string_view(fit_text("Saving", 29.0f, theme, scratch,
                                    sizeof(scratch)).text) == "S...");
    CHECK(std::string_view(fit_text("Saving", 23.0f, theme, scratch,
                                    sizeof(scratch)).text) == "...");

    // Past the bare dots: nothing drawable - empty text, a zero
    // advance; a zero budget draws nothing either.
    const LabelFit none =
        fit_text("Saving", 17.0f, theme, scratch, sizeof(scratch));
    CHECK(none.advance == 0.0f);
    CHECK(std::string_view(none.text).empty());
    CHECK(fit_text("Saving", 0.0f, theme, scratch,
                   sizeof(scratch)).advance == 0.0f);

    // The scratch bounds the copy: a 130-byte label truncating into a
    // 128-byte scratch caps its prefix at 124 bytes, and the longest
    // FITTING prefix under the cap wins (97 bytes + the dots = 100
    // bytes = 600 px at a 600 budget).
    char long_label[131];
    for (int i = 0; i < 130; ++i) {
        long_label[i] = 'x';
    }
    long_label[130] = '\0';
    const LabelFit capped =
        fit_text(long_label, 600.0f, theme, scratch, sizeof(scratch));
    CHECK(capped.text == scratch);
    CHECK(capped.advance == doctest::Approx(600.0f));
    CHECK(std::string_view(capped.text).size() == 100);  // 97 + the dots
}

TEST_CASE("fit_text: the truncation never splits a UTF-8 codepoint") {
    using namespace ws::ui::gui;
    char scratch[128];
    const ChromeTheme& theme = kDefaultChrome;  // 6 px per byte

    // "\xC3\xA9" "AAA" ("e-acute" + AAA) is five bytes: whole = 30. At
    // a 24 budget the 2-byte prefix ("é" + dots = 5 bytes = 30) misses,
    // and the 1-byte cut would split the codepoint - the backup skips
    // it, and the answer is the bare "..." (18), never a lone lead
    // byte + dots (24).
    const LabelFit split =
        fit_text("\xC3\xA9" "AAA", 24.0f, theme, scratch, sizeof(scratch));
    CHECK(split.text == scratch);
    CHECK(std::string_view(split.text) == "...");
    CHECK(split.advance == doctest::Approx(18.0f));

    // "\xC3\xA9" "AAAA" (six bytes, whole 36) at 30: the whole
    // codepoint's prefix fits - é + dots (5 bytes = 30).
    const LabelFit kept =
        fit_text("\xC3\xA9" "AAAA", 30.0f, theme, scratch, sizeof(scratch));
    CHECK(std::string_view(kept.text) == "\xC3\xA9" "...");
    CHECK(kept.advance == doctest::Approx(30.0f));
}

TEST_CASE("widgets: a ## suffix ids the widget but never draws or sizes it") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;
    bool suffixed = false;
    bool plain = false;

    // Two checkboxes in one panel scope, one label apart: "signal" and
    // "signal##extra" - DIFFERENT widgets (the full string ids), one
    // display (both rows size to "signal": box 24 + gap 8 + 36 = 68
    // wide, not the 110 the full string would estimate).
    // Content (12, 12, 376, 276): row 0 at (12, 12, 68, 24), row 1 at
    // (12, 44, 68, 24).
    gui.begin_frame(snap(300.0f, 300.0f));
    (void)begin_panel(gui, layout, kDefaultChrome, "page",
                      {0.0f, 0.0f, 400.0f, 300.0f});
    CHECK(checkbox(gui, layout, kDefaultChrome, "signal##extra", suffixed)
          == false);
    CHECK(checkbox(gui, layout, kDefaultChrome, "signal", plain) == false);
    end_panel(gui, layout);
    gui.end_frame();

    // A click on row 0 (the suffixed one) toggles it and leaves the
    // plain one alone: the press owns, the release lands.
    gui.begin_frame(snap(16.0f, 16.0f, true, true));
    (void)begin_panel(gui, layout, kDefaultChrome, "page",
                      {0.0f, 0.0f, 400.0f, 300.0f});
    CHECK(!checkbox(gui, layout, kDefaultChrome, "signal##extra", suffixed));
    CHECK(!checkbox(gui, layout, kDefaultChrome, "signal", plain));
    end_panel(gui, layout);
    gui.end_frame();
    CHECK(suffixed == false);  // owned, no flip yet
    CHECK(plain == false);

    gui.begin_frame(snap(16.0f, 16.0f, false, false, true));
    (void)begin_panel(gui, layout, kDefaultChrome, "page",
                      {0.0f, 0.0f, 400.0f, 300.0f});
    CHECK(checkbox(gui, layout, kDefaultChrome, "signal##extra", suffixed));
    CHECK(!checkbox(gui, layout, kDefaultChrome, "signal", plain));
    end_panel(gui, layout);
    gui.end_frame();
    CHECK(suffixed == true);
    CHECK(plain == false);

    // A click at x 117 sits past the display-sized row (right edge 80)
    // but inside the full string's estimate (122): the suffix sized
    // nothing, so there is nothing to claim out there.
    gui.begin_frame(snap(117.0f, 16.0f, true, true));
    (void)begin_panel(gui, layout, kDefaultChrome, "page",
                      {0.0f, 0.0f, 400.0f, 300.0f});
    CHECK(!checkbox(gui, layout, kDefaultChrome, "signal##extra", suffixed));
    CHECK(!checkbox(gui, layout, kDefaultChrome, "signal", plain));
    end_panel(gui, layout);
    gui.end_frame();

    gui.begin_frame(snap(117.0f, 16.0f, false, false, true));
    (void)begin_panel(gui, layout, kDefaultChrome, "page",
                      {0.0f, 0.0f, 400.0f, 300.0f});
    CHECK(!checkbox(gui, layout, kDefaultChrome, "signal##extra", suffixed));
    CHECK(!checkbox(gui, layout, kDefaultChrome, "signal", plain));
    end_panel(gui, layout);
    gui.end_frame();
    CHECK(suffixed == true);  // unchanged: the click landed on nothing
    CHECK(plain == false);
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

    // Ten rows at a 32-px pitch (24-tall buttons plus the theme's 8-px
    // gaps) = 312 px of content; the scroll range is 312 - 76 = 236.
    // Two click snapshots recur through the frames: a full click at the
    // unscrolled row 0's center, and one at row 3's visible center at
    // offset 32 (the row pokes past the viewport; the click lands
    // mid-visible, clear of the clip's edges).
    const InputFrame click_origin = snap(60.0f, 24.0f, false, true, true);
    const InputFrame click_row3 = snap(60.0f, 82.0f, false, true, true);

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
    // (kDefaultChrome.scroll_speed) - and row 3, laid out at y 76..100
    // (content 108..132 minus the scroll, visible 76..88), takes the same
    // frame's click at its visible center.
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

    // Frame 6: a huge wheel-down clamps at the bottom (offset 236): row 9
    // spans y 64..88, its bottom edge EXACTLY the viewport's bottom (the
    // clamp always parks the content's bottom on the viewport's).
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

    // Frames 8-9: the content shrinks to two rows (56 px - shorter than
    // the viewport). Frame 8 is the documented one-frame lag: the offset
    // still holds 236 against last frame's range, so the two rows flow far
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

TEST_CASE("scroll: clear_persistent_state resets a region's place") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;

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

    // Prime (frame 1 measures the range and settles the wheel claim),
    // scroll one notch (frame 2), hold (frame 3): offset 32.
    (void)pass(snap(100.0f, 50.0f), 10);
    InputFrame f = snap(100.0f, 50.0f);
    f.scroll_dy = -1.0f;
    (void)pass(f, 10);
    (void)pass(snap(100.0f, 50.0f), 10);

    // At offset 32 row 1 spans screen y 12..36: the click that was
    // row 0's (at its unscrolled spot) is row 1's now.
    CHECK(pass(snap(60.0f, 24.0f, false, true, true), 10).second == 1);

    // The wholesale clear - between frames, the documented moment. The
    // offset entry is gone, so the region starts over at the top: the
    // same click is row 0's again.
    gui.clear_persistent_state();
    CHECK(pass(snap(60.0f, 24.0f, false, true, true), 10).second == 0);
}

TEST_CASE("scroll: the wheel is dead while no mouse is present") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;

    // The same flow-measuring pass as the wheel test above; the frames
    // below differ only in mouse_present.
    const auto pass = [&](const InputFrame& frame) {
        gui.begin_frame(frame);
        (void)begin_scroll_panel(
            gui, layout, kDefaultChrome, "list", {0.0f, 0.0f, 200.0f, 100.0f});
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

    // Frame 1: present and idle - measures the scroll range (ten 24-px
    // rows against the 76-px viewport: 164) for what follows.
    CHECK(pass(snap(100.0f, 50.0f)) == -1);

    // Frame 2: an absent mouse wheels over the panel. A touch-only
    // device's scroll_dy is never nonzero in practice, but a cursor that
    // just left the window (or any hand-built frame) must not scroll -
    // the presence flag gates the whole device, not the position.
    InputFrame ghost = snap(100.0f, 50.0f);
    ghost.scroll_dy = -1.0f;
    ghost.mouse_present = false;
    CHECK(pass(ghost) == -1);

    // Frame 3: the click at the unscrolled origin still lands on row 0 -
    // frame 2 moved nothing.
    CHECK(pass(snap(60.0f, 24.0f, false, true, true)) == 0);

    // Frame 4: the present mouse wheels - 32 px, as always.
    InputFrame wheel = snap(100.0f, 50.0f);
    wheel.scroll_dy = -1.0f;
    CHECK(pass(wheel) == -1);

    // Frame 5: the same origin click now lands on row 1 - row 0 scrolled
    // out from under it (it sits at y -20..4).
    CHECK(pass(snap(60.0f, 24.0f, false, true, true)) == 1);
}

TEST_CASE("scroll: two regions keep independent offsets") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;

    // Two side-by-side panels, five 24-tall rows each (152 px of content
    // at the 32-px pitch, range 152 - 76 = 76). Returns which row a click
    // landed on per panel.
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

    // Frame 1: idle, pointer parked INSIDE the left viewport - both
    // panels measure their range this frame, and the left one settles
    // the wheel arbitration (the winner publishes one frame late, like
    // hot_prev, so frame 2's wheel needs this frame's registration).
    int a = -2, b = -2;
    pass(snap(60.0f, 50.0f), a, b);
    CHECK(a == -1);
    CHECK(b == -1);

    // Frame 2: the pointer sits inside the LEFT viewport only; its wheel
    // scrolls the left panel 32 px and must not touch the right one.
    InputFrame f = snap(60.0f, 50.0f);
    f.scroll_dy = -1.0f;
    pass(f, a, b);
    CHECK(a == -1);
    CHECK(b == -1);

    // Frame 3: the left panel's row 3 (y 76..100 at offset 32, visible
    // 76..88) takes the click at its visible center - the right panel
    // would put row 3 at y 108..132.
    pass(snap(60.0f, 82.0f, false, true, true), a, b);
    CHECK(a == 3);
    CHECK(b == -1);

    // Frame 4: the RIGHT panel's row 0 still sits at its unscrolled y
    // (12..36) - had it scrolled 32, row 1 would sit at y 12..36 and take
    // this click (row 0's spot) instead.
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
        // The outer cursor resumes below the header plus one spacing,
        // parked while the scroll region flowed its own rows.
        const Rect after = layout.next(50.0f, 10.0f);
        CHECK(after.y == 44.0f);  // the header's bottom (36) + spacing (8)
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

    // An idle pass: five rows measure the inner range - 152 px of content
    // (32-px pitch) against the viewport (24, 24, 76, 76) => 76. The
    // pointer parks inside the inner viewport so the NEXT frame's wheel
    // settles on the innermost claimant (both panels claim; the smaller
    // bounds win).
    int clicked = -2;
    pass(snap(60.0f, 76.0f), clicked);
    CHECK(clicked == -1);

    // A wheel unit scrolls the inner region 32 - row 3, laid out at y
    // 88..112 (visible 88..100), takes the click at its visible center;
    // the header, laid out by the OUTER panel, never moved.
    InputFrame f = snap(60.0f, 94.0f, false, true, true);
    f.scroll_dy = -1.0f;
    pass(f, clicked);
    CHECK(clicked == 3);
}

TEST_CASE("scroll: the wheel settles one frame, then the innermost panel "
          "takes it") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;

    // Nested scroll panels: an outer 200x300 (viewport 12,12,176,276,
    // range 196 over fifteen 24-px rows at the 32-px pitch) with an inner
    // 88x100 (viewport 112,24,64,76, range 76 over five rows) sitting to
    // the right of the outer's own narrow rows, so a click at either side
    // answers its panel alone. The pass returns which row each side
    // landed on.
    const auto pass = [&](const InputFrame& frame, int& outer, int& inner) {
        gui.begin_frame(frame);
        (void)begin_scroll_panel(gui, layout, kDefaultChrome, "outer",
                                 {0.0f, 0.0f, 200.0f, 300.0f});
        outer = -1;
        for (int i = 0; i < 15; ++i) {
            gui.ids().push(i);
            if (button(gui, layout, kDefaultChrome, "row", 80.0f)) {
                outer = i;
            }
            gui.ids().pop();
        }
        (void)begin_scroll_panel(gui, layout, kDefaultChrome, "inner",
                                 {100.0f, 12.0f, 88.0f, 100.0f});
        inner = -1;
        for (int i = 0; i < 5; ++i) {
            gui.ids().push(i);
            if (button(gui, layout, kDefaultChrome, "row", 60.0f)) {
                inner = i;
            }
            gui.ids().pop();
        }
        end_scroll_panel(gui, layout, kDefaultChrome);
        end_scroll_panel(gui, layout, kDefaultChrome);
        gui.end_frame();
    };

    // The pointer starts AWAY, so the next frame over the inner panel
    // has no settled winner yet: its wheel must settle, not scroll -
    // this pins the arbitration lag apart from the range lag, which
    // this same idle frame measures away.
    int outer = -2, inner = -2;
    pass(snap(400.0f, 400.0f), outer, inner);
    CHECK(outer == -1);
    CHECK(inner == -1);

    // Over the inner viewport (inside both panels' bounds), wheel down:
    // no scroll - the winner publishes at THIS frame's end, one frame
    // behind the hover like hot_prev. Inner row 0 still sits at its
    // content spot y 24..48; the click at its center proves it (and
    // lands on no outer row - the inner's rows start at x 112).
    InputFrame f = snap(130.0f, 60.0f);
    f.scroll_dy = -1.0f;
    pass(f, outer, inner);
    CHECK(inner == -1);
    f = snap(142.0f, 36.0f, false, true, true);
    pass(f, outer, inner);
    CHECK(inner == 0);
    CHECK(outer == -1);

    // The second notch over the inner viewport scrolls - the INNERMOST
    // panel won the settle (both claim the pointer; the smaller bounds
    // take it), so the inner moves 32 and the outer moves nothing:
    // the nested double-scroll defect. Inner row 3 (content y 120..144)
    // lands at 88..112 (visible 88..100); the outer's row 0 never left
    // y 12..36.
    f = snap(130.0f, 60.0f);
    f.scroll_dy = -1.0f;
    pass(f, outer, inner);
    f = snap(142.0f, 94.0f, false, true, true);
    pass(f, outer, inner);
    CHECK(inner == 3);
    f = snap(52.0f, 24.0f, false, true, true);
    pass(f, outer, inner);
    CHECK(outer == 0);
}

TEST_CASE("scroll: a popup panel eats the wheel over the panel below") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;

    // A scroll panel under a popup-layer panel that OVERLAPS the
    // viewport: the popup registers as a claimant like every panel, and
    // its higher layer eats the wheel even though it scrolls nothing -
    // the popup-over-a-panel scroll defect. The popup panel is drawn
    // chrome-only (begin_panel, no widgets), so the rows below stay
    // clickable outside its rect.
    const auto pass = [&](const InputFrame& frame, bool popup, int& clicked) {
        gui.begin_frame(frame);
        (void)begin_scroll_panel(gui, layout, kDefaultChrome, "list",
                                 {0.0f, 0.0f, 200.0f, 100.0f});
        clicked = -1;
        for (int i = 0; i < 10; ++i) {
            gui.ids().push(i);
            if (button(gui, layout, kDefaultChrome, "row", 120.0f)) {
                clicked = i;
            }
            gui.ids().pop();
        }
        end_scroll_panel(gui, layout, kDefaultChrome);
        if (popup) {
            gui.begin_layer(Layer::popup);
            (void)begin_panel(gui, layout, kDefaultChrome, "over",
                              {50.0f, 20.0f, 100.0f, 50.0f});
            end_panel(gui, layout);
            gui.end_layer();
        }
        gui.end_frame();
    };

    // Settle the winner with the popup up: it claims at layer 20 over
    // the base panel's 10, so the resolution must hand it the wheel.
    int clicked = -2;
    pass(snap(100.0f, 50.0f), true, clicked);
    CHECK(clicked == -1);

    // The wheel under the popup: dead. Row 0 keeps its unscrolled
    // spot - the click left of the popup's rect proves it.
    InputFrame f = snap(100.0f, 50.0f);
    f.scroll_dy = -1.0f;
    pass(f, true, clicked);
    pass(snap(20.0f, 24.0f, false, true, true), true, clicked);
    CHECK(clicked == 0);

    // The popup gone: the settle hands the wheel back to the panel, and
    // the same notch now scrolls 32 - row 1 takes the origin click.
    pass(snap(100.0f, 50.0f), false, clicked);
    f.scroll_dy = -1.0f;
    pass(f, false, clicked);
    pass(snap(20.0f, 24.0f, false, true, true), false, clicked);
    CHECK(clicked == 1);
}

TEST_CASE("scroll: the modal veil kills the wheel - and the raw-input "
          "gestures with it") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;

    // The veil is frame-wide and blocks the strata under it: the wheel
    // resolves over UNVEILED claims only, so a scroll panel below a
    // modal goes inert even with nothing drawn over it. The raw-input
    // gestures (the touch arm, and the thumb grab) gate on the same
    // claims_open() the hit rules answer with, or a scroll would drag
    // through a dialog. The modal scope opens BEFORE the panel it
    // blocks - the documented pattern, so the veil is already up when
    // the panel's begin runs its claims.
    const auto pass = [&](const InputFrame& frame, bool veil, int& clicked) {
        gui.begin_frame(frame);
        if (veil) {
            gui.begin_layer(Layer::modal);
            (void)begin_panel(gui, layout, kDefaultChrome, "dialog",
                              {150.0f, 150.0f, 40.0f, 20.0f});
            end_panel(gui, layout);
            CHECK(gui.claims_open());  // the modal's own chrome stays live
            gui.end_layer();
        }
        (void)begin_scroll_panel(gui, layout, kDefaultChrome, "list",
                                 {0.0f, 0.0f, 200.0f, 100.0f});
        clicked = -1;
        for (int i = 0; i < 10; ++i) {
            gui.ids().push(i);
            if (button(gui, layout, kDefaultChrome, "row", 120.0f)) {
                clicked = i;
            }
            gui.ids().pop();
        }
        end_scroll_panel(gui, layout, kDefaultChrome);
        gui.end_frame();
    };

    // A quiet frame under the veil: the claim registers (the panel
    // still runs) but the resolution drops it - the veil won.
    int clicked = -2;
    pass(snap(100.0f, 50.0f), true, clicked);
    CHECK(clicked == -1);
    CHECK(!gui.claims_open());  // and after the scope, the base is shut

    // The wheel below the veil: dead, veil or not.
    InputFrame f = snap(100.0f, 50.0f);
    f.scroll_dy = -1.0f;
    pass(f, true, clicked);
    CHECK(clicked == -1);

    // A touch press on a row under the veil: the row cannot claim
    // (hit_test blocks), and the ARM must not fire either - drag the
    // contact UP well past the threshold and the rows must stay put.
    // A fired arm would scroll +60 by the lift, and the origin click
    // below would then find row 4, not row 1.
    pass(touch_frame({{4, 60.0f, 80.0f, true, false}}), true, clicked);
    CHECK(clicked == -1);
    pass(touch_frame({{4, 60.0f, 20.0f}}), true, clicked);  // 60 px up, no drag
    pass(touch_frame({{4, 60.0f, 20.0f, false, true}}), true, clicked);
    CHECK(clicked == -1);  // the veiled row never had the press

    // The veil lifts (a frame with no modal scope) and the wheel lives
    // again: settle, scroll, and the origin click finds row 1.
    pass(snap(100.0f, 50.0f), false, clicked);
    f.scroll_dy = -1.0f;
    pass(f, false, clicked);
    pass(snap(20.0f, 24.0f, false, true, true), false, clicked);
    CHECK(clicked == 1);
}

TEST_CASE("scroll: a nested touch drag takes only the innermost panel") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;

    // The nested pair from the wheel-arbitration test: an outer
    // 200x300 (viewport 12,12,176,276) with fifteen narrow rows, an
    // inner 88x100 (viewport 112,24,64,76) with five rows to the
    // right. A finger pressing inside the inner viewport sits inside
    // BOTH panels' viewports - the defect had both panels drag.
    const auto pass = [&](const InputFrame& frame, int& outer, int& inner) {
        gui.begin_frame(frame);
        (void)begin_scroll_panel(gui, layout, kDefaultChrome, "outer",
                                 {0.0f, 0.0f, 200.0f, 300.0f});
        outer = -1;
        for (int i = 0; i < 15; ++i) {
            gui.ids().push(i);
            if (button(gui, layout, kDefaultChrome, "row", 80.0f)) {
                outer = i;
            }
            gui.ids().pop();
        }
        (void)begin_scroll_panel(gui, layout, kDefaultChrome, "inner",
                                 {100.0f, 12.0f, 88.0f, 100.0f});
        inner = -1;
        for (int i = 0; i < 5; ++i) {
            gui.ids().push(i);
            if (button(gui, layout, kDefaultChrome, "row", 60.0f)) {
                inner = i;
            }
            gui.ids().pop();
        }
        end_scroll_panel(gui, layout, kDefaultChrome);
        end_scroll_panel(gui, layout, kDefaultChrome);
        gui.end_frame();
    };

    // Press on the inner's row 2 (content y 88..112; its visible center
    // is 94 - the clip cuts the row at the viewport's bottom edge) - the
    // row claims the tap in progress; both panels' begins watch it,
    // and the arm claim hands the drag to the inner (its begin runs
    // after the outer's, later wins).
    const auto inner_row_id = [](int row) {
        IdStack ids;
        ids.push("outer");
        ids.push("inner");
        ids.push(row);
        return ids.id("row");
    };
    int outer = -2, inner = -2;
    pass(touch_frame({{4, 130.0f, 94.0f, true, false}}), outer, inner);
    CHECK(inner == -1);
    CHECK(gui.active_id(PointerId{4}) == inner_row_id(2));  // the tap in
                                                           // progress

    // Up 14 px: past the 8-px threshold. The inner crossed and owns the
    // finger - its row's press is cancelled, no click on the lift -
    // and the drag starts at the crossing.
    pass(touch_frame({{4, 130.0f, 80.0f}}), outer, inner);
    CHECK(inner == -1);
    CHECK(gui.active_id(PointerId{4}) ==
          kInvalidWidgetId);  // the crossing cancelled the row's press

    // Up 10 more, and lift: the INNER scrolled 10 (finger up = later
    // content); the cancelled row clicks nothing on the lift, and the
    // outer's watched track dropped silently - the outer's rows never
    // moved.
    pass(touch_frame({{4, 130.0f, 70.0f, false, true}}), outer, inner);
    CHECK(inner == -1);  // cancelled at the crossing: the lift clicks nothing

    // The outer's row 0 is exactly where it started, and the inner's
    // row 0 (content y 24..48, now at 14..38 after the 10-px scroll)
    // takes a click at its scrolled center. A click answers one panel:
    // the outer's spot is x 50 (its rows span 12..92), the inner's x
    // 130 (rows 112..172).
    pass(snap(50.0f, 24.0f, false, true, true), outer, inner);
    CHECK(outer == 0);
    pass(snap(130.0f, 26.0f, false, true, true), outer, inner);
    CHECK(inner == 0);
}

TEST_CASE("scroll: the thumb is grabbable - the gutter jumps to the "
          "pointer") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;

    // Ten WIDE rows (x 12..188 - the full viewport width, so the right
    // gutter band x 176..188 sits over row pixels: a press there must
    // be taken away from the row). Content 240, viewport 76, range 164;
    // the thumb is 76*76/240 = 24.07 px tall, so the track travels
    // 51.93 px for the full 164.
    const auto pass = [&](const InputFrame& frame, int& clicked) {
        gui.begin_frame(frame);
        (void)begin_scroll_panel(gui, layout, kDefaultChrome, "list",
                                 {0.0f, 0.0f, 200.0f, 100.0f});
        clicked = -1;
        for (int i = 0; i < 10; ++i) {
            gui.ids().push(i);
            if (button(gui, layout, kDefaultChrome, "row", 176.0f)) {
                clicked = i;
            }
            gui.ids().pop();
        }
        end_scroll_panel(gui, layout, kDefaultChrome);
        gui.end_frame();
    };

    // Idle: measure the range and settle the wheel (irrelevant here,
    // but the panel must run a full frame before the grab).
    int clicked = -2;
    pass(snap(60.0f, 50.0f), clicked);
    CHECK(clicked == -1);

    // Press ON the thumb (y 24, inside its 12..36.07 span at offset 0)
    // in the gutter over row 0: the row claims the press for the
    // widgets' one frame, then the grab cancels it - on the lift the
    // row must not click. The grab keeps the pixel under the pointer,
    // so the offset stays 0 this frame.
    pass(snap(180.0f, 24.0f, true, true), clicked);
    CHECK(clicked == -1);

    // Drag down to y 60, held: the offset maps through the track -
    // (60 - 12 - 12) / 51.93 * 164 = 113.66 - and the lift there
    // leaves it. The wide row under the band gets NO click from the
    // cancelled press.
    pass(snap(180.0f, 60.0f, true), clicked);
    CHECK(clicked == -1);
    pass(snap(180.0f, 60.0f, false, false, true), clicked);
    CHECK(clicked == -1);

    // The offset held: row 5 (content y 132..156) sits at 18.3..42.3
    // and takes a click at its scrolled center; row 0 is long gone
    // from y 12..36 (it is at -101.7..-77.7).
    pass(snap(60.0f, 30.0f, false, true, true), clicked);
    CHECK(clicked == 5);

    // A press in the gutter BELOW the thumb (the thumb now spans
    // y 48..72) jumps it centered under the pointer: y 30 parks the
    // thumb top at 30 - 12.03 = 17.97, offset (17.97 - 12) / 51.93 *
    // 164 = 18.9. The lift is quiet again (a row sits under every
    // gutter pixel - the cancel owns each of these presses).
    pass(snap(180.0f, 30.0f, true, true), clicked);
    CHECK(clicked == -1);
    pass(snap(180.0f, 30.0f, false, false, true), clicked);
    CHECK(clicked == -1);

    // Row 1 (content y 36..60) now sits at 17.1..41.1 and takes the
    // click at its scrolled center - row 5 would be at 113.1..137.1.
    pass(snap(60.0f, 30.0f, false, true, true), clicked);
    CHECK(clicked == 1);
}

TEST_CASE("scroll: Tab lands the focus in view") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;

    // Eleven checkbox rows (24 px each) in a 200x100 panel: content
    // 264 against the 76-px viewport, range 188. The checkboxes
    // register focusable, so Tab walks them; the panel shifts the
    // offset to pull the newly focused row into the viewport - the
    // frame after the landing, the row latches its bounds first.
    const auto pass = [&](InputFrame frame, int& clicked) {
        gui.begin_frame(frame);
        (void)begin_scroll_panel(gui, layout, kDefaultChrome, "list",
                                 {0.0f, 0.0f, 200.0f, 100.0f});
        clicked = -1;
        for (int i = 0; i < 11; ++i) {
            gui.ids().push(i);
            static bool s_row_values[11] = {};
            if (checkbox(gui, layout, kDefaultChrome, "row",
                         s_row_values[static_cast<std::size_t>(i)])) {
                clicked = i;
            }
            gui.ids().pop();
        }
        end_scroll_panel(gui, layout, kDefaultChrome);
        gui.end_frame();
    };
    const auto row_id = [](int row) {
        IdStack ids;
        ids.push("list");
        ids.push(row);
        return ids.id("row");
    };

    // Idle, then nine Tab frames: the first lands the focus on row 0
    // (nothing held it), the rest walk to row 8 - content y 204..228,
    // far below the viewport's bottom edge at 88.
    int clicked = -2;
    pass(snap(400.0f, 400.0f), clicked);
    for (int tab = 0; tab < 9; ++tab) {
        InputFrame f = snap(400.0f, 400.0f);
        f.key_pressed[keys::kTab] = true;
        pass(f, clicked);
    }
    CHECK(gui.is_focused(row_id(8)));
    CHECK(clicked == -1);  // no pointer ever pressed anything

    // The landing queued the into-view shift for THIS frame; the row
    // ran and latched, and the panel's end pulled it in: offset 140
    // (228 - 88, within the 188 range). Row 8 now sits at 64..88 - a
    // click at its scrolled center finds it, and the focus holds.
    pass(snap(400.0f, 400.0f), clicked);
    CHECK(clicked == -1);
    pass(snap(60.0f, 76.0f, false, true, true), clicked);
    CHECK(clicked == 8);
    CHECK(gui.is_focused(row_id(8)));

    // One more Tab walks to row 9 (content y 228..252, latch at
    // 88..112): the shift lands it at 64..88 the same way.
    InputFrame tab = snap(400.0f, 400.0f);
    tab.key_pressed[keys::kTab] = true;
    pass(tab, clicked);
    CHECK(gui.is_focused(row_id(9)));
    pass(snap(400.0f, 400.0f), clicked);  // the into-view frame
    pass(snap(60.0f, 76.0f, false, true, true), clicked);
    CHECK(clicked == 9);

    // Shift+Tab walks BACK to row 8 - at offset 164 it sits at
    // 40..64, already fully in view, and the shift is a no-op: the
    // same click spot finds row 8 again, not a scrolled-away row.
    InputFrame back = snap(400.0f, 400.0f);
    back.key_pressed[keys::kTab] = true;
    back.key_down[static_cast<std::size_t>(keys::kLeftShift)] = true;
    pass(back, clicked);
    CHECK(gui.is_focused(row_id(8)));
    pass(snap(400.0f, 400.0f), clicked);
    pass(snap(60.0f, 52.0f, false, true, true), clicked);
    CHECK(clicked == 8);
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

    // Frame 1: finger 7 presses row 2 (y 76..100, at its visible center
    // 82). The child claims the press - the tap is the CHILD's so far,
    // the panel only watches.
    CHECK(pass(touch_frame({{7, 60.0f, 82.0f, /*pressed=*/true}})) == -1);
    CHECK(gui.active_id(PointerId{7}) == row_id(2));

    // Frame 2: 20 px up - past the 8 px threshold. The crossing frame
    // cancels the child's press (finger 7 will never click now) and
    // applies NO jump: the offset is still 0, so the probe tap at
    // (60, 24) still lands on row 0.
    CHECK(pass(touch_frame({{7, 60.0f, 62.0f},
                            {99, 60.0f, 24.0f, true, true}})) == 0);
    CHECK(gui.active_id(PointerId{7}) == kInvalidWidgetId);  // the scroll owns the finger

    // Frame 3: 30 px more of drag - the offset follows 1:1. At offset 30
    // row 1 spans y 14..38, so the same probe point now clicks row 1.
    CHECK(pass(touch_frame({{7, 60.0f, 32.0f},
                            {99, 60.0f, 24.0f, true, true}})) == 1);

    // Frame 4: finger 7 lifts at (60, 32) - INSIDE row 1's scrolled rect
    // (y 14..38). The cancelled press means its release clicks nothing.
    CHECK(pass(touch_frame({{7, 60.0f, 32.0f, false, true}})) == -1);

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

    // Hand-built frames, the mouse NOT parked but PRESENT: the first
    // contact bridges and the mouse mirrors it - the duplicate shape, a
    // real mouse plus a dragging finger (Android + Bluetooth), where the
    // finger is two pointers at once. A touch-only device lists the finger
    // alone, and the cancel's mirror has nothing to pair. Without
    // cancel_pointer's mirror rule the mouse's copy of the press would
    // survive the cancel and click the row on release.
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

    // Frame 1: contact 7 presses row 2 (at its visible center, y 82) -
    // the mouse mirrors the press, so BOTH tracks claim the row (the
    // documented duplicate).
    InputFrame press;
    press.touch_down(PointerId{7}, 60.0f, 82.0f);
    press.mouse_present = true;
    CHECK(pass(press) == -1);
    CHECK(gui.active_id() == row_id(2));  // the mouse's track pressed...
    CHECK(gui.active_id(PointerId{7}) == row_id(2));  // ...and so did the touch's

    // Frame 2: 20 px up - the crossing. cancel_pointer(7) must take the
    // mouse's claim down with the touch's.
    InputFrame cross;
    cross.touch_down(PointerId{7}, 60.0f, 82.0f);  // the entry; bridges again
    cross.touch_move(PointerId{7}, 60.0f, 62.0f);  // the bridge mirrors the position
    cross.touches[0].pressed = false;  // held, not pressed: clear the
    cross.mouse_pressed[0] = false;    // edges both sides of the bridge
    cross.mouse_present = true;
    CHECK(pass(cross) == -1);
    CHECK(gui.active_id() == kInvalidWidgetId);  // BOTH claims cancelled
    CHECK(gui.active_id(PointerId{7}) == kInvalidWidgetId);

    // Frame 3: drag back down to the press point - the offset would go
    // negative and clamps at 0, so a probe at (60, 24) still clicks row 0.
    // The probe is a same-frame tap: its press edge STAYS (a click is
    // press-then-release; a bare release edge clicks nothing).
    InputFrame back;
    back.touch_down(PointerId{7}, 60.0f, 82.0f);
    back.touch_down(PointerId{99}, 60.0f, 24.0f);  // the probe (not the bridge - 7 is)
    back.touch_up(PointerId{99}, 60.0f, 24.0f, false);
    back.touches[0].pressed = false;
    back.mouse_pressed[0] = false;
    back.mouse_present = true;
    CHECK(pass(back) == 0);

    // Frame 4: release exactly where it pressed - row 2 still spans
    // y 76..100, so the lift is INSIDE the row. The cancelled press clicks
    // nothing; had the mouse's claim survived, this would click row 2.
    InputFrame lift;
    lift.touch_down(PointerId{7}, 60.0f, 82.0f);  // the entry, back for its lift
    lift.touch_up(PointerId{7}, 60.0f, 82.0f, false);  // mirrors the mouse's release
    lift.touches[0].pressed = false;  // down+up in one frame: the press
    lift.mouse_pressed[0] = false;    // edge is not this frame's fact
    lift.mouse_present = true;
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

    // Frame 1: two presses inside the viewport, rows 0 and 1.
    CHECK(pass(touch_frame({{1, 60.0f, 30.0f, /*pressed=*/true},
                            {2, 60.0f, 60.0f, /*pressed=*/true}})) == -1);

    // Frame 2: both move 20 px up - both cross, both cancel; no jump.
    CHECK(pass(touch_frame({{1, 60.0f, 10.0f},
                            {2, 60.0f, 40.0f}})) == -1);

    // Frame 3: both drag 10 px more - BOTH deltas apply (offset 20): at
    // offset 20 row 1 spans y 24..48 and the probe at (60, 24) clicks
    // row 1 (its scrolled top edge). A single-track drag would sit at
    // offset 10 and click row 0.
    CHECK(pass(touch_frame({{1, 60.0f, 0.0f},
                            {2, 60.0f, 30.0f},
                            {99, 60.0f, 24.0f, true, true}})) == 1);

    // Frame 4: finger 1 lifts; finger 2 drags 10 more (offset 30) - the
    // survivor's bookkeeping is its own. At offset 30 row 1 spans
    // y 14..38 and the probe at (60, 14) clicks row 1 (at offset 20 it
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

// --- gui hit clips (the clip-aware hit seam) ---------------------------------------

TEST_CASE("hit clips: a scrolled-out row cannot claim what sits under the panel") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;

    // The reported bug, end to end: 11 rows of 24 px at the 32-px pitch
    // = 344 px of content inside a 200x100 panel (viewport (12, 12, 176,
    // 76), scroll range 268). Even unscrolled, the rows past y 88 sit
    // below the viewport, and scrolled to offset 100 they straddle both
    // ends - rows 0..2 above, row 3 poking past the top, row 5 past the
    // bottom, rows 6..10 fully below. A widget UNDER the panel must win
    // any press landing below the viewport; the pass probes one such
    // rect after the panel closes (root scope - the panel popped its id
    // and hit-clip scopes).
    const auto pass = [&](const InputFrame& frame, HitTest& under) {
        gui.begin_frame(frame);
        (void)begin_scroll_panel(gui, layout, kDefaultChrome, "list",
                                 {0.0f, 0.0f, 200.0f, 100.0f});
        int clicked = -1;
        for (int i = 0; i < 11; ++i) {
            gui.ids().push(i);
            if (button(gui, layout, kDefaultChrome, "row", 120.0f)) {
                clicked = i;
            }
            gui.ids().pop();
        }
        end_scroll_panel(gui, layout, kDefaultChrome);
        const WidgetId under_id = gui.ids().id("under");
        under = gui.hit_test(under_id, {60.0f, 88.0f, 120.0f, 40.0f});
        gui.end_frame();
        return clicked;
    };

    // Frame 1: idle, pointer inside the viewport - measures the range.
    HitTest under;
    CHECK(pass(snap(60.0f, 50.0f), under) == -1);
    CHECK(!under.pressed);

    // A click at (60, 116), below the viewport at offset 0: row 3 spans
    // y 108..132 there - fully hidden by the clip, so without the fix
    // it wins the first-hit-test press and the under-panel widget sees
    // nothing. With it the row claims nothing and the widget below the
    // panel takes the click.
    CHECK(pass(snap(60.0f, 116.0f, false, true, true), under) == -1);
    CHECK(under.pressed);
    CHECK(under.clicked);

    // Park the pointer back inside the viewport: the click frame above
    // sat outside it, so the wheel arbitration settled on nobody -
    // this idle frame re-settles the panel as the winner for the wheel
    // frame below (the one-frame claim settle).
    CHECK(pass(snap(60.0f, 50.0f), under) == -1);
    CHECK(!under.pressed);

    // -3.125 wheel units = exactly 100 px of offset. Rows now flow
    // from y -88: row 4 spans 40..64, row 5 straddles the bottom edge
    // (72..88 visible), row 6 spans 104..128 - fully below again.
    InputFrame wheel = snap(60.0f, 50.0f);
    wheel.scroll_dy = -100.0f / kDefaultChrome.scroll_speed;
    CHECK(pass(wheel, under) == -1);

    // The same spot at the scrolled offset - hidden row 6's center -
    // still belongs to the widget below the panel, not to the row.
    CHECK(pass(snap(60.0f, 116.0f, false, true, true), under) == -1);
    CHECK(under.pressed);
    CHECK(under.clicked);

    // The visible rows still claim at their scrolled positions: row 4
    // (40..64) takes a click at its center.
    CHECK(pass(snap(60.0f, 52.0f, false, true, true), under) == 4);

    // Row 5 straddles the bottom edge: its visible part claims (y 80),
    // its clipped part (y 92, past the edge at 88) does not - the
    // under-panel widget owns that spot instead.
    CHECK(pass(snap(60.0f, 80.0f, false, true, true), under) == 5);
    CHECK(pass(snap(60.0f, 92.0f, false, true, true), under) == -1);
    CHECK(under.pressed);
    CHECK(under.clicked);
}

TEST_CASE("hit clips: a plain panel's children claim only inside the content rect") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;

    // begin_panel pushes the hit clip at the same rect it hands
    // r2d::push_clip - the content rect. A child poking past the panel
    // claims inside the content and goes dead outside it, exactly what
    // the renderer's clip does to its pixels. Panel {0,0,100,60},
    // padding 12: content (12, 12, 76, 36) - right edge 88, bottom 48.
    // The child rect spans x 50..150, y 40..80: claimable only at its
    // overlap with the content (x 50..88, y 40..48).
    const Rect poking{50.0f, 40.0f, 100.0f, 40.0f};

    // Inside the overlap: a plain click.
    gui.begin_frame(snap(60.0f, 44.0f, false, true, true));
    (void)begin_panel(gui, layout, kDefaultChrome, "page",
                      {0.0f, 0.0f, 100.0f, 60.0f});
    const Rect* clip = gui.current_hit_clip();
    REQUIRE(clip != nullptr);
    CHECK(clip->x == 12.0f);
    CHECK(clip->y == 12.0f);
    CHECK(clip->width == 76.0f);
    CHECK(clip->height == 36.0f);
    const HitTest inside = gui.hit_test(gui.ids().id("child"), poking);
    CHECK(inside.pressed);
    CHECK(inside.clicked);
    end_panel(gui, layout);
    CHECK(gui.current_hit_clip() == nullptr);  // the pop undid the push
    gui.end_frame();

    // Outside the content ((100, 60) is inside the child's full rect,
    // past both content edges): dead - what the clip hides from the
    // pixels cannot claim the pointer.
    gui.begin_frame(snap(100.0f, 60.0f, false, true, true));
    (void)begin_panel(gui, layout, kDefaultChrome, "page",
                      {0.0f, 0.0f, 100.0f, 60.0f});
    const HitTest dead = gui.hit_test(gui.ids().id("child"), poking);
    CHECK(!dead.pressed);
    CHECK(!dead.clicked);
    end_panel(gui, layout);
    gui.end_frame();

    // The panel closed, the clip gone: the same rect claims everywhere
    // again - the pop handed the whole screen back to the pointer.
    gui.begin_frame(snap(100.0f, 60.0f, false, true, true));
    const HitTest unclipped = gui.hit_test(gui.ids().id("child"), poking);
    CHECK(unclipped.pressed);
    CHECK(unclipped.clicked);
    gui.end_frame();
}

TEST_CASE("hit clips: the seam - nesting, overflow, balance, frame reset") {
    using namespace ws::ui::gui;
    GuiContext gui;

    // The public seam, driven directly. The probe rect covers everything
    // - only the clips can veto a claim.
    const Rect big{0.0f, 0.0f, 400.0f, 300.0f};
    const auto click = [](float x, float y) {
        return snap(x, y, false, true, true);
    };

    // Nesting intersects at push time: {50,50,100,100} inside
    // {0,0,100,100} stores {50,50,50,50} - the effective clip, not the
    // pushed rect.
    gui.begin_frame(snap(300.0f, 300.0f));
    gui.push_hit_clip({0.0f, 0.0f, 100.0f, 100.0f});
    gui.push_hit_clip({50.0f, 50.0f, 100.0f, 100.0f});
    const Rect* top = gui.current_hit_clip();
    REQUIRE(top != nullptr);
    CHECK(top->x == 50.0f);
    CHECK(top->y == 50.0f);
    CHECK(top->width == 50.0f);
    CHECK(top->height == 50.0f);
    gui.end_frame();

    // Inside both clips: claims. Inside only the outer: dead - the
    // intersection already cut it, though the probe rect covers both.
    gui.begin_frame(click(60.0f, 60.0f));
    gui.push_hit_clip({0.0f, 0.0f, 100.0f, 100.0f});
    gui.push_hit_clip({50.0f, 50.0f, 100.0f, 100.0f});
    CHECK(gui.hit_test(gui.ids().id("probe"), big).clicked);
    gui.end_frame();

    gui.begin_frame(click(25.0f, 25.0f));
    gui.push_hit_clip({0.0f, 0.0f, 100.0f, 100.0f});
    gui.push_hit_clip({50.0f, 50.0f, 100.0f, 100.0f});
    const HitTest cut = gui.hit_test(gui.ids().id("probe"), big);
    CHECK(!cut.pressed);
    CHECK(!cut.clicked);
    gui.end_frame();

    // A pop restores the enclosing clip's coverage: (25, 25) claims with
    // the inner clip popped.
    gui.begin_frame(click(25.0f, 25.0f));
    gui.push_hit_clip({0.0f, 0.0f, 100.0f, 100.0f});
    gui.push_hit_clip({50.0f, 50.0f, 100.0f, 100.0f});
    gui.pop_hit_clip();
    CHECK(gui.hit_test(gui.ids().id("probe"), big).clicked);
    gui.end_frame();

    // Pops past empty are no-ops, drop-safe like every gui pop.
    gui.begin_frame(click(25.0f, 25.0f));
    gui.pop_hit_clip();
    gui.pop_hit_clip();
    CHECK(gui.current_hit_clip() == nullptr);
    CHECK(gui.hit_test(gui.ids().id("probe"), big).clicked);
    gui.end_frame();

    // Overflow stays balanced: 8 pushes store, the 9th and 10th only
    // count depth (warned once per process) - the far clip pushed past
    // the clamp never takes effect, so (5, 5) still claims inside the
    // 8th stored entry...
    gui.begin_frame(click(5.0f, 5.0f));
    for (int i = 0; i < 8; ++i) {
        gui.push_hit_clip({0.0f, 0.0f, 10.0f, 10.0f});
    }
    gui.push_hit_clip({200.0f, 200.0f, 10.0f, 10.0f});
    gui.push_hit_clip({200.0f, 200.0f, 10.0f, 10.0f});
    CHECK(gui.hit_test(gui.ids().id("probe"), big).clicked);
    gui.end_frame();

    // ...and (205, 205), inside the pushed-but-unstored rects, does not.
    gui.begin_frame(click(205.0f, 205.0f));
    for (int i = 0; i < 8; ++i) {
        gui.push_hit_clip({0.0f, 0.0f, 10.0f, 10.0f});
    }
    gui.push_hit_clip({200.0f, 200.0f, 10.0f, 10.0f});
    gui.push_hit_clip({200.0f, 200.0f, 10.0f, 10.0f});
    const Rect* clamped = gui.current_hit_clip();
    REQUIRE(clamped != nullptr);
    CHECK(clamped->x == 0.0f);  // the 8th stored entry, not the far pushes
    CHECK(clamped->y == 0.0f);
    CHECK(!gui.hit_test(gui.ids().id("probe"), big).pressed);
    gui.end_frame();

    // Ten pops unwind to exactly nothing: the overflowed pops never eat
    // a real scope.
    gui.begin_frame(click(205.0f, 205.0f));
    for (int i = 0; i < 10; ++i) {
        if (i < 8) {
            gui.push_hit_clip({0.0f, 0.0f, 10.0f, 10.0f});
        } else {
            gui.push_hit_clip({200.0f, 200.0f, 10.0f, 10.0f});
        }
    }
    for (int i = 0; i < 10; ++i) {
        gui.pop_hit_clip();
    }
    CHECK(gui.current_hit_clip() == nullptr);  // balanced to nothing
    CHECK(gui.hit_test(gui.ids().id("probe"), big).clicked);
    gui.end_frame();

    // Frame-scoped: a push an end_frame never popped cannot blank the
    // next frame's claims.
    gui.begin_frame(snap(300.0f, 300.0f));
    gui.push_hit_clip({0.0f, 0.0f, 10.0f, 10.0f});
    gui.end_frame();
    gui.begin_frame(click(25.0f, 25.0f));
    CHECK(gui.current_hit_clip() == nullptr);
    CHECK(gui.hit_test(gui.ids().id("probe"), big).clicked);
    gui.end_frame();

    // A degenerate clip - empty, like the too-small viewport's - claims
    // nothing anywhere: honestly nothing, rather than half measures.
    gui.begin_frame(click(60.0f, 60.0f));
    gui.push_hit_clip({12.0f, 12.0f, 0.0f, 0.0f});
    const HitTest nothing = gui.hit_test(gui.ids().id("probe"), big);
    CHECK(!nothing.pressed);
    CHECK(!nothing.clicked);
    gui.end_frame();
}

TEST_CASE("hit clips: capture is not clipped - an owned press outlives the scroll") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;

    // The clip gates NEW claims, never an in-flight gesture: a press
    // claimed on a visible row stays owned while a wheel scrolls the row
    // out from under the held button, and the release lands wherever the
    // pointer is - clicked only if it ends inside the claimable part.
    // Here the release happens while the row is fully scrolled away, so
    // the drag ends released with no click: the release-outside rule,
    // unchanged.
    const auto pass = [&](const InputFrame& frame) {
        gui.begin_frame(frame);
        (void)begin_scroll_panel(gui, layout, kDefaultChrome, "list",
                                 {0.0f, 0.0f, 200.0f, 100.0f});
        int clicked = -1;
        for (int i = 0; i < 11; ++i) {
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

    // Frame 1: idle - measures the 11-row range (264 px, range 188).
    CHECK(pass(snap(60.0f, 50.0f)) == -1);

    // Frame 2: press row 0 (y 12..36 at offset 0) and hold.
    CHECK(pass(snap(60.0f, 24.0f, true, true)) == -1);
    CHECK(gui.active_id() == row_id(0));  // the press carried the frame

    // Frame 3: still held, wheel 100 px - row 0 flows to y -88..-64,
    // fully above the viewport, clipped away. Capture: row 0 keeps the
    // press, and row 4, whose visible part now holds the pointer, can
    // claim neither the press nor even hot (the mouse owns a live
    // press).
    InputFrame f = snap(60.0f, 24.0f, true);
    f.scroll_dy = -100.0f / kDefaultChrome.scroll_speed;
    CHECK(pass(f) == -1);
    CHECK(gui.active_id() == row_id(0));  // the clip did not end the drag
    CHECK(!gui.is_hot(row_id(4)));

    // Frame 4: release at the same spot - outside row 0's clipped-away
    // rect. The drag ends with no click and nothing else claims the
    // release; the press is over.
    CHECK(pass(snap(60.0f, 24.0f, false, false, true)) == -1);
    CHECK(gui.active_id() == kInvalidWidgetId);
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

TEST_CASE("fit_text: a theme font measures the fit glyph-honestly") {
    using namespace ws::ui::gui;
    const ws::ui::r2d::Font font = ab_font();  // A=10, B=5, blank dots
    ChromeTheme theme = kDefaultChrome;
    theme.font = &font;
    char scratch[128];

    // The whole fits: real glyph advances ("AB" = 15), the label's own
    // bytes.
    const LabelFit whole =
        fit_text("AB", 100.0f, theme, scratch, sizeof(scratch));
    CHECK(whole.text != scratch);
    CHECK(std::string_view(whole.text) == "AB");
    CHECK(whole.advance == doctest::Approx(15.0f));

    // A miss truncates prefix by prefix: "AA" (20) at 12 fits only
    // "A..." (10 - the dots bake blank and advance nothing), where the
    // fontless estimate would have said 24.
    const LabelFit cut =
        fit_text("AA", 12.0f, theme, scratch, sizeof(scratch));
    CHECK(cut.text == scratch);
    CHECK(std::string_view(cut.text) == "A...");
    CHECK(cut.advance == doctest::Approx(10.0f));

    // The marker cut composes before any measuring: "AA##two" fits its
    // display (20) at exactly 20.
    const LabelFit marked =
        fit_text("AA##two", 20.0f, theme, scratch, sizeof(scratch));
    CHECK(marked.text == scratch);
    CHECK(std::string_view(marked.text) == "AA");
    CHECK(marked.advance == doctest::Approx(20.0f));

    // A blank-only fit draws nothing: "zz" bakes blank (advance 0), so
    // the whole "fits" at 0 - and a truncation whose only content
    // would be blank dots ("AAzz" at 5) answers empty, the shared
    // advance guard's doing.
    CHECK(fit_text("zz", 100.0f, theme, scratch,
                   sizeof(scratch)).advance == 0.0f);
    const LabelFit blanks =
        fit_text("AAzz", 5.0f, theme, scratch, sizeof(scratch));
    CHECK(blanks.advance == 0.0f);
    CHECK(std::string_view(blanks.text).empty());
}

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

// --- hot/press agreement, the focus trap, disabled scopes (M10) ----------------------

TEST_CASE("overlaps: the later widget wins hot AND the fresh press - hover and click agree") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const WidgetId under = IdStack{}.id("under");
    const WidgetId over = IdStack{}.id("over");
    const Rect rect{0.0f, 0.0f, 100.0f, 100.0f};

    // Hover first: the later widget settles hot (the painter's rule) and
    // reports hovered next frame.
    gui.begin_frame(snap(50.0f, 50.0f));
    (void)gui.state(under, rect);
    (void)gui.state(over, rect);
    CHECK(gui.hot_id() == over);
    gui.end_frame();

    // The press frame: `under` hit-tests first and claims, then `over` -
    // the later widget, same stratum - STEALS the fresh press. Painter
    // order is the topmost pixel, the one the user sees: the widget that
    // shows hover is the one that clicks. (The steal leaves `under`
    // holding a stale pressed for THIS frame only - its part was
    // computed before the steal; the release frame gives it nothing.)
    gui.begin_frame(snap(50.0f, 50.0f, true, true));
    const WidgetResult under_hit = gui.state(under, rect);
    const WidgetResult over_hit = gui.state(over, rect);
    CHECK(gui.active_id() == over);
    CHECK(gui.hot_id() == over);
    CHECK(over_hit.hit.pressed);
    CHECK(over_hit.hit.hovered);   // hot_prev settled on the later widget
    CHECK(under_hit.hit.pressed);  // the one-frame steal artifact
    gui.end_frame();

    // The release clicks the widget the user sees; the stolen-from one
    // learns nothing - not even a released.
    gui.begin_frame(snap(50.0f, 50.0f, false, false, true));
    const WidgetResult under_end = gui.state(under, rect);
    const WidgetResult over_end = gui.state(over, rect);
    CHECK(over_end.hit.clicked);
    CHECK(!under_end.hit.clicked);
    CHECK(!under_end.hit.released);
    gui.end_frame();
}

TEST_CASE("overlaps: a carried press is capture - the later widget cannot steal it") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const WidgetId under = IdStack{}.id("under");
    const WidgetId over = IdStack{}.id("over");
    const Rect rect{0.0f, 0.0f, 100.0f, 100.0f};

    // The press lands on `under` alone - `over` does not exist this frame.
    gui.begin_frame(snap(50.0f, 50.0f, true, true));
    (void)gui.state(under, rect);
    CHECK(gui.active_id() == under);
    gui.end_frame();

    // `over` appears on the overlap while the press is HELD (no edge):
    // carried is capture, the steal is fresh-only - not even a
    // same-stratum later widget takes it, and `under` keeps hot too
    // (ownership gates the claim).
    gui.begin_frame(snap(50.0f, 50.0f, true));
    (void)gui.state(under, rect);
    (void)gui.state(over, rect);
    CHECK(gui.active_id() == under);
    CHECK(gui.hot_id() == under);
    gui.end_frame();

    // The release clicks the widget that owned the whole drag.
    gui.begin_frame(snap(50.0f, 50.0f, false, false, true));
    CHECK(gui.state(under, rect).hit.clicked);
    CHECK(!gui.state(over, rect).hit.clicked);
    gui.end_frame();
}

TEST_CASE("modals: Tab lands only inside the veil - the focus trap") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const WidgetId modal_a = IdStack{}.id("modal-a");
    const WidgetId modal_b = IdStack{}.id("modal-b");
    const WidgetId base_w = IdStack{}.id("base-w");

    // Chrome-first, like production: the modal scope opens at the top,
    // its widgets register inside it, and the base widget's attempt
    // under the veil is silently refused - the refusal IS the trap.
    const auto veiled_frame = [&gui, modal_a, modal_b, base_w](InputFrame frame) {
        gui.begin_frame(frame);
        gui.begin_layer(Layer::modal);
        gui.register_focusable(modal_a);
        gui.register_focusable(modal_b);
        gui.end_layer();
        gui.register_focusable(base_w);  // refused: under the veil
        gui.end_frame();
    };

    // Tab walks the modal's own registrations and wraps inside them -
    // never escaping to the base widget the veil hides.
    InputFrame tab = snap(200.0f, 200.0f);
    tab.key_pressed[keys::kTab] = true;
    veiled_frame(tab);
    CHECK(gui.is_focused(modal_a));
    veiled_frame(tab);
    CHECK(gui.is_focused(modal_b));
    veiled_frame(tab);
    CHECK(gui.is_focused(modal_a));  // wrapped, not escaped
    CHECK(!gui.is_focused(base_w));

    // The veil lifts on a quiet frame (the rise stashed an empty focus,
    // so the restore clears it), and now Tab reaches the base widget:
    // registration under an open sky is ordinary again.
    gui.begin_frame(snap(200.0f, 200.0f));
    gui.register_focusable(base_w);
    gui.end_frame();
    CHECK(!gui.is_focused(modal_a));

    InputFrame tab_plain = snap(200.0f, 200.0f);
    tab_plain.key_pressed[keys::kTab] = true;
    gui.begin_frame(tab_plain);
    gui.register_focusable(base_w);
    gui.end_frame();
    CHECK(gui.is_focused(base_w));
}

TEST_CASE("modals: the veil lifts and hands the focus back") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const WidgetId opener = IdStack{}.id("opener");
    const WidgetId dialog = IdStack{}.id("dialog");
    const WidgetId elsewhere = IdStack{}.id("elsewhere");
    const Rect rect{0.0f, 0.0f, 400.0f, 300.0f};

    // The opener takes the focus the ordinary way: a press on it.
    gui.begin_frame(snap(50.0f, 50.0f, true, true));
    gui.register_focusable(opener);
    (void)gui.state(opener, rect);
    CHECK(gui.is_focused(opener));
    gui.end_frame();
    gui.begin_frame(snap(50.0f, 50.0f, false, false, true));
    gui.register_focusable(opener);
    (void)gui.state(opener, rect);
    gui.end_frame();

    // The veil rises (chrome-first): the opener's focus drops this
    // frame - the rise stashes it - and the veil is up from here.
    gui.begin_frame(snap(200.0f, 200.0f));
    gui.begin_layer(Layer::modal);
    gui.register_focusable(dialog);
    gui.end_layer();
    const WidgetResult veiled = gui.state(opener, rect);
    CHECK(veiled.state == WidgetState::Normal);  // no hover under the veil
    CHECK(!gui.is_focused(opener));  // dropped: the veil blocks the keyboard
    gui.end_frame();

    // Still up the next frame: no restore while the veil holds.
    gui.begin_frame(snap(200.0f, 200.0f));
    gui.begin_layer(Layer::modal);
    gui.end_layer();
    (void)gui.state(opener, rect);
    CHECK(!gui.is_focused(opener));
    gui.end_frame();

    // The closing frame is quiet (no press, no nav): mid-frame the
    // focus is still gone - the lift is only confirmable at end_frame -
    // and end_frame hands the stash back.
    gui.begin_frame(snap(200.0f, 200.0f));
    (void)gui.state(opener, rect);
    CHECK(!gui.is_focused(opener));
    gui.end_frame();
    CHECK(gui.is_focused(opener));

    // And it settles: with the pointer away, the restored opener wears
    // the resting Focused state.
    gui.begin_frame(snap(500.0f, 500.0f));
    gui.register_focusable(opener);
    CHECK(gui.state(opener, rect).state == WidgetState::Focused);
    gui.end_frame();

    // A LOUD closing frame declines: a press that lands a focus keeps
    // it. Re-raise the veil (the opener's focus drops again), then
    // close it on a press on a different focusable.
    gui.begin_frame(snap(200.0f, 200.0f));
    gui.begin_layer(Layer::modal);
    gui.end_layer();
    (void)gui.state(opener, rect);
    CHECK(!gui.is_focused(opener));
    gui.end_frame();
    gui.begin_frame(snap(250.0f, 250.0f, true, true));
    gui.register_focusable(elsewhere);
    (void)gui.state(elsewhere, rect);
    CHECK(gui.is_focused(elsewhere));
    gui.end_frame();
    CHECK(gui.is_focused(elsewhere));  // the restore yielded to the press
    gui.begin_frame(snap(250.0f, 250.0f, false, false, true));
    gui.register_focusable(elsewhere);
    (void)gui.state(elsewhere, rect);
    gui.end_frame();
}

TEST_CASE("citizenship: the checkbox takes focus and Enter toggles like a click") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;
    bool value = true;
    bool other = true;

    // Click the row: the press takes the focus (checkboxes register
    // now), the release completes the click and flips the bool.
    gui.begin_frame(snap(4.0f, 4.0f, true, true));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(!checkbox(gui, layout, kDefaultChrome, "mute", value));
    (void)checkbox(gui, layout, kDefaultChrome, "solo", other);
    layout.end_panel();
    gui.end_frame();
    gui.begin_frame(snap(4.0f, 4.0f, false, false, true));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(checkbox(gui, layout, kDefaultChrome, "mute", value));
    (void)checkbox(gui, layout, kDefaultChrome, "solo", other);
    layout.end_panel();
    gui.end_frame();
    CHECK(value == false);
    CHECK(other == true);
    CHECK(gui.is_focused(IdStack{}.id("mute")));

    // Enter toggles the focused checkbox - and only it.
    InputFrame key_enter = snap(200.0f, 200.0f);
    key_enter.key_pressed[keys::kEnter] = true;
    gui.begin_frame(key_enter);
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(checkbox(gui, layout, kDefaultChrome, "mute", value));
    (void)checkbox(gui, layout, kDefaultChrome, "solo", other);
    layout.end_panel();
    gui.end_frame();
    CHECK(value == true);
    CHECK(other == true);

    // Space does the same.
    InputFrame key_space = snap(200.0f, 200.0f);
    key_space.key_pressed[keys::kSpace] = true;
    gui.begin_frame(key_space);
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(checkbox(gui, layout, kDefaultChrome, "mute", value));
    (void)checkbox(gui, layout, kDefaultChrome, "solo", other);
    layout.end_panel();
    gui.end_frame();
    CHECK(value == false);

    // Clearing the focus kills the key path: the same Space does nothing.
    InputFrame key_dead = snap(200.0f, 200.0f);
    key_dead.key_pressed[keys::kSpace] = true;
    gui.begin_frame(key_dead);
    gui.set_focus(kInvalidWidgetId);
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(!checkbox(gui, layout, kDefaultChrome, "mute", value));
    (void)checkbox(gui, layout, kDefaultChrome, "solo", other);
    layout.end_panel();
    gui.end_frame();
    CHECK(value == false);
}

TEST_CASE("citizenship: the arrows step the focused slider") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;
    float v = 0.25f;

    // Press the exact middle: click-to-set parks 0.5, and the press
    // takes the focus (sliders register now).
    gui.begin_frame(snap(60.0f, 12.0f, true, true));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(slider_float(gui, layout, kDefaultChrome, "vol", v, 0.0f, 1.0f));
    layout.end_panel();
    gui.end_frame();
    CHECK(v == doctest::Approx(0.5f));
    CHECK(gui.is_focused(IdStack{}.id("vol")));

    // The release inside maps the same value: no change, no report.
    gui.begin_frame(snap(60.0f, 12.0f, false, false, true));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(!slider_float(gui, layout, kDefaultChrome, "vol", v, 0.0f, 1.0f));
    layout.end_panel();
    gui.end_frame();

    // Right steps 1% of the range; only a real move reports changed.
    InputFrame key_right = snap(200.0f, 200.0f);
    key_right.key_pressed[keys::kRight] = true;
    gui.begin_frame(key_right);
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(slider_float(gui, layout, kDefaultChrome, "vol", v, 0.0f, 1.0f));
    layout.end_panel();
    gui.end_frame();
    CHECK(v == doctest::Approx(0.51f));

    // Left steps back.
    InputFrame key_left = snap(200.0f, 200.0f);
    key_left.key_pressed[keys::kLeft] = true;
    gui.begin_frame(key_left);
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(slider_float(gui, layout, kDefaultChrome, "vol", v, 0.0f, 1.0f));
    layout.end_panel();
    gui.end_frame();
    CHECK(v == doctest::Approx(0.5f));

    // Without the focus the arrows do nothing and report nothing.
    InputFrame key_dead = snap(200.0f, 200.0f);
    key_dead.key_pressed[keys::kRight] = true;
    gui.begin_frame(key_dead);
    gui.set_focus(kInvalidWidgetId);
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(!slider_float(gui, layout, kDefaultChrome, "vol", v, 0.0f, 1.0f));
    layout.end_panel();
    gui.end_frame();
    CHECK(v == doctest::Approx(0.5f));
}

TEST_CASE("citizenship: slider_int steps by one and clamps at the end") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;
    int n = 3;

    // Press at the left edge: click-to-set parks 0 and the press takes
    // the focus.
    gui.begin_frame(snap(0.0f, 12.0f, true, true));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(slider_int(gui, layout, kDefaultChrome, "n", n, 0, 3));
    layout.end_panel();
    gui.end_frame();
    CHECK(n == 0);
    CHECK(gui.is_focused(IdStack{}.id("n")));

    gui.begin_frame(snap(0.0f, 12.0f, false, false, true));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(!slider_int(gui, layout, kDefaultChrome, "n", n, 0, 3));
    layout.end_panel();
    gui.end_frame();

    // Right steps 1 per press, reporting changed on each real move.
    InputFrame key_right = snap(200.0f, 200.0f);
    key_right.key_pressed[keys::kRight] = true;
    for (int expect = 1; expect <= 3; ++expect) {
        gui.begin_frame(key_right);
        (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
        CHECK(slider_int(gui, layout, kDefaultChrome, "n", n, 0, 3));
        layout.end_panel();
        gui.end_frame();
        CHECK(n == expect);
    }

    // At the max the same arrow reports nothing and the value holds.
    gui.begin_frame(key_right);
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(!slider_int(gui, layout, kDefaultChrome, "n", n, 0, 3));
    layout.end_panel();
    gui.end_frame();
    CHECK(n == 3);
}

TEST_CASE("citizenship: tabs take focus in strip order; Enter selects") {
    using namespace ws::ui::gui;
    GuiContext gui;
    LayoutCursor layout;
    const char* labels[] = {"one", "two", "three"};
    int active = -1;

    // Click the first tab: the press takes the focus (each tab registers
    // now, in strip order) and selects nothing yet; the RELEASE completes
    // the click and moves the selection.
    gui.begin_frame(snap(30.0f, 12.0f, true, true));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(!tab_bar(gui, layout, kDefaultChrome, "pages", labels, 3, active));
    layout.end_panel();
    gui.end_frame();
    CHECK(active == -1);  // the press frame selects nothing
    gui.begin_frame(snap(30.0f, 12.0f, false, false, true));
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(tab_bar(gui, layout, kDefaultChrome, "pages", labels, 3, active));
    layout.end_panel();
    gui.end_frame();
    CHECK(active == 0);

    // Tab walks the strip's registrations: the focus moves to the
    // second tab WITHOUT moving the selection.
    InputFrame tab = snap(200.0f, 200.0f);
    tab.key_pressed[keys::kTab] = true;
    gui.begin_frame(tab);
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(!tab_bar(gui, layout, kDefaultChrome, "pages", labels, 3, active));
    layout.end_panel();
    gui.end_frame();
    CHECK(active == 0);  // the focus moved, the selection did not

    // Enter selects the focused tab - exactly like a click.
    InputFrame key_enter = snap(200.0f, 200.0f);
    key_enter.key_pressed[keys::kEnter] = true;
    gui.begin_frame(key_enter);
    (void)layout.begin_panel({0.0f, 0.0f, 400.0f, 300.0f}, 0.0f);
    CHECK(tab_bar(gui, layout, kDefaultChrome, "pages", labels, 3, active));
    layout.end_panel();
    gui.end_frame();
    CHECK(active == 1);
}

TEST_CASE("disabled: the scope withdraws claims, focus and Tab") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const WidgetId locked = IdStack{}.id("locked");
    const WidgetId below = IdStack{}.id("below");
    const Rect rect{0.0f, 0.0f, 100.0f, 100.0f};

    // Inside the scope the widget is dead: the Disabled face, zero
    // claims - and the press falls through to the widget underneath.
    gui.begin_frame(snap(50.0f, 50.0f, true, true));
    gui.begin_disabled();
    CHECK(gui.disabled());
    gui.register_focusable(locked);  // refused while locked
    const WidgetResult dead = gui.state(locked, rect);
    CHECK(dead.state == WidgetState::Disabled);
    CHECK(dead.hit.pressed == false);
    CHECK(gui.hot_id() == kInvalidWidgetId);  // nothing claimed yet
    gui.end_disabled();
    gui.register_focusable(below);
    const WidgetResult through = gui.state(below, rect);
    CHECK(through.hit.pressed);  // pass-through: the click is below's
    CHECK(gui.active_id() == below);
    gui.end_frame();

    // The release clicks the widget underneath, not the locked one.
    gui.begin_frame(snap(50.0f, 50.0f, false, false, true));
    const WidgetResult under_end = gui.state(below, rect);
    CHECK(under_end.hit.clicked);
    gui.end_frame();

    // A conditional scope that does not hold locks nothing: the widget
    // claims like always.
    gui.begin_frame(snap(50.0f, 50.0f));
    gui.begin_disabled(false);
    CHECK(!gui.disabled());
    const WidgetResult live = gui.state(locked, rect);
    CHECK(live.state == WidgetState::Normal);
    CHECK(gui.hot_id() == locked);
    gui.end_disabled();
    gui.end_frame();

    // The balanced clamp: a false begin still pushes (its end must not
    // eat an enclosing true scope), and a true inside locks regardless
    // of the false around it - the mask is the OR of the conditions.
    gui.begin_frame(snap(200.0f, 200.0f));
    gui.begin_disabled();
    gui.begin_disabled(false);
    CHECK(gui.disabled());
    gui.end_disabled();  // pops the FALSE scope only
    CHECK(gui.disabled());  // the enclosing true still holds
    gui.end_disabled();
    CHECK(!gui.disabled());
    gui.end_disabled();  // nothing open - drop-safe like every gui pop
    CHECK(!gui.disabled());
    gui.end_frame();

    // Tab has nowhere to land while the only focusable is locked: the
    // registration is refused, the nav resolves against nothing, and
    // the focus holds still.
    InputFrame tab = snap(200.0f, 200.0f);
    tab.key_pressed[keys::kTab] = true;
    gui.begin_frame(tab);
    gui.begin_disabled();
    gui.register_focusable(locked);  // refused
    (void)gui.state(locked, rect);    // Disabled, no claims
    gui.end_disabled();
    gui.end_frame();
    CHECK(gui.is_focused(below));  // unchanged: nowhere to land
}

TEST_CASE("disabled: a layer scope resets the lock and end_layer restores it") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const WidgetId locked = IdStack{}.id("locked");
    const WidgetId chrome = IdStack{}.id("chrome");
    const Rect rect{0.0f, 0.0f, 100.0f, 100.0f};

    // A locked form opens a popup: the chrome inside the scope draws
    // LIVE - registration works, the press lands, focus and all. Chrome
    // is not part of the form that locked.
    gui.begin_frame(snap(50.0f, 50.0f, true, true));
    gui.begin_disabled();
    gui.begin_layer(Layer::popup);
    gui.register_focusable(chrome);
    const WidgetResult live = gui.state(chrome, rect);
    CHECK(live.hit.pressed);
    CHECK(live.state == WidgetState::Pressed);
    CHECK(gui.is_focused(chrome));
    gui.end_layer();
    // Back under the lock: the base widget is dead again - and the
    // chrome keeps its press (the lock never touched it).
    const WidgetResult dead = gui.state(locked, rect);
    CHECK(dead.state == WidgetState::Disabled);
    CHECK(gui.active_id() == chrome);
    gui.end_disabled();
    gui.end_frame();

    // Lift the chrome's press (its owner does not run; end_frame drops
    // the vanished release), then a fresh press is the base widget's
    // own again: the scope ended.
    gui.begin_frame(snap(50.0f, 50.0f, false, false, true));
    (void)gui.state(locked, rect);  // blocked by the live press
    gui.end_frame();
    gui.begin_frame(snap(50.0f, 50.0f, true, true));
    const WidgetResult revived = gui.state(locked, rect);
    CHECK(revived.hit.pressed);
    CHECK(revived.state == WidgetState::Pressed);
    gui.end_frame();
}

TEST_CASE("disabled: begin_frame clears a scope an early return never closed") {
    using namespace ws::ui::gui;
    GuiContext gui;
    const WidgetId w = IdStack{}.id("w");
    const Rect rect{0.0f, 0.0f, 100.0f, 100.0f};

    // An early return strands the scope: the frame ends still disabled.
    gui.begin_frame(snap(50.0f, 50.0f));
    gui.begin_disabled();
    const WidgetResult dead = gui.state(w, rect);
    CHECK(dead.state == WidgetState::Disabled);
    gui.end_frame();  // no end_disabled - stranded open

    // The next frame starts clean: the widget claims again.
    gui.begin_frame(snap(50.0f, 50.0f, true, true));
    CHECK(!gui.disabled());
    const WidgetResult live = gui.state(w, rect);
    CHECK(live.hit.pressed);
    CHECK(gui.active_id() == w);
    gui.end_frame();
}

TEST_CASE("theme: dim() multiplies the alpha channel only") {
    using namespace ws::ui::gui;
    // Colors are AARRGGBB - the alpha is the top byte.
    // 1.0 is the identity.
    CHECK(dim(0xFF203040u, 1.0f) == 0xFF203040u);
    CHECK(dim(0x80203040u, 1.0f) == 0x80203040u);
    // 255 * 0.5 truncates to 127 - the low bits drop, they do not round.
    CHECK(dim(0xFF203040u, 0.5f) == 0x7F203040u);
    // Zero and a negative factor drain the alpha, never the color.
    CHECK(dim(0x40203040u, 0.0f) == 0x00203040u);
    CHECK(dim(0xFF203040u, -1.0f) == 0x00203040u);
    // An oversaturated factor clamps at full alpha.
    CHECK(dim(0x80203040u, 2.0f) == 0xFF203040u);
    // An already-transparent color stays transparent.
    CHECK(dim(0x00203040u, 0.5f) == 0x00203040u);
}
