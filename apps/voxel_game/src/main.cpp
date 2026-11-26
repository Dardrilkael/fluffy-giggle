// apps/voxel_game - a first-person voxel sandbox on the ws stack.
//
// THE COMPOSITION (bounce's shape, four libs deep):
//   ws::ui      - the App shell (window, Vulkan, r2d/r3d), the HUD's
//                 text/line/quad draws, the input frame
//   ws::physics - backend-independent swept-AABB character collision
//                 against the voxel occupancy grid; Jolt remains available
//                 for ordinary dynamic rigid bodies
//   ws::audio   - synthesized place/break one-shots (bounce's PCM
//                 pattern through SoundDesc)
//   ws::core    - Result/Status logging on every fallible call
//
// THE LOOP: mouse-look from the MouseMovedEvent deltas accumulated in
// on_event (the platform seam exposes no pointer lock, so the look is
// the classic free-cursor pattern - deltas between successive events,
// the pointer visible, look stops at the window's edges), WASD +
// space + shift-sprint through the controller the headless prototype
// validated, the eye ray's DDA march for break (LMB) / place (RMB)
// with chunk-mesh rebuilds, and a 60 Hz fixed-tick physics step.
//
// THE FRAME RECORD ORDER (the weave's painter key is (layer, epoch),
// epochs assigned in begin() order): the sky's 2D quad block records
// FIRST (epoch 1, layer 0), then the 3D scene block (epoch 2, layer
// 0) - the sky paints behind the meshes. The HUD rides layer 10. The
// meshes are depth-tested among themselves; the sky's depth-off quad
// simply never contests them.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <optional>
#include <vector>

#ifdef __ANDROID__
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include <android_native_app_glue.h>
#include <unistd.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#endif

#include <ws/audio/engine.hpp>
#include <ws/core/log.hpp>
#include <ws/input/event.hpp>
#include <ws/input/input.hpp>
#include <ws/math/vecmath.hpp>
#include <ws/ui/app.hpp>
#include <ws/ui/renderer2d.hpp>
#include <ws/ui/renderer3d.hpp>

#include "menu.hpp"
#include "inventory.hpp"
#include "bench.hpp"
#include "perf.hpp"
#include "player.hpp"
#include "renderer.hpp"
#include "world.hpp"

namespace {

namespace r2d = ws::ui::r2d;
namespace r3d = ws::ui::r3d;
using ws::math::Vec3;

// GLFW key codes (input.hpp's contract: printables match ASCII).
constexpr int kKeyW = 87;
constexpr int kKeyA = 65;
constexpr int kKeyS = 83;
constexpr int kKeyD = 68;
constexpr int kKeySpace = 32;
constexpr int kKeyShiftL = 340;  // GLFW_KEY_LEFT_SHIFT
constexpr int kKey1 = 49;

constexpr int kHotbarSlots = static_cast<int>(voxel::kHotbarSize);
constexpr int kKeyE = 69;
constexpr int kKeyC = 67;
constexpr int kKeyF1 = 290;

[[nodiscard]] const char* item_name(voxel::Item item) {
  constexpr const char* names[] = {"grass","dirt","stone","wood","sand",
    "cobble","planks","sticks","bricks","glass","leaves","coal","iron ore","iron"};
  return names[static_cast<std::size_t>(item)];
}
[[nodiscard]] std::uint32_t item_tint(voxel::Item item) {
  switch (item) {
    case voxel::Item::grass: return voxel::tint_of(voxel::Block::grass);
    case voxel::Item::dirt: return voxel::tint_of(voxel::Block::dirt);
    case voxel::Item::stone: return voxel::tint_of(voxel::Block::stone);
    case voxel::Item::wood: return voxel::tint_of(voxel::Block::wood);
    case voxel::Item::sand: return voxel::tint_of(voxel::Block::sand);
    case voxel::Item::cobblestone: return voxel::tint_of(voxel::Block::cobblestone);
    case voxel::Item::planks: return voxel::tint_of(voxel::Block::planks);
    case voxel::Item::bricks: return voxel::tint_of(voxel::Block::bricks);
    case voxel::Item::glass: return voxel::tint_of(voxel::Block::glass);
    case voxel::Item::leaves: return voxel::tint_of(voxel::Block::leaves);
    case voxel::Item::coal: return voxel::tint_of(voxel::Block::coal_ore);
    case voxel::Item::iron_ore: return voxel::tint_of(voxel::Block::iron_ore);
    case voxel::Item::sticks: return r2d::rgba(180,140,90);
    case voxel::Item::iron_ingot: return r2d::rgba(205,210,220);
    default: return r2d::rgba(255,255,255);
  }
}
[[nodiscard]] bool item_to_block(voxel::Item item, voxel::Block& block) {
  switch (item) {
    case voxel::Item::grass: block=voxel::Block::grass; return true;
    case voxel::Item::dirt: block=voxel::Block::dirt; return true;
    case voxel::Item::stone: block=voxel::Block::stone; return true;
    case voxel::Item::wood: block=voxel::Block::wood; return true;
    case voxel::Item::sand: block=voxel::Block::sand; return true;
    case voxel::Item::cobblestone: block=voxel::Block::cobblestone; return true;
    case voxel::Item::planks: block=voxel::Block::planks; return true;
    case voxel::Item::bricks: block=voxel::Block::bricks; return true;
    case voxel::Item::glass: block=voxel::Block::glass; return true;
    case voxel::Item::leaves: block=voxel::Block::leaves; return true;
    default: return false;
  }
}
[[nodiscard]] voxel::Item block_drop(voxel::Block block) {
  switch (block) {
    case voxel::Block::grass: return voxel::Item::grass;
    case voxel::Block::dirt: return voxel::Item::dirt;
    case voxel::Block::stone: return voxel::Item::cobblestone;
    case voxel::Block::wood: return voxel::Item::wood;
    case voxel::Block::sand: return voxel::Item::sand;
    case voxel::Block::cobblestone: return voxel::Item::cobblestone;
    case voxel::Block::planks: return voxel::Item::planks;
    case voxel::Block::bricks: return voxel::Item::bricks;
    case voxel::Block::glass: return voxel::Item::glass;
    case voxel::Block::leaves: return voxel::Item::leaves;
    case voxel::Block::coal_ore: return voxel::Item::coal;
    case voxel::Block::iron_ore: return voxel::Item::iron_ore;
    default: return voxel::Item::dirt;
  }
}

// --- sound: the break/place one-shots (bounce's synth pattern) -------------------
constexpr std::uint32_t kRate = 44'100;

[[nodiscard]] std::vector<std::int16_t> synth_tock(double base_hz,
                                                   double partner_hz,
                                                   double decay_per_s) {
  // 80 ms of an exponentially decaying two-sine pair: the body
  // (base_hz) plus a clangy partner - the block "tock".
  constexpr std::uint32_t kFrames = kRate * 80 / 1000;
  std::vector<std::int16_t> out(kFrames);
  for (std::uint32_t i = 0; i < kFrames; ++i) {
    const double t = static_cast<double>(i) / kRate;
    const double decay = std::exp(-t * decay_per_s);
    const double attack = std::min(1.0, i / 40.0);  // ~1 ms onset
    const double tone =
        std::sin(2.0 * std::numbers::pi * base_hz * t) +
        0.5 * std::sin(2.0 * std::numbers::pi * partner_hz * t);
    out[i] = static_cast<std::int16_t>(tone * decay * attack * 11'000.0);
  }
  return out;
}

}  // namespace

class VoxelApp final : public ws::ui::App {
 public:
  explicit VoxelApp(const ws::ui::AppConfig& config) : App(config) {}

 protected:
  void on_event(ws::input::Event& e) override {
    // The look's raw material: the deltas between successive
    // MouseMovedEvents, accumulated between frames. The events carry
    // window-relative coords; consecutive differences are the motion,
    // whatever the pointer's absolute position is. The live absolute
    // position is kept too - the pause menu's buttons hit-test it
    // (the cursor is free while the menu is open, so the OS is
    // delivering real positions then).
    ws::input::EventDispatcher d{e};
    (void)d.dispatch<ws::input::MouseMovedEvent>(
        [this](const ws::input::MouseMovedEvent& ev) {
          if (!paused_ && have_last_mouse_) {
            look_dx_ += ev.x - last_mouse_x_;
            look_dy_ += ev.y - last_mouse_y_;
          }
          have_last_mouse_ = true;
          last_mouse_x_ = ev.x;
          last_mouse_y_ = ev.y;
          mouse_x_ = ev.x;
          mouse_y_ = ev.y;
          return false;  // not consumed: the shell keeps its own bookkeeping
        });
    // The menu's click: only when the menu is up (a paused game must
    // not dig blocks behind the overlay), and consumed so the frame's
    // break/place pass never sees it.
    (void)d.dispatch<ws::input::MouseButtonPressedEvent>(
        [this](const ws::input::MouseButtonPressedEvent& ev) {
          if (paused_) {
            clicked_ = ev.button == 0;
            if (clicked_) {
              return true;  // consumed: the menu ate the click
            }
          }
          return false;
        });
  }

  void on_setup() override {
    // --- the world --------------------------------------------------------
    world_.generate(kSeed);
    const int sx = voxel::kWorldSizeX / 2;
    const int sz = voxel::kWorldSizeZ / 2;
    const int surface = world_.surface_height(sx, sz);
    const Vec3 spawn{static_cast<float>(sx) + 0.5f,
                     static_cast<float>(surface) + 1.0f +
                         player_.tuning_half_height(),
                     static_cast<float>(sz) + 0.5f};

    // The player queries the world's chunk storage directly. Terrain is
    // not represented by rigid bodies; the general physics world remains
    // available to other gameplay entities when they are introduced.
    if (!player_.spawn(spawn)) {
      dead_ = true;
      return;
    }

    // Starter supplies make the sandbox immediately playable; all later
    // blocks must be gathered or crafted.
    (void)inventory_.add(voxel::Item::grass, 32);
    (void)inventory_.add(voxel::Item::dirt, 32);
    (void)inventory_.add(voxel::Item::stone, 32);
    (void)inventory_.add(voxel::Item::wood, 16);
    (void)inventory_.add(voxel::Item::sand, 16);

    // --- the chunk meshes (the renderer's one-time upload) -----------------
    world_.rebuild_all();

    // --- the HUD font -------------------------------------------------------
    font_ = r2d::load_default_font(16.0f,
                                   r2d::FontBakeOptions::dark_chrome());

    // --- the block-texture atlas (the uploaded resource pack) ---------------
    tile_textures_ = voxel::load_tile_textures();
    for (int i = 0; i < voxel::atlas::kTileCount; ++i) {
      if (tile_textures_[i].id == 0) {
        ws::core::warn("voxel: tile {} fell back to white - flat tint", i);
      }
    }

    // --- audio: the break/place pair ----------------------------------------
    if (audio_.init().has_value()) {
      break_pcm_ = synth_tock(240.0, 610.0, 26.0);  // low, woody
      place_pcm_ = synth_tock(520.0, 1040.0, 32.0);  // brighter tick
      struct Load {
        const std::vector<std::int16_t>* pcm;
      };
      const Load loads[2] = {{&break_pcm_}, {&place_pcm_}};
      for (const Load& load : loads) {
        ws::audio::SoundDesc sd{};
        sd.samples = load.pcm->data();
        sd.frame_count = static_cast<std::uint32_t>(load.pcm->size());
        sd.channels = 1;
        sd.sample_rate = kRate;
        const auto handle = audio_.load_sound(sd);
        if (!handle.has_value()) {
          ws::core::warn("voxel: a sound failed to load - running quieter");
          continue;
        }
        sounds_[sounds_loaded_++] = *handle;
      }
      audio_ok_ = sounds_loaded_ == 2;
    } else {
      ws::core::warn("voxel: audio device unavailable - running silent");
    }
    // --- the gameplay pointer: hidden from the first frame -------------------
    // The mouse-look grab: the OS pointer vanishes and motion arrives as
    // raw deltas (never a cursor pinned at a screen edge). ESC's pause
    // menu is the one place that hands the visible pointer back.
    set_cursor_mode(ws::ui::CursorMode::hidden);

    // The scripted benchmark (WS_VOXEL_BENCH=<seconds>): the frame loop
    // feeds perf.hpp's counters and bench.hpp prints one CSV line per
    // second; request_stop lands at the end of the run.
    const float bench_seconds = voxel::bench::bench_seconds_from_env();
    if (bench_seconds > 0.0f) {
      voxel::bench::Config cfg{};
      cfg.seconds = bench_seconds;
      bench_.start(cfg);
      ws::core::info("voxel: benchmark - {} s of scripted phases",
                     bench_seconds);
    }
  }

  void on_update() override {
    if (dead_) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    const float dt =
        last_time_.has_value()
            ? std::chrono::duration<float>(now - *last_time_).count()
            : 0.0f;
    last_time_ = now;
    const float frame_dt = std::clamp(dt, 0.0f, 0.05f);
    const auto frame_t0 = std::chrono::steady_clock::now();
    voxel::perf::FrameTimings timings{};
    mesh_rebuild_ms_ = 0.0f;  // mesh and collision chunk rebuilds feed this

    const ws::input::InputFrame& input = ws::input::frame_input();
    const r2d::FrameSize frame = r2d::frame_size();

    // --- esc opens/closes the pause menu ------------------------------------
    // The old esc-quit is gone: the game keeps running until the menu's
    // Quit (or the title bar's X). ESC toggles - open freezes play and
    // hands the OS cursor back, close regrabs it and resumes. (The
    // look-delta accumulator is gated in on_event, and the click flag
    // is consumed here, so no paused-frame input leaks into the game.)
    if (input.key_pressed[256]) {  // GLFW_KEY_ESCAPE
      paused_ = !paused_;
      set_cursor_mode(paused_ ? ws::ui::CursorMode::normal
                              : ws::ui::CursorMode::hidden);
      clicked_ = false;
      // Re-anchor the look accumulator: while paused, deltas were not
      // accumulated, but events kept arriving - drop any motion the
      // resume could otherwise read as one giant turn.
      look_dx_ = 0.0f;
      look_dy_ = 0.0f;
    }

    if (paused_) {
      // --- the paused frame: last game state + the menu overlay ------------
      menu_action(frame);
      r2d::begin();  // the 2D epoch: records first, paints first
      r2d::set_layer(0);
      r2d::draw_quad(0.0f, 0.0f, frame.width, frame.height,
                     r2d::rgba(135, 196, 250));
      r2d::end();

      const auto cam = voxel::begin_scene(player_, frame.width / frame.height);
      voxel::draw_world(world_, tile_textures_, cam,
                         frame.width / frame.height);
      r2d::end();

      r2d::begin();
      r2d::set_layer(10);
      draw_hud(frame);
      draw_pause_menu(frame);
      r2d::end();
      return;
    }

    if (input.key_pressed[kKeyE]) inventory_open_ = !inventory_open_;
    if (input.key_pressed[kKeyC]) crafting_open_ = !crafting_open_;
    if (inventory_open_ || crafting_open_) {
      // The panels are modal: do not move or edit the world while open.
      r2d::begin(); r2d::set_layer(0);
      r2d::draw_quad(0, 0, frame.width, frame.height, r2d::rgba(135,196,250));
      const auto cam = voxel::begin_scene(player_, frame.width / frame.height);
      voxel::draw_world(world_, tile_textures_, cam, frame.width / frame.height);
      r2d::end();
      r2d::begin(); r2d::set_layer(10); draw_hud(frame); draw_inventory_panels(frame); r2d::end();
      return;
    }

    // --- the controller tick -------------------------------------------------
    voxel::Player::MoveInput move{};
    if (bench_.running()) {
      // The scripted benchmark drives the same input path.
      move = bench_.input(frame_dt);
    } else {
      move.forward = input.key_down[kKeyW];
      move.back = input.key_down[kKeyS];
      move.left = input.key_down[kKeyA];
      move.right = input.key_down[kKeyD];
      move.jump = input.key_pressed[kKeySpace];
      move.sprint = input.key_down[kKeyShiftL];
      move.look_dx = look_dx_;
      move.look_dy = look_dy_;
      look_dx_ = 0.0f;  // consumed: the next frame starts fresh
      look_dy_ = 0.0f;
    }

    const auto phys_t0 = std::chrono::steady_clock::now();
    player_.tick(move, frame_dt, world_.collision_grid());
    (void)audio_.update();
    timings.physics_ms = voxel::perf::ms_since(phys_t0);

    // --- hotbar + break/place ------------------------------------------------
    for (int i = 0; i < kHotbarSlots; ++i)
      if (input.key_pressed[kKey1 + i]) hotbar_ = i;
    for (int i = 0; i < static_cast<int>(voxel::kRecipes.size()); ++i)
      if (input.key_pressed[kKeyF1 + i]) (void)voxel::craft(inventory_, voxel::kRecipes[static_cast<std::size_t>(i)]);
    const bool wants_break =
        (input.mouse_pressed[0] && !bench_.running()) ||
        (bench_.running() && bench_.wants_edit() && bench_.edit_ready() &&
         ((bench_frame_ & 15) == 8));  // one burst pair per ~1/4 s
    const bool wants_place =
        (input.mouse_pressed[1] && !bench_.running()) ||
        (bench_.running() && bench_.wants_edit() && bench_.edit_ready() &&
         ((bench_frame_ & 15) == 0));
    const bool bench_break =
        bench_.running() && bench_.wants_edit() && bench_.edit_ready() &&
        ((bench_frame_ & 15) == 8);
    const bool bench_place =
        bench_.running() && bench_.wants_edit() && bench_.edit_ready() &&
        ((bench_frame_ & 15) == 0);
    const bool do_break = wants_break || bench_break;
    const bool do_place = wants_place || bench_place;
    if (do_break || do_place) {
      const Vec3 eye = player_.eye_position();
      const Vec3 look = player_.look_direction();
      const float origin[3] = {eye.x, eye.y, eye.z};
      const float dir[3] = {look.x, look.y, look.z};
      const voxel::World::RayHit hit = world_.raycast(origin, dir, 7.0f);
      if (do_break && hit.hit) {  // break
        const voxel::Block broken = world_.block_at(hit.cell[0], hit.cell[1], hit.cell[2]);
        if (world_.set_block(hit.cell[0], hit.cell[1], hit.cell[2],
                             voxel::Block::air)) {
          (void)inventory_.add(block_drop(broken), 1);
          rebuild_around(hit.cell[0], hit.cell[2]);
          play_sound(kSoundBreak, 0.9f);
        }
      } else if (do_place && hit.hit) {  // place
        const int px = hit.cell[0] + hit.normal[0];
        const int py = hit.cell[1] + hit.normal[1];
        const int pz = hit.cell[2] + hit.normal[2];
        const auto& selected = inventory_.slot(static_cast<std::size_t>(hotbar_));
        voxel::Block place{};
        const bool placeable = !selected.empty() && item_to_block(selected.item, place);
        // Place only into air, and never entomb the player (the AABB
        // overlap rule - the classic voxel place guard).
        if (placeable && world_.block_at(px, py, pz) == voxel::Block::air &&
            !overlaps_player(px, py, pz)) {
          if (world_.set_block(px, py, pz, place)) {
            (void)inventory_.remove(selected.item, 1);
            rebuild_around(px, pz);
            play_sound(kSoundPlace, 0.8f);
          }
        }
      }
    }

    // --- the frame: sky (epoch 1) -> world (epoch 2) -> HUD (layer 10) ----
    r2d::begin();  // the 2D epoch: records first, paints first
    r2d::set_layer(0);
    r2d::draw_quad(0.0f, 0.0f, frame.width, frame.height,
                   r2d::rgba(135, 196, 250));
    r2d::end();

    const auto draw_t0 = std::chrono::steady_clock::now();
    const auto cam = voxel::begin_scene(player_, frame.width / frame.height);
    voxel::draw_world(world_, tile_textures_, cam,
                       frame.width / frame.height, &last_counters_);
    r2d::end();
    timings.draw_ms = voxel::perf::ms_since(draw_t0);
    // Voxel terrain and the character controller use direct grid queries;
    // neither allocates rigid bodies.
    last_counters_.bodies = 0;

    r2d::begin();
    r2d::set_layer(10);
    draw_hud(frame);
    r2d::end();

    // --- the instrumentation close -----------------------------------------
    timings.meshes_ms = mesh_rebuild_ms_;
    timings.total_ms = voxel::perf::ms_since(frame_t0);
    last_timings_ = timings;
    if (bench_.running()) {
      ++bench_frame_;
      bench_.frame(frame_dt, timings, last_counters_);
      if (bench_.advance_phase(frame_dt)) {
        std::printf("bench: done (%.1f s)\n", bench_.seconds());
        std::fflush(stdout);
        request_stop();
      }
    }
  }

 private:
  [[nodiscard]] bool overlaps_player(int x, int y, int z) const {
    const auto box = player_.bounds();
    const ws::physics3d::Aabb block{
        Vec3{static_cast<float>(x), static_cast<float>(y),
             static_cast<float>(z)},
        Vec3{static_cast<float>(x + 1), static_cast<float>(y + 1),
             static_cast<float>(z + 1)}};
    return box.max.x > block.min.x && box.min.x < block.max.x &&
           box.max.y > block.min.y && box.min.y < block.max.y &&
           box.max.z > block.min.z && box.min.z < block.max.z;
  }

  // --- the HUD --------------------------------------------------------------------
  void draw_hud(const r2d::FrameSize& frame) {
    if (font_.empty()) {
      return;
    }
    // The crosshair: two crossing hairlines at the center.
    const float cx = frame.width * 0.5f;
    const float cy = frame.height * 0.5f;
    r2d::draw_line(cx - 7.0f, cy, cx + 7.0f, cy, 2.0f,
                   r2d::rgba(255, 255, 255, 220));
    r2d::draw_line(cx, cy - 7.0f, cx, cy + 7.0f, 2.0f,
                   r2d::rgba(255, 255, 255, 220));

    // Nine-slot hotbar, backed by the first nine inventory slots.
    constexpr float slot_w = 72.0f;
    constexpr float bar_w = slot_w * kHotbarSlots;
    const float bx = (frame.width - bar_w) * 0.5f;
    const float by = frame.height - 48.0f;
    for (int i = 0; i < kHotbarSlots; ++i) {
      const float sx = bx + static_cast<float>(i) * slot_w;
      const bool selected = i == hotbar_;
      r2d::draw_quad(sx, by, slot_w - 6.0f, 38.0f,
                     r2d::rgba(28, 30, 36, 200));
      const auto& stack = inventory_.slot(static_cast<std::size_t>(i));
      if (!stack.empty()) {
        r2d::draw_quad(sx + 5.0f, by + 6.0f, 22.0f, 22.0f, item_tint(stack.item));
        char label[32]; std::snprintf(label, sizeof(label), "%d %s", i + 1, item_name(stack.item));
        (void)r2d::draw_text(sx + 30.0f, by + 5.0f, label, font_, selected ? r2d::rgba(255,255,255) : r2d::rgba(168,172,182));
        char amount[8]; std::snprintf(amount, sizeof(amount), "%u", stack.count);
        (void)r2d::draw_text(sx + 50.0f, by + 23.0f, amount, font_, r2d::rgba(255,255,255));
      }
      if (selected) {
        r2d::draw_line(sx - 1.0f, by - 1.0f, sx + slot_w - 7.0f, by - 1.0f,
                       2.0f, r2d::rgba(255, 255, 255, 210));
        r2d::draw_line(sx - 1.0f, by + 39.0f, sx + slot_w - 7.0f, by + 39.0f,
                       2.0f, r2d::rgba(255, 255, 255, 210));
      }
    }

    // The FPS window: one second, counted frames.
    ++frames_;
    const auto now = std::chrono::steady_clock::now();
    if (fps_clock_.time_since_epoch().count() == 0) {
      fps_clock_ = now;
    } else if (std::chrono::duration<float>(now - fps_clock_).count() >=
               1.0f) {
      fps_ = frames_;
      frames_ = 0;
      fps_clock_ = now;
    }

    // The stats + help lines, top-left. The perf line carries the
    // instrumentation's last sample (frame phase means, the draw and
    // body counts) so the running game answers "where does the frame
    // go" without a debugger.
    char stats[160];
    const Vec3 p = player_.position();
    std::snprintf(stats, sizeof(stats),
                  "fps %u   pos %.1f %.1f %.1f   %s\n"
                  "cpu %.2fms (phys %.2f mesh %.2f draw %.2f)   draws %u"
                  " tris %u   bodies %u",
                  fps_, p.x, p.y, p.z,
                  player_.grounded() ? "ground" : "air", last_timings_.total_ms,
                  last_timings_.physics_ms, last_timings_.meshes_ms,
                  last_timings_.draw_ms, last_counters_.draw_calls,
                  last_counters_.triangles, last_counters_.bodies);
    (void)r2d::draw_text(12.0f, 12.0f, stats, font_, r2d::rgba(235, 235, 240));
    (void)r2d::draw_text(
        12.0f, 32.0f,
        "LMB break - RMB place - 1-5 select - WASD move - space jump - "
        "shift sprint - E inventory - C crafting - F1-F6 craft - esc menu",
        font_, r2d::rgba(160, 164, 174));
  }

  void draw_inventory_panels(const r2d::FrameSize& frame) {
    if (font_.empty()) return;
    const float x = frame.width * 0.5f - 250.0f, y = frame.height * 0.5f - 190.0f;
    r2d::draw_quad(x, y, 500, 380, r2d::rgba(22,25,32,238));
    if (inventory_open_) {
      (void)r2d::draw_text(x+18,y+16,"INVENTORY  (E to close)",font_,r2d::rgba(245,245,250));
      for (int i=0;i<27;++i) {
        const int col=i%9,row=i/9; const float sx=x+18+col*52, sy=y+54+row*58;
        r2d::draw_quad(sx,sy,46,50,r2d::rgba(55,60,70));
        const auto& st=inventory_.slot(static_cast<std::size_t>(i));
        if (!st.empty()) {
          r2d::draw_quad(sx+7,sy+7,20,20,item_tint(st.item));
          char txt[24]; std::snprintf(txt,sizeof(txt),"%s x%u",item_name(st.item),st.count);
          (void)r2d::draw_text(sx+3,sy+31,txt,font_,r2d::rgba(230,232,238));
        }
      }
    }
    if (crafting_open_) {
      (void)r2d::draw_text(x+18,y+250,"CRAFTING  (F1-F6 craft, C to close)",font_,r2d::rgba(245,245,250));
      for (std::size_t i=0;i<voxel::kRecipes.size();++i) {
        const auto& recipe=voxel::kRecipes[i]; char txt[96];
        std::snprintf(txt,sizeof(txt),"F%u  %s x%u  [%s]",static_cast<unsigned>(i+1),item_name(recipe.output),recipe.output_count,voxel::can_craft(inventory_,recipe)?"ready":"missing materials");
        (void)r2d::draw_text(x+22,y+278+static_cast<float>(i)*16,txt,font_,voxel::can_craft(inventory_,recipe)?r2d::rgba(150,230,160):r2d::rgba(155,160,170));
      }
    }
  }

  // --- the pause menu ----------------------------------------------------------------
  // The layout + hit test live in menu.hpp (engine-free, pinned by the
  // logic suite); this side owns the state (paused_, the cursor mode,
  // the consumed click) and the painting - plain quads + text in the
  // game's own look, no widget framework.

  // Consumes the menu frame's click (if any) against the layout.
  void menu_action(const r2d::FrameSize& frame) {
    if (!clicked_) {
      return;
    }
    const int action = voxel::menu::click_action(
        static_cast<float>(frame.width), static_cast<float>(frame.height),
        mouse_x_, mouse_y_);
    clicked_ = false;  // eaten either way: the modal shade owns the screen
    if (action == 0) {  // resume: the ESC-toggle path, shared verbatim
      paused_ = false;
      set_cursor_mode(ws::ui::CursorMode::hidden);
      look_dx_ = 0.0f;
      look_dy_ = 0.0f;
    } else if (action == 1) {  // quit
      request_stop();
    }
  }

  void draw_pause_menu(const r2d::FrameSize& frame) {
    if (font_.empty()) {
      return;
    }
    // The modal shade: the frame darkened under the menu, the classic
    // "game is paused" read, drawn over the world + HUD beneath.
    r2d::draw_quad(0.0f, 0.0f, frame.width, frame.height,
                   r2d::rgba(10, 12, 16, 160));

    // The title.
    (void)r2d::draw_text(frame.width * 0.5f - 40.0f,
                         frame.height * 0.5f - 96.0f, "paused", font_,
                         r2d::rgba(235, 235, 240));

    // The buttons: the hovered one brightens (the live mouse position
    // is the OS cursor's own - the pointer is free while the menu is
    // up), the rest sit at rest.
    voxel::menu::Button buttons[voxel::menu::kButtonCount];
    voxel::menu::layout(static_cast<float>(frame.width),
                        static_cast<float>(frame.height), buttons);
    static constexpr const char* kLabels[voxel::menu::kButtonCount] = {
        "resume (esc)", "quit game"};
    for (int i = 0; i < voxel::menu::kButtonCount; ++i) {
      const voxel::menu::Button& b = buttons[i];
      const bool hovered = voxel::menu::hit(b, mouse_x_, mouse_y_);
      r2d::draw_quad(b.x, b.y, b.w, b.h,
                     hovered ? r2d::rgba(58, 62, 72, 230)
                             : r2d::rgba(38, 42, 50, 230));
      // Centered label: draw_text returns the drawn width, so draw
      // once off-screen to measure, then draw at the centered origin
      // (the Font's own measure member needs the source-side header).
      const float tw = r2d::draw_text(-1000.0f, -1000.0f, kLabels[i], font_);
      (void)r2d::draw_text(b.x + (b.w - tw) * 0.5f,
                           b.y + b.h * 0.5f - 8.0f, kLabels[i], font_,
                           hovered ? r2d::rgba(255, 255, 255)
                                   : r2d::rgba(190, 194, 204));
    }
  }

 private:
  // --- helpers -----------------------------------------------------------------------
  void rebuild_around(int x, int z) {
    const int cx = x / voxel::kChunkSizeX;
    const int cz = z / voxel::kChunkSizeZ;
    world_.rebuild_chunk(cx, cz);
    // If the block sits on a chunk boundary, rebuild the neighbor too.
    if (x % voxel::kChunkSizeX == 0 && cx > 0) {
      world_.rebuild_chunk(cx - 1, cz);
    }
    if (x % voxel::kChunkSizeX == voxel::kChunkSizeX - 1 && cx < voxel::kWorldChunksX - 1) {
      world_.rebuild_chunk(cx + 1, cz);
    }
    if (z % voxel::kChunkSizeZ == 0 && cz > 0) {
      world_.rebuild_chunk(cx, cz - 1);
    }
    if (z % voxel::kChunkSizeZ == voxel::kChunkSizeZ - 1 && cz < voxel::kWorldChunksZ - 1) {
      world_.rebuild_chunk(cx, cz + 1);
    }
  }

  void play_sound(int id, float gain) {
    if (!audio_ok_ || id < 0 || id >= 2 || !sounds_[id].valid()) {
      return;
    }
    (void)audio_.play(sounds_[id], gain);
  }

  // --- the state -------------------------------------------------------------------
  static constexpr std::uint32_t kSeed = 0xC0FFEEu;
  static constexpr int kSoundBreak = 0;
  static constexpr int kSoundPlace = 1;

  voxel::World world_;
  voxel::Player player_;
  int hotbar_ = 0;
  voxel::Inventory inventory_{};
  bool inventory_open_ = false;
  bool crafting_open_ = false;
  // The pause menu's state: paused_ gates the whole simulation (the
  // controller tick, physics, break/place all skip while it is up) and
  // owns the cursor mode; mouse_x_/mouse_y_ is the live OS pointer -
  // the menu hit-tests it; clicked_ is the frame's menu click,
  // consumed by the frame that reads it.
  bool paused_ = false;
  float mouse_x_ = 0.0f;
  float mouse_y_ = 0.0f;
  bool clicked_ = false;
  // The per-tile block textures (the uploaded resource pack's art,
  // loaded once at setup). id 0 = the white fallback - that tile's
  // draw shades the flat tint alone.
  std::array<ws::ui::r2d::Texture, voxel::atlas::kTileCount>
      tile_textures_{};
  bool dead_ = false;
  bool audio_ok_ = false;
  ws::audio::SoundHandle sounds_[2]{};
  int sounds_loaded_ = 0;
  std::vector<std::int16_t> break_pcm_;
  std::vector<std::int16_t> place_pcm_;
  ws::audio::AudioEngine audio_;
  r2d::Font font_{};

  // The accumulated look deltas (on_event's MouseMoved differences).
  float look_dx_ = 0.0f;
  float look_dy_ = 0.0f;
  float last_mouse_x_ = 0.0f;
  float last_mouse_y_ = 0.0f;
  bool have_last_mouse_ = false;

  std::optional<std::chrono::steady_clock::time_point> last_time_{};
  std::uint32_t fps_ = 0;
  std::uint32_t frames_ = 0;
  std::chrono::steady_clock::time_point fps_clock_{};

  // The perf instrumentation's last sample (perf.hpp) + the scripted
  // benchmark driver (bench.hpp; idle unless WS_VOXEL_BENCH is set).
  voxel::perf::FrameTimings last_timings_{};
  voxel::perf::Counters last_counters_{};
  float mesh_rebuild_ms_ = 0.0f;
  voxel::bench::Driver bench_{};
  std::uint32_t bench_frame_ = 0;
};

#ifdef __ANDROID__
// The phone entry: the glue hands android_main the shell handle. The
// touch bridges to mouse button 0 in the ui backend (the bounce note),
// so tap-to-break works; the drag-look and the HUD are the next
// platform pass (the desktop look's seam does not exist yet).
extern "C" void android_main(android_app* app) {
  chdir(app->activity->internalDataPath);
  ws::ui::AppConfig config;
  config.title = "ws voxel";
  config.app_handle = app;
  VoxelApp game{config};
  (void)game.run();
}
#else
int main() {
  ws::ui::AppConfig config;
  config.title = "ws voxel";
  config.width = 1280;
  config.height = 720;
  VoxelApp game{config};
  return game.run();
}
#endif
