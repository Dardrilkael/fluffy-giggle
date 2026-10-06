// apps/bounce - the ws::physics + ws::ui + ws::audio composition example.
//
// A "pit" of boxes and circles: click anywhere to drop one, ws::physics2d
// simulates them at a fixed 60 Hz tick, r2d draws the frame with render
// interpolation (prev_transforms -> transforms by interpolation_alpha),
// a ws-gui panel drives real app state (spawn/reset/sound/restitution),
// and every contact hit event above the speed threshold becomes a
// one-shot synthesized thud - pitch and gain follow the impact speed.
//
// Desktop and phone alike (chatter's entry pattern): on Android the glue
// hands android_main the shell handle, the system Roboto face stands in
// for segoeui, and primary touch already bridges to mouse button 0 in the
// ui backend - so tap-to-drop works with zero lib edits.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <numbers>
#include <optional>
#include <random>
#include <vector>

#ifdef __ANDROID__
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include <android_native_app_glue.h>  // android_app + the android_main contract
#include <unistd.h>                   // chdir
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#endif

#include <ws/audio/engine.hpp>
#include <ws/core/log.hpp>
#include <ws/math/vecmath.hpp>
#include <ws/physics/2d/body.hpp>
#include <ws/physics/2d/contact.hpp>
#include <ws/physics/2d/shape.hpp>
#include <ws/physics/2d/world.hpp>
#include <ws/ui/app.hpp>
#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/input_frame.hpp>
#include <ws/ui/gui/layout_cursor.hpp>
#include <ws/ui/gui/rect.hpp>
#include <ws/ui/gui/theme.hpp>
#include <ws/ui/gui/widgets.hpp>
#include <ws/ui/renderer2d.hpp>

namespace {

namespace r2d = ws::ui::r2d;
namespace gui = ws::ui::gui;
namespace p2d = ws::physics2d;
using ws::math::Vec2;

// --- the pit ------------------------------------------------------------------
// The world's units are pixels: r2d is y-down (world +y renders downward),
// so gravity is +y and the floor's top face sits kFloorOffset above the
// bottom edge. The walls' inner faces land exactly on x=0 and x=pit_w, so
// drops bounce off the window's side edges; both statics are created big
// enough for any window and only ever REpositioned by refit_pit().
constexpr float kGravityY = 1400.0f;     // px/s^2, down-screen
constexpr float kFloorOffset = 60.0f;    // floor top face above the bottom edge
constexpr float kFloorHalf = 30.0f;      // half of the floor's thickness
constexpr float kFloorHalfWidth = 4096.0f;  // covers any window, never refit
constexpr float kWallHalf = 60.0f;       // wall thickness; inner face at x=0/w
constexpr float kWallReach = 4000.0f;    // wall half-height, covers any window

constexpr std::uint32_t kBodiesBudget = 256;  // statics + the drop ring below
constexpr std::size_t kMaxDrops = 220;   // destroy-oldest past this
constexpr float kHitThreshold = 130.0f;  // px/s; slower impacts stay silent
constexpr int kPlaysPerFrame = 4;        // a collapsing pile must not spam

// --- sound: three synthesized thuds (the wavplayer in-memory pattern) ---------
constexpr std::uint32_t kRate = 44'100;
constexpr std::uint32_t kThudFrames = kRate * 90 / 1000;  // 90 ms

[[nodiscard]] std::vector<std::int16_t> synth_thud(double hz) {
  std::vector<std::int16_t> out(kThudFrames);
  for (std::uint32_t i = 0; i < kThudFrames; ++i) {
    const double t = static_cast<double>(i) / kRate;
    const double decay = std::exp(-t * 38.0);
    const double attack = std::min(1.0, i / 60.0);  // ~1 ms click-free onset
    const double tone =
        std::sin(2.0 * std::numbers::pi * hz * t) +
        0.45 * std::sin(2.0 * std::numbers::pi * hz * 2.76 * t);
    out[i] = static_cast<std::int16_t>(tone * decay * attack * 13'000.0);
  }
  return out;
}

constexpr std::uint32_t kDropPalette[8] = {
    r2d::rgba(255, 120, 60),  r2d::rgba(90, 200, 110),
    r2d::rgba(80, 160, 255),  r2d::rgba(200, 110, 255),
    r2d::rgba(255, 210, 70),  r2d::rgba(255, 110, 160),
    r2d::rgba(120, 230, 220), r2d::rgba(180, 180, 190),
};

// A live drop: its body id, its look, and the half-extents the draw needs
// (for circles half_w IS the radius and half_h is unused).
struct Drop {
  p2d::BodyId id{};
  std::uint32_t color = 0;
  bool round = false;
  float half_w = 0.0f;
  float half_h = 0.0f;
};

}  // namespace

class BounceApp final : public ws::ui::App {
 public:
  // The config comes from the platform entry (android_main supplies the
  // glue handle; the desktop supplies the window size) - chatter's shape.
  explicit BounceApp(const ws::ui::AppConfig& config)
      : App(config) {}

 protected:
  void on_setup() override {
    ui_scale_ = ui_scale();
    chrome_ = gui::kDefaultChrome.scaled(ui_scale_);
#ifdef __ANDROID__
    // System face this side of the fence - no font ships in the APK
    // (chatter's choice; an empty font just drops text, the pit lives on).
    font_ = r2d::load_font("/system/fonts/Roboto-Regular.ttf",
                           15.0f * ui_scale_);
#else
    font_ = r2d::load_font("C:/Windows/Fonts/segoeui.ttf", 15.0f * ui_scale_);
#endif
    chrome_.font = font_.empty() ? nullptr : &font_;

    // --- physics: the pit world ---------------------------------------------
    p2d::World2D::Desc desc{};
    desc.gravity = Vec2{0.0f, kGravityY};
    desc.fixed_dt = 1.0f / 60.0f;
    desc.max_bodies = kBodiesBudget;
    desc.max_contact_events = 512;
    desc.hit_speed_threshold = kHitThreshold;
    desc.thread_count = 0;  // a demo wants the simple, deterministic mode
    desc.max_substeps = 4;
    auto world = p2d::World2D::create(desc);
    if (!world.has_value()) {
      ws::core::log_error(world.error());
      dead_ = true;
      return;
    }
    world_.emplace(std::move(*world));

    floor_ = make_static(p2d::Box{Vec2{kFloorHalfWidth, kFloorHalf}});
    wall_left_ = make_static(p2d::Box{Vec2{kWallHalf, kWallReach}});
    wall_right_ = make_static(p2d::Box{Vec2{kWallHalf, kWallReach}});
    // Positions are dummy until refit_pit() runs on the first frame, before
    // any drop can exist - statics don't fall, so the gap is invisible.

    // --- audio: three thuds, low/mid/high ------------------------------------
    if (audio_.init()) {
      static constexpr double kThudHz[3] = {95.0, 190.0, 340.0};
      bool all_loaded = true;
      for (int i = 0; i < 3; ++i) {
        thud_pcm_[i] = synth_thud(kThudHz[i]);
        ws::audio::SoundDesc sound{};
        sound.samples = thud_pcm_[i].data();
        sound.frame_count = kThudFrames;
        sound.channels = 1;
        sound.sample_rate = kRate;
        const auto handle = audio_.load_sound(sound);
        if (handle.has_value()) {
          thuds_[i] = *handle;
        } else {
          all_loaded = false;
        }
      }
      if (!all_loaded) {
        audio_dead_ = true;
        ws::core::warn("bounce: sound load failed - running silent");
      }
    } else {
      audio_dead_ = true;
      ws::core::warn("bounce: audio device unavailable - running silent");
    }
  }

  void on_update() override {
    if (dead_ || !world_.has_value()) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    const float dt =
        last_time_.has_value()
            ? std::chrono::duration<float>(now - *last_time_).count()
            : 0.0f;
    last_time_ = now;
    const float frame_dt = std::clamp(dt, 0.0f, 0.05f);

    gui_.new_frame(gui::frame_input());
    r2d::begin();

    // Resize refit: FrameSize is the space the world lives in (DPI-correct,
    // unlike the resize event's screen coords). First frame always refits.
    const r2d::FrameSize frame = r2d::frame_size();
    if (frame.width != pit_w_ || frame.height != pit_h_) {
      refit_pit(frame.width, frame.height);
    }

    // --- panel (layer 10, above the world) ----------------------------------
    r2d::set_layer(10);
    const float scale = ui_scale_;
    const float panel_w = 200.0f * scale;
    const gui::Rect panel_bounds{frame.width - panel_w - 16.0f, 16.0f,
                                 panel_w, 210.0f * scale};
    const gui::Rect content =
        gui::begin_panel(gui_, layout_, chrome_, "controls", panel_bounds);
    const float widget_w = content.width;
    if (gui::button(gui_, layout_, chrome_, "drop 10", widget_w)) {
      for (int i = 0; i < 10; ++i) {
        spawn_random();
      }
    }
    if (gui::button(gui_, layout_, chrome_, "reset pit", widget_w)) {
      reset_drops();
    }
    (void)gui::checkbox(gui_, layout_, chrome_, "sound", sound_on_);
    (void)gui::slider_float(gui_, layout_, chrome_, "new drop bounce",
                            new_restitution_, 0.0f, 0.95f, widget_w);
    gui::end_panel(gui_, layout_);

    char status[96];
    std::snprintf(status, sizeof(status), "drops %zu   hits %llu   plays %llu",
                  drops_.size(), static_cast<unsigned long long>(hits_),
                  static_cast<unsigned long long>(plays_));
    r2d::draw_text(panel_bounds.x, panel_bounds.y + panel_bounds.height +
                                        12.0f * scale,
                   status, font_, r2d::rgba(225, 225, 230));

    // Click-to-drop: anywhere the panel does not own. Read before
    // end_frame() - the InputFrame belongs to this gui frame.
    const auto& input = gui::frame_input();
    if (input.mouse_pressed[0] &&
        !gui::contains(panel_bounds, input.mouse_x, input.mouse_y)) {
      spawn_drop(input.mouse_x, input.mouse_y);
    }
    gui_.end_frame();

    // --- step the world, turn hits into thuds -------------------------------
    world_->advance(frame_dt);
    int plays_this_frame = 0;
    for (const p2d::ContactEvent& event : world_->contact_events()) {
      if (event.phase != p2d::ContactPhase::hit) {
        continue;
      }
      ++hits_;
      if (!sound_on_ || audio_dead_ ||
          plays_this_frame >= kPlaysPerFrame) {
        continue;
      }
      const int variant =
          event.speed < 260.0f ? 0 : (event.speed < 600.0f ? 1 : 2);
      const float gain = std::clamp(
          0.12f + (event.speed - kHitThreshold) / 1400.0f, 0.12f, 0.9f);
      if (audio_.play(thuds_[variant], gain).has_value()) {
        ++plays_;
        ++plays_this_frame;
      }
    }
    (void)audio_.update();  // reap finished sources for reuse

    // --- the world (layer 0) -------------------------------------------------
    r2d::set_layer(0);
    r2d::draw_quad(0.0f, pit_h_ - kFloorOffset, pit_w_, 2.0f * kFloorHalf,
                   r2d::rgba(70, 74, 84));
    const float alpha = world_->interpolation_alpha();
    const std::span<const p2d::Transform> cur = world_->transforms();
    const std::span<const p2d::Transform> prev = world_->prev_transforms();
    for (const Drop& drop : drops_) {
      const p2d::Transform& c = cur[drop.id.index];
      const p2d::Transform& p = prev[drop.id.index];
      const float x = p.position.x + (c.position.x - p.position.x) * alpha;
      const float y = p.position.y + (c.position.y - p.position.y) * alpha;
      // Shortest-arc angle lerp: a spin crossing +-pi between ticks would
      // otherwise flip the sprite the long way around for one frame.
      constexpr float kTwoPi = 2.0f * std::numbers::pi_v<float>;
      float d_rot = c.rotation - p.rotation;
      if (d_rot > std::numbers::pi_v<float>) {
        d_rot -= kTwoPi;
      } else if (d_rot < -std::numbers::pi_v<float>) {
        d_rot += kTwoPi;
      }
      const float rot = p.rotation + d_rot * alpha;
      if (drop.round) {
        r2d::draw_rounded_quad(x - drop.half_w, y - drop.half_w,
                               2.0f * drop.half_w, 2.0f * drop.half_w,
                               drop.half_w, drop.color, rot);
      } else {
        // Texture{} spelled out: it sits between color and rotation in the
        // 2D signature, and without it this call resolves to the 3D
        // billboard overload (x, y, z, w, h, color, ...).
        r2d::draw_quad(x - drop.half_w, y - drop.half_h, 2.0f * drop.half_w,
                       2.0f * drop.half_h, drop.color, r2d::Texture{}, rot);
      }
    }

    r2d::end();
  }

 private:
  [[nodiscard]] p2d::BodyId make_static(const p2d::Shape& shape) {
    p2d::BodyDesc desc{};
    desc.kind = ws::physics::BodyKind::static_;
    desc.shape = shape;
    const auto id = world_->create_body(desc);
    return id.has_value() ? *id : p2d::BodyId{};
  }

  // Re-lays the statics over the current frame. Atomic bulk move: an
  // invalid id would fail the whole call, but the ids are create results.
  void refit_pit(float w, float h) {
    pit_w_ = w;
    pit_h_ = h;
    const std::vector<p2d::BodyMove> moves{
        p2d::BodyMove{floor_, Vec2{w * 0.5f, h - kFloorOffset + kFloorHalf},
                      0.0f},
        p2d::BodyMove{wall_left_, Vec2{-kWallHalf, h * 0.5f}, 0.0f},
        p2d::BodyMove{wall_right_, Vec2{w + kWallHalf, h * 0.5f}, 0.0f}};
    (void)world_->set_transforms(moves);
  }

  void spawn_random() {
    std::uniform_real_distribution<float> px(
        80.0f, std::max(160.0f, pit_w_ - 80.0f));
    std::uniform_real_distribution<float> py(50.0f, 160.0f);
    spawn_drop(px(rng_), py(rng_));
  }

  void spawn_drop(float x, float y) {
    if (drops_.size() >= kMaxDrops) {
      (void)world_->destroy_body(drops_.front().id);
      drops_.pop_front();
    }
    // Keep spawns inside the pit, clear of the walls and the floor.
    x = std::clamp(x, 24.0f, std::max(24.0f, pit_w_ - 24.0f));
    y = std::clamp(y, 24.0f, std::max(24.0f, pit_h_ - kFloorOffset - 24.0f));

    std::uniform_real_distribution<float> half_dist(10.0f, 24.0f);
    std::uniform_real_distribution<float> aspect_dist(0.7f, 1.4f);
    std::uniform_real_distribution<float> vel_dist(-80.0f, 80.0f);
    std::uniform_real_distribution<float> ang_dist(-3.0f, 3.0f);

    p2d::BodyDesc desc{};
    const bool round = (rng_() & 1u) != 0u;
    const float half_w = half_dist(rng_);
    float half_h = half_w;
    if (round) {
      desc.shape = p2d::Circle{half_w};
    } else {
      half_h = half_w * aspect_dist(rng_);
      desc.shape = p2d::Box{Vec2{half_w, half_h}};
    }
    desc.transform = p2d::Transform{Vec2{x, y}, 0.0f};
    desc.linear_velocity = Vec2{vel_dist(rng_), 0.0f};
    desc.angular_velocity = ang_dist(rng_);
    desc.restitution = new_restitution_;
    desc.friction = 0.3f;

    const auto id = world_->create_body(desc);
    if (!id.has_value()) {
      return;  // full budget: the destroy-oldest ring should prevent this
    }
    drops_.push_back(
        Drop{*id, kDropPalette[(next_palette_++) & 7u], round, half_w, half_h});
  }

  void reset_drops() {
    for (const Drop& drop : drops_) {
      (void)world_->destroy_body(drop.id);
    }
    drops_.clear();
  }

  // --- state -------------------------------------------------------------------
  std::optional<p2d::World2D> world_;
  p2d::BodyId floor_{};
  p2d::BodyId wall_left_{};
  p2d::BodyId wall_right_{};
  float pit_w_ = 0.0f;
  float pit_h_ = 0.0f;

  ws::audio::AudioEngine audio_;
  bool audio_dead_ = false;
  bool sound_on_ = true;
  std::vector<std::int16_t> thud_pcm_[3];
  ws::audio::SoundHandle thuds_[3]{};

  std::deque<Drop> drops_;
  float new_restitution_ = 0.45f;  // restitution for drops spawned from now on
  std::uint64_t hits_ = 0;
  std::uint64_t plays_ = 0;
  std::uint32_t next_palette_ = 0;
  std::mt19937 rng_{std::random_device{}()};

  gui::GuiContext gui_;
  gui::LayoutCursor layout_;
  gui::ChromeTheme chrome_ = gui::kDefaultChrome;
  r2d::Font font_;
  float ui_scale_ = 1.0f;
  std::optional<std::chrono::steady_clock::time_point> last_time_;
  bool dead_ = false;
};

#ifdef __ANDROID__
// Android entry: the glue calls android_main on the app's own thread and
// hands over the handle the shell needs (AppConfig::app_handle). No
// assets ship in the APK (the font is a system face); the CWD only
// matters if imgui wants to write its .ini on shutdown, so point it at
// the writable data dir - the same move chatter makes.
extern "C" void android_main(android_app* app) {
  chdir(app->activity->internalDataPath);
  ws::ui::AppConfig config;
  config.title = "ws bounce";
  config.app_handle = app;
  BounceApp bounce{config};
  (void)bounce.run();
}
#else
int main() {
  ws::ui::AppConfig config;
  config.title = "ws bounce";
  config.width = 1280;
  config.height = 720;
  BounceApp app{config};
  return app.run();
}
#endif
