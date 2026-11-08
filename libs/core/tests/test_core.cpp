#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <cctype>
#include <filesystem>
#include <memory>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <ws/core/assert.hpp>
#include <ws/core/log.hpp>
#include <ws/core/result.hpp>

// ---------------------------------------------------------------------------
// Test support: snapshot/restore the global logging state, and a capturing
// sink factory. Every test case that touches global state takes a guard.
// ---------------------------------------------------------------------------

namespace {

class LogStateGuard {
 public:
  LogStateGuard()
      : min_(ws::core::min_level()),
        flush_(ws::core::flush_level()),
        pattern_(ws::core::pattern()),
        sinks_(ws::core::sinks()),
        rules_(ws::core::category_min_levels()) {}

  ~LogStateGuard() {
    ws::core::set_min_level(min_);
    ws::core::set_flush_level(flush_);
    ws::core::set_pattern(pattern_);
    ws::core::clear_sinks();
    for (ws::core::LogSinkPtr& sink : sinks_) {
      ws::core::add_sink(std::move(sink));
    }
    ws::core::clear_category_min_levels();
    for (const auto& [category, level] : rules_) {
      ws::core::set_category_min_level(category, level);
    }
  }

  LogStateGuard(const LogStateGuard&) = delete;
  LogStateGuard& operator=(const LogStateGuard&) = delete;

 private:
  ws::core::LogLevel min_;
  ws::core::LogLevel flush_;
  std::string pattern_;
  std::vector<ws::core::LogSinkPtr> sinks_;
  std::vector<std::pair<std::string, ws::core::LogLevel>> rules_;
};

// Everything one record carries, copied while the record is alive.
struct CapturedLine {
  ws::core::LogLevel level = ws::core::LogLevel::info;
  std::string category;
  std::string message;
  std::string file;
  int line = 0;
  std::string text;
};

class Capture {
 public:
  Capture() {
    sink_ = std::make_shared<ws::core::CallbackSink>(
        [this](const ws::core::LogRecord& record) {
          lines_.push_back(CapturedLine{record.level,
                                        std::string(record.category),
                                        std::string(record.message),
                                        std::string(record.file), record.line,
                                        std::string(record.text)});
        });
    ws::core::clear_sinks();
    ws::core::add_sink(sink_);
  }

  const std::vector<CapturedLine>& lines() const { return lines_; }
  const ws::core::LogSinkPtr& sink() const { return sink_; }

 private:
  ws::core::LogSinkPtr sink_;
  std::vector<CapturedLine> lines_;
};

}  // namespace

// ---------------------------------------------------------------------------
// Original suite (kept verbatim: the compatibility contract)
// ---------------------------------------------------------------------------

TEST_CASE("logging functions accept plain and formatted messages") {
  ws::core::debug("plain debug message");
  ws::core::info("formatted: {} + {} = {}", 2, 2, 4);
  ws::core::warn("a warning");
  ws::core::error("an error: {}", "details here");
  CHECK(true);  // reaching here without crashing is the test
}

TEST_CASE("log levels are ordered") {
  CHECK(ws::core::LogLevel::debug < ws::core::LogLevel::info);
  CHECK(ws::core::LogLevel::info < ws::core::LogLevel::warn);
  CHECK(ws::core::LogLevel::warn < ws::core::LogLevel::error);
}

TEST_CASE("min level is stored and retrievable") {
  const auto previous = ws::core::min_level();
  ws::core::set_min_level(ws::core::LogLevel::warn);
  CHECK(ws::core::min_level() == ws::core::LogLevel::warn);
  ws::core::info("this message is filtered out");
  ws::core::set_min_level(previous);
}

TEST_CASE("assert with true condition is a no-op") {
  WS_CORE_ASSERT(1 + 1 == 2);
  CHECK(true);
}

namespace {

ws::core::Status ok_status() {
  return {};
}

ws::core::Status bad_status() {
  return ws::core::fail(3, "test", "bad status");
}

ws::core::Result<int> ok_value() {
  return 42;
}

ws::core::Result<int> bad_value() {
  return ws::core::fail(9, "test", "bad value");
}

ws::core::Status try_propagates() {
  WS_TRY(ok_status());
  WS_TRY(bad_status());
  return {};
}

ws::core::Status try_passes_through() {
  WS_TRY(ok_status());
  return {};
}

ws::core::Result<int> try_assign_ok() {
  WS_TRY_ASSIGN(const auto v, ok_value());
  return v;
}

ws::core::Result<int> try_assign_propagates() {
  WS_TRY_ASSIGN(const auto v, bad_value());
  return v;
}

}  // namespace

TEST_CASE("fail() produces the right code, category and message") {
  const auto here = std::source_location::current().line();
  const auto failed = ws::core::fail(7, "test", "boom");
  const ws::core::Error err = failed.value();
  CHECK(err.code == 7);
  CHECK(std::string_view(err.category) == "test");
  CHECK(err.message == "boom");
  CHECK(err.where.line() == here + 1);  // captured one line above the call
  CHECK(bool(err));
}

TEST_CASE("WS_TRY propagates an error out of a Status function") {
  const ws::core::Status s = try_propagates();
  CHECK(!s);
  CHECK(s.error().code == 3);
  CHECK(s.error().message == "bad status");
  CHECK(try_passes_through().has_value());
}

TEST_CASE("WS_TRY_ASSIGN unwraps a value or propagates the error") {
  const ws::core::Result<int> ok = try_assign_ok();
  REQUIRE(ok);
  CHECK(*ok == 42);
  const ws::core::Result<int> bad = try_assign_propagates();
  CHECK(!bad);
  CHECK(bad.error().code == 9);
}

TEST_CASE("Result<void> success path") {
  const ws::core::Status s{};
  CHECK(s.has_value());
  CHECK(bool(s));
  CHECK(ok_status().has_value());
}

TEST_CASE("Error converts to bool: truthy only with a nonzero code") {
  CHECK(!ws::core::Error{});
  CHECK(ws::core::Error{1, "test", "x", {}});
}

TEST_CASE("log_error formats without crashing") {
  ws::core::log_error(ws::core::make_error(5, "test", "logged error"));
  CHECK(true);  // reaching here without crashing is the test
}

// ---------------------------------------------------------------------------
// Levels
// ---------------------------------------------------------------------------

TEST_CASE("the level family is fully ordered, including trace/critical/off") {
  CHECK(ws::core::LogLevel::trace < ws::core::LogLevel::debug);
  CHECK(ws::core::LogLevel::error < ws::core::LogLevel::critical);
  CHECK(ws::core::LogLevel::critical < ws::core::LogLevel::off);
}

TEST_CASE("to_string names every level") {
  CHECK(std::string_view(ws::core::to_string(ws::core::LogLevel::trace)) == "TRACE");
  CHECK(std::string_view(ws::core::to_string(ws::core::LogLevel::debug)) == "DEBUG");
  CHECK(std::string_view(ws::core::to_string(ws::core::LogLevel::info)) == "INFO");
  CHECK(std::string_view(ws::core::to_string(ws::core::LogLevel::warn)) == "WARN");
  CHECK(std::string_view(ws::core::to_string(ws::core::LogLevel::error)) == "ERROR");
  CHECK(std::string_view(ws::core::to_string(ws::core::LogLevel::critical)) == "CRITICAL");
  CHECK(std::string_view(ws::core::to_string(ws::core::LogLevel::off)) == "OFF");
}

TEST_CASE("parse_level accepts case-insensitive names and rejects the rest") {
  ws::core::LogLevel level = ws::core::LogLevel::off;
  CHECK(ws::core::parse_level("warn", level));
  CHECK(level == ws::core::LogLevel::warn);
  CHECK(ws::core::parse_level("CRITICAL", level));
  CHECK(level == ws::core::LogLevel::critical);
  CHECK(ws::core::parse_level("Trace", level));
  CHECK(level == ws::core::LogLevel::trace);
  CHECK(ws::core::parse_level("OFF", level));
  CHECK(level == ws::core::LogLevel::off);
  const ws::core::LogLevel sentinel = ws::core::LogLevel::error;
  level = sentinel;
  CHECK(!ws::core::parse_level("bogus", level));
  CHECK(!ws::core::parse_level("", level));
  CHECK(!ws::core::parse_level("information", level));
  CHECK(level == sentinel);  // out is untouched on failure
}

TEST_CASE("the new trace and critical free functions format like the rest") {
  const LogStateGuard guard;
  const Capture capture;
  ws::core::set_min_level(ws::core::LogLevel::trace);  // trace hides behind the default debug floor
  ws::core::trace("plain trace");
  ws::core::critical("critical: {}", 42);
  REQUIRE(capture.lines().size() == 2);
  CHECK(capture.lines()[0].level == ws::core::LogLevel::trace);
  CHECK(capture.lines()[0].message == "plain trace");
  CHECK(capture.lines()[1].level == ws::core::LogLevel::critical);
  CHECK(capture.lines()[1].message == "critical: 42");
}

// ---------------------------------------------------------------------------
// Sinks and the registry
// ---------------------------------------------------------------------------

TEST_CASE("a console sink is installed by default and the registry manages it") {
  const LogStateGuard guard;
  CHECK(ws::core::sinks().size() == 1);
  CHECK(ws::core::sinks().front() != nullptr);

  const auto extra = std::make_shared<ws::core::ConsoleSink>();
  ws::core::add_sink(extra);
  CHECK(ws::core::sinks().size() == 2);

  CHECK(ws::core::remove_sink(extra.get()));
  CHECK(ws::core::sinks().size() == 1);
  CHECK(!ws::core::remove_sink(extra.get()));  // already gone
  CHECK(!ws::core::remove_sink(nullptr));      // null never matches

  ws::core::clear_sinks();
  CHECK(ws::core::sinks().empty());
  ws::core::add_sink(nullptr);  // ignored, not a crash
  CHECK(ws::core::sinks().empty());
}

TEST_CASE("records reach every sink with their level, category and message") {
  const LogStateGuard guard;
  const Capture capture;
  ws::core::log(ws::core::LogLevel::warn, "ui", "panel detached");
  REQUIRE(capture.lines().size() == 1);
  CHECK(capture.lines()[0].level == ws::core::LogLevel::warn);
  CHECK(capture.lines()[0].category == "ui");
  CHECK(capture.lines()[0].message == "panel detached");
}

TEST_CASE("per-sink min levels filter independently of the global level") {
  const LogStateGuard guard;
  std::vector<ws::core::LogLevel> loud;
  std::vector<ws::core::LogLevel> quiet;
  const auto loud_sink =
      std::make_shared<ws::core::CallbackSink>([&loud](const ws::core::LogRecord& r) {
        loud.push_back(r.level);
      });
  const auto quiet_sink =
      std::make_shared<ws::core::CallbackSink>([&quiet](const ws::core::LogRecord& r) {
        quiet.push_back(r.level);
      });
  quiet_sink->set_min_level(ws::core::LogLevel::error);
  ws::core::clear_sinks();
  ws::core::add_sink(loud_sink);
  ws::core::add_sink(quiet_sink);

  ws::core::warn("heard by loud only");
  CHECK(loud.size() == 1);
  CHECK(quiet.empty());

  ws::core::critical("heard by both");
  CHECK(loud.size() == 2);
  CHECK(quiet.size() == 1);
  CHECK(quiet.front() == ws::core::LogLevel::critical);
}

TEST_CASE("the global min level filters before any sink sees the record") {
  const LogStateGuard guard;
  const Capture capture;
  ws::core::set_min_level(ws::core::LogLevel::error);
  ws::core::warn("dropped globally");
  ws::core::critical("kept");
  REQUIRE(capture.lines().size() == 1);
  CHECK(capture.lines()[0].level == ws::core::LogLevel::critical);
  CHECK(ws::core::should_log(ws::core::LogLevel::warn) == false);
  CHECK(ws::core::should_log(ws::core::LogLevel::critical) == true);
}

TEST_CASE("set_min_level(off) silences everything; off is never a record level") {
  const LogStateGuard guard;
  const Capture capture;
  ws::core::set_min_level(ws::core::LogLevel::off);
  ws::core::critical("silenced");
  CHECK(capture.lines().empty());
  CHECK(!ws::core::should_log(ws::core::LogLevel::critical));

  // A record logged AT off is a configuration mistake, not a record: dropped.
  ws::core::log(ws::core::LogLevel::off, "not a real level");
  CHECK(capture.lines().empty());
}

TEST_CASE("the flush level is stored and retrievable") {
  const LogStateGuard guard;
  CHECK(ws::core::flush_level() == ws::core::LogLevel::error);  // the default
  ws::core::set_flush_level(ws::core::LogLevel::info);
  CHECK(ws::core::flush_level() == ws::core::LogLevel::info);
}

// ---------------------------------------------------------------------------
// Patterns
// ---------------------------------------------------------------------------

TEST_CASE("the default pattern is time, level, message") {
  const LogStateGuard guard;
  CHECK(ws::core::pattern() == "[{time}] [{level}] {message}");
}

TEST_CASE("every pattern token renders, unknown tokens pass through") {
  const LogStateGuard guard;
  const Capture capture;
  ws::core::set_pattern("L={level} C={category} M={message} F={file}:{line} "
                        "T={thread} X={nope} {unterminated");
  const int here = __LINE__ + 1;
  WS_LOG_INFO("hello {}", "world");

  REQUIRE(capture.lines().size() == 1);
  const auto& line = capture.lines()[0];
  CHECK(line.text.starts_with("L=INFO  C=- M=hello world F=test_core.cpp:"));
  CHECK(line.text.find("T=") != std::string::npos);
  CHECK(line.text.find("X={nope} {unterminated") != std::string::npos);
  CHECK(line.line == here);
  CHECK(line.file == "test_core.cpp");
}

TEST_CASE("the timestamp token renders local wall clock with milliseconds") {
  const LogStateGuard guard;
  const Capture capture;
  ws::core::set_pattern("{time}");
  ws::core::info("t");
  REQUIRE(capture.lines().size() == 1);
  const std::string& text = capture.lines()[0].text;
  // "YYYY-MM-DD HH:MM:SS.mmm" - 23 characters, fixed separators.
  REQUIRE(text.size() == 23);
  CHECK(text[4] == '-');
  CHECK(text[7] == '-');
  CHECK(text[10] == ' ');
  CHECK(text[13] == ':');
  CHECK(text[16] == ':');
  CHECK(text[19] == '.');
  for (const int position :
       {0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18, 20, 21, 22}) {
    CHECK(std::isdigit(static_cast<unsigned char>(text[position])) != 0);
  }
}

TEST_CASE("the thread token renders a numeric id, distinct per thread") {
  const LogStateGuard guard;
  const Capture capture;
  ws::core::set_pattern("{thread}");
  std::string main_thread = "unset";
  ws::core::info("from main");
  REQUIRE(capture.lines().size() == 1);
  main_thread = capture.lines()[0].text;
  CHECK(!main_thread.empty());
  for (const char c : main_thread) {
    CHECK(std::isdigit(static_cast<unsigned char>(c)) != 0);
  }

  std::jthread worker([&capture] { ws::core::info("from worker"); });
  worker.join();
  REQUIRE(capture.lines().size() == 2);
  CHECK(capture.lines()[1].text != main_thread);
}

// ---------------------------------------------------------------------------
// Categories and the Logger object
// ---------------------------------------------------------------------------

TEST_CASE("a Logger stamps its category on every record") {
  const LogStateGuard guard;
  const Capture capture;
  ws::core::Logger log("net");
  CHECK(log.category() == "net");
  log.info("peer {} connected", 7);
  log.log(ws::core::LogLevel::error, "peer lost");
  REQUIRE(capture.lines().size() == 2);
  CHECK(capture.lines()[0].category == "net");
  CHECK(capture.lines()[0].message == "peer 7 connected");
  CHECK(capture.lines()[1].category == "net");
  CHECK(capture.lines()[1].message == "peer lost");
}

TEST_CASE("category rules apply hierarchically at dot boundaries only") {
  const LogStateGuard guard;
  ws::core::set_category_min_level("net", ws::core::LogLevel::warn);

  const ws::core::Logger net("net");
  const ws::core::Logger net_tcp("net.tcp");
  const ws::core::Logger network("network");
  const ws::core::Logger other("other");

  CHECK(!net.should_log(ws::core::LogLevel::info));
  CHECK(net.should_log(ws::core::LogLevel::warn));
  CHECK(!net_tcp.should_log(ws::core::LogLevel::info));   // child inherits
  CHECK(net_tcp.should_log(ws::core::LogLevel::warn));
  CHECK(network.should_log(ws::core::LogLevel::info));    // prefix, not child
  CHECK(other.should_log(ws::core::LogLevel::info));      // untouched
  CHECK(ws::core::should_log(ws::core::LogLevel::info, "net") == false);
  CHECK(ws::core::should_log(ws::core::LogLevel::info, "netchat") == true);

  // Rules are listed, clearable individually and in bulk.
  const auto rules = ws::core::category_min_levels();
  REQUIRE(rules.size() == 1);
  CHECK(rules[0].first == "net");
  CHECK(rules[0].second == ws::core::LogLevel::warn);
  CHECK(ws::core::clear_category_min_level("net"));
  CHECK(!ws::core::clear_category_min_level("net"));
  CHECK(net.should_log(ws::core::LogLevel::info));  // back to the global floor
}

TEST_CASE("the most specific category rule wins") {
  const LogStateGuard guard;
  ws::core::set_category_min_level("net", ws::core::LogLevel::warn);
  ws::core::set_category_min_level("net.db", ws::core::LogLevel::error);

  const ws::core::Logger net_io("net.io");
  const ws::core::Logger net_db("net.db");
  CHECK(net_io.should_log(ws::core::LogLevel::warn));   // the "net" rule
  CHECK(!net_io.should_log(ws::core::LogLevel::info));
  CHECK(!net_db.should_log(ws::core::LogLevel::warn));  // the "net.db" rule
  CHECK(net_db.should_log(ws::core::LogLevel::error));
}

TEST_CASE("category rules only tighten, never loosen, the global floor") {
  const LogStateGuard guard;
  ws::core::set_min_level(ws::core::LogLevel::warn);
  ws::core::set_category_min_level("ui", ws::core::LogLevel::trace);
  const ws::core::Logger ui("ui");
  CHECK(!ui.should_log(ws::core::LogLevel::info));  // the global floor wins
  CHECK(ui.should_log(ws::core::LogLevel::error));
}

TEST_CASE("filtered category records never reach a sink") {
  const LogStateGuard guard;
  const Capture capture;
  ws::core::set_category_min_level("net", ws::core::LogLevel::warn);
  const ws::core::Logger log("net.tcp");
  log.info("quiet");
  log.warn("loud");
  REQUIRE(capture.lines().size() == 1);
  CHECK(capture.lines()[0].message == "loud");
}

// ---------------------------------------------------------------------------
// File sink
// ---------------------------------------------------------------------------

TEST_CASE("the file sink appends, rotates by size and keeps full backups") {
  const LogStateGuard guard;
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "ws_core_log_test";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  const fs::path log = dir / "app.log";

  {
    ws::core::set_pattern("{message}");  // fixed-width lines: 8 chars + '\n'
    const auto sink = std::make_shared<ws::core::FileSink>(
        log.string(), false,
        ws::core::FileSink::Rotate{.max_bytes = 96, .backups = 5});
    REQUIRE(sink->is_open());
    CHECK(sink->path() == log.string());
    ws::core::clear_sinks();
    ws::core::add_sink(sink);
    for (int i = 0; i < 37; ++i) {  // 9 bytes/line, 10 lines/file, 4 files
      ws::core::info("line {:03}", i);
    }
    ws::core::clear_sinks();  // close the file before reading it back
  }

  CHECK(fs::exists(log));
  CHECK(fs::exists(dir / "app.log.1"));
  CHECK(fs::exists(dir / "app.log.2"));
  CHECK(fs::exists(dir / "app.log.3"));
  CHECK(!fs::exists(dir / "app.log.4"));  // only 3 rotations happened

  int seen = 0;
  for (const fs::path& file :
       {log, dir / "app.log.1", dir / "app.log.2", dir / "app.log.3"}) {
    std::FILE* in = std::fopen(file.string().c_str(), "rb");
    REQUIRE(in != nullptr);
    char line[64];
    while (std::fgets(line, sizeof(line), in) != nullptr) {
      ++seen;
      CHECK(std::string_view(line).starts_with("line "));
    }
    std::fclose(in);
  }
  CHECK(seen == 37);  // capacity 60 lines: nothing was dropped

  fs::remove_all(dir, ec);
}

TEST_CASE("the file sink survives an unopenable path") {
  const LogStateGuard guard;
  ws::core::FileSink sink("/definitely/not/a/real/dir/app.log");
  CHECK(!sink.is_open());
  sink.write(ws::core::LogRecord{});  // inert, not a crash
  sink.flush();
}

TEST_CASE("a file sink with truncate starts from an empty file") {
  const LogStateGuard guard;
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "ws_core_log_test_trunc";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  const fs::path log = dir / "app.log";

  {
    const auto first = std::make_shared<ws::core::FileSink>(log.string());
    REQUIRE(first->is_open());
    ws::core::clear_sinks();
    ws::core::add_sink(first);
    ws::core::info("first run");
    ws::core::clear_sinks();
  }
  {
    const auto second = std::make_shared<ws::core::FileSink>(log.string(), true);
    REQUIRE(second->is_open());
    ws::core::clear_sinks();
    ws::core::add_sink(second);
    ws::core::set_pattern("{message}");
    ws::core::info("second run");
    ws::core::clear_sinks();
  }
  std::FILE* in = std::fopen(log.string().c_str(), "rb");
  REQUIRE(in != nullptr);
  char line[64];
  int seen = 0;
  while (std::fgets(line, sizeof(line), in) != nullptr) {
    ++seen;
  }
  std::fclose(in);
  CHECK(seen == 1);  // truncated, not appended
  fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// Concurrency and reentrancy
// ---------------------------------------------------------------------------

TEST_CASE("concurrent logging through one sink is complete and consistent") {
  const LogStateGuard guard;
  constexpr int k_threads = 4;
  constexpr int k_per_thread = 250;

  auto count = std::make_shared<std::atomic<int>>(0);
  const auto sink = std::make_shared<ws::core::CallbackSink>(
      [count](const ws::core::LogRecord&) {
        count->fetch_add(1, std::memory_order_relaxed);
      });
  ws::core::clear_sinks();
  ws::core::add_sink(sink);

  std::vector<std::jthread> threads;
  for (int t = 0; t < k_threads; ++t) {
    threads.emplace_back([] {
      for (int i = 0; i < k_per_thread; ++i) {
        ws::core::info("thread line {}", i);
      }
    });
  }
  threads.clear();  // jthread destructors join

  CHECK(count->load() == k_threads * k_per_thread);
}

TEST_CASE("a sink that logs is detected and dropped, not deadlocked") {
  const LogStateGuard guard;
  // A recursive sink: it logs back from inside write(). The outer record
  // must survive, the inner call must be dropped - no deadlock, no crash.
  ws::core::clear_sinks();
  int inner_attempts = 0;
  const auto sink = std::make_shared<ws::core::CallbackSink>(
      [&](const ws::core::LogRecord& record) {
        if (record.message.find("outer") != std::string_view::npos) {
          ++inner_attempts;
          ws::core::warn("recursive inner line");  // dropped with a note
        }
      });
  ws::core::add_sink(sink);

  ws::core::info("outer line");  // must return
  CHECK(inner_attempts == 1);
}

TEST_CASE("a throwing callback loses its line but not the process") {
  const LogStateGuard guard;
  const auto throwing = std::make_shared<ws::core::CallbackSink>(
      [](const ws::core::LogRecord&) { throw std::runtime_error("boom"); });
  int received = 0;
  const auto counting =
      std::make_shared<ws::core::CallbackSink>(
          [&received](const ws::core::LogRecord&) { ++received; });
  ws::core::clear_sinks();
  ws::core::add_sink(throwing);
  ws::core::add_sink(counting);
  ws::core::info("survives a throwing sink");
  ws::core::info("and keeps going");
  CHECK(received == 2);
}

// ---------------------------------------------------------------------------
// Compile-time configuration
// ---------------------------------------------------------------------------

TEST_CASE("the compile-time floor defaults to everything active") {
  CHECK(ws::core::detail::compile_min_level() == 0);
  static_assert(WS_LOG_COMPILE_ACTIVE(ws::core::LogLevel::trace));
  static_assert(WS_LOG_COMPILE_ACTIVE(ws::core::LogLevel::critical));
}

TEST_CASE("WS_LOG macros format, capture location and respect filters") {
  const LogStateGuard guard;
  const Capture capture;
  ws::core::set_min_level(ws::core::LogLevel::trace);  // let TRACE through
  const int here = __LINE__ + 1;
  WS_LOG_TRACE("macro trace {}", 1);
  WS_LOG_DEBUG("macro debug");
  WS_LOG_INFO("macro info");
  WS_LOG_WARN("macro warn");
  WS_LOG_ERROR("macro error");
  WS_LOG_CRITICAL("macro critical");

  REQUIRE(capture.lines().size() == 6);
  CHECK(capture.lines()[0].level == ws::core::LogLevel::trace);
  CHECK(capture.lines()[0].message == "macro trace 1");
  CHECK(capture.lines()[2].level == ws::core::LogLevel::info);
  CHECK(capture.lines()[5].level == ws::core::LogLevel::critical);
  CHECK(capture.lines()[2].line == here + 2);  // third macro, two lines down
  CHECK(capture.lines()[2].file == "test_core.cpp");

  // The macros pass through the same runtime filters as the free functions.
  ws::core::set_min_level(ws::core::LogLevel::error);
  WS_LOG_INFO("compiled in, filtered out at runtime");
  CHECK(capture.lines().size() == 6);
}

TEST_CASE("log_error renders category, code, message and location") {
  const LogStateGuard guard;
  const Capture capture;
  ws::core::log_error(ws::core::make_error(5, "test", "logged error"));
  REQUIRE(capture.lines().size() == 1);
  const auto& line = capture.lines()[0];
  CHECK(line.level == ws::core::LogLevel::error);
  CHECK(line.message.find("[test] #5: logged error") != std::string::npos);
  CHECK(line.message.find("test_core.cpp") != std::string::npos);
}
