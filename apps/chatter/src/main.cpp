// apps/chatter - the gui sibling of netchat, built for the phone (and the
// desktop), v2. The wire side lives in chat_core.hpp (see its header for
// the whole protocol story): Lan peers keep the v1 discovery beats and
// the HELLO/SAY framed TCP, and now a relay carries the chat across
// networks that will never see a beat -
//
//   chatter --serve [port]        host a room (the "server")
//   chatter --relay host:port     guest in a room, from a normal client
//
// so a phone on Wi-Fi and a PC on wired LAN still share one chat through
// a host sitting anywhere on the internet, while everyone on the same
// network keeps talking direct P2P with no server in the path.
//
// v2 display: normal text, not prefixed. A message is its own plain text;
// the sender's name is a small colored label above it, only when the
// sender changes from the line below - no brackets anywhere, the status
// line included. System lines (joins, links, room events) render dim.
//
// v2 Android layout: the phrase palette sits above the navigation bar.
// The window is the whole surface (the fullscreen theme eliminates the
// status bar but not the 3-button bar - it overlays the bottom edge), so
// on_update polls the bottom system-bar inset through the activity (a
// 4-hop reflection over getWindow/getDecorView/getRootWindowInsets, the
// same per-call JNI shape the ui lib's clipboard seam uses - no Java
// shim needed, the app stays app-pure) and lifts the panel by it. The
// relay/name/serve launch extras are read the same way, once, from the
// intent: `am start ... --es relay 203.0.113.7:27475 --es name alice`.
//
// v1's boundaries all hold: input stays a tap-phrase palette (IME commits
// still cannot reach a hasCode NativeActivity without a window-layer
// change in the ui lib, and this app still makes zero lib edits); the
// net loop runs on its own thread; lines arrive to the gui through a
// mutex-guarded queue drained once per frame; palette taps cross back
// with post(); teardown posts the close-everything shutdown and joins
// the thread before the core can destruct. The protocol core itself is
// now chat_core.hpp, header-only and gui-free, so the same code runs
// under a headless harness.
#include "chat_core.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include <ws/core/log.hpp>
#include <ws/ui/app.hpp>
#include <ws/ui/gui/widgets.hpp>
#include <ws/ui/renderer2d.hpp>

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

namespace {

using chatter::ChatLine;

// The Android seam this app owns: the nav-bar inset (polled per frame)
// and the launch extras (read once). Everything else below is the gui,
// identical on both platforms.
#ifdef __ANDROID__

std::atomic<int> g_nav_inset{0};     // px, the bottom system-bar inset
ANativeActivity* g_activity = nullptr;  // set in android_main, alive for it

// The ui lib's clipboard pattern, copied small: attach for the call's
// span (the shell thread is not the Java thread), detach after - no
// thread-local state that could race the loop's exit.
struct ScopedJavaThread {
  JavaVM* vm = nullptr;
  JNIEnv* env = nullptr;
  bool attached = false;

  explicit ScopedJavaThread(JavaVM* v) : vm(v) {
    if (vm == nullptr) {
      return;
    }
    const jint state = vm->GetEnv(reinterpret_cast<void**>(&env),
                                  JNI_VERSION_1_6);
    if (state == JNI_OK) {
      return;  // the caller already rides the JVM
    }
    if (state == JNI_EDETACHED &&
        vm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
      attached = true;
    }
  }
  ~ScopedJavaThread() {
    if (attached) {
      (void)vm->DetachCurrentThread();
    }
  }
  [[nodiscard]] bool ok() const { return env != nullptr; }
};

bool jni_threw(JNIEnv* env, const char* what) {
  if (!env->ExceptionCheck()) {
    return false;
  }
  env->ExceptionDescribe();
  env->ExceptionClear();
  ws::core::error("chatter: jni: {}", what);
  return true;
}

// One poll: window -> decor view -> root insets -> the bottom inset, in
// px. The method IDs resolve once (they are JVM-stable); every jobject
// is a local reference and gets deleted before returning - a per-frame
// poll that leaked them would exhaust the local-ref table in seconds.
void poll_nav_inset() {
  if (g_activity == nullptr || g_activity->clazz == nullptr) {
    return;
  }
  ScopedJavaThread jni{g_activity->vm};
  if (!jni.ok()) {
    return;
  }
  JNIEnv* e = jni.env;
  static jmethodID get_window = nullptr;   // Activity.getWindow
  static jmethodID get_decor = nullptr;    // Window.getDecorView
  static jmethodID get_insets = nullptr;   // View.getRootWindowInsets
  static jmethodID get_bottom = nullptr;   // WindowInsets.getSystemWindowInsetBottom
  if (get_window == nullptr) {
    const jclass cls = e->GetObjectClass(g_activity->clazz);
    get_window = e->GetMethodID(cls, "getWindow", "()Landroid/view/Window;");
    e->DeleteLocalRef(cls);
    if (get_window == nullptr || jni_threw(e, "getWindow missing")) {
      return;
    }
  }
  const jobject window = e->CallObjectMethod(g_activity->clazz, get_window);
  if (window == nullptr || jni_threw(e, "getWindow failed")) {
    return;
  }
  if (get_decor == nullptr) {
    const jclass cls = e->GetObjectClass(window);
    get_decor = e->GetMethodID(cls, "getDecorView", "()Landroid/view/View;");
    e->DeleteLocalRef(cls);
    if (get_decor == nullptr || jni_threw(e, "getDecorView missing")) {
      e->DeleteLocalRef(window);
      return;
    }
  }
  const jobject decor = e->CallObjectMethod(window, get_decor);
  e->DeleteLocalRef(window);
  if (decor == nullptr || jni_threw(e, "getDecorView failed")) {
    return;
  }
  if (get_insets == nullptr) {
    const jclass cls = e->GetObjectClass(decor);
    get_insets =
        e->GetMethodID(cls, "getRootWindowInsets", "()Landroid/WindowInsets;");
    e->DeleteLocalRef(cls);
    if (get_insets == nullptr || jni_threw(e, "getRootWindowInsets missing")) {
      e->DeleteLocalRef(decor);
      return;
    }
  }
  const jobject insets = e->CallObjectMethod(decor, get_insets);
  e->DeleteLocalRef(decor);
  if (insets == nullptr || jni_threw(e, "getRootWindowInsets failed")) {
    return;
  }
  if (get_bottom == nullptr) {
    const jclass cls = e->GetObjectClass(insets);
    get_bottom = e->GetMethodID(cls, "getSystemWindowInsetBottom", "()I");
    e->DeleteLocalRef(cls);
    if (get_bottom == nullptr || jni_threw(e, "inset getter missing")) {
      e->DeleteLocalRef(insets);
      return;
    }
  }
  const jint bottom = e->CallIntMethod(insets, get_bottom);
  e->DeleteLocalRef(insets);
  if (!jni_threw(e, "inset read failed")) {
    g_nav_inset.store(static_cast<int>(bottom));
  }
}

// The launch extras, read once from the intent in android_main: relay
// (comma-separated host:port list), name, serve ("1" or a port number).
struct AndroidLaunch {
  std::string name;
  std::vector<std::string> relays;
  int serve = 0;
};

jstring intent_extra(JNIEnv* e, jobject intent, const char* key) {
  static jmethodID get_extra = nullptr;
  if (get_extra == nullptr) {
    get_extra = e->GetMethodID(e->GetObjectClass(intent), "getStringExtra",
                               "(Ljava/lang/String;)Ljava/lang/String;");
    if (get_extra == nullptr || jni_threw(e, "getStringExtra missing")) {
      return nullptr;
    }
  }
  const jstring key_s = e->NewStringUTF(key);
  const jstring value =
      static_cast<jstring>(e->CallObjectMethod(intent, get_extra, key_s));
  e->DeleteLocalRef(key_s);
  if (jni_threw(e, "getStringExtra failed")) {
    return nullptr;
  }
  return value;
}

std::string extra_string(JNIEnv* e, jobject intent, const char* key) {
  const jstring value = intent_extra(e, intent, key);
  if (value == nullptr) {
    return {};
  }
  const char* bytes = e->GetStringUTFChars(value, nullptr);
  std::string out = bytes != nullptr ? bytes : "";
  if (bytes != nullptr) {
    e->ReleaseStringUTFChars(value, bytes);
  }
  e->DeleteLocalRef(value);
  return out;
}

AndroidLaunch read_launch(ANativeActivity* activity) {
  AndroidLaunch out;
  if (activity == nullptr || activity->clazz == nullptr) {
    return out;
  }
  ScopedJavaThread jni{activity->vm};
  if (!jni.ok()) {
    return out;
  }
  JNIEnv* e = jni.env;
  const jmethodID get_intent = e->GetMethodID(
      e->GetObjectClass(activity->clazz), "getIntent", "()Landroid/content/Intent;");
  if (get_intent == nullptr || jni_threw(e, "getIntent missing")) {
    return out;
  }
  const jobject intent = e->CallObjectMethod(activity->clazz, get_intent);
  if (intent == nullptr || jni_threw(e, "getIntent failed")) {
    return out;
  }
  out.name = extra_string(e, intent, "name");
  const std::string relay = extra_string(e, intent, "relay");
  if (!relay.empty()) {
    // "host:port,host:port" - one list extra instead of N adb round-trips.
    std::string_view rest = relay;
    while (!rest.empty()) {
      std::size_t comma = rest.find(',');
      const std::string_view one =
          comma == std::string_view::npos ? rest : rest.substr(0, comma);
      if (!one.empty()) {
        out.relays.emplace_back(one);
      }
      if (comma == std::string_view::npos) {
        break;
      }
      rest.remove_prefix(comma + 1);
    }
  }
  const std::string serve = extra_string(e, intent, "serve");
  if (!serve.empty()) {
    // A real port (1024..65535) is that port; anything else truthy
    // ("1", "true", junk) means "serve, on the default port".
    const int parsed = atoi(serve.c_str());
    out.serve = (parsed >= 1024 && parsed < 65536) ? parsed : -1;
  }
  e->DeleteLocalRef(intent);
  return out;
}

inline int bottom_inset() {
  poll_nav_inset();
  return g_nav_inset.load();
}

#else  // !__ANDROID__

// Desktop: the OS chrome owns the bottom edge; nothing to lift over.
inline int bottom_inset() { return 0; }

#endif  // __ANDROID__

constexpr std::uint32_t kBackdrop = ws::ui::r2d::rgba(22, 24, 30);
constexpr std::uint32_t kText = ws::ui::r2d::rgba(235, 238, 242);
constexpr std::uint32_t kDim = ws::ui::r2d::rgba(140, 145, 152);
constexpr std::uint32_t kYou = ws::ui::r2d::rgba(120, 220, 140);
constexpr std::uint32_t kPanel = ws::ui::r2d::rgba(30, 33, 40);
constexpr std::uint32_t kEdge = ws::ui::r2d::rgba(48, 52, 60);

// The tap vocabulary - v1's whole input surface (see the header comment).
constexpr const char* kPhrases[] = {"hello!", "yes", "no", "brb",
                                    "ping", "thanks", "lol", "bye"};
constexpr std::size_t kPhraseCount = sizeof(kPhrases) / sizeof(kPhrases[0]);

// The default room port: one past discovery's 27474, TCP. Overridable
// everywhere a port can be given (--serve [port], --relay host:port).
constexpr int kDefaultRelayPort = 27475;

// A stable tint per sender: FNV-1a over the name, six gentle faces. "you"
// is fixed so own lines are instantly recognizable.
std::uint32_t name_color(std::string_view name) {
  if (name == "you") {
    return kYou;
  }
  constexpr std::uint32_t kTints[] = {
      ws::ui::r2d::rgba(126, 200, 226), ws::ui::r2d::rgba(200, 160, 235),
      ws::ui::r2d::rgba(140, 220, 150), ws::ui::r2d::rgba(235, 175, 130),
      ws::ui::r2d::rgba(235, 140, 150), ws::ui::r2d::rgba(180, 210, 130)};
  std::uint32_t hash = 2166136261u;
  for (const char c : name) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 16777619u;
  }
  return kTints[hash % (sizeof(kTints) / sizeof(kTints[0]))];
}

// The UI face: the BUNDLED Inter by default, with a path override for
// custom fonts - WS_FONT_PATH wins when it is set and loadable, the
// bundled face answers otherwise (playground's helper, this app's copy:
// one constant per TU, the platform boundary lives in the app).
[[nodiscard]] ws::ui::r2d::Font load_ui_font(
    float pixel_height, const ws::ui::r2d::FontBakeOptions& options) {
#if defined(_WIN32)
  char* custom = nullptr;
  size_t len = 0;
  _dupenv_s(&custom, &len, "WS_FONT_PATH");
  const char* custom_c = custom;
#else
  const char* custom_c = std::getenv("WS_FONT_PATH");
#endif
  if (custom_c) {
    ws::ui::r2d::Font loaded =
        ws::ui::r2d::load_font(custom_c, pixel_height, options);
    if (!loaded.empty()) {
#if defined(_WIN32)
      free(custom);
#endif
      return loaded;
    }
    // A dead override falls through to the bundled face.
  }
#if defined(_WIN32)
  free(custom);
#endif
  return ws::ui::r2d::load_default_font(pixel_height, options);
}

// One chat's startup ask, desktop edition: what the flag parser hands
// the app. Kept beside the app (not in chat_core.hpp) because it is an
// argv concern; ChatCore::Options is the transport concern.
struct LaunchRequest {
  chatter::ChatCore::Options core;
  bool valid = true;  // false: usage error already logged; run anyway
                      // as a plain offline LAN chatter, never crash on bad flags
};

LaunchRequest parse_args(int argc, char* argv[]) {
  LaunchRequest out;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--serve") {
      int port = kDefaultRelayPort;
      if (i + 1 < argc) {
        char* end = nullptr;
        const long parsed = std::strtol(argv[i + 1], &end, 10);
        if (end != nullptr && *end == '\0' && parsed > 0 &&
            parsed < 65536) {
          port = static_cast<int>(parsed);
          ++i;  // consumed the port token
        }
      }
      if (out.core.serve_port == 0) {
        out.core.serve_port = port;
      }
    } else if (arg == "--relay") {
      if (i + 1 >= argc) {
        ws::core::error("chatter: --relay needs host:port");
        out.valid = false;
        continue;
      }
      const auto addr =
          ws::net::Address::parse(argv[++i]);
      if (addr) {
        out.core.relays.push_back(*addr);
      } else {
        ws::core::log_error(addr.error());
        out.valid = false;
      }
    } else if (arg == "--name") {
      if (i + 1 >= argc) {
        ws::core::error("chatter: --name needs a value");
        out.valid = false;
        continue;
      }
      out.core.name = argv[++i];
    } else if (!arg.empty()) {
      out.core.name = std::string{arg};  // v1's positional form
    }
  }
  if (out.core.serve_port > 0 && !out.core.relays.empty()) {
    ws::core::error("chatter: --serve ignores --relay (a host does not guest elsewhere); relays dropped");
    out.core.relays.clear();
  }
  return out;
}

// The gui: a status line, the feed (newest at the bottom, auto-trimmed to
// what fits - no scroll), and the palette. All input flows through the
// gui widgets, all drawing through r2d; chatter draws no ImGui windows.
class ChatterApp final : public ws::ui::App {
 public:
  ChatterApp(ws::ui::AppConfig config,
             chatter::ChatCore::Options core_options)
      : ws::ui::App(std::move(config)),
        options_(std::move(core_options)) {}

  // Joins the net thread. Call after run() returns (both entries do) so
  // the core stops before the app object destructs.
  void stop_net() { core_.stop(); }

 protected:
  void on_setup() override {
    scale_ = ui_scale();
    // The UI face: the BUNDLED Inter by default (the stock look, the
    // same on every platform - no system font walk), with a path
    // override for custom fonts. The bake pair follows the active
    // theme's brightness (chatter draws the stock dark chrome only).
    const ws::ui::r2d::FontBakeOptions bake =
        ws::ui::r2d::FontBakeOptions::dark_chrome();
    font_ = load_ui_font(16.0f * scale_, bake);
    font_strong_ = ws::ui::r2d::load_default_font(
        ws::ui::r2d::DefaultFontWeight::semibold, 16.0f * scale_, bake);
    chrome_ = ws::ui::gui::kDefaultChrome.scaled(scale_);
    chrome_.font = font_.empty() ? nullptr : &font_;
    chrome_.font_strong =
        font_strong_.empty() ? nullptr : &font_strong_;
    core_.start(options_);
  }

  void on_update() override {
    // Adopt everything the net thread queued since the last frame.
    std::vector<ChatLine> arrived = core_.pull();
    for (ChatLine& line : arrived) {
      lines_.push_back(std::move(line));
    }
    if (lines_.size() > kShownLines) {
      lines_.erase(lines_.begin(),
                   lines_.end() - static_cast<std::ptrdiff_t>(kShownLines));
    }

    const ws::ui::r2d::FrameSize frame = ws::ui::r2d::frame_size();
    const float pad = chrome_.padding;
    const float line_h =
        font_.empty() ? chrome_.control_height : font_.line_height();
    const float line_gap = 4.0f * scale_;

    // The bottom lift: on Android the navigation bar (3-button or
    // gesture) overlays the surface's bottom edge; poll it and park the
    // palette above it. Desktop answers 0 forever.
    const float inset = static_cast<float>(bottom_inset());

    // The palette panel: two rows of four phrase buttons, full width,
    // above the nav bar (padding + row + spacing + row + padding).
    const float panel_h = chrome_.padding * 2.0f +
                          chrome_.control_height * 2.0f + chrome_.spacing;
    const ws::ui::gui::Rect palette{0.0f, frame.height - panel_h - inset,
                                    frame.width, panel_h};
    const float status_h = chrome_.padding * 2.0f + line_h;

    ws::ui::r2d::begin();
    ws::ui::r2d::draw_quad(0.0f, 0.0f, frame.width, frame.height, kBackdrop);
    // A panel face + a separating edge, so the lifted palette reads as a
    // sheet over the feed, not a hole in it.
    ws::ui::r2d::draw_quad(palette.x, palette.y, palette.width, palette.height,
                           kPanel);
    ws::ui::r2d::draw_quad(palette.x, palette.y - 1.0f * scale_,
                           palette.width, 1.0f * scale_, kEdge);

    // Status line: identity, port, live connections - plain text, the
    // same no-brackets rule the feed follows.
    if (!font_.empty()) {
      const int n = core_.peers();
      const char* who =
          core_.serving() ? (n == 1 ? " member" : " members")
                           : (n == 1 ? " peer" : " peers");
      const std::string status = core_.name() + " \u00B7 port " +
                                 std::to_string(core_.port()) + " \u00B7 " +
                                 std::to_string(n) + who;
      ws::ui::r2d::draw_text(pad, pad, status.c_str(), font_, kDim);
    }

    // The feed, newest at the bottom: a message is its own plain text;
    // the sender's name is a small strong-tinted label line above it,
    // shown only when the sender changes from the line below (the line
    // below is newer - it was drawn in an earlier pass of this walk).
    // System lines render dim with no label and break the sender run.
    // Clipped so a long line cuts at the screen edge (v1 had no
    // truncation; that has not changed).
    ws::ui::r2d::push_clip(0.0f, status_h, frame.width, palette.y - status_h);
    float y = palette.y - chrome_.spacing - line_h;
    std::string_view prev;  // the line below's sender (navigating up = older)
    bool have_prev = false;
    for (std::size_t i = lines_.size(); i-- > 0;) {
      if (y < status_h) {
        break;
      }
      const ChatLine& line = lines_[i];
      if (font_.empty()) {
        // Degraded feed: one bar per line, tinted by sender, so a failed
        // font load still shows a visibly live chat.
        ws::ui::r2d::draw_quad(pad, y + line_h * 0.5f, 64.0f * scale_,
                               3.0f * scale_,
                               line.system ? kDim : name_color(line.name));
        y -= line_h + line_gap;
        have_prev = false;
        continue;
      }
      if (line.system) {
        ws::ui::r2d::draw_text(pad, y, line.text.c_str(), font_, kDim);
        y -= line_h + line_gap;
        have_prev = false;  // a system line breaks the sender run
        continue;
      }
      const bool label = !have_prev || prev != line.name;
      if (label) {
        const ws::ui::r2d::Font* face =
            font_strong_.empty() ? &font_ : &font_strong_;
        ws::ui::r2d::draw_text(pad, y - line_h, line.name.c_str(), *face,
                               name_color(line.name));
      }
      ws::ui::r2d::draw_text(pad, y, line.text.c_str(), font_, kText);
      y -= (label ? 2.0f : 1.0f) * line_h + line_gap;
      prev = line.name;
      have_prev = true;
    }
    ws::ui::r2d::pop_clip();
    ws::ui::r2d::end();

    // The palette: gui widgets above the world. A tap posts one SAY
    // through the core.
    ws::ui::r2d::set_layer(10);
    ws::ui::r2d::begin();
    gui_.begin_frame(ws::input::frame_input());
    const ws::ui::gui::Rect content =
        ws::ui::gui::begin_panel(gui_, layout_, chrome_, "palette", palette);
    const float btn_w = (content.width - chrome_.spacing * 3.0f) / 4.0f;
    for (std::size_t i = 0; i < kPhraseCount; ++i) {
      gui_.ids().push(static_cast<int>(i));
      if (ws::ui::gui::button(gui_, layout_, chrome_, kPhrases[i], btn_w)) {
        core_.send(kPhrases[i]);
      }
      gui_.ids().pop();
      if (i % 4 != 3) {
        layout_.same_line();  // four per row, two rows
      }
    }
    ws::ui::gui::end_panel(gui_, layout_);
    ws::ui::r2d::end();
    ws::ui::r2d::set_layer(0);
    gui_.end_frame();
  }

 private:
  static constexpr std::size_t kShownLines = 200;

  chatter::ChatCore core_;
  chatter::ChatCore::Options options_;  // handed to start() in on_setup
  ws::ui::gui::GuiContext gui_;
  ws::ui::gui::LayoutCursor layout_;
  ws::ui::gui::ChromeTheme chrome_;
  ws::ui::r2d::Font font_;
  ws::ui::r2d::Font font_strong_;  // the semibold sibling (sender labels)
  float scale_ = 1.0f;
  std::vector<ChatLine> lines_;
};

}  // namespace

#ifdef __ANDROID__
// Android entry: the glue calls android_main on the app's own thread and
// hands over the handle the shell needs (AppConfig::app_handle). No
// assets ship in the APK (the font is a bundled face), so there is
// nothing to unpack - just point the CWD at the writable data dir and
// run the same code the desktop runs. Launch extras (relay/name/serve)
// ride the intent, read once through the seam above.
extern "C" void android_main(android_app* app) {
  chdir(app->activity->internalDataPath);
  g_activity = app->activity;
  const AndroidLaunch launch = read_launch(app->activity);
  chatter::ChatCore::Options options;
  options.name = launch.name;
  for (const std::string& relay : launch.relays) {
    const auto addr = ws::net::Address::parse(relay);
    if (addr) {
      options.relays.push_back(*addr);
    } else {
      ws::core::log_error(addr.error());
    }
  }
  if (launch.serve != 0) {
    // -1 came through as "serve, default port".
    options.serve_port = launch.serve > 0 ? launch.serve : kDefaultRelayPort;
    options.relays.clear();
  }
  ws::ui::AppConfig config;
  config.title = "ws chatter";
  config.app_handle = app;
  ChatterApp chatter{config, std::move(options)};
  (void)chatter.run();
  chatter.stop_net();
}
#else
int main(int argc, char* argv[]) {
  ws::ui::AppConfig config;
  config.title = "ws chatter";
  config.width = 420;
  config.height = 760;
  const LaunchRequest launch = parse_args(argc, argv);
  if (!launch.valid) {
    ws::core::error(
        "usage: chatter [name] [--name n] [--serve [port]] [--relay host:port]...");
  }
  ChatterApp chatter{config, launch.core};
  const int rc = chatter.run();
  chatter.stop_net();
  return rc;
}
#endif
