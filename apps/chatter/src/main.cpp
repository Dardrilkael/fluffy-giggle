// apps/chatter - a native ws::ui chat client for desktop and Android.
// Transport/protocol code lives in chat_core.hpp; this file owns only
// the app shell and its presentation. The UI deliberately uses the
// monorepo's stock ThemeData, ChromeTheme, panels, scroll panel, text
// input and button widgets instead of maintaining a parallel skin.
//
// Desktop:
//   chatter [name] [--name n] [--serve [port]] [--relay host:port]...
// Android launch extras: name, serve and relay.
//
// The network loop remains separate from the frame loop. Incoming
// messages are drained on the UI thread; outgoing messages are posted
// to the core. The app does not modify ws::ui itself.
#include "chat_core.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include <ws/core/log.hpp>
#include <ws/ui/app.hpp>
#include <ws/ui/gui/widgets.hpp>
#include <ws/ui/gui/theme_data.hpp>
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
// Falls back to the deprecated getWindowInsets() if getRootWindowInsets()
// is missing (some API 30 devices lack it on DecorView).
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
  static jmethodID get_insets_fallback = nullptr; // View.getWindowInsets (deprecated)
  static jmethodID get_bottom = nullptr;   // WindowInsets.getSystemWindowInsetBottom
  static bool tried_insets = false;
  static bool tried_fallback = false;
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
  // Try getRootWindowInsets first (API 30+), fall back to getWindowInsets
  if (!tried_insets && get_insets == nullptr) {
    const jclass cls = e->GetObjectClass(decor);
    get_insets = e->GetMethodID(cls, "getRootWindowInsets", "()Landroid/view/WindowInsets;");
    e->DeleteLocalRef(cls);
    if (get_insets == nullptr) {
      e->ExceptionClear(); // NoSuchMethodError is a linkage error, clear it
    }
    tried_insets = true;
  }
  if (get_insets == nullptr && !tried_fallback && get_insets_fallback == nullptr) {
    const jclass cls = e->GetObjectClass(decor);
    get_insets_fallback = e->GetMethodID(cls, "getWindowInsets", "()Landroid/view/WindowInsets;");
    e->DeleteLocalRef(cls);
    if (get_insets_fallback == nullptr) {
      e->ExceptionClear();
    }
    tried_fallback = true;
  }
  jmethodID use_insets = get_insets ? get_insets : get_insets_fallback;
  if (use_insets == nullptr) {
    e->DeleteLocalRef(decor);
    if (jni_threw(e, "both inset getters missing")) {
      // already logged
    }
    return;
  }
  const jobject insets = e->CallObjectMethod(decor, use_insets);
  e->DeleteLocalRef(decor);
  if (insets == nullptr || jni_threw(e, "inset getter failed")) {
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
    const jclass cls = e->GetObjectClass(intent);
    if (cls == nullptr) {
      return nullptr;
    }
    get_extra = e->GetMethodID(cls, "getStringExtra",
                               "(Ljava/lang/String;)Ljava/lang/String;");
    e->DeleteLocalRef(cls);
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
  const jclass activity_class = e->GetObjectClass(activity->clazz);
  if (activity_class == nullptr) {
    return out;
  }
  const jmethodID get_intent = e->GetMethodID(
      activity_class, "getIntent", "()Landroid/content/Intent;");
  e->DeleteLocalRef(activity_class);
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
  int inset = g_nav_inset.load();
  // Fallback: if inset not yet polled (0), use a safe default (~48dp = 72px at mdpi)
  // This prevents the input bar from overlapping the navigation bar on first frames.
  if (inset <= 0) inset = 72;
  return inset;
}

#else  // !__ANDROID__

// Desktop: the OS chrome owns the bottom edge; nothing to lift over.
inline int bottom_inset() { return 0; }

#endif  // __ANDROID__

// The default room port: one past discovery's 27474, TCP. Overridable
// everywhere a port can be given (--serve [port], --relay host:port).
constexpr int kDefaultRelayPort = 27475;

// Chatter uses the same stock theme, typography, panels and widgets as
// the rest of ws::ui. The conversation is content inside a scroll panel;
// it is not a second, app-specific visual system.
namespace gui = ws::ui::gui;

using gui::Rect;
using gui::WidgetId;

[[nodiscard]] ws::ui::r2d::FontBakeOptions bake_options_for(bool dark) {
  return dark ? ws::ui::r2d::FontBakeOptions::dark_chrome()
              : ws::ui::r2d::FontBakeOptions::light_chrome();
}

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
  if (custom_c != nullptr) {
    ws::ui::r2d::Font loaded =
        ws::ui::r2d::load_font(custom_c, pixel_height, options);
    if (!loaded.empty()) {
#if defined(_WIN32)
      free(custom);
#endif
      return loaded;
    }
  }
#if defined(_WIN32)
  free(custom);
#endif
  return ws::ui::r2d::load_default_font(pixel_height, options);
}

std::string stamp_hhmm(int hh, int mm) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%02d:%02d",
                (hh < 0 || hh > 23) ? 0 : hh,
                (mm < 0 || mm > 59) ? 0 : mm);
  return buf;
}

class ChatterApp final : public ws::ui::App {
 public:
  ChatterApp(ws::ui::AppConfig config,
             chatter::ChatCore::Options core_options)
      : ws::ui::App(std::move(config)),
        options_(std::move(core_options)) {}

  void stop_net() { core_.stop(); }

 protected:
  void on_setup() override {
    scale_ = ui_scale();
    theme_dark_ = prefers_dark();
    theme_ = theme_dark_ ? gui::default_dark() : gui::default_light();

    const ws::ui::r2d::FontBakeOptions bake =
        bake_options_for(theme_dark_);
    font_ = load_ui_font(theme_.regular.size * scale_, bake);
    font_strong_ = ws::ui::r2d::load_default_font(
        ws::ui::r2d::DefaultFontWeight::semibold,
        theme_.bold.size * scale_, bake);

    chrome_ = theme_.chrome.scaled(scale_);
    chrome_.font = font_.empty() ? nullptr : &font_;
    chrome_.font_strong =
        font_strong_.empty() ? nullptr : &font_strong_;
    install_clipboard(gui_);
    install_ime_rect(gui_);

    core_.start(options_);
    if (std::getenv("CHATTER_SELFTEST") != nullptr) {
      core_.send("hello from alice!");
      core_.send("and here is a longer line that should wrap in the stock "
                 "conversation panel");
    }
  }

  void on_update() override {
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
    const float inset = static_cast<float>(bottom_inset());
    const float width = std::max(0.0f, frame.width - pad * 2.0f);
    const float header_h =
        chrome_.control_height + (font_.empty() ? chrome_.control_height
                                                : font_.line_height()) +
        pad * 2.0f + chrome_.spacing;
    const float composer_h = chrome_.control_height + pad * 2.0f;
    const float header_y = pad;
    const float composer_y =
        std::max(header_y + header_h + chrome_.spacing,
                 frame.height - inset - pad - composer_h);
    const Rect header{pad, header_y, width, header_h};
    const Rect feed{pad, header.y + header.height + chrome_.spacing, width,
                    std::max(0.0f, composer_y -
                                        (header.y + header.height +
                                         chrome_.spacing * 2.0f))};
    const Rect composer{pad, composer_y, width, composer_h};

    const ws::input::InputFrame& input = ws::input::frame_input();
    gui_.begin_frame(input);
    ws::ui::r2d::begin();
    ws::ui::r2d::draw_quad(0.0f, 0.0f, frame.width, frame.height,
                           chrome_.bar_bg);
    gui_.begin_layer(gui::Layer::base);

    // Standard application header: stock panel, heading and secondary
    // metadata. Theme tokens provide all colors and typography.
    (void)gui::begin_panel(gui_, layout_, chrome_, "header", header);
    gui::heading(layout_, chrome_, "Chatter");
    const int peers = core_.peers();
    const std::string status =
        core_.name() + "  ·  " +
        (core_.serving() ? "Room host" : "LAN participant") +
        "  ·  " + std::to_string(peers) +
        (peers == 1 ? " connection" : " connections") +
        "  ·  port " + std::to_string(core_.port());
    gui::text(layout_, chrome_, status.c_str());
    gui::end_panel(gui_, layout_);

    // Conversation content uses the library's scroll panel. The
    // panel owns wheel, touch-drag, clipping and scrollbar behavior.
    // Keep following new messages until the reader intentionally scrolls
    // away; the stock ScrollState remains the sole scroll implementation.
    (void)gui::begin_scroll_panel(gui_, layout_, chrome_, "conversation",
                                  feed);
    const WidgetId scroll_id = gui_.ids().internal_id("scroll");
    gui::ScrollState& conversation_scroll =
        gui_.persistent_state<gui::ScrollState>(scroll_id);
    bool reader_interacted = false;
    if (input.mouse_present && input.scroll_dy != 0.0f &&
        input.mouse_x >= feed.x && input.mouse_x < feed.x + feed.width &&
        input.mouse_y >= feed.y && input.mouse_y < feed.y + feed.height) {
      reader_interacted = true;
      if (input.scroll_dy > 0.0f) {
        follow_latest_ = false;
      }
    }
    for (const ws::input::TouchPoint& touch : input.touches) {
      if (touch.pressed && touch.x >= feed.x &&
          touch.x < feed.x + feed.width && touch.y >= feed.y &&
          touch.y < feed.y + feed.height) {
        reader_interacted = true;
        follow_latest_ = false;
      }
    }
    if (follow_latest_ && !reader_interacted) {
      conversation_scroll.offset = conversation_scroll.max_offset;
    }
    if (lines_.empty()) {
      gui::heading(layout_, chrome_, "No messages yet");
      gui::text_wrapped(layout_, chrome_,
                        "Messages from LAN peers and relay participants "
                        "will appear here.");
    } else {
      gui::ChromeTheme secondary = chrome_;
      secondary.text = chrome_.text_secondary;
      gui::ChromeTheme sender_theme = chrome_;
      if (!font_strong_.empty()) {
        sender_theme.font = &font_strong_;
      }
      for (const ChatLine& line : lines_) {
        if (line.system) {
          gui::text_wrapped(layout_, secondary, line.text.c_str());
        } else {
          const std::string sender =
              line.name == "you" ? "You" : line.name;
          gui::text(layout_, sender_theme, sender.c_str());
          layout_.same_line();
          const std::string stamp =
              stamp_hhmm(line.sent_hh, line.sent_mm);
          gui::text(layout_, secondary, stamp.c_str());
          gui::text_wrapped(layout_, chrome_, line.text.c_str());
        }
        gui::separator(layout_, chrome_);
      }
    }
    gui::end_scroll_panel(gui_, layout_, chrome_);
    if (follow_latest_) {
      conversation_scroll.offset = conversation_scroll.max_offset;
    } else if (!reader_interacted &&
               conversation_scroll.offset >=
                   conversation_scroll.max_offset - 1.0f) {
      follow_latest_ = true;
    }

    // Stock text field + primary button. The field keeps the library's
    // selection, caret, keyboard and clipboard behavior on both targets.
    (void)gui::begin_panel(gui_, layout_, chrome_, "composer", composer);
    constexpr float kSendWidth = 84.0f;
    const float input_width =
        std::max(48.0f, layout_.remaining_width() -
                            kSendWidth * scale_ - chrome_.spacing);
    gui::text_input(gui_, layout_, chrome_, "message", input_,
                    kInputCapacity, input_width);
    layout_.same_line();
    const bool send_clicked =
        gui::button(gui_, layout_, chrome_, "Send", kSendWidth * scale_, {},
                    false, gui::ButtonStyle::primary);

    const WidgetId field_id = gui_.ids().id("message");
    field_focus_ = gui_.is_focused(field_id);
    const bool enter =
        gui_.input().key_pressed[ws::input::keys::kEnter] && field_focus_;
    if ((send_clicked || enter) && input_[0] != '\0') {
      core_.send(input_);
      input_[0] = '\0';
    }
    gui::end_panel(gui_, layout_);

    gui_.end_layer();
    ws::ui::r2d::end();
    ws::ui::r2d::set_layer(0);
    gui_.end_frame();
  }

 private:
  static constexpr std::size_t kShownLines = 200;
  static constexpr int kInputCapacity = 256;

  chatter::ChatCore core_;
  chatter::ChatCore::Options options_;
  gui::GuiContext gui_;
  gui::LayoutCursor layout_;
  gui::ThemeData theme_;
  gui::ChromeTheme chrome_;
  ws::ui::r2d::Font font_;
  ws::ui::r2d::Font font_strong_;
  float scale_ = 1.0f;
  bool theme_dark_ = false;
  bool follow_latest_ = true;
  std::vector<ChatLine> lines_;
  char input_[kInputCapacity] = {};
  bool field_focus_ = false;
};

}  // namespace

#ifndef __ANDROID__

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
      if (i + 1 < argc && argv[i + 1][0] >= '0' &&
          argv[i + 1][0] <= '9') {
        char* end = nullptr;
        const long parsed = std::strtol(argv[i + 1], &end, 10);
        if (end == argv[i + 1] || end == nullptr || *end != '\0' ||
            parsed < 1 || parsed > 65535) {
          ws::core::error("chatter: --serve port must be 1..65535");
          out.valid = false;
        } else {
          port = static_cast<int>(parsed);
          ++i;
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
    } else if (!arg.empty() && arg.front() == '-') {
      ws::core::error("chatter: unknown option '{}'", arg);
      out.valid = false;
    } else if (!arg.empty()) {
      if (!out.core.name.empty()) {
        ws::core::error("chatter: only one positional name is allowed; use --name");
        out.valid = false;
      } else {
        out.core.name = std::string{arg};  // v1's positional form
      }
    }
  }
  if (out.core.serve_port > 0 && !out.core.relays.empty()) {
    ws::core::error("chatter: --serve ignores --relay (a host does not guest elsewhere); relays dropped");
    out.core.relays.clear();
  }
  return out;
}

#endif

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
        "usage: chatter [name] [--name n] [--serve [1..65535]] [--relay host:port]...");
    return 2;
  }
  ChatterApp chatter{config, launch.core};
  const int rc = chatter.run();
  chatter.stop_net();
  return rc;
}
#endif
