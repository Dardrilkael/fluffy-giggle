// apps/voxel_game - a first-person voxel sandbox on the ws stack.
//
// THE COMPOSITION (bounce's shape, four libs deep):
//   ws::ui      - the App shell (window, Vulkan, r2d/r3d), the HUD's
//                 text/line/quad draws, the input frame
//   ws::physics - physics3d: the dynamic Box player against a sparse
//                 static grid of the surface-band solid blocks
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
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <optional>
#include <unordered_map>
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
#include <ws/physics/3d/body.hpp>
#include <ws/physics/3d/contact.hpp>
#include <ws/physics/3d/shape.hpp>
#include <ws/physics/3d/world.hpp>
#include <ws/physics/common.hpp>  // kMaxBodiesPerWorld (the shell's budget)
#include <ws/ui/app.hpp>
#include <ws/ui/renderer2d.hpp>
#include <ws/ui/renderer3d.hpp>

#include "menu.hpp"
#include "player.hpp"
#include "renderer.hpp"
#include "world.hpp"

namespace {

namespace r2d = ws::ui::r2d;
namespace r3d = ws::ui::r3d;
namespace pd = ws::physics3d;
using ws::math::Vec3;

// GLFW key codes (input.hpp's contract: printables match ASCII).
constexpr int kKeyW = 87;
constexpr int kKeyA = 65;
constexpr int kKeyS = 83;
constexpr int kKeyD = 68;
constexpr int kKeySpace = 32;
constexpr int kKeyShiftL = 340;  // GLFW_KEY_LEFT_SHIFT
constexpr int kKey1 = 49;

// The hotbar's place-able types, slot order (keys 1-5).
constexpr voxel::Block kHotbar[5] = {
    voxel::Block::grass, voxel::Block::dirt, voxel::Block::stone,
    voxel::Block::wood, voxel::Block::sand,
};
constexpr int kHotbarSlots = 5;
constexpr const char* kHotbarNames[kHotbarSlots] = {"grass", "dirt",
                                                    "stone", "wood", "sand"};

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

    // --- physics: the world's gravity + the player --------------------------
    pd::World3D::Desc desc{};
    desc.gravity = Vec3{0.0f, player_.tuning_gravity(), 0.0f};
    desc.fixed_dt = 1.0f / 60.0f;
    desc.thread_count = 0;  // single-threaded: deterministic, simple
    // The collision shell of a 128x128x64 world: ~17k exposed cells for
    // the shipped seed (the first build's 20000 answer was the shipped
    // bug - the old surface-BAND grid needed ~50k and the world died
    // at capacity, taking the player's body with it). The cap is the
    // lib's own published maximum; the shell is measured well under it
    // (the regression test pins the margin), the player's own body sits
    // in the same budget.
    desc.max_bodies = ws::physics::kMaxBodiesPerWorld;
    auto world3d = pd::World3D::create(desc);
    if (!world3d.has_value()) {
      ws::core::log_error(world3d.error());
      dead_ = true;
      return;
    }
    phys_.emplace(std::move(*world3d));

    // The collision shell: one static Box per EXPOSED solid cell (see
    // World::is_exposed) - the terrain's skin, the trunks, every
    // reachable face. Buried cells never collide (nothing can touch
    // them), and every edit re-derives membership around the changed
    // cell (a dig exposes the wall behind it, a place buries what it
    // covers), so digging deep and tunneling mid-hill just work.
    build_physics_grid();

    if (!player_.spawn(*phys_, spawn)) {
      dead_ = true;
      return;
    }

    // --- the chunk meshes (the renderer's one-time upload) -----------------
    world_.rebuild_all();

    // --- the HUD font -------------------------------------------------------
    font_ = r2d::load_default_font(16.0f,
                                   r2d::FontBakeOptions::dark_chrome());

    // --- the block-texture atlas (the uploaded resource pack) ---------------
    atlas_ = voxel::load_atlas_texture();
    if (atlas_.id == 0) {
      ws::core::warn(
          "voxel: the texture atlas fell back to white - flat tints");
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
  }

  void on_update() override {
    if (dead_ || !phys_.has_value()) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    const float dt =
        last_time_.has_value()
            ? std::chrono::duration<float>(now - *last_time_).count()
            : 0.0f;
    last_time_ = now;
    const float frame_dt = std::clamp(dt, 0.0f, 0.05f);

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

      voxel::begin_scene(player_, frame.width / frame.height);
      voxel::draw_world(world_, atlas_);
      r2d::end();

      r2d::begin();
      r2d::set_layer(10);
      draw_hud(frame);
      draw_pause_menu(frame);
      r2d::end();
      return;
    }

    // --- the controller tick -------------------------------------------------
    voxel::Player::MoveInput move{};
    move.forward = input.key_down[kKeyW];
    move.back = input.key_down[kKeyS];
    move.left = input.key_down[kKeyA];
    move.right = input.key_down[kKeyD];
    move.jump = input.key_down[kKeySpace];
    move.sprint = input.key_down[kKeyShiftL];
    move.look_dx = look_dx_;
    move.look_dy = look_dy_;
    look_dx_ = 0.0f;  // consumed: the next frame starts fresh
    look_dy_ = 0.0f;

    player_.tick(move, frame_dt, *phys_);
    phys_->advance(frame_dt);
    player_.on_contacts(phys_->contact_events());
    (void)audio_.update();

    // --- hotbar + break/place ------------------------------------------------
    for (int i = 0; i < kHotbarSlots; ++i) {
      if (input.key_pressed[kKey1 + i]) {
        hotbar_ = i;
      }
    }
    if (input.mouse_pressed[0] || input.mouse_pressed[1]) {
      const Vec3 eye = player_.eye_position();
      const Vec3 look = player_.look_direction();
      const float origin[3] = {eye.x, eye.y, eye.z};
      const float dir[3] = {look.x, look.y, look.z};
      const voxel::World::RayHit hit = world_.raycast(origin, dir, 7.0f);
      if (input.mouse_pressed[0] && hit.hit) {  // break
        if (world_.set_block(hit.cell[0], hit.cell[1], hit.cell[2],
                             voxel::Block::air)) {
          rebuild_around(hit.cell[0], hit.cell[2]);
          update_shell(hit.cell[0], hit.cell[1], hit.cell[2]);
          play_sound(kSoundBreak, 0.9f);
        }
      } else if (input.mouse_pressed[1] && hit.hit) {  // place
        const int px = hit.cell[0] + hit.normal[0];
        const int py = hit.cell[1] + hit.normal[1];
        const int pz = hit.cell[2] + hit.normal[2];
        const voxel::Block place = kHotbar[hotbar_];
        // Place only into air, and never entomb the player (the AABB
        // overlap rule - the classic voxel place guard).
        if (world_.block_at(px, py, pz) == voxel::Block::air &&
            !overlaps_player(px, py, pz)) {
          if (world_.set_block(px, py, pz, place)) {
            rebuild_around(px, pz);
            update_shell(px, py, pz);
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

    voxel::begin_scene(player_, frame.width / frame.height);
    voxel::draw_world(world_, atlas_);
    r2d::end();

    r2d::begin();
    r2d::set_layer(10);
    draw_hud(frame);
    r2d::end();
  }

 private:
  // --- the collision shell -------------------------------------------------------
  void build_physics_grid() {
    // The shell walk: a column's stack up to its highest solid block
    // (surface_height - a trunk top counts; the trunk's cells and the
    // ground under it are all shell candidates, the first build's
    // band missed the ground under trees). Buried cells read false and
    // are skipped; the walk is y-major per column so the early exit
    // from the top bounds the cost.
    for (int z = 0; z < voxel::kWorldSizeZ; ++z) {
      for (int x = 0; x < voxel::kWorldSizeX; ++x) {
        const int top = world_.surface_height(x, z);
        for (int y = top; y >= 0; --y) {
          if (world_.is_exposed(x, y, z)) {
            add_physics_at(x, y, z);
          }
        }
      }
    }
    if (cells_refused_ > 0) {
      ws::core::warn("voxel: collision shell clipped by the body cap - "
                     "the far corners may be soft");
    }
  }

  void add_physics_at(int x, int y, int z) {
    const std::uint64_t key = cell_key(x, y, z);
    if (physics_cells_.find(key) != physics_cells_.end()) {
      return;  // already standing
    }
    pd::BodyDesc desc{};
    desc.kind = ws::physics::BodyKind::static_;
    desc.shape = pd::Box{Vec3{0.5f, 0.5f, 0.5f}};
    desc.transform = pd::Transform{
        Vec3{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f,
             static_cast<float>(z) + 0.5f},
        {}};
    const auto id = phys_->create_body(desc);
    if (id.has_value()) {
      physics_cells_[key] = *id;
    } else if (!cells_refused_) {
      // Once, not per cell: a capacity refusal would otherwise flood
      // the log with one error per remaining cell (the user's #3
      // report was exactly this symptom).
      ++cells_refused_;
      ws::core::log_error(id.error());
    } else {
      ++cells_refused_;
    }
  }

  void remove_physics_at(int x, int y, int z) {
    const auto it = physics_cells_.find(cell_key(x, y, z));
    if (it == physics_cells_.end()) {
      return;
    }
    (void)phys_->destroy_body(it->second);
    physics_cells_.erase(it);
  }

  // Re-derives shell membership for one cell and its 6 neighbors: the
  // edit's whole blast radius. A break exposes the wall behind it (the
  // formerly-buried neighbor now has a face); a place buries the cells
  // it covers (their faces against it are gone) while itself standing
  // if it has any open face. Membership is is_exposed - the same
  // authority the build walk used - so the shell never drifts from
  // the table it mirrors.
  void update_shell(int x, int y, int z) {
    static constexpr int kDirs[6][3] = {{0, 1, 0},  {0, -1, 0}, {1, 0, 0},
                                         {-1, 0, 0}, {0, 0, 1},  {0, 0, -1}};
    if (world_.is_exposed(x, y, z)) {
      add_physics_at(x, y, z);
    } else {
      remove_physics_at(x, y, z);
    }
    for (const auto& d : kDirs) {
      const int nx = x + d[0];
      const int ny = y + d[1];
      const int nz = z + d[2];
      if (nx < 0 || nx >= voxel::kWorldSizeX || nz < 0 ||
          nz >= voxel::kWorldSizeZ || ny < 0 || ny >= voxel::kChunkSizeY) {
        continue;
      }
      if (world_.is_exposed(nx, ny, nz)) {
        add_physics_at(nx, ny, nz);
      } else {
        remove_physics_at(nx, ny, nz);
      }
    }
  }

  [[nodiscard]] static std::uint64_t cell_key(int x, int y, int z) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 40) |
           (static_cast<std::uint64_t>(static_cast<std::uint32_t>(y)) << 20) |
           static_cast<std::uint64_t>(static_cast<std::uint32_t>(z));
  }

  // Would a block at (x, y, z) overlap the player's AABB? (The
  // player's box: 0.6 wide, 1.8 tall, centered at position_.)
  [[nodiscard]] bool overlaps_player(int x, int y, int z) const {
    const Vec3 p = player_.position();
    const float half_w = 0.3f + 0.01f;  // the block's half extent + skin
    const float half_h = 0.9f + 0.01f;
    const float bx = static_cast<float>(x) + 0.5f;
    const float by = static_cast<float>(y) + 0.5f;
    const float bz = static_cast<float>(z) + 0.5f;
    return std::abs(p.x - bx) < half_w + 0.5f &&
           std::abs(p.y - by) < half_h + 0.5f &&
           std::abs(p.z - bz) < half_w + 0.5f;
  }

  // Rebuilds the chunk containing the cell (x, z), and the neighbor
  // chunk too when the cell rides a chunk boundary (the neighbor's
  // culled face against this cell may need its own face now).
  void rebuild_around(int x, int z) {
    const int cx = x / voxel::kChunkSizeX;
    const int cz = z / voxel::kChunkSizeZ;
    world_.rebuild_chunk(cx, cz);
    const int lx = x % voxel::kChunkSizeX;
    const int lz = z % voxel::kChunkSizeZ;
    if (lx == 0 && cx > 0) {
      world_.rebuild_chunk(cx - 1, cz);
    }
    if (lx == voxel::kChunkSizeX - 1 && cx < voxel::kWorldChunksX - 1) {
      world_.rebuild_chunk(cx + 1, cz);
    }
    if (lz == 0 && cz > 0) {
      world_.rebuild_chunk(cx, cz - 1);
    }
    if (lz == voxel::kChunkSizeZ - 1 && cz < voxel::kWorldChunksZ - 1) {
      world_.rebuild_chunk(cx, cz + 1);
    }
  }

  void play_sound(int which, float gain) {
    if (!audio_ok_ || which < 0 || which >= sounds_loaded_) {
      return;
    }
    (void)audio_.play(sounds_[which], gain);
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

    // The hotbar: 5 slots along the bottom, the selected one framed.
    constexpr float slot_w = 96.0f;
    constexpr float bar_w = slot_w * kHotbarSlots;
    const float bx = (frame.width - bar_w) * 0.5f;
    const float by = frame.height - 48.0f;
    for (int i = 0; i < kHotbarSlots; ++i) {
      const float sx = bx + static_cast<float>(i) * slot_w;
      const bool selected = i == hotbar_;
      r2d::draw_quad(sx, by, slot_w - 6.0f, 38.0f,
                     r2d::rgba(28, 30, 36, 200));
      r2d::draw_quad(sx + 6.0f, by + 7.0f, 24.0f, 24.0f,
                     voxel::tint_of(kHotbar[i]));
      char label[24];
      std::snprintf(label, sizeof(label), "%d %s", i + 1, kHotbarNames[i]);
      (void)r2d::draw_text(sx + 36.0f, by + 10.0f, label, font_,
                           selected ? r2d::rgba(255, 255, 255)
                                    : r2d::rgba(168, 172, 182));
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

    // The stats + help lines, top-left.
    char stats[128];
    const Vec3 p = player_.position();
    std::snprintf(stats, sizeof(stats), "fps %u   pos %.1f %.1f %.1f   %s",
                  fps_, p.x, p.y, p.z, player_.grounded() ? "ground" : "air");
    (void)r2d::draw_text(12.0f, 12.0f, stats, font_, r2d::rgba(235, 235, 240));
    (void)r2d::draw_text(
        12.0f, 32.0f,
        "LMB break - RMB place - 1-5 select - WASD move - space jump - "
        "shift sprint - esc menu",
        font_, r2d::rgba(160, 164, 174));
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
  // --- the state -------------------------------------------------------------------
  static constexpr std::uint32_t kSeed = 0xC0FFEEu;
  static constexpr int kSoundBreak = 0;
  static constexpr int kSoundPlace = 1;

  voxel::World world_;
  std::optional<pd::World3D> phys_;
  std::unordered_map<std::uint64_t, pd::BodyId> physics_cells_{};
  // How many shell cells the body cap refused (0 = none; the warn-once
  // guard in add_physics_at keeps the log at one error + one warning).
  std::uint32_t cells_refused_ = 0;
  voxel::Player player_;
  int hotbar_ = 0;
  // The pause menu's state: paused_ gates the whole simulation (the
  // controller tick, physics, break/place all skip while it is up) and
  // owns the cursor mode; mouse_x_/mouse_y_ is the live OS pointer -
  // the menu hit-tests it; clicked_ is the frame's menu click,
  // consumed by the frame that reads it.
  bool paused_ = false;
  float mouse_x_ = 0.0f;
  float mouse_y_ = 0.0f;
  bool clicked_ = false;
  // The block-texture atlas (the uploaded resource pack's art, loaded
  // once at setup). id 0 = the white fallback - the frame shades the
  // flat tints alone.
  ws::ui::r2d::Texture atlas_{};
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
