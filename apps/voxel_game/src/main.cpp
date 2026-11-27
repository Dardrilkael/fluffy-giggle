// apps/voxel_game - a first-person voxel sandbox on the ws stack.
#define _CRT_SECURE_NO_WARNINGS
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
#include <filesystem>
#include <future>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
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
#include <ws/rgba_terrain_core/metadata.hpp>
#include <ws/rgba_terrain_core/rgba_terrain.hpp>
#include <ws/rgba_terrain_core/voxel_file.hpp>
#include <ws/rgba_terrain_core/voxel_validation.hpp>

#include "menu.hpp"
#include "hotbar.hpp"
#include "settings_menu.hpp"
#include "inventory.hpp"
#include "bench.hpp"
#include "perf.hpp"
#include "player.hpp"
#include "renderer.hpp"
#include "world.hpp"
#include "terrain_import.hpp"
#include "terrain_world_io.hpp"
#include "voxel_audio.hpp"

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

constexpr int kHotbarSlots = voxel::hotbar::kSlots;
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
[[nodiscard]] int item_tile(voxel::Item item) {
  // Inventory icons use the exact same generated PNGs as world geometry.
  // Sticks and ingots are crafted materials, so they keep a clean tint icon.
  switch (item) {
    case voxel::Item::grass: return 0;
    case voxel::Item::dirt: return 1;
    case voxel::Item::stone: return 3;
    case voxel::Item::wood: return 4;
    case voxel::Item::sand: return 6;
    case voxel::Item::cobblestone: return 8;
    case voxel::Item::planks: return 9;
    case voxel::Item::bricks: return 10;
    case voxel::Item::glass: return 11;
    case voxel::Item::leaves: return 12;
    case voxel::Item::coal: return 13;
    case voxel::Item::iron_ore: return 14;
    default: return -1;
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
          if (!entry_menu_ && !paused_ && !inventory_open_ && !crafting_open_ && !settings_open_ && have_last_mouse_) {
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
          if (entry_menu_ || paused_ || inventory_open_ || crafting_open_ || settings_open_) {
            clicked_ = ev.button == 0;
            if (clicked_) return true;  // modal UI owns this click
          }
          return false;
        });
  }

  void on_setup() override {
    // A benchmark distance override changes only the view range; the world,
    // seed, spawn point and camera orientation remain identical between runs.
    const float requested_render_distance =
        voxel::bench::bench_render_distance_from_env();
    if (requested_render_distance > 0.0f) {
      settings_.render_distance =
          std::clamp(requested_render_distance, 16.0f, 512.0f);
    }

    // --- the world --------------------------------------------------------
    const auto generation_start = std::chrono::steady_clock::now();
    const char* terrain_png = std::getenv("WS_VOXEL_TERRAIN_PNG");
    const char* terrain_meta = std::getenv("WS_VOXEL_TERRAIN_META");
    const char* terrain_voxels = std::getenv("WS_VOXEL_TERRAIN_VOXELS");
    const bool has_terrain_png = terrain_png != nullptr && terrain_png[0] != '\0';
    const bool has_terrain_meta =
        terrain_meta != nullptr && terrain_meta[0] != '\0';
    const bool has_terrain_voxels =
        terrain_voxels != nullptr && terrain_voxels[0] != '\0';
    const bool terrain_requested =
        has_terrain_png || has_terrain_meta || has_terrain_voxels;
    const float bench_seconds = voxel::bench::bench_seconds_from_env();
    entry_menu_ = !terrain_requested && bench_seconds <= 0.0f;
    if (!terrain_requested && !entry_menu_) {
      world_.generate(kSeed);
    } else if (terrain_requested) {
      if (!has_terrain_png || !has_terrain_meta) {
        ws::core::error(
            "voxel: RGBA terrain import requires "
            "WS_VOXEL_TERRAIN_PNG and WS_VOXEL_TERRAIN_META; "
            "WS_VOXEL_TERRAIN_VOXELS is optional for validation");
        dead_ = true;
        return;
      }
      try {
        const auto metadata =
            ws::rgba_terrain_core::load_metadata(terrain_meta);
        const auto image =
            ws::rgba_terrain_core::load_rgba_image(terrain_png);
        const auto terrain = ws::rgba_terrain_core::generate_voxel_volume(
            image, metadata.layout, metadata.settings);

        // PNG + metadata are authoritative. The persisted voxel file is
        // only an optional validation artifact; it never supplies the world.
        if (has_terrain_voxels) {
          const auto stored =
              ws::rgba_terrain_core::read_voxel_file(terrain_voxels);
          if (!stored) {
            throw std::runtime_error(stored.error().message);
          }
          const auto validation =
              ws::rgba_terrain_core::compare_voxel_volumes(terrain, *stored);
          if (!validation.exact_match()) {
            throw std::runtime_error(
                std::format("optional voxel artifact does not exactly match PNG + metadata "
                "(mismatched voxels: {0})", validation.mismatched_voxels));          }          ws::core::info(
              "voxel: optional voxel artifact validated ({0} voxels)",
              validation.compared_voxels);
        }

        const auto imported = voxel::import_terrain_volume(world_, terrain);
        if (!imported) {
          throw std::runtime_error(imported.error().message);
        }
        ws::core::info(
            "voxel: reconstructed and imported RGBA terrain {0}x{1}x{2} "
            "from PNG + metadata ({3} voxels, {4} columns)",
            terrain.width, terrain.depth, terrain.height,
            imported->voxel_count, imported->column_count);
        const auto populated_biome_types = std::count_if(
            imported->biome_columns.begin(), imported->biome_columns.end(),
            [](std::uint64_t columns) { return columns != 0; });
        ws::core::info(
            "voxel: imported {0} populated biome types",
            populated_biome_types);
      } catch (const std::exception& exception) {
        ws::core::error("voxel: RGBA terrain import failed: {}",
                        exception.what());
        dead_ = true;
        return;
      }
    }
    world_loaded_ = !entry_menu_;
    if (!entry_menu_) {
      const float generation_ms = voxel::perf::ms_since(generation_start);

      // The player queries the world's chunk storage directly. Terrain is
      // not represented by rigid bodies; the general physics world remains
      // available to other gameplay entities when they are introduced.
      spawn_at_world_center();

      // --- the chunk meshes (the renderer's one-time upload) -----------------
      voxel::perf::FrameTimings startup_metrics{};
      voxel_renderer_.rebuild_all(world_, &startup_metrics);
      constexpr float kChunkCount = static_cast<float>(
          voxel::kWorldChunksX * voxel::kWorldChunksZ);
      ws::core::info(
          "voxel: startup generation {:.2f} ms ({:.3f} ms/chunk); "
          "CPU meshing {:.2f} ms ({:.3f} ms/chunk); "
          "CPU upload {:.2f} ms, {} bytes",
          generation_ms, generation_ms / kChunkCount,
          startup_metrics.cpu_mesh_ms, startup_metrics.cpu_mesh_ms / kChunkCount,
          startup_metrics.upload_ms, startup_metrics.upload_bytes);
    }

    // Starter supplies make the sandbox immediately playable; all later
    // blocks must be gathered or crafted.
    (void)inventory_.add(voxel::Item::grass, 32);
    (void)inventory_.add(voxel::Item::dirt, 32);
    (void)inventory_.add(voxel::Item::stone, 32);
    (void)inventory_.add(voxel::Item::wood, 16);
    (void)inventory_.add(voxel::Item::sand, 16);

    // --- reusable selection outline ---------------------------------------
    const auto outline = voxel::create_target_outline();
    if (outline.has_value()) {
      target_outline_ = *outline;
    } else {
      ws::core::warn("voxel: target outline mesh unavailable");
    }

    // --- the HUD font -------------------------------------------------------
    font_ = r2d::load_default_font(16.0f,
                                   r2d::FontBakeOptions::dark_chrome());

    // Show an existing PNG immediately, but keep the 3D world unloaded until
    // PLAY WORLD is selected. The channel sheet is display-only.
    if (entry_menu_ && saved_map_available()) {
      try {
        load_saved_png_preview();
        generation_status_ = "Saved PNG preview ready. Press PLAY WORLD to enter.";
      } catch (const std::exception& exception) {
        generation_status_ = std::string("Could not preview saved PNG: ") +
                             exception.what();
      }
    }

    // --- the block-texture atlas (the uploaded resource pack) ---------------
    tile_textures_ = voxel::load_tile_textures();
    for (int i = 0; i < voxel::atlas::kTileCount; ++i) {
      if (tile_textures_[i].id == 0) {
        ws::core::warn("voxel: tile {} fell back to white - flat tint", i);
      }
    }

    // --- audio: reusable DSP + geometry-derived early reflections ----------
    audio_ok_ = audio_.init().has_value();
    if (audio_ok_) {
      break_pcm_ = synth_tock(240.0, 610.0, 26.0);  // low, woody
      place_pcm_ = synth_tock(520.0, 1040.0, 32.0);  // brighter tick
    } else {
      ws::core::warn("voxel: audio device unavailable - running silent");
    }
    // --- the gameplay pointer: hidden from the first frame -------------------
    // The mouse-look grab: the OS pointer vanishes and motion arrives as
    // raw deltas (never a cursor pinned at a screen edge). ESC's pause
    // menu is the one place that hands the visible pointer back.
    set_cursor_mode(entry_menu_ ? ws::ui::CursorMode::normal
                                : ws::ui::CursorMode::hidden);

    // The scripted benchmark (WS_VOXEL_BENCH=<seconds>): the frame loop
    // feeds perf.hpp's counters and bench.hpp prints one CSV line per
    // second; request_stop lands at the end of the run.
    if (bench_seconds > 0.0f) {
      benchmark_shadow_flags_ =
          voxel::bench::bench_shadow_flags_from_env();
      voxel::bench::Config cfg{};
      cfg.seconds = bench_seconds;
      cfg.fixed_scene = voxel::bench::bench_fixed_scene_from_env();
      cfg.warmup_seconds = 3.0f;
      bench_.start(cfg);
      const auto benchmark_frame = r2d::frame_size();
      std::printf(
          "bench:config,mode=%s,seed=%u,render_distance=%.1f,"
          "width=%.0f,height=%.0f,warmup_s=%.1f,total_s=%.1f,"
          "shadow_mode=%s\n",
          cfg.fixed_scene ? "scale" : "scripted", kSeed,
          settings_.render_distance, benchmark_frame.width,
          benchmark_frame.height, cfg.fixed_scene ? cfg.warmup_seconds : 0.0f,
          bench_seconds,
          voxel::bench::shadow_mode_name(benchmark_shadow_flags_));
      std::fflush(stdout);
      ws::core::info("voxel: benchmark mode={} total={} s",
                     cfg.fixed_scene ? "fixed-scene scale" : "scripted",
                     bench_seconds);
    }
  }

  void on_update() override {
    if (dead_) {
      return;
    }
    poll_world_generation();
    const auto now = std::chrono::steady_clock::now();
    const float dt =
        last_time_.has_value()
            ? std::chrono::duration<float>(now - *last_time_).count()
            : 0.0f;
    last_time_ = now;
    const float frame_dt = std::clamp(dt, 0.0f, 0.05f);
    const auto frame_t0 = std::chrono::steady_clock::now();
    voxel::perf::FrameTimings timings{};
    timings.wall_frame_ms = std::max(0.0f, dt * 1000.0f);
    // Renderer statistics expose only completed query results. The serial
    // prevents repeatedly counting the same asynchronously resolved sample.
    const auto renderer_stats = r2d::stats();
    gpu_timing_supported_ = renderer_stats.gpu_timing_supported;
    if (renderer_stats.gpu_frame_valid &&
        renderer_stats.gpu_frame_serial != last_gpu_frame_serial_) {
      last_gpu_frame_serial_ = renderer_stats.gpu_frame_serial;
      last_gpu_frame_ms_ = renderer_stats.gpu_frame_ms;
      gpu_frame_valid_ = true;
      timings.gpu_frame_ms = renderer_stats.gpu_frame_ms;
      timings.gpu_frame_valid = true;
      gpu_frame_times_.add(renderer_stats.gpu_frame_ms);
    }
    const Vec3 mesh_priority_origin = player_.eye_position();
    voxel_renderer_.set_priority_origin(mesh_priority_origin.x, mesh_priority_origin.z);
    voxel_renderer_.process_completed(2, &timings);

    const ws::input::InputFrame& input = ws::input::frame_input();
    const r2d::FrameSize frame = r2d::frame_size();

    if (entry_menu_) {
      entry_menu_action(frame);
      draw_entry_menu_frame(frame);
      return;
    }

    // --- esc opens/closes the pause menu ------------------------------------
    // The old esc-quit is gone: the game keeps running until the menu's
    // Quit (or the title bar's X). ESC toggles - open freezes play and
    // hands the OS cursor back, close regrabs it and resumes. (The
    // look-delta accumulator is gated in on_event, and the click flag
    // is consumed here, so no paused-frame input leaks into the game.)
    if (input.key_pressed[256]) {  // GLFW_KEY_ESCAPE
      if (settings_open_) settings_open_ = false;
      else paused_ = !paused_;
      set_cursor_mode((paused_ || settings_open_)
                          ? ws::ui::CursorMode::normal
                          : ws::ui::CursorMode::hidden);
      clicked_ = false;
      // Re-anchor the look accumulator: while paused, deltas were not
      // accumulated, but events kept arriving - drop any motion the
      // resume could otherwise read as one giant turn.
      look_dx_ = 0.0f;
      look_dy_ = 0.0f;
    }

    if (!paused_ && input.key_pressed[80]) {  // P opens settings
      settings_open_ = !settings_open_;
      set_cursor_mode(settings_open_ ? ws::ui::CursorMode::normal
                                     : ws::ui::CursorMode::hidden);
      clicked_ = false;
    }
    if (settings_open_) {
      settings_action(frame);
      r2d::begin(); r2d::set_layer(0);
      r2d::draw_quad(0, 0, frame.width, frame.height, r2d::rgba(135,196,250));
      const auto cam = voxel::begin_scene(player_, settings_.fov_degrees,
                                             settings_.render_distance,
                                             benchmark_shadow_flags_.cast);
      voxel::draw_world(voxel_renderer_, tile_textures_, cam,
                         frame.width / frame.height, nullptr,
                         benchmark_shadow_flags_.cast,
                         benchmark_shadow_flags_.receive);
      r2d::end();
      r2d::begin(); r2d::set_layer(10);
      draw_hud(frame);
      draw_settings_panel(frame);
      r2d::end();
      return;
    }

    if (paused_) {
      // --- the paused frame: last game state + the menu overlay ------------
      menu_action(frame);
      r2d::begin();  // the 2D epoch: records first, paints first
      r2d::set_layer(0);
      r2d::draw_quad(0.0f, 0.0f, frame.width, frame.height,
                     r2d::rgba(135, 196, 250));
      r2d::end();

      const auto cam = voxel::begin_scene(player_, settings_.fov_degrees,
                                             settings_.render_distance,
                                             benchmark_shadow_flags_.cast);
      voxel::draw_world(voxel_renderer_, tile_textures_, cam,
                         frame.width / frame.height, nullptr,
                         benchmark_shadow_flags_.cast,
                         benchmark_shadow_flags_.receive);
      r2d::end();

      r2d::begin();
      r2d::set_layer(10);
      draw_hud(frame);
      draw_pause_menu(frame);
      r2d::end();
      return;
    }

    if (input.key_pressed[kKeyE]) {
      inventory_open_ = !inventory_open_;
      if (inventory_open_) crafting_open_ = false;
      set_cursor_mode((inventory_open_ || crafting_open_)
                          ? ws::ui::CursorMode::normal
                          : ws::ui::CursorMode::hidden);
      clicked_ = false;
    }
    if (input.key_pressed[kKeyC]) {
      crafting_open_ = !crafting_open_;
      if (crafting_open_) inventory_open_ = false;
      set_cursor_mode((inventory_open_ || crafting_open_)
                          ? ws::ui::CursorMode::normal
                          : ws::ui::CursorMode::hidden);
      clicked_ = false;
    }
    if (inventory_open_ || crafting_open_) {
      inventory_action(frame);
      // The panels are modal: do not move or edit the world while open.
      r2d::begin(); r2d::set_layer(0);
      r2d::draw_quad(0, 0, frame.width, frame.height, r2d::rgba(135,196,250));
      const auto cam = voxel::begin_scene(player_, settings_.fov_degrees,
                                             settings_.render_distance,
                                             benchmark_shadow_flags_.cast);
      voxel::draw_world(voxel_renderer_, tile_textures_, cam,
                         frame.width / frame.height, nullptr,
                         benchmark_shadow_flags_.cast,
                         benchmark_shadow_flags_.receive);
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
    if (audio_ok_) {
      const Vec3 listener = player_.eye_position();      const Vec3 forward = player_.look_direction();
      (void)audio_.set_listener_position(listener.x, listener.y, listener.z);
      (void)audio_.set_listener_orientation(
          forward.x, forward.y, forward.z, 0.0f, 1.0f, 0.0f);
      (void)audio_.update();
    }
    timings.physics_ms = voxel::perf::ms_since(phys_t0);

    // --- target ray + hotbar selection --------------------------------------
    for (int i = 0; i < kHotbarSlots; ++i)
      if (input.key_pressed[kKey1 + i]) hotbar_ = i;
    const Vec3 target_eye = player_.eye_position();
    const Vec3 target_look = player_.look_direction();
    const float target_origin[3] = {target_eye.x, target_eye.y, target_eye.z};
    const float target_direction[3] = {
        target_look.x, target_look.y, target_look.z};
    target_hit_ = world_.raycast(target_origin, target_direction, 7.0f);
    if (crafting_open_) {
      for (int i = 0; i < static_cast<int>(voxel::kRecipes.size()); ++i)
        if (input.key_pressed[kKeyF1 + i])
          (void)voxel::craft(inventory_, voxel::kRecipes[static_cast<std::size_t>(i)]);
    }
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
      const auto& hit = target_hit_;
      if (do_break && hit.hit) {  // break
        const voxel::Block broken = world_.block_at(hit.cell.x, hit.cell.y, hit.cell.z);
        if (world_.set_block(hit.cell.x, hit.cell.y, hit.cell.z,
                             voxel::Block::air)) {
          (void)inventory_.add(block_drop(broken), 1);
          rebuild_around(hit.cell.x, hit.cell.z);
          play_sound(kSoundBreak, 0.9f,
                     Vec3{hit.cell.x + 0.5f, hit.cell.y + 0.5f,
                          hit.cell.z + 0.5f});
        }
      } else if (do_place && hit.hit) {  // place
        const int px = hit.cell.x + hit.normal.x;
        const int py = hit.cell.y + hit.normal.y;
        const int pz = hit.cell.z + hit.normal.z;
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
            play_sound(kSoundPlace, 0.8f,
                       Vec3{px + 0.5f, py + 0.5f, pz + 0.5f});
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

    const auto cam = voxel::begin_scene(player_, settings_.fov_degrees,
                                             settings_.render_distance,
                                             benchmark_shadow_flags_.cast);
    const auto draw_t0 = std::chrono::steady_clock::now();
    voxel::draw_world(voxel_renderer_, tile_textures_, cam,
                       frame.width / frame.height, &last_counters_,
                       benchmark_shadow_flags_.cast,
                       benchmark_shadow_flags_.receive);
    voxel::draw_target_outline(target_outline_, target_hit_);
    const auto draw_t1 = std::chrono::steady_clock::now();
    r2d::end();
    timings.draw_ms = voxel::perf::ms_since(draw_t0, draw_t1);
    // Voxel terrain and the character controller use direct grid queries;
    // neither allocates rigid bodies.
    last_counters_.bodies = 0;

    r2d::begin();
    r2d::set_layer(10);
    draw_hud(frame);
    r2d::end();

    // --- the instrumentation close -----------------------------------------
    // Refresh percentiles from the preceding window first, then close this
    // frame's timer so the occasional bounded sort is included in frame cost.
    const auto percentile_now = std::chrono::steady_clock::now();
    if (percentile_clock_.time_since_epoch().count() == 0 ||
        percentile_now - percentile_clock_ >= std::chrono::seconds(1)) {
      frame_percentiles_ = frame_times_.percentiles();
      gpu_frame_percentiles_ = gpu_frame_times_.percentiles();
      percentile_clock_ = percentile_now;
    }
    timings.total_ms = voxel::perf::ms_since(frame_t0);
    last_timings_ = timings;
    frame_times_.add(timings.total_ms);
    if (bench_.running()) {
      ++bench_frame_;
      bench_.frame(frame_dt, timings, last_counters_);
      if (bench_.advance_phase(frame_dt)) {
        bench_.finish();
        request_stop();
      }
    }
  }

  [[nodiscard]] bool wants_frame() const override {
    return generation_in_progress_;
  }

 private:
  using UiRect = ws::ui::layout::Rect;

  [[nodiscard]] static bool entry_hit(
      const UiRect& rect, float x, float y) noexcept {
    return x >= rect.x && x < rect.x + rect.w &&
           y >= rect.y && y < rect.y + rect.h;
  }

  [[nodiscard]] UiRect entry_panel(const r2d::FrameSize& frame) const {
    const float width = std::min(1120.0f, std::max(288.0f, frame.width - 24.0f));
    const float height = std::min(640.0f, std::max(420.0f, frame.height - 24.0f));
    return {(frame.width - width) * 0.5f,
            (frame.height - height) * 0.5f, width, height};
  }

  [[nodiscard]] std::array<UiRect, 3> entry_seed_controls(
      const r2d::FrameSize& frame) const {
    const UiRect panel = entry_panel(frame);
    const float y = panel.y + 142.0f;
    return {{{panel.x + 560.0f, y, 46.0f, 34.0f},
             {panel.x + 616.0f, y, 46.0f, 34.0f},
             {panel.x + 672.0f, y, panel.w - 700.0f, 34.0f}}};
  }

  [[nodiscard]] std::array<UiRect, 4> entry_buttons(
      const r2d::FrameSize& frame) const {
    const UiRect panel = entry_panel(frame);
    const float width = panel.w - 600.0f;
    const float x = panel.x + 550.0f;
    return {{{x, panel.y + 226.0f, width, 48.0f},
             {x, panel.y + 284.0f, width, 48.0f},
             {x, panel.y + 342.0f, width, 48.0f},
             {x, panel.y + 400.0f, width, 48.0f}}};
  }

  [[nodiscard]] bool saved_map_available() const {
    const auto files = voxel::terrain_world_io::generated_map_files();
    std::error_code error;
    const bool png = std::filesystem::is_regular_file(files.png, error);
    if (error || !png) return false;
    error.clear();
    return std::filesystem::is_regular_file(files.metadata, error) && !error;
  }

  void spawn_at_world_center() {
    const int origin_x = voxel::kWorldSizeX / 2;
    const int origin_z = voxel::kWorldSizeZ / 2;
    int spawn_x = -1;
    int spawn_z = -1;
    int surface = -1;

    // Search outward in deterministic square rings. Water is walk-through
    // in this game but has no swimming yet, so prefer the nearest dry column.
    const auto try_column = [&](int x, int z) {
      if (x < 0 || x >= voxel::kWorldSizeX ||
          z < 0 || z >= voxel::kWorldSizeZ) {
        return false;
      }
      const int candidate_surface = world_.surface_height(x, z);
      if (candidate_surface < 0 ||
          candidate_surface + 1 >= voxel::kChunkSizeY ||
          world_.block_at(x, candidate_surface + 1, z) == voxel::Block::water) {
        return false;
      }
      spawn_x = x;
      spawn_z = z;
      surface = candidate_surface;
      return true;
    };

    const int max_radius = voxel::kWorldSizeX + voxel::kWorldSizeZ;
    for (int radius = 0; radius < max_radius && spawn_x < 0; ++radius) {
      for (int dx = -radius; dx <= radius && spawn_x < 0; ++dx) {
        if (try_column(origin_x + dx, origin_z - radius)) break;
        if (radius != 0 &&
            try_column(origin_x + dx, origin_z + radius)) break;
      }
      for (int dz = -radius + 1; dz < radius && spawn_x < 0; ++dz) {
        if (try_column(origin_x - radius, origin_z + dz)) break;
        if (radius != 0 &&
            try_column(origin_x + radius, origin_z + dz)) break;
      }
    }

    // An all-water map is unusual but still playable: fall back to the
    // center's solid seabed rather than refusing to start the world.
    if (spawn_x < 0) {
      spawn_x = origin_x;
      spawn_z = origin_z;
      surface = world_.surface_height(spawn_x, spawn_z);
    }
    if (surface < 0 || surface + 1 >= voxel::kChunkSizeY) {
      throw std::runtime_error("the generated world has no solid spawn surface");
    }

    const Vec3 spawn{static_cast<float>(spawn_x) + 0.5f,
                     static_cast<float>(surface) + 1.0f +
                         player_.tuning_half_height(),
                     static_cast<float>(spawn_z) + 0.5f};
    if (!player_.spawn(spawn)) {
      throw std::runtime_error("could not place the player in the generated world");
    }
    target_hit_ = {};
    look_dx_ = 0.0f;
    look_dy_ = 0.0f;
    have_last_mouse_ = false;
  }

  void enter_current_world() {
    if (!entry_preview_loaded_) {
      generation_status_ = "Load or generate a PNG preview before playing.";
      return;
    }
    if (!world_loaded_) {
      try {
        const auto files = voxel::terrain_world_io::generated_map_files();
        const auto imported = voxel::terrain_world_io::import_png_map(world_, files);
        (void)imported;
        spawn_at_world_center();
        voxel::perf::FrameTimings rebuild_metrics{};
        voxel_renderer_.rebuild_all(world_, &rebuild_metrics);
        world_loaded_ = true;
      } catch (const std::exception& exception) {
        generation_status_ = std::string("Could not load world: ") +
                             exception.what();
        ws::core::error("voxel: {}", generation_status_);
        return;
      }
    }
    entry_menu_ = false;
    clicked_ = false;
    look_dx_ = 0.0f;
    look_dy_ = 0.0f;
    have_last_mouse_ = false;
    set_cursor_mode(ws::ui::CursorMode::hidden);
  }

  void load_saved_png_preview() {
    const auto files = voxel::terrain_world_io::generated_map_files();
    voxel::terrain_world_io::generate_channel_preview_png(files);
    if (terrain_preview_texture_.id != 0) {
      r2d::destroy_texture(terrain_preview_texture_);
    }
    const std::string preview_path = files.channel_preview.string();
    terrain_preview_texture_ = r2d::load_texture(preview_path.c_str());
    if (terrain_preview_texture_.id == 0) {
      throw std::runtime_error("could not upload raw RGBA channel preview texture");
    }
    entry_preview_loaded_ = true;
    ws::core::info("voxel: showing raw RGBA channels from '{}'",
                   files.png.string());
  }

  void start_world_generation() {
    if (generation_in_progress_) return;
    const auto files = voxel::terrain_world_io::generated_map_files();
    const std::uint32_t seed = world_seed_;
    generation_status_ = "Generating 256 x 256 world data...";
    generation_in_progress_ = true;
    try {
      generation_job_ = std::async(std::launch::async, [files, seed]() {
        voxel::terrain_world_io::generate_png_map(files, seed);
      });
    } catch (const std::exception& exception) {
      generation_in_progress_ = false;
      generation_status_ = std::string("Could not start generation: ") +
                           exception.what();
    }
  }

  void poll_world_generation() {
    if (!generation_in_progress_ || !generation_job_.valid()) return;
    if (generation_job_.wait_for(std::chrono::seconds(0)) !=        std::future_status::ready) {
      return;
    }

    generation_in_progress_ = false;
    try {
      generation_job_.get();
      load_saved_png_preview();
      generation_status_ = "New PNG preview ready. Press PLAY WORLD to enter.";
    } catch (const std::exception& exception) {
      generation_status_ = std::string("Generation/load failed: ") +
                           exception.what();
      ws::core::error("voxel: {}", generation_status_);
    }
  }

  void entry_menu_action(const r2d::FrameSize& frame) {
    if (!clicked_) return;
    clicked_ = false;

    const auto seed_controls = entry_seed_controls(frame);
    if (!generation_in_progress_ && entry_hit(
            seed_controls[0], mouse_x_, mouse_y_)) {
      --world_seed_;
      return;
    }
    if (!generation_in_progress_ && entry_hit(
            seed_controls[1], mouse_x_, mouse_y_)) {
      ++world_seed_;      return;
    }
    if (!generation_in_progress_ && entry_hit(
            seed_controls[2], mouse_x_, mouse_y_)) {
      const auto ticks = std::chrono::steady_clock::now()
                             .time_since_epoch().count();
      world_seed_ = static_cast<std::uint32_t>(
          static_cast<std::uint64_t>(ticks) ^
          (static_cast<std::uint64_t>(ticks) >> 32u) ^
          (static_cast<std::uint64_t>(world_seed_) * 0x9E3779B97F4A7C15ull));
      return;
    }

    const auto buttons = entry_buttons(frame);
    if (entry_hit(buttons[0], mouse_x_, mouse_y_)) {
      start_world_generation();
    } else if (!generation_in_progress_ &&
               entry_hit(buttons[1], mouse_x_, mouse_y_)) {
      try {
        load_saved_png_preview();
        generation_status_ = "Saved PNG preview loaded. Press PLAY WORLD to enter.";
      } catch (const std::exception& exception) {
        generation_status_ = std::string("Could not load saved map: ") +
                             exception.what();
      }
    } else if (!generation_in_progress_ &&
               entry_preview_loaded_ &&
               entry_hit(buttons[2], mouse_x_, mouse_y_)) {
      enter_current_world();
    } else if (!generation_in_progress_ &&
               entry_hit(buttons[3], mouse_x_, mouse_y_)) {
      request_stop();
    }
  }

  void draw_entry_button(
      const UiRect& bounds, const char* label, bool hovered,
      bool disabled = false) {
    const auto fill = disabled ? r2d::rgba(39, 43, 48, 210)
        : hovered ? r2d::rgba(87, 116, 68, 255)
                  : r2d::rgba(47, 61, 49, 245);
    r2d::draw_rounded_quad(bounds.x, bounds.y, bounds.w, bounds.h, 8.0f, fill);
    const float text_width =
        r2d::draw_text(-1000.0f, -1000.0f, label, font_);
    (void)r2d::draw_text(
        bounds.x + (bounds.w - text_width) * 0.5f,
        bounds.y + (bounds.h - 16.0f) * 0.5f,
        label, font_, disabled ? r2d::rgba(130, 137, 137)
                               : r2d::rgba(239, 243, 229));
  }

  void draw_entry_menu(const r2d::FrameSize& frame) {
    if (font_.empty()) return;
    const UiRect panel = entry_panel(frame);
    r2d::draw_quad(0.0f, 0.0f, frame.width, frame.height,
                   r2d::rgba(8, 12, 17, 170));
    r2d::draw_rounded_quad(panel.x, panel.y + 7.0f, panel.w, panel.h,
                           16.0f, r2d::rgba(0, 0, 0, 130));
    r2d::draw_rounded_quad(panel.x, panel.y, panel.w, panel.h,
                           16.0f, r2d::rgba(22, 29, 34, 248));
    r2d::draw_rounded_quad(panel.x + 2.0f, panel.y + 2.0f,
                           panel.w - 4.0f, 66.0f, 14.0f,
                           r2d::rgba(39, 54, 47, 255));

    const char* title = "VOXEL WORLDS";
    const float title_width =
        r2d::draw_text(-1000.0f, -1000.0f, title, font_);
    (void)r2d::draw_text(panel.x + (panel.w - title_width) * 0.5f,
                         panel.y + 22.0f, title, font_,
                         r2d::rgba(239, 247, 225));
    const auto controls = entry_seed_controls(frame);
    r2d::draw_rounded_quad(controls[0].x, controls[0].y,
                           controls[0].w, controls[0].h, 6.0f,
                           r2d::rgba(54, 63, 59, 255));
    r2d::draw_rounded_quad(controls[1].x, controls[1].y,
                           controls[1].w, controls[1].h, 6.0f,
                           r2d::rgba(54, 63, 59, 255));
    r2d::draw_rounded_quad(controls[2].x, controls[2].y,
                           controls[2].w, controls[2].h, 6.0f,
                           r2d::rgba(54, 63, 59, 255));
    (void)r2d::draw_text(controls[0].x + 17.0f, controls[0].y + 9.0f,
                         "-", font_, r2d::rgba(245, 245, 235));
    (void)r2d::draw_text(controls[1].x + 17.0f, controls[1].y + 9.0f,
                         "+", font_, r2d::rgba(245, 245, 235));
    (void)r2d::draw_text(controls[2].x + 12.0f, controls[2].y + 9.0f,
                         "randomize", font_, r2d::rgba(225, 233, 218));
    const auto buttons = entry_buttons(frame);
    const bool saved = saved_map_available();
    const char* labels[4] = {
        generation_in_progress_ ? "GENERATING PNG..." : "GENERATE / REGENERATE PNG",
        "LOAD SAVED PNG  /  PREVIEW",
        "PLAY WORLD",
        "QUIT GAME"};
    for (std::size_t i = 0; i < buttons.size(); ++i) {
      const bool disabled = (generation_in_progress_ && i != 0u) ||
                            (i == 1u && !saved) ||
                            (i == 2u && !entry_preview_loaded_);
      const bool hovered = !disabled &&
          entry_hit(buttons[i], mouse_x_, mouse_y_);
      draw_entry_button(buttons[i], labels[i], hovered, disabled);
    }

    // Left side: a 2x2 channel sheet derived from the saved RGBA PNG.
    const float preview_x = panel.x + 28.0f;
    const float preview_y = panel.y + 118.0f;
    constexpr float preview_size = 480.0f;
    (void)r2d::draw_text(preview_x, panel.y + 88.0f, "RAW RGBA CHANNELS",
                         font_, r2d::rgba(181, 194, 178));
    r2d::draw_rounded_quad(preview_x - 3.0f, preview_y - 3.0f,
                           preview_size + 6.0f, preview_size + 6.0f, 5.0f,
                           r2d::rgba(8, 11, 13, 255));
    if (entry_preview_loaded_ && terrain_preview_texture_.id != 0) {
      r2d::draw_quad(preview_x, preview_y, preview_size, preview_size,
                     r2d::rgba(255, 255, 255, 255), terrain_preview_texture_);
      const float label_w = 184.0f;
      const float label_h = 25.0f;
      const float labels_x[4] = {preview_x + 6.0f, preview_x + 246.0f,
                                 preview_x + 6.0f, preview_x + 246.0f};
      const float labels_y[4] = {preview_y + 6.0f, preview_y + 6.0f,
                                 preview_y + 246.0f, preview_y + 246.0f};
      const char* channel_labels[4] = {
          "R  HEIGHT MSB", "G  HEIGHT LSB",
          "B  MATERIAL / FLAGS", "A  BIOME ID"};
      for (int i = 0; i < 4; ++i) {
        r2d::draw_quad(labels_x[i], labels_y[i], label_w, label_h,
                       r2d::rgba(8, 12, 14, 205));
        (void)r2d::draw_text(labels_x[i] + 7.0f, labels_y[i] + 5.0f,
                             channel_labels[i], font_,
                             r2d::rgba(245, 247, 239));
      }
    } else {
      r2d::draw_rounded_quad(preview_x, preview_y, preview_size,
                             preview_size, 3.0f, r2d::rgba(31, 39, 42, 255));
      (void)r2d::draw_text(preview_x + 100.0f, preview_y + 225.0f,
                           "No saved terrain PNG yet", font_,
                           r2d::rgba(155, 170, 160));
    }
    (void)r2d::draw_text(preview_x, panel.y + 608.0f,
                         "Each quadrant shows exact 8-bit channel intensity.",
                         font_, r2d::rgba(155, 170, 160));

    // Right side: seed controls and world actions.
    const float controls_x = panel.x + 560.0f;
    (void)r2d::draw_text(controls_x, panel.y + 94.0f,
                         "WORLD SEED", font_, r2d::rgba(181, 194, 178));
    char seed_text[48]{};
    std::snprintf(seed_text, sizeof(seed_text), "%u", world_seed_);
    (void)r2d::draw_text(controls_x + 130.0f, panel.y + 94.0f,
                         seed_text, font_, r2d::rgba(220, 226, 210));
    if (!generation_status_.empty()) {
      (void)r2d::draw_text(controls_x, panel.y + 500.0f,
                           generation_status_.c_str(), font_,
                           r2d::rgba(194, 211, 179));
    }
  }

  void draw_entry_menu_frame(const r2d::FrameSize& frame) {
    r2d::begin();
    r2d::set_layer(0);
    r2d::draw_quad(
        0.0f, 0.0f, frame.width, frame.height,
        entry_preview_loaded_ ? r2d::rgba(135, 196, 250)
                              : r2d::rgba(14, 21, 25));
    r2d::end();

    // The title screen is a 2D data-map viewer; no voxel world is imported
    // or rendered until the player explicitly selects PLAY WORLD.

    r2d::begin();
    r2d::set_layer(10);
    draw_entry_menu(frame);
    r2d::end();
  }

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

    // Compact, texture-backed hotbar. Geometry and pointer hit testing
    // share hotbar.hpp, so the HUD and input cannot drift apart.
    const auto slots = voxel::hotbar::layout(frame.width, frame.height);
    const float bx = slots.front().bounds.x;
    const float by = slots.front().bounds.y;
    const float bar_w = voxel::hotbar::kSlotStep * kHotbarSlots;
    r2d::draw_rounded_quad(
        bx - voxel::hotbar::kPanelPadding, by - 7.0f,
        bar_w + 2.0f * voxel::hotbar::kPanelPadding,
        voxel::hotbar::kPanelHeight, 9, r2d::rgba(17,21,25,218));
    for (int i = 0; i < kHotbarSlots; ++i) {
      const auto& slot = slots[static_cast<std::size_t>(i)];
      const float sx = slot.bounds.x;
      const float sy = slot.bounds.y;
      const float slot_size = slot.bounds.w;
      const bool selected = slot.index == hotbar_;
      const auto& stack = inventory_.slot(static_cast<std::size_t>(i));
      r2d::draw_rounded_quad(sx, sy, slot_size, slot_size, 6,
          selected ? r2d::rgba(139,177,91,255)
                   : r2d::rgba(78,87,83,255));
      r2d::draw_rounded_quad(sx + 2, sy + 2, slot_size - 4, slot_size - 4, 5,
          selected ? r2d::rgba(35,47,34,255)
                   : r2d::rgba(31,36,40,245));
      if (!stack.empty()) {
        draw_item_icon(stack.item, sx + 9, sy + 8, 32);
        char amount[8];
        std::snprintf(amount, sizeof(amount), "%u", stack.count);
        (void)r2d::draw_text(sx + 34, sy + 34, amount, font_,
                             r2d::rgba(255,255,255));
      }
      char key[2] = {static_cast<char>('1' + i), '\0'};
      (void)r2d::draw_text(sx + 5, sy + 4, key, font_,
                           selected ? r2d::rgba(236,246,217)
                                    : r2d::rgba(170,180,174));
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
    char gpu_stats[64];
    if (!gpu_timing_supported_) {
      std::snprintf(gpu_stats, sizeof(gpu_stats), "gpu n/a");
    } else if (!gpu_frame_valid_) {
      std::snprintf(gpu_stats, sizeof(gpu_stats), "gpu pending");
    } else {
      std::snprintf(gpu_stats, sizeof(gpu_stats),
                    "gpu %.2fms p95 %.2fms", last_gpu_frame_ms_,
                    gpu_frame_percentiles_.p95_ms);
    }
    char stats[384];
    const Vec3 p = player_.position();
    std::snprintf(stats, sizeof(stats),
                  "fps %u   pos %.1f %.1f %.1f   %s\n"
                  "cpu %.2fms (phys %.2f mesh %.2f upload %.2f submit %.2f)\n"                  "%s   cpu p50/p95/p99 %.2f/%.2f/%.2fms  "
                  "upload %.1f KB  mesh jobs %u  draws %u tris %u",
                  fps_, p.x, p.y, p.z,
                  player_.grounded() ? "ground" : "air", last_timings_.total_ms,
                  last_timings_.physics_ms, last_timings_.cpu_mesh_ms,
                  last_timings_.upload_ms, last_timings_.draw_ms,
                  gpu_stats, frame_percentiles_.p50_ms, frame_percentiles_.p95_ms,
                  frame_percentiles_.p99_ms,
                  static_cast<double>(last_timings_.upload_bytes) / 1024.0,
                  last_timings_.mesh_jobs, last_counters_.draw_calls,
                  last_counters_.triangles);
    (void)r2d::draw_text(12.0f, 12.0f, stats, font_, r2d::rgba(235, 235, 240));
    (void)r2d::draw_text(
        12.0f, 64.0f,
        "LMB break - RMB place - 1-5 select - WASD move - space jump - "
        "shift sprint - E inventory - C crafting - P settings - F1-F6 craft - esc menu",
        font_, r2d::rgba(160, 164, 174));
  }

  void draw_item_icon(voxel::Item item, float x, float y, float size) {
    const int tile = item_tile(item);
    if (tile >= 0 && tile < voxel::atlas::kTileCount) {
      r2d::draw_quad(x, y, size, size, r2d::rgba(255,255,255),
                     tile_textures_[static_cast<std::size_t>(tile)]);
    } else {
      // Crafted non-block items have no matching world texture.
      r2d::draw_rounded_quad(x, y, size, size, 5.0f, item_tint(item));
      if (item == voxel::Item::sticks) {
        r2d::draw_line(x + size*0.25f, y + size*0.78f,
                       x + size*0.76f, y + size*0.20f, 4.0f,
                       r2d::rgba(226,184,119));
        r2d::draw_line(x + size*0.40f, y + size*0.85f,
                       x + size*0.90f, y + size*0.28f, 3.0f,
                       r2d::rgba(150,105,62));
      } else {
        r2d::draw_rounded_quad(x + size*0.12f, y + size*0.34f,
                               size*0.76f, size*0.34f, 4.0f,
                               r2d::rgba(235,239,245));
      }
    }
  }

  void inventory_action(const r2d::FrameSize& frame) {
    if (!clicked_) return;
    clicked_ = false;
    if (inventory_open_) {
      const float x = frame.width * 0.5f - 350.0f;
      const float y = (frame.height - 370.0f) * 0.5f;
      const float gx = x + 28.0f, gy = y + 100.0f;
      for (int i = 0; i < static_cast<int>(voxel::kInventorySize); ++i) {
        const int col = i % 9, row = i / 9;
        const float sx = gx + col * 54.0f, sy = gy + row * 56.0f;
        if (mouse_x_ < sx || mouse_x_ > sx + 48.0f ||
            mouse_y_ < sy || mouse_y_ > sy + 48.0f) continue;
        if (i < kHotbarSlots) {
          hotbar_ = i;
        } else {
          inventory_.swap_slots(static_cast<std::size_t>(hotbar_),
                                static_cast<std::size_t>(i));
        }
        return;      }
    } else if (crafting_open_) {
      const float x = frame.width * 0.5f - 270.0f;
      const float y = (frame.height - 430.0f) * 0.5f;
      for (std::size_t i = 0; i < voxel::kRecipes.size(); ++i) {
        const float sy = y + 100.0f + static_cast<float>(i) * 48.0f;
        if (mouse_x_ >= x + 24.0f && mouse_x_ <= x + 516.0f &&
            mouse_y_ >= sy && mouse_y_ <= sy + 42.0f) {
          (void)voxel::craft(inventory_, voxel::kRecipes[i]);
          return;
        }
      }
    }
  }

  void draw_inventory_panels(const r2d::FrameSize& frame) {
    if (font_.empty()) return;
    const bool inventory_view = inventory_open_;
    const float panel_w = inventory_view ? 700.0f : 540.0f;
    const float panel_h = inventory_view ? 370.0f : 430.0f;
    const float x = (frame.width - panel_w) * 0.5f;
    const float y = (frame.height - panel_h) * 0.5f;

    // Soft dimmer + layered panel, matching the resource pack's earthy palette.
    r2d::draw_quad(0, 0, frame.width, frame.height, r2d::rgba(7,10,15,150));
    r2d::draw_rounded_quad(x, y + 5, panel_w, panel_h, 12,
                           r2d::rgba(0,0,0,105));
    r2d::draw_rounded_quad(x, y, panel_w, panel_h, 12,
                           r2d::rgba(25,29,35,248));
    r2d::draw_rounded_quad(x + 2, y + 2, panel_w - 4, 48, 10,
                           r2d::rgba(42,49,57,255));
    r2d::draw_quad(x + 16, y + 49, panel_w - 32, 1,
                   r2d::rgba(112,139,91,210));

    if (inventory_view) {
      (void)r2d::draw_text(x + 22, y + 15, "INVENTORY",
                           font_, r2d::rgba(247,244,232));
      (void)r2d::draw_text(x + panel_w - 178, y + 17,
                           "E  CLOSE", font_, r2d::rgba(177,187,174));

      (void)r2d::draw_text(x + 28, y + 72, "ITEMS",
                           font_, r2d::rgba(177,187,174));
      const float gx = x + 28.0f, gy = y + 100.0f;
      for (int i = 0; i < static_cast<int>(voxel::kInventorySize); ++i) {
        const int col = i % 9, row = i / 9;
        const float sx = gx + col * 54.0f, sy = gy + row * 56.0f;
        const bool selected = i == hotbar_;
        const bool hovered = mouse_x_ >= sx && mouse_x_ <= sx + 48.0f &&
                             mouse_y_ >= sy && mouse_y_ <= sy + 48.0f;
        const auto& stack = inventory_.slot(static_cast<std::size_t>(i));
        r2d::draw_rounded_quad(sx, sy, 48, 48, 6,
            selected ? r2d::rgba(102,132,76,255)
                     : hovered ? r2d::rgba(83,94,82,255)
                               : r2d::rgba(47,54,60,255));
        r2d::draw_rounded_quad(sx + 2, sy + 2, 44, 44, 5,
            selected ? r2d::rgba(31,43,34,255)
                     : r2d::rgba(31,36,42,255));
        if (!stack.empty()) {
          draw_item_icon(stack.item, sx + 8, sy + 7, 31);
          char amount[8];
          std::snprintf(amount, sizeof(amount), "%u", stack.count);
          (void)r2d::draw_text(sx + 30, sy + 33, amount, font_,
                               r2d::rgba(255,255,255));
        }
        if (i < kHotbarSlots) {
          char key[2] = {static_cast<char>('1' + i), '\0'};
          (void)r2d::draw_text(sx + 4, sy + 3, key, font_,
                               r2d::rgba(190,198,184));
        }
      }

      // Selected-item inspector.
      const float ix = x + 525.0f, iy = y + 76.0f;
      r2d::draw_rounded_quad(ix, iy, 150, 220, 8,
                             r2d::rgba(35,41,46,255));
      (void)r2d::draw_text(ix + 14, iy + 14, "SELECTED",
                           font_, r2d::rgba(177,187,174));
      const auto& selected = inventory_.slot(static_cast<std::size_t>(hotbar_));
      if (!selected.empty()) {
        draw_item_icon(selected.item, ix + 48, iy + 48, 54);
        (void)r2d::draw_text(ix + 14, iy + 116, item_name(selected.item),
                             font_, r2d::rgba(246,244,232));
        char count[32];
        std::snprintf(count, sizeof(count), "Quantity: %u", selected.count);
        (void)r2d::draw_text(ix + 14, iy + 143, count, font_,
                             r2d::rgba(184,194,179));
      } else {
        (void)r2d::draw_text(ix + 14, iy + 58, "Empty slot",
                             font_, r2d::rgba(155,163,165));
      }
      (void)r2d::draw_text(x + 28, y + panel_h - 34,
          "Click a hotbar slot to select  |  Click another item to swap it into the hotbar",
          font_, r2d::rgba(164,174,164));
    } else {
      (void)r2d::draw_text(x + 22, y + 15, "CRAFTING",
                           font_, r2d::rgba(247,244,232));
      (void)r2d::draw_text(x + panel_w - 172, y + 17,
                           "C  CLOSE", font_, r2d::rgba(177,187,174));
      (void)r2d::draw_text(x + 24, y + 72, "RECIPES",
                           font_, r2d::rgba(177,187,174));
      for (std::size_t i = 0; i < voxel::kRecipes.size(); ++i) {
        const auto& recipe = voxel::kRecipes[i];
        const bool ready = voxel::can_craft(inventory_, recipe);
        const float sy = y + 100.0f + static_cast<float>(i) * 48.0f;
        r2d::draw_rounded_quad(x + 20, sy, 500, 42, 6,
            ready ? r2d::rgba(48,65,48,255) : r2d::rgba(39,44,49,255));
        draw_item_icon(recipe.output, x + 30, sy + 6, 30);
        char output[48];
        std::snprintf(output, sizeof(output), "%s  x%u",
                      item_name(recipe.output), recipe.output_count);
        (void)r2d::draw_text(x + 72, sy + 7, output, font_,
                             r2d::rgba(242,241,229));
        char key[16];
        std::snprintf(key, sizeof(key), "F%u", static_cast<unsigned>(i + 1));
        (void)r2d::draw_text(x + 452, sy + 7, key, font_,
                             ready ? r2d::rgba(174,222,145)
                                   : r2d::rgba(132,140,143));
        (void)r2d::draw_text(x + 72, sy + 24,
            ready ? "Ready to craft" : "Missing ingredients",
            font_, ready ? r2d::rgba(165,211,143)
                         : r2d::rgba(145,151,153));
      }
      (void)r2d::draw_text(x + 24, y + panel_h - 30,
          "Click a recipe or press F1-F6  |  C to close",
          font_, r2d::rgba(164,174,164));
    }
  }

  // --- the settings panel ------------------------------------------------------------
  void settings_action(const r2d::FrameSize& frame) {
    if (!clicked_) return;
    clicked_ = false;
    const int index = voxel::settings_menu::selected(
        static_cast<float>(frame.width), static_cast<float>(frame.height),
        mouse_x_, mouse_y_);
    if (index < 0) return;
    const auto sliders = voxel::settings_menu::layout(
        static_cast<float>(frame.width), static_cast<float>(frame.height));
    const float value = voxel::settings_menu::value_from_x(
        sliders[static_cast<std::size_t>(index)], mouse_x_);
    if (index == 0) {
      settings_.mouse_sensitivity = value;
      player_.set_mouse_sensitivity(value);
    } else if (index == 1) {
      settings_.fov_degrees = value;
    } else {
      settings_.render_distance = value;
    }
  }

  void draw_settings_panel(const r2d::FrameSize& frame) {
    if (font_.empty()) return;
    const float x = (frame.width - voxel::settings_menu::kPanelWidth) * 0.5f;
    const float y = (frame.height - voxel::settings_menu::kPanelHeight) * 0.5f;
    r2d::draw_quad(0, 0, frame.width, frame.height, r2d::rgba(7,10,15,155));
    r2d::draw_rounded_quad(x, y + 5, voxel::settings_menu::kPanelWidth,
                           voxel::settings_menu::kPanelHeight, 12, r2d::rgba(0,0,0,110));
    r2d::draw_rounded_quad(x, y, voxel::settings_menu::kPanelWidth,
                           voxel::settings_menu::kPanelHeight, 12, r2d::rgba(25,29,35,250));
    r2d::draw_rounded_quad(x + 2, y + 2, voxel::settings_menu::kPanelWidth - 4,
                           48, 10, r2d::rgba(42,49,57,255));
    (void)r2d::draw_text(x + 22, y + 15, "SETTINGS", font_, r2d::rgba(247,244,232));
    (void)r2d::draw_text(x + 365, y + 17, "P / ESC  CLOSE", font_, r2d::rgba(177,187,174));
    const auto sliders = voxel::settings_menu::layout(
        static_cast<float>(frame.width), static_cast<float>(frame.height));
    const float values[voxel::settings_menu::kSliderCount] = {
        settings_.mouse_sensitivity, settings_.fov_degrees, settings_.render_distance};
    const char* labels[voxel::settings_menu::kSliderCount] = {
        "Mouse sensitivity", "Field of view", "Render distance"};
    char formatted[voxel::settings_menu::kSliderCount][32]{};
    std::snprintf(formatted[0], sizeof(formatted[0]), "%.4f", values[0]);
    std::snprintf(formatted[1], sizeof(formatted[1]), "%.0f deg", values[1]);
    std::snprintf(formatted[2], sizeof(formatted[2]), "%.0f blocks", values[2]);
    for (int i = 0; i < voxel::settings_menu::kSliderCount; ++i) {
      const auto& slider = sliders[static_cast<std::size_t>(i)];
      const float t = (values[i] - slider.minimum) / (slider.maximum - slider.minimum);
      const float knob_x = slider.bounds.x + t * slider.bounds.w;
      (void)r2d::draw_text(slider.bounds.x, slider.bounds.y - 30.0f, labels[i],
                           font_, r2d::rgba(232,235,225));
      (void)r2d::draw_text(slider.bounds.x + slider.bounds.w - 90.0f,
                           slider.bounds.y - 30.0f, formatted[i], font_,
                           r2d::rgba(177,211,151));
      r2d::draw_rounded_quad(slider.bounds.x, slider.bounds.y, slider.bounds.w,
                             slider.bounds.h, 6, r2d::rgba(62,70,72,255));
      r2d::draw_rounded_quad(slider.bounds.x, slider.bounds.y, slider.bounds.w * t,
                             slider.bounds.h, 6, r2d::rgba(112,151,83,255));
      r2d::draw_rounded_quad(knob_x - 7.0f, slider.bounds.y - 5.0f, 14.0f,
                             slider.bounds.h + 10.0f, 6, r2d::rgba(226,235,211,255));
    }
    (void)r2d::draw_text(x + 32, y + 272,
        "Click a track to adjust  |  Changes apply immediately",
        font_, r2d::rgba(164,174,164));
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

    // Measure the actual font run, then center it with the pure menu layout.
    const float title_width = r2d::draw_text(-1000.0f, -1000.0f, "paused", font_);
    const auto title = voxel::menu::layout_title(
        static_cast<float>(frame.width), static_cast<float>(frame.height),
        title_width);
    (void)r2d::draw_text(title.x, title.y, "paused", font_,
                         r2d::rgba(235, 235, 240));

    // The buttons: the hovered one brightens (the live mouse position
    // is the OS cursor's own - the pointer is free while the menu is
    // up), the rest sit at rest.
    const auto buttons = voxel::menu::layout(
        static_cast<float>(frame.width), static_cast<float>(frame.height));
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
    voxel_renderer_.request_rebuild_chunk(world_, cx, cz);
    // If the block sits on a chunk boundary, rebuild the neighbor too.
    if (x % voxel::kChunkSizeX == 0 && cx > 0) {
      voxel_renderer_.request_rebuild_chunk(world_, cx - 1, cz);
    }
    if (x % voxel::kChunkSizeX == voxel::kChunkSizeX - 1 && cx < voxel::kWorldChunksX - 1) {
      voxel_renderer_.request_rebuild_chunk(world_, cx + 1, cz);
    }
    if (z % voxel::kChunkSizeZ == 0 && cz > 0) {
      voxel_renderer_.request_rebuild_chunk(world_, cx, cz - 1);
    }
    if (z % voxel::kChunkSizeZ == voxel::kChunkSizeZ - 1 && cz < voxel::kWorldChunksZ - 1) {
      voxel_renderer_.request_rebuild_chunk(world_, cx, cz + 1);
    }
  }

  void play_sound(int id, float gain, const Vec3& source_position) {
    if (!audio_ok_ || id < 0 || id >= 2) return;
    const auto& dry = id == kSoundBreak ? break_pcm_ : place_pcm_;
    if (dry.empty()) return;

    // Fixed stack storage: the early-reflection pass adds no heap work to
    // a break/place event. The transient AudioEngine overload copies PCM
    // into an owned OpenAL buffer before this array leaves scope.
    constexpr std::uint32_t kMaxProcessedFrames = 16384;
    std::array<std::int16_t, kMaxProcessedFrames> processed;
    const auto response = voxel::acoustics::room_response(
        world_, player_.eye_position(), kRate);
    const auto output_frames = ws::audio::process_early_reflections(
        dry.data(), static_cast<std::uint32_t>(dry.size()), 1,
        processed.data(), kMaxProcessedFrames, response);
    if (output_frames == 0) return;

    const ws::audio::SoundDesc desc{
        processed.data(), output_frames, 1, kRate};
    const auto source = audio_.play(desc, gain);
    if (source.has_value()) {
      (void)audio_.set_position(*source, source_position.x,
                                source_position.y, source_position.z);
    }
  }

  // --- the state -------------------------------------------------------------------
  static constexpr std::uint32_t kSeed = 0xC0FFEEu;
  static constexpr int kSoundBreak = 0;
  static constexpr int kSoundPlace = 1;

  voxel::World world_;
  voxel::VoxelRenderer voxel_renderer_;
  voxel::Player player_;
  voxel::settings_menu::Settings settings_{};
  bool settings_open_ = false;
  ws::ui::r3d::Mesh target_outline_{};
  voxel::World::RayHit target_hit_{};
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
  // The title screen owns generation/loading until the player selects a world.
  bool entry_menu_ = false;
  bool world_loaded_ = false;
  bool entry_preview_loaded_ = false;
  r2d::Texture terrain_preview_texture_{};
  std::uint32_t world_seed_ = kSeed;
  bool generation_in_progress_ = false;
  std::future<void> generation_job_{};
  std::string generation_status_ =
      "No world loaded. Load a saved PNG or generate a new map.";
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
  voxel::perf::FrameTimeWindow frame_times_{};
  voxel::perf::FrameTimeWindow::Percentiles frame_percentiles_{};
  voxel::perf::FrameTimeWindow gpu_frame_times_{};
  voxel::perf::FrameTimeWindow::Percentiles gpu_frame_percentiles_{};
  std::uint64_t last_gpu_frame_serial_ = 0;
  float last_gpu_frame_ms_ = 0.0f;
  bool gpu_timing_supported_ = false;
  bool gpu_frame_valid_ = false;
  std::chrono::steady_clock::time_point percentile_clock_{};
  voxel::bench::Driver bench_{};
  voxel::bench::ShadowFlags benchmark_shadow_flags_{};
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
  config.width = voxel::bench::bench_dimension_from_env(
      "WS_VOXEL_BENCH_WIDTH", 1280, 320, 7680);
  config.height = voxel::bench::bench_dimension_from_env(
      "WS_VOXEL_BENCH_HEIGHT", 720, 240, 4320);
  config.vsync = voxel::bench::bench_vsync_from_env(true);
  if (voxel::bench::bench_seconds_from_env() > 0.0f) {
    std::printf("bench:launch,width=%d,height=%d,vsync=%s\n",
                config.width, config.height,
                config.vsync ? "on" : "off");
    std::fflush(stdout);
  }
  VoxelApp game{config};
  return game.run();
}
#endif