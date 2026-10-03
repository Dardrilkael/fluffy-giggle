// apps/chatter - the gui sibling of netchat, built for the phone (and the
// desktop). Same wire protocol: the "netchat <port> <name>" discovery beat
// every 500 ms on two channels (the LAN broadcast for peers on other
// machines, the 127.255.255.255 directed broadcast for peers on this
// one), the smaller-listen-port-dials pairing rule that gives every pair
// exactly one framed TCP connection, and the HELLO/SAY frames. A beat
// this copy cannot dial (its port is the bigger one) still earns an
// answer - one unicast copy of our own beat straight back at the source:
// a LAN hop can carry broadcasts one way only (the wired PC here has
// never received the phone's Wi-Fi beats), and the unicast answer crosses
// it, so both sides hold both candidates and the port rule still gives
// the pair exactly one connection. Sharing
// the beat's prefix is deliberate: a chatter and a terminal netchat
// discover each other and share one chat.
//
// v1 input is a tap-phrase palette, not the soft keyboard: IME commits
// cannot reach a hasCode=false NativeActivity without a window-layer
// change in the ui lib (window_android.cpp), and this app deliberately
// stays app-pure - zero lib edits. The desktop window gets the same
// palette; typing stays netchat's job.
//
// Threading: the net loop runs on its own thread (slicing the loop into
// the render loop with run_once would stall the gui for up to the beat
// period whenever the loop waits). Lines arrive to the gui through a
// mutex-guarded queue drained once per frame; palette taps cross back
// with post(), the loop's one thread-safe call. Teardown posts the same
// close-everything shutdown netchat uses and joins the thread before the
// loop can destruct, so nothing queued outlives the app object.
// Connection Ptrs live in the core's scope and callbacks capture raw
// pointers only - the shared_ptr cycle rule from tcp.hpp.

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

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <ws/core/log.hpp>
#include <ws/core/result.hpp>
#include <ws/net/address.hpp>
#include <ws/net/discovery.hpp>
#include <ws/net/errors.hpp>
#include <ws/net/event_loop.hpp>
#include <ws/net/tcp.hpp>
#include <ws/ui/app.hpp>
#include <ws/ui/gui/widgets.hpp>
#include <ws/ui/renderer2d.hpp>

namespace {

// std::byte <-> text, the netdemo/netchat translation pair.
std::span<const std::byte> to_bytes(std::string_view text) {
  return std::as_bytes(std::span<const char>{text.data(), text.size()});
}

std::string_view as_text(std::span<const std::byte> data) {
  if (data.empty()) {
    return {};
  }
  return {reinterpret_cast<const char*>(data.data()), data.size()};
}

// One chat line as the gui sees it. `name` is the sender's HELLO name (or
// "you" for own sends); system lines (joins, leaves, the banner) render
// dim instead of "[name] text".
struct ChatLine {
  std::string name;
  std::string text;
  bool system = false;
};

// One live connection plus the name its HELLO carried - netchat's Peer,
// verbatim. The vector in ChatCore's scope holds the ownership; nothing
// ever captures a connection Ptr by value inside that connection's own
// stored callbacks.
struct Peer {
  ws::net::TcpConnection::Ptr conn;
  std::shared_ptr<std::string> name;  // "?" until HELLO lands
};

// netchat's protocol core, wrapped for a cross-thread gui. The loop and
// every socket live on the net thread; the gui thread touches only
// start/stop, send(), pull() and the status accessors.
class ChatCore {
 public:
  ChatCore() = default;
  ~ChatCore() { stop(); }  // no-op when already stopped; joins first
  ChatCore(const ChatCore&) = delete;
  ChatCore& operator=(const ChatCore&) = delete;

  // Brings up the listener, browser and both announcers, then starts the
  // net thread. `display_name` overrides the "chatter-<port>" default.
  void start(std::string display_name) {
    auto listener_r =
        ws::net::TcpListener::listen(loop_, ws::net::Address::any(0));
    if (listener_r) {
      listener_ = *listener_r;
      // Any-bind caveat: local() reports 0.0.0.0 - bind-all, a port to
      // READ, never a destination. Only the port is real.
      port_ = static_cast<int>(listener_->local().port);
    } else {
      ws::core::log_error(listener_r.error());
      // No listener, no pairing - but run anyway: a browser that works is
      // still a feed, and the failure is on the screen via the status.
    }
    name_ = display_name.empty()
                ? "chatter-" + std::to_string(port_)
                : std::move(display_name);

    if (listener_) {
      listener_->set_on_connection(
          [this](ws::net::TcpConnection::Ptr conn) { adopt(conn); });
    }

    auto browser_r = ws::net::DiscoveryBrowser::browse(loop_);
    if (browser_r) {
      browser_ = *browser_r;
      browser_->set_on_peer(
          [this](std::span<const std::byte> payload, ws::net::Address from) {
            on_beat(payload, from);
          });
    } else {
      ws::core::log_error(browser_r.error());
    }

    // The beat says "netchat", not "chatter": the prefix is the
    // protocol's app tag, and sharing it is what lets chatter and the
    // terminal netchat find each other.
    if (listener_) {
      const std::string beat =
          "netchat " + std::to_string(port_) + " " + name_;
      auto lan_r = ws::net::DiscoveryAnnouncer::announce(
          loop_, to_bytes(beat),
          ws::net::Address::broadcast(ws::net::kDefaultDiscoveryPort), 500);
      if (lan_r) {
        lan_announcer_ = *lan_r;
      } else {
        ws::core::log_error(lan_r.error());
      }
      // The same-machine rendezvous: Windows does not loop 255.255.255.255
      // back to the sender's own listeners (the netdemo run showed 0
      // local beats), but 127.255.255.255 is loopback's directed
      // broadcast - it stays on this machine and reaches every browser
      // bound here. The library already sets SO_BROADCAST on every UDP
      // socket, so this announce needs no special arrangement.
      auto loopback_r = ws::net::DiscoveryAnnouncer::announce(
          loop_, to_bytes(beat),
          ws::net::Address{"127.255.255.255", ws::net::kDefaultDiscoveryPort},
          500);
      if (loopback_r) {
        loopback_announcer_ = *loopback_r;
      } else {
        ws::core::log_error(loopback_r.error());
      }
    }

    push_line({name_, "here (port " + std::to_string(port_) + ")", true});
    running_.store(true);
    thread_ = std::thread([this]() {
      const ws::core::Status ran = loop_.run();
      if (!ran) {
        ws::core::log_error(ran.error());
      }
      running_.store(false);
    });
  }

  // Gui thread -> the loop. A tap posts one SAY broadcast; the sent line
  // echoes into the queue from the loop thread, exactly like received
  // lines, so ordering is the wire's ordering.
  void send(std::string_view line) {
    if (!running_.load() || line.empty()) {
      return;
    }
    std::string outgoing = "SAY " + std::string{line};
    (void)loop_.post([this, outgoing = std::move(outgoing)]() {
      push_line({"you",
                 std::string{std::string_view{outgoing}.substr(4)}, false});
      for (const Peer& p : peers_) {
        (void)p.conn->send(to_bytes(outgoing));
      }
    });
  }

  // Net thread -> gui: swaps out every line queued since the last pull.
  std::vector<ChatLine> pull() {
    std::vector<ChatLine> out;
    {
      const std::lock_guard<std::mutex> lock{mtx_};
      out.swap(pending_);
    }
    return out;
  }

  // Posts the shutdown - every live connection closed in flight, then the
  // listener, both announcers and the browser; run() drains and returns -
  // and joins the net thread. Safe to call twice; the dtor calls it
  // defensively. Must run before ChatCore's members die, so the app calls
  // it right after App::run() returns.
  void stop() {
    if (!thread_.joinable()) {
      return;  // never started, or already stopped
    }
    if (running_.load()) {
      (void)loop_.post([this]() {
        // on_closed fires during these closes and erases peers_, so
        // iterate a snapshot.
        const std::vector<Peer> snapshot = peers_;
        for (const Peer& p : snapshot) {
          (void)p.conn->close();
        }
        if (listener_) {
          listener_->close();
        }
        if (lan_announcer_) {
          lan_announcer_->stop();
        }
        if (loopback_announcer_) {
          loopback_announcer_->stop();
        }
        if (browser_) {
          browser_->stop();
        }
      });
    }
    thread_.join();
  }

  int port() const { return port_; }
  const std::string& name() const { return name_; }
  int peers() const { return live_.load(); }

 private:
  // Everything a fresh connection needs, dialed and accepted alike:
  // HELLO on connect, HELLO/SAY parsing after, bookkeeping on close -
  // netchat's adopt, pushing to the gui queue instead of logging.
  void adopt(const ws::net::TcpConnection::Ptr& conn) {
    const auto tag = std::make_shared<std::string>("?");
    ws::net::TcpConnection* self = conn.get();
    peers_.push_back({conn, tag});
    live_.fetch_add(1);
    // Both sides greet: the dialed connection's on_connected, and the
    // accepted one's - which fires after on_connection returns.
    conn->set_on_connected([self, this]() {
      (void)self->send(to_bytes("HELLO " + name_));
    });
    conn->set_on_message([tag, this](std::span<const std::byte> message) {
      std::string_view text = as_text(message);
      if (text.starts_with("HELLO ")) {
        text.remove_prefix(6);
        *tag = std::string{text};  // copy: the span dies with the callback
        push_line({*tag, "joined", true});
        return;
      }
      // SAY is this app's chat wrapper. Anything else still shows as-is.
      const std::string_view body =
          text.starts_with("SAY ") ? text.substr(4) : text;
      push_line({*tag, std::string{body}, false});
    });
    conn->set_on_closed([tag, self, this](ws::net::NetError why) {
      if (*tag != "?") {
        push_line({*tag,
                   std::string{"left ("}
                       .append(ws::net::describe(why))
                       .append(")"),
                   true});
      }
      std::erase_if(peers_,
                    [self](const Peer& p) { return p.conn.get() == self; });
      std::erase_if(dialed_, [self](const std::pair<const std::string,
                                                   ws::net::TcpConnection*>& kv) {
        return kv.second == self;
      });
      live_.fetch_sub(1);
    });
  }

  // The beat parser: netchat's, verbatim, reading this copy's port from
  // port_ instead of a captured local.
  void on_beat(std::span<const std::byte> payload, ws::net::Address from) {
    if (!listener_) {
      return;  // nothing to be dialed into; announcing was skipped too
    }
    std::string_view text = as_text(payload);
    constexpr std::string_view kPrefix = "netchat ";
    if (!text.starts_with(kPrefix)) {
      return;  // another app's beat (netdemo says hello, ...) - not ours
    }
    text.remove_prefix(kPrefix.size());
    const std::size_t space = text.find(' ');
    if (space == std::string_view::npos) {
      return;
    }
    const std::string_view port_s = text.substr(0, space);
    int port = 0;
    const auto [end, ec] =
        std::from_chars(port_s.data(), port_s.data() + port_s.size(), port);
    if (ec != std::errc{} || end != port_s.data() + port_s.size() ||
        port <= 0 || port > 65535) {
      return;
    }
    if (port == port_) {
      return;  // my own beat, looped back to me
    }
    // First sight of a peer's port - the one log a quiet run ever prints
    // (joins and errors say the rest): that discovery itself is alive,
    // and where the beat came from. Beats repeat every 500 ms; this
    // fires once per port per process.
    if (noticed_.insert(std::string{port_s}).second) {
      ws::core::info("chatter: beat - {} (port {}) via {}",
                     text.substr(space + 1), port_s, from.host);
    }
    if (dialed_.count(std::string{port_s}) != 0) {
      return;  // already dialing, or talking, to this announced port
    }
    if (port_ >= port) {
      answer_beat(from);  // teach a one-way-deaf peer this copy exists
      return;  // the smaller listen port dials: one connection per pair
    }
    auto conn_r = ws::net::TcpConnection::connect(
        loop_, ws::net::Address{from.host, static_cast<std::uint16_t>(port)});
    if (!conn_r) {
      ws::core::log_error(conn_r.error());  // the next beat retries
      return;
    }
    ws::net::TcpConnection::Ptr conn = *conn_r;
    dialed_[std::string{port_s}] = conn.get();
    adopt(conn);
  }

  // The answer a heard beat earns when this copy is not the dialer: one
  // unicast copy of our own beat, straight back at the beat's source -
  // the announcer's own socket, off-cadence. A LAN hop can carry
  // broadcasts one way only (the wired PC here has never once received
  // the phone's Wi-Fi beats), so the peer on the far side may never
  // learn this copy exists; the unicast answer crosses that hop, and
  // the port rule on its side ends up holding both candidates.
  // Throttled per host - beats keep arriving every 500ms and each would
  // earn a fresh answer otherwise; a peer already connected to us
  // ignores the answer (its dialed map holds our port), so nothing
  // extra tracks "paired".
  void answer_beat(const ws::net::Address& from) {
    if (!lan_announcer_) {
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    auto [it, fresh] = answered_.try_emplace(from.host, now);
    if (!fresh && now - it->second < std::chrono::seconds{2}) {
      return;
    }
    it->second = now;
    if (fresh) {
      // Once per host: the one line that says the one-way-deaf hop is
      // being bridged. Steady-state re-answers stay quiet.
      ws::core::info("chatter: answered {} with a unicast beat", from.host);
    }
    const ws::core::Status sent = lan_announcer_->send_once(
        ws::net::Address{from.host, ws::net::kDefaultDiscoveryPort});
    if (!sent) {
      // The cadence beats stay best-effort-silent by the lib's design,
      // but the answer is the one datagram pairing can hinge on - a
      // send that never even left deserves a line.
      ws::core::log_error(sent.error());
    }
  }

  // Queue one line for the gui (called on the net thread).
  void push_line(ChatLine line) {
    const std::lock_guard<std::mutex> lock{mtx_};
    pending_.push_back(std::move(line));
    if (pending_.size() > kQueuedLines) {
      pending_.erase(pending_.begin(),
                     pending_.end() - static_cast<std::ptrdiff_t>(kQueuedLines));
    }
  }

  static constexpr std::size_t kQueuedLines = 256;

  // Declaration order matters: loop_ is declared first so it destructs
  // LAST - after the thread (which stop() has already joined) and after
  // every handle Ptr, the same order netchat's main-scope declarations
  // give. The thread is declared last, so it destructs first, joined.
  ws::net::EventLoop loop_;
  ws::net::TcpListener::Ptr listener_;
  ws::net::DiscoveryBrowser::Ptr browser_;
  ws::net::DiscoveryAnnouncer::Ptr lan_announcer_;
  ws::net::DiscoveryAnnouncer::Ptr loopback_announcer_;

  std::vector<Peer> peers_;  // every live connection (loop thread only)
  // Announced ports this copy has dialed, and the connection each got
  // (loop thread only). Keyed by the announced port alone: a peer is its
  // listen port in this protocol. on_closed clears the entry, so a beat
  // that keeps arriving can re-dial a peer that came back.
  std::map<std::string, ws::net::TcpConnection*> dialed_;
  // Last unicast answer time per source host (loop thread only) -
  // answer_beat's throttle.
  std::map<std::string, std::chrono::steady_clock::time_point> answered_;
  // Peer ports this copy has already logged a beat line for (loop
  // thread only) - on_beat's once-per-port discovery log.
  std::set<std::string> noticed_;

  // Written once in start(), before the net thread exists - read-only to
  // both threads after, no lock needed.
  std::string name_;
  int port_ = 0;

  std::atomic<int> live_{0};           // connections up, for the status line
  std::atomic<bool> running_{false};   // run() has not returned yet

  mutable std::mutex mtx_;             // guards pending_ only
  std::vector<ChatLine> pending_;      // loop thread writes, gui drains
  std::thread thread_;                 // the net thread; joined in stop()
};

constexpr std::uint32_t kBackdrop = ws::ui::r2d::rgba(22, 24, 30);
constexpr std::uint32_t kText = ws::ui::r2d::rgba(235, 238, 242);
constexpr std::uint32_t kDim = ws::ui::r2d::rgba(140, 145, 152);
constexpr std::uint32_t kYou = ws::ui::r2d::rgba(120, 220, 140);

// The tap vocabulary - v1's whole input surface (see the header comment).
constexpr const char* kPhrases[] = {"hello!", "yes", "no", "brb",
                                    "ping", "thanks", "lol", "bye"};
constexpr std::size_t kPhraseCount = sizeof(kPhrases) / sizeof(kPhrases[0]);

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

// The gui: a status line, the feed (newest at the bottom, auto-trimmed to
// what fits - no scroll in v1), and the palette. All input flows through
// the gui widgets, all drawing through r2d; chatter draws no ImGui
// windows at all.
class ChatterApp final : public ws::ui::App {
 public:
  ChatterApp(ws::ui::AppConfig config, std::string display_name)
      : ws::ui::App(std::move(config)),
        display_name_(std::move(display_name)) {}

  // Joins the net thread. Call after run() returns (both entries do) so
  // the core stops before the app object destructs.
  void stop_net() { core_.stop(); }

 protected:
  void on_setup() override {
    scale_ = ui_scale();
    // The system face, playground's platform-conditional constant - the
    // lib's rule is the caller resolves the path.
#ifdef __ANDROID__
    const char* font_path = "/system/fonts/Roboto-Regular.ttf";
#else
    const char* font_path = "C:/Windows/Fonts/segoeui.ttf";
#endif
    font_ = ws::ui::r2d::load_font(font_path, 16.0f * scale_);
    chrome_ = ws::ui::gui::kDefaultChrome.scaled(scale_);
    chrome_.font = font_.empty() ? nullptr : &font_;
    core_.start(display_name_);
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

    // The palette panel: two rows of four phrase buttons, full width at
    // the bottom (padding + row + spacing + row + padding).
    const float panel_h = chrome_.padding * 2.0f +
                          chrome_.control_height * 2.0f + chrome_.spacing;
    const ws::ui::gui::Rect palette{0.0f, frame.height - panel_h,
                                    frame.width, panel_h};
    const float status_h = chrome_.padding * 2.0f + line_h;

    ws::ui::r2d::begin();
    ws::ui::r2d::draw_quad(0.0f, 0.0f, frame.width, frame.height, kBackdrop);

    // Status line: identity, port, live connections.
    if (!font_.empty()) {
      const std::string status =
          "[" + core_.name() + "] port " + std::to_string(core_.port()) +
          " - " + std::to_string(core_.peers()) +
          (core_.peers() == 1 ? " peer" : " peers");
      ws::ui::r2d::draw_text(pad, pad, status.c_str(), font_, kDim);
    }

    // The feed: newest line just above the palette, older ones upward,
    // stopping at the status bar. Clipped so a long line cuts at the
    // screen edge instead of running over the palette (v1 has no
    // truncation - text wider than the window simply clips).
    ws::ui::r2d::push_clip(0.0f, status_h, frame.width, palette.y - status_h);
    float y = palette.y - chrome_.spacing - line_h;
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
      } else if (line.system) {
        ws::ui::r2d::draw_text(pad, y, line.text.c_str(), font_, kDim);
      } else {
        const std::string who = "[" + line.name + "]";
        const float who_w = ws::ui::r2d::draw_text(pad, y, who.c_str(), font_,
                                                   name_color(line.name));
        ws::ui::r2d::draw_text(pad + who_w + chrome_.spacing, y,
                               line.text.c_str(), font_, kText);
      }
      y -= line_h + line_gap;
    }
    ws::ui::r2d::pop_clip();
    ws::ui::r2d::end();

    // The palette: gui widgets above the world, the playground panel
    // shape. A tap posts one SAY through the core.
    ws::ui::r2d::set_layer(10);
    ws::ui::r2d::begin();
    gui_.new_frame(ws::ui::gui::frame_input());
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

  ChatCore core_;
  std::string display_name_;  // "" -> "chatter-<port>"
  ws::ui::gui::GuiContext gui_;
  ws::ui::gui::LayoutCursor layout_;
  ws::ui::gui::ChromeTheme chrome_;
  ws::ui::r2d::Font font_;
  float scale_ = 1.0f;
  std::vector<ChatLine> lines_;
};

}  // namespace

#ifdef __ANDROID__
// Android entry: the glue calls android_main on the app's own thread and
// hands over the handle the shell needs (AppConfig::app_handle). No
// assets ship in the APK (the font is a system face), so there is
// nothing to unpack - just point the CWD at the writable data dir
// (imgui.ini's home on shutdown) and run the same code the desktop runs.
extern "C" void android_main(android_app* app) {
  chdir(app->activity->internalDataPath);
  ws::ui::AppConfig config;
  config.title = "ws chatter";
  config.app_handle = app;
  ChatterApp chatter{config, ""};  // the name derives from the listen port
  (void)chatter.run();
  chatter.stop_net();
}
#else
int main(int argc, char* argv[]) {
  ws::ui::AppConfig config;
  config.title = "ws chatter";
  config.width = 420;
  config.height = 760;
  // argv[1] names this copy (netchat's rule); default derives from port.
  ChatterApp chatter{config, argc > 1 ? argv[1] : ""};
  const int rc = chatter.run();
  chatter.stop_net();
  return rc;
}
#endif
