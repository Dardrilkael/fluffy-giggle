// apps/chatter - the gui sibling of netchat, built for the phone (and the
// desktop), v3. The wire side lives in chat_core.hpp (see its header for
// the whole protocol story): Lan peers keep the v1 discovery beats and
// the HELLO/SAY framed TCP, and a relay carries the chat across
// networks that will never see a beat -
//
//   chatter --serve [port]        host a room (the "server")
//   chatter --relay host:port     guest in a room, from a normal client
//
// so a phone on Wi-Fi and a PC on wired LAN still share one chat through
// a host sitting anywhere on the internet, while everyone on the same
// network keeps talking direct P2P with no server in the path.
//
// v3 display: the WhatsApp look. A teal header band carries the room
// identity and a live member line; the feed rides a warm paper field and
// draws every message as a rounded bubble - own sends right on green
// with a tail, a timestamp and a delivery check, everyone else left on
// white with a colored sender name (hidden mid-run, like the app it
// echoes), system lines as small centered pills. Text wraps at the
// bubble's max width. The feed scrolls - wheel and touch drag - and
// sits pinned to the newest line until the reader lifts it, then a
// tap on the floating "new messages" chip (or dragging to the floor)
// returns it.
//
// v3 input: real text, the way the ui lib grew it. The ui lib's Android
// window now owns an ime-rect seam (a focused field opens the soft
// keyboard, and the visible keyboard's keycodes type straight into the
// gui) - v1's tap-phrase palette existed because that seam did not; it
// is gone. The bar is the WhatsApp row: a white rounded field holding
// most of the width, a round green send button beside it. Enter sends
// from the field, the button sends by tap, an empty buffer sends
// nothing. The Android nav-bar inset poll survives (the bar lifts over
// the 3-button strip the fullscreen theme keeps), and the relay/name/
// serve launch extras are read the same way, once, from the intent:
// `am start ... --es relay 203.0.113.7:27475 --es name alice`.
//
// v1's boundaries all hold: the net loop runs on its own thread; lines
// arrive to the gui through a mutex-guarded queue drained once per
// frame; sends cross back with post(); teardown posts the close-
// everything shutdown and joins the thread before the core can
// destruct. The protocol core itself is chat_core.hpp, header-only and
// gui-free, so the same code runs under a headless harness. The app
// still makes zero lib edits - every pixel here rides r2d's quads and
// the stock widget seams.
#include "chat_core.hpp"

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

// The v3 face, the WhatsApp palette: a teal header, a warm paper feed,
// green own bubbles, white foreign ones. The light chrome means the
// font bakes with light_chrome() - dark text reads on light surfaces.
constexpr std::uint32_t kHeader = ws::ui::r2d::rgba(7, 94, 84);       // #075E54 teal
constexpr std::uint32_t kHeaderTitle = ws::ui::r2d::rgba(255, 255, 255);
constexpr std::uint32_t kHeaderSub = ws::ui::r2d::rgba(190, 214, 210);
constexpr std::uint32_t kPaper = ws::ui::r2d::rgba(230, 224, 212);   // #E0E0D4-ish warm field
constexpr std::uint32_t kBubbleMine = ws::ui::r2d::rgba(220, 248, 198);  // #DCF8C6 pale green
constexpr std::uint32_t kBubbleTheirs = ws::ui::r2d::rgba(255, 255, 255);
constexpr std::uint32_t kText = ws::ui::r2d::rgba(33, 33, 33);        // near-black on light
constexpr std::uint32_t kTimeDim = ws::ui::r2d::rgba(128, 138, 128);  // the bubble corner's HH:MM
[[maybe_unused]] constexpr std::uint32_t kPill = ws::ui::r2d::rgba(255, 255, 255);    // system pill face
constexpr std::uint32_t kPillText = ws::ui::r2d::rgba(110, 116, 110);
constexpr std::uint32_t kBar = ws::ui::r2d::rgba(245, 245, 245);     // the input strip
constexpr std::uint32_t kField = ws::ui::r2d::rgba(255, 255, 255);    // the input pill
constexpr std::uint32_t kSend = ws::ui::r2d::rgba(7, 150, 120);      // the round send button
constexpr std::uint32_t kSendHover = ws::ui::r2d::rgba(16, 172, 140);
constexpr std::uint32_t kChip = ws::ui::r2d::rgba(7, 94, 84);        // the "new messages" chip
constexpr std::uint32_t kYou = ws::ui::r2d::rgba(7, 94, 84);         // "you" in the rare label contexts

// A stable tint per sender for the foreign bubbles' name line: FNV-1a
// over the name, WhatsApp's warm set. "you" is fixed (the own side
// never shows a name anyway - the tail says whose it is).
std::uint32_t name_color(std::string_view name) {
  if (name == "you") {
    return kYou;
  }
  constexpr std::uint32_t kTints[] = {
      ws::ui::r2d::rgba(230, 100, 90),   // coral
      ws::ui::r2d::rgba(150, 105, 200),  // violet
      ws::ui::r2d::rgba(60, 150, 210),   // blue
      ws::ui::r2d::rgba(210, 130, 45),   // amber
      ws::ui::r2d::rgba(90, 160, 90),   // leaf
      ws::ui::r2d::rgba(190, 80, 130)}; // rose
  std::uint32_t hash = 2166136261u;
  for (const char c : name) {
    hash ^= static_cast<unsigned char>(c);
    hash *= 16777619u;
  }
  return kTints[hash % (sizeof(kTints) / sizeof(kTints[0]))];
}

// One text run split into the lines a bubble's max width holds - the
// measure-based greedy walk (every break at a space, a run wider than
// the room stays whole and clips, WhatsApp's own behavior on an
// unbreakable word). The field wraps identically on desktop and phone
// because it is the font's own measure.
std::vector<std::string> wrap_text(std::string_view text,
                                    const ws::ui::r2d::Font& font,
                                    float max_w) {
  std::vector<std::string> out;
  std::string line;
  std::string word;
  const auto flush_word = [&]() {
    if (word.empty()) {
      return;
    }
    const std::string candidate = line.empty() ? word : line + " " + word;
    if (!line.empty() && font.measure_text(candidate.c_str()) > max_w) {
      out.push_back(std::move(line));
      line.clear();
    }
    // Split overlong words only at UTF-8 code-point boundaries. This
    // prevents invalid byte fragments from reaching the font renderer.
    if (font.measure_text(word.c_str()) > max_w) {
      std::string piece;
      for (std::size_t i = 0; i < word.size();) {
        const unsigned char lead = static_cast<unsigned char>(word[i]);
        std::size_t count = lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2
                                     : (lead & 0xF0) == 0xE0 ? 3
                                     : (lead & 0xF8) == 0xF0 ? 4 : 1;
        count = std::min(count, word.size() - i);
        const std::string_view cp{word.data() + i, count};
        const std::string next = piece + std::string{cp};
        if (!piece.empty() && font.measure_text(next.c_str()) > max_w) {
          if (!line.empty()) {
            out.push_back(std::move(line));
            line.clear();
          }
          out.push_back(std::move(piece));
          piece.clear();
        }
        piece.append(cp);
        i += count;
      }
      if (!piece.empty()) {
        if (!line.empty()) {
          out.push_back(std::move(line));
          line.clear();
        }
        line = std::move(piece);
      }
    } else {
      line = line.empty() ? word : line + " " + word;
    }
    word.clear();
  };
  for (const char c : text) {
    if (c == '\n' || c == '\r') {
      flush_word();
      out.push_back(std::move(line));
      line.clear();
    } else if (c == ' ' || c == '\t') {
      flush_word();
    } else {
      word.push_back(c);
    }
  }
  flush_word();
  if (!line.empty() || out.empty()) {
    out.push_back(std::move(line));
  }
  return out;
}

// HH:MM as the bubble corner spells it.
std::string stamp_hhmm(int hh, int mm) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%02d:%02d", ((hh < 0 || hh > 23) ? 0 : hh),
                ((mm < 0 || mm > 59) ? 0 : mm));
  return buf;
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

// The gui's own spellings this TU writes often enough to want short.
using ws::ui::gui::Rect;
using ws::ui::gui::WidgetId;
using ws::ui::gui::TextInputResult;
using ws::ui::gui::TextInputState;

// The gui: the v3 WhatsApp face - a teal header over a scrolling bubble
// feed over the input bar. All input flows through the gui widgets, all
// drawing through r2d; chatter draws no ImGui windows.
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
    // The light-chrome bake: dark-on-light text needs the ~1.05 gamma the
    // light stock uses, not the 1.6 the dark chrome lifts with - a 1.6
    // bake on white reads as a bold weight at best, a smear at worst.
    const ws::ui::r2d::FontBakeOptions bake =
        ws::ui::r2d::FontBakeOptions::light_chrome();
    font_ = load_ui_font(15.0f * scale_, bake);
    font_strong_ = ws::ui::r2d::load_default_font(
        ws::ui::r2d::DefaultFontWeight::semibold, 15.0f * scale_, bake);
    font_small_ = ws::ui::r2d::load_default_font(
        ws::ui::r2d::DefaultFontWeight::regular, 11.0f * scale_, bake);
    chrome_ = ws::ui::gui::kDefaultChrome.scaled(scale_);
    chrome_.font = font_.empty() ? nullptr : &font_;
    chrome_.font_strong =
        font_strong_.empty() ? nullptr : &font_strong_;
    core_.start(options_);
    // The selftest row: CHATTER_SELFTEST=1 makes this copy open the
    // conversation itself once (a "hello" and a longer line, the own-
    // bubble and wrap paths' visual proof under a headless harness) -
    // nothing about the protocol changes, the sends are ordinary ones.
    if (std::getenv("CHATTER_SELFTEST") != nullptr) {
      core_.send("hello from alice!");
      core_.send("and here is a longer line that should wrap onto "
                 "two rows inside the green bubble");
    }
  }

  void on_update() override {
    // Adopt everything the net thread queued since the last frame; a new
    // tail while we sit at the floor keeps the pin.
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
    const float small_h =
        font_small_.empty() ? line_h : font_small_.line_height();

    // The bottom lift: on Android the navigation bar (3-button or
    // gesture) overlays the surface's bottom edge; poll it and park the
    // input bar above it. Desktop answers 0 forever.
    const float inset = static_cast<float>(bottom_inset());

    // The chrome bands, WhatsApp's own ladder: header, feed, input bar.
    const float header_h = pad * 2.0f + line_h + small_h;
    const float bar_h = pad * 2.0f + chrome_.control_height;
    const Rect header{0.0f, 0.0f, frame.width, header_h};
    const Rect feed{0.0f, header_h, frame.width,
                    std::max(0.0f, frame.height - header_h - bar_h - inset)};
    const Rect bar{0.0f, feed.y + feed.height, frame.width, bar_h};

    // --- the feed's scroll: wheel, drag, and the stick-to-bottom pin ---
    // content_h is measured THIS frame (the walk below lays the bubbles
    // top-down into `layout_y`); the wheel and drag read it one frame
    // stale, like every immediate-mode scroll. Pinned: at the floor (or
    // within a line of it) the offset tracks the content so the newest
    // bubble stays at the bottom; lifting the finger off the floor
    // breaks the pin, and the chip returns it.
    const float view_h = std::max(0.0f, feed.height - pad * 2.0f);
    float content_h = 0.0f;
    scroll_input(feed, view_h, pad);
    if (at_floor_) {
      scroll_ = std::max(0.0f, content_h_ - view_h);  // last frame's height: pinned
    }
    const float scroll = std::min(scroll_, std::max(0.0f, content_h_ - view_h));

    ws::ui::r2d::begin();
    ws::ui::r2d::draw_quad(0.0f, 0.0f, frame.width, frame.height, kPaper);
    ws::ui::r2d::draw_quad(header.x, header.y, header.width, header.height,
                           kHeader);

    // The header: identity as the title, the live room line under it.
    if (!font_.empty()) {
      const int n = core_.peers();
      const char* who =
          core_.serving() ? (n == 1 ? " member" : " members")
                          : (n == 1 ? " peer" : " peers");
      const std::string title = core_.name();
      const std::string sub = "port " + std::to_string(core_.port()) +
                              ", " + std::to_string(n) + who +
                              ", tap to type";
      ws::ui::r2d::draw_text(pad, header.y + pad, title.c_str(), font_,
                             kHeaderTitle);
      if (!font_small_.empty()) {
        ws::ui::r2d::draw_text(pad, header.y + pad + line_h, sub.c_str(),
                               font_small_, kHeaderSub);
      }
    }

    // --- the feed: bubbles laid top-down inside the scrolled window ---
    // The walk measures with the font's own metrics; a failed font load
    // degrades to one small bar per line (tinted by sender), so the chat
    // still reads as visibly live.
    ws::ui::r2d::push_clip(feed.x, feed.y, feed.width, feed.height);
    const float bubble_max = feed.width * 0.72f;
    const float bubble_pad = 8.0f * scale_;
    const float bubble_gap = 6.0f * scale_;
    const float bubble_tail = 9.0f * scale_;
    float y = feed.y + pad - scroll + std::max(0.0f, view_h - content_h_);
    std::string_view prev_name;
    bool have_prev = false;
    // The day chip: the feed opens on WhatsApp's date divider, the one
    // badge every chat carries ("TODAY"), riding centered under the
    // first bubble's gap.
    {
      const float chip_h = small_h + bubble_pad * 0.6f;
      content_h += chip_h + bubble_gap;
      if (!font_small_.empty() && y + chip_h > feed.y &&
          y < feed.y + feed.height) {
        const std::string label = "TODAY";
        const float cw =
            font_small_.measure_text(label.c_str()) + bubble_pad * 2.4f;
        const float px = feed.x + (feed.width - cw) * 0.5f;
        ws::ui::r2d::draw_rounded_quad(px, y, cw, chip_h, chip_h * 0.5f,
                                       ws::ui::r2d::rgba(255, 255, 255, 235));
        ws::ui::r2d::draw_text(px + bubble_pad * 1.2f, y + bubble_pad * 0.3f,
                               label.c_str(), font_small_, kPillText);
      }
      y += chip_h + bubble_gap;
    }
    for (const ChatLine& line : lines_) {
      const float bubble_room = bubble_max - bubble_pad * 2.0f - 40.0f * scale_;
      const std::vector<std::string> wrapped =
          font_.empty() ? std::vector<std::string>{line.text}
                        : wrap_text(line.text, font_, bubble_room);
      const float text_w = [&]() {
        float w = 0.0f;
        if (font_.empty()) {
          return 60.0f * scale_;
        }
        for (const std::string& one : wrapped) {
          w = std::max(w, font_.measure_text(one.c_str()));
        }
        return w;
      }();
      const float stamp_w =
          font_small_.empty() ? 0.0f : font_small_.measure_text("00:00") +
                                           18.0f * scale_;
      const float name_h = (line.system || line.name == "you" ||
                            font_strong_.empty())
                                ? 0.0f
                                : small_h;
      const float body_h =
          (line.system ? small_h
                        : line_h * static_cast<float>(wrapped.size())) +
          (line.system ? 0.0f : 4.0f * scale_);
      const float bubble_h = name_h + body_h + bubble_pad * 2.0f;
      const float bubble_w = line.system
                                 ? std::min(bubble_max * 0.8f,
                                            text_w + bubble_pad * 2.0f)
                                 : std::min(bubble_max, std::max(
                                     text_w + bubble_pad * 2.0f + stamp_w,
                                     60.0f * scale_));
      content_h += bubble_h + bubble_gap;

      // The pill: a system line centered, small, translucent over the
      // wallpaper (WhatsApp's "messages are encrypted" chip style).
      if (line.system) {
        if (y + bubble_h > feed.y && y < feed.y + feed.height) {
          const float px =
              feed.x + (feed.width - bubble_w) * 0.5f;
          ws::ui::r2d::draw_rounded_quad(px, y, bubble_w, bubble_h,
                                         bubble_h * 0.5f,
                                         ws::ui::r2d::rgba(255, 255, 255, 235));
          if (!font_small_.empty()) {
            ws::ui::r2d::draw_text(px + bubble_pad, y + bubble_pad,
                                   line.text.c_str(), font_small_,
                                   kPillText);
          }
        }
        y += bubble_h + bubble_gap;

        have_prev = false;
        continue;
      }

      // A foreign run hides the name on the second and later lines of
      // the run (WhatsApp's own group rule), so a run of bubbles from
      // one sender reads as one block with one name.
      const bool mine = line.name == "you";
      const bool show_name = !mine && (!have_prev || prev_name != line.name);
      const float name_used = show_name ? small_h : 0.0f;
      const float bh = bubble_h - name_h + name_used;

      // Degraded feed (no font): one tinted bar per line.
      if (font_.empty()) {
        const float bx = mine ? feed.x + feed.width - pad - 80.0f * scale_
                              : feed.x + pad;
        ws::ui::r2d::draw_quad(bx, y + line_h * 0.5f, 80.0f * scale_,
                               3.0f * scale_,
                               mine ? kHeader : name_color(line.name));
        y += line_h + bubble_gap;

        prev_name = line.name;
        have_prev = true;
        continue;
      }

      const float bx =
          mine ? feed.x + feed.width - pad - bubble_w : feed.x + pad;
      // The face, the tail, the text, the corner: rounded corners at
      // the three far ends, a small square tail toward the sender's
      // edge, the timestamp bottom-INSIDE the bubble, the check beside
      // it on own sends.
      ws::ui::r2d::draw_rounded_quad(
          bx + (mine ? 0.0f : bubble_tail), y, bubble_w - bubble_tail, bh,
          10.0f * scale_, mine ? kBubbleMine : kBubbleTheirs);
      ws::ui::r2d::draw_quad(
          mine ? bx : bx + bubble_w - bubble_tail,
          y + bh - 14.0f * scale_, bubble_tail, 14.0f * scale_,
          mine ? kBubbleMine : kBubbleTheirs);
      float ty = y + bubble_pad;
      if (show_name && !font_strong_.empty()) {
        ws::ui::r2d::draw_text(bx + (mine ? bubble_pad : bubble_tail + bubble_pad),
                               ty, line.name.c_str(), font_strong_,
                               name_color(line.name));
        ty += small_h;
      } else if (name_h > 0.0f) {
        ty += small_h;  // the run's hidden name keeps its blank
      }
      for (const std::string& one : wrapped) {
        ws::ui::r2d::draw_text(bx + (mine ? bubble_pad : bubble_tail + bubble_pad),
                               ty, one.c_str(), font_, kText);
        ty += line_h;
      }
      // The corner: HH:MM riding inside the bubble's bottom-right, with
      // the delivery check on own sends - a small V of two quads (the
      // bake's ASCII range carries no check glyph; drawn beats tofu).
      if (!font_small_.empty()) {
        const std::string stamp = stamp_hhmm(line.sent_hh, line.sent_mm);
        const float sw = font_small_.measure_text(stamp.c_str()) +
                         (mine ? 14.0f * scale_ : 0.0f);
        const float sx =
            bx + (mine ? 0.0f : bubble_tail) + bubble_w - bubble_tail -
            sw - bubble_pad;
        const float sy = y + bh - small_h - bubble_pad * 0.5f;
        ws::ui::r2d::draw_text(sx, sy, stamp.c_str(), font_small_, kTimeDim);
        if (mine) {
          // The delivery ticks: the single check's V, and a second V
          // beside it once the line has at least one peer to have
          // reached (delivered, the pair; a lone copy reads as sent).
          const float vx = sx + font_small_.measure_text(stamp.c_str()) +
                           5.0f * scale_;
          const float vy = sy + small_h * 0.62f;
          const float t = 1.4f * scale_;   // stroke thickness
          const float arm = 4.5f * scale_;  // each arm's run
          // A peer being connected is not an acknowledgement. Keep the
          // single "sent" tick until the protocol has message IDs + ACKs.
          const int marks = 1;
          for (int m = 0; m < marks; ++m) {
            const float ox = vx + m * 4.5f * scale_;
            // Left arm: from up-left down to the meeting point.
            const float arm_l[4][2] = {
                {ox - arm, vy - arm * 0.9f - t},
                 {ox - arm + t, vy - arm * 0.9f},
                 {ox, vy}, {ox, vy}};
            // Right arm: from the meeting point up to the top-right.
            const float arm_r[4][2] = {
                {ox, vy},
                 {ox + arm * 0.7f - t, vy - arm * 1.4f + t},
                 {ox + arm * 0.7f, vy - arm * 1.4f},
                 {ox + arm * 0.7f, vy - arm * 1.4f}};
            const float zeros[4][2] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};
            const float gray[4][4] = {
                {0.5f, 0.55f, 0.5f, 1}, {0.5f, 0.55f, 0.5f, 1},
                {0.5f, 0.55f, 0.5f, 1}, {0.5f, 0.55f, 0.5f, 1}};
            ws::ui::r2d::draw_vertex_quad(arm_l, zeros, gray);
            ws::ui::r2d::draw_vertex_quad(arm_r, zeros, gray);
          }
        }
      }
      y += bh + bubble_gap;

      prev_name = line.name;
      have_prev = true;
    }
    if (content_h > 0.0f) {
      content_h -= bubble_gap;  // the walk's trailing gap is not content
    }
    ws::ui::r2d::pop_clip();

    // The "new messages" chip: a small teal pill riding the feed's floor
    // while the pin is off, one tap away from returning it.
    if (!at_floor_ && content_h_ > view_h && !font_small_.empty()) {
      const std::string label = "new messages";
      const float lw = font_small_.measure_text(label.c_str()) +
                       bubble_pad * 2.0f;
      const Rect chip{feed.x + (feed.width - lw) * 0.5f,
                      bar.y - small_h - bubble_pad, lw,
                      small_h + bubble_pad * 0.5f};
      ws::ui::r2d::draw_rounded_quad(chip.x, chip.y, chip.width, chip.height,
                                     chip.height * 0.5f, kChip);
      ws::ui::r2d::draw_text(chip.x + bubble_pad,
                             chip.y + bubble_pad * 0.25f, label.c_str(),
                             font_small_, kHeaderTitle);
      chip_rect_ = chip;
    } else {
      chip_rect_ = {};
    }

    // The input bar's strip, under the feed.
    ws::ui::r2d::draw_quad(bar.x, bar.y, bar.width, bar.height, kBar);
    ws::ui::r2d::end();

    // --- the input bar: the field and the send button, gui widgets ---
    ws::ui::r2d::set_layer(10);
    ws::ui::r2d::begin();
    gui_.begin_frame(ws::input::frame_input());
    // The chip rides this layer's pass too: a hit test over its drawn
    // rect (the draw happened in the world pass; only the click lives
    // here), one tap re-pins the feed.
    if (chip_rect_.width > 0.0f) {
      const WidgetId chip_id = gui_.ids().id("newmsg");
      if (ws::ui::gui::button_behavior(gui_, chip_id, chip_rect_).clicked) {
        at_floor_ = true;
      }
    }
    // A WhatsApp-height row: field filling the width minus the button,
    // the round green button beside it. The row opens as a bare layout
    // panel (the strip's face is already drawn; the widgets need only
    // the cursor). The field is the stock behavior (caret, selection,
    // clipboard, undo) under this app's own look: the WhatsApp white
    // pill, dark text, the dim placeholder while empty - the split-halves
    // seam text_input exposes for exactly this.
    const float send_w = chrome_.control_height * 1.2f;
    const float field_w = content_width(bar, pad) - send_w - chrome_.spacing;
    (void)layout_.begin_panel(bar, pad, chrome_.spacing);
    if (field_w > 20.0f) {
      const Rect row = layout_.next(field_w, chrome_.control_height);
      field_row_ = row;
      gui_.ids().push("bar");
      const WidgetId field_id = gui_.ids().id("message");
      TextInputState& edit = gui_.persistent_state<TextInputState>(field_id);
      const TextInputResult result = ws::ui::gui::text_input_behavior(
          gui_, field_id, row, input_, kInputCapacity, edit, chrome_);
      gui_.ids().pop();
      // The look: the white pill, the placeholder or text, the caret.
      ws::ui::r2d::draw_rounded_quad(row.x, row.y, row.width, row.height,
                                     row.height * 0.5f, kField);
      const float text_x = row.x + 12.0f * scale_;
      if (input_[0] == '\0') {
        ws::ui::r2d::draw_text(text_x, row.y + row.height * 0.5f -
                                         (font_.empty() ? 0.0f : font_.line_height() * 0.5f),
                               "type a message", font_, kPillText);
      } else {
        ws::ui::r2d::draw_text(text_x, row.y + row.height * 0.5f -
                                         (font_.empty() ? 0.0f : font_.line_height() * 0.5f),
                               input_, font_, kText);
        // The caret rides the behavior's mapped x, blinking.
        if (result.focused && result.blink < 0.5f) {
          ws::ui::r2d::draw_quad(result.caret_x, row.y + 4.0f * scale_,
                                 2.0f * scale_, row.height - 8.0f * scale_,
                                 kSend);
        }
      }
    }
    layout_.end_panel();
    // Enter in the field is the desktop's send edge (the field never
    // takes Enter itself); the button answers for touch. The id is
    // derived in the pushed scope the field already used ("message"
    // under "bar").
    gui_.ids().push("bar");
    const WidgetId field_id = gui_.ids().id("message");
    gui_.ids().pop();
    const bool enter = gui_.input().key_pressed[ws::input::keys::kEnter];
    const bool send_tapped = send_button(bar, pad);
    if ((enter && field_focus_) || send_tapped) {
      if (input_[0] != '\0') {
        core_.send(input_);
        input_[0] = '\0';  // the field's own caret reset rides the next frame
      }
    }
    field_focus_ = gui_.is_focused(field_id);
    gui_.end_frame();
    ws::ui::r2d::end();
    ws::ui::r2d::set_layer(0);

    // The scroll bookkeeping rides the frame's measurements: content_h_
    // for the next frame's wheel/drag/pin. The pin itself is owned only
    // by its transitions (wheel/drag off the floor, drag/chip back to it);
    // re-deriving it here would re-pin the moment a short feed shrinks
    // under a lifted finger, which is not the WhatsApp behavior.
    content_h_ = content_h;
    view_h_ = view_h;
  }

 private:
  // The feed's wheel-and-drag scroll, run before the draw: the wheel
  // moves the offset directly (mouse frames only), and a touch drag
  // that started inside the feed follows the finger past the threshold
  // (the tap stays a tap under it, the scroll eats it over). The pin
  // breaks the moment the scroll leaves the floor.
  void scroll_input(const Rect& feed, float view_h, float pad) {
    (void)pad;
    const ws::input::InputFrame& in = ws::input::frame_input();
    const float line_h =
        font_.empty() ? chrome_.control_height : font_.line_height();
    if (in.mouse_present && in.scroll_dy != 0.0f) {
      scroll_ -= in.scroll_dy * line_h * 2.0f;
      at_floor_ = false;
    }
    for (const ws::input::TouchPoint& t : in.touches) {
      if (!t.down && !t.released) {
        continue;  // an empty slot (entries linger one frame past release)
      }
      const bool inside = t.x >= feed.x && t.x < feed.x + feed.width &&
                          t.y >= feed.y && t.y < feed.y + feed.height;
      if (t.pressed && inside) {
        drag_id_ = t.id;
        drag_last_y_ = t.y;
        drag_active_ = false;
      } else if (t.id == drag_id_ && t.down) {
        if (!drag_active_ &&
            std::abs(t.y - drag_last_y_) > chrome_.drag_threshold) {
          drag_active_ = true;
          at_floor_ = false;
        }
        if (drag_active_) {
          scroll_ -= (t.y - drag_last_y_);
        }
        drag_last_y_ = t.y;
      } else if (t.id == drag_id_ && t.released) {
        drag_id_ = ws::input::kNoPointer;
        drag_active_ = false;
      }
    }
    scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, content_h_ - view_h));
    // A drag that lands back at the floor re-pins (WhatsApp returns to
    // live-follow on the fling's own end).
    if (drag_active_ && content_h_ > view_h &&
        scroll_ >= content_h_ - view_h - line_h * 0.5f) {
      at_floor_ = true;
    }
  }

  // The send button: the WhatsApp round green disc with the paper-plane
  // arrow, drawn and hit through the widget seam's split halves - the
  // behavior (identity, focus, the click) with a custom draw (the disc
  // and glyph over it, no label). One third of the control height in
  // from the bar's right edge.
  bool send_button(const Rect& bar, float pad) {
    const float dia = chrome_.control_height * 0.92f;
    const float cy = bar.y + (bar.height - dia) * 0.5f;
    const float cx = bar.x + bar.width - pad - dia;
    const Rect disc{cx, cy, dia, dia};
    gui_.ids().push("bar");
    const WidgetId id = gui_.ids().id("send");
    const ws::ui::gui::ButtonResult result =
        ws::ui::gui::button_behavior(gui_, id, disc);
    gui_.ids().pop();
    // The custom look: the hovered rung, the disc, the glyph - a
    // paper-plane wedge the vertex triangles spell.
    const bool hot = result.hit.hovered || result.hit.pressed;
    const std::uint32_t disc_face =
        (result.hit.pressed || hot) ? kSendHover : kSend;
    ws::ui::r2d::draw_rounded_quad(disc.x, disc.y, disc.width, disc.height,
                                   disc.width * 0.5f, disc_face);
    // The glyph: the send arrow - one clean solid white triangle, nose
    // to the right (the play-arrow shape every messenger's empty-state
    // send wears). draw_vertex_quad spells a triangle as the degenerate
    // quad the index pattern takes; the disc's green shows nowhere
    // inside it.
    {
      const float cxg = disc.x + dia * 0.5f;
      const float cyg = disc.y + dia * 0.5f;
      const float r = dia * 0.20f;
      const float positions0[4][2] = {
          {cxg - r * 0.75f, cyg - r}, {cxg + r, cyg},
          {cxg - r * 0.75f, cyg + r}, {cxg - r * 0.75f, cyg + r}};
      const float zeros[4][2] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};
      const float white[4][4] = {{1, 1, 1, 1}, {1, 1, 1, 1},
                                 {1, 1, 1, 1}, {1, 1, 1, 1}};
      ws::ui::r2d::draw_vertex_quad(positions0, zeros, white);
    }
    return result.activated();
  }

  // The bar's usable width between its paddings.
  static float content_width(const Rect& bar, float pad) {
    return std::max(0.0f, bar.width - pad * 2.0f);
  }

  static constexpr std::size_t kShownLines = 200;
  static constexpr int kInputCapacity = 256;

  chatter::ChatCore core_;
  chatter::ChatCore::Options options_;  // handed to start() in on_setup
  ws::ui::gui::GuiContext gui_;
  ws::ui::gui::LayoutCursor layout_;
  ws::ui::gui::ChromeTheme chrome_;
  ws::ui::r2d::Font font_;
  ws::ui::r2d::Font font_strong_;  // the semibold sibling (sender labels)
  ws::ui::r2d::Font font_small_;   // the small face (stamps, pills, header sub)
  float scale_ = 1.0f;
  std::vector<ChatLine> lines_;

  // The feed's scroll state (app-owned; the pin and the chip own it).
  float scroll_ = 0.0f;        // px the content is shifted up by
  float content_h_ = 0.0f;      // last frame's laid content height
  float view_h_ = 0.0f;        // last frame's viewport height
  bool at_floor_ = true;       // stick-to-bottom pin
  ws::input::PointerId drag_id_ = ws::input::kNoPointer;
  float drag_last_y_ = 0.0f;
  bool drag_active_ = false;

  // The input bar's state: the field's buffer and its focus answer.
  char input_[kInputCapacity] = {};
  bool field_focus_ = false;
  Rect chip_rect_ = {};
  Rect field_row_ = {};  // last frame's field row (the caret's backdrop)
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
        "usage: chatter [name] [--name n] [--serve [port]] [--relay host:port]...");
  }
  ChatterApp chatter{config, launch.core};
  const int rc = chatter.run();
  chatter.stop_net();
  return rc;
}
#endif
