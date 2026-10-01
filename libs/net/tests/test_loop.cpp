// The public EventLoop over the real libuv backend: run/stop, the
// thread-safe post, timers, and teardown with live state. Needs an OS
// event loop but no sockets.
//
// Rule for these tests (and for user code): callbacks only set flags -
// never doctest assertions inside a loop callback. A throw from inside a
// libuv callback terminates the process; the no-throw callback contract
// is the loop's, and a failing REQUIRE would crash the suite instead of
// failing the case.

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

#include <ws/net/errors.hpp>
#include <ws/net/event_loop.hpp>

namespace {

using std::chrono::milliseconds;
using ws::net::EventLoop;
using ws::net::NetError;

std::int32_t code_of(const ws::core::Status& status) {
  return status.error().code;
}

}  // namespace

TEST_CASE("posted work runs FIFO on the loop, then run() drains") {
  EventLoop loop;
  std::atomic<int> ran{0};
  REQUIRE(loop.post([&] { ran += 1; }).has_value());
  REQUIRE(loop.post([&] { ran += 10; }).has_value());
  REQUIRE(loop.run().has_value());
  CHECK(ran == 11);
  CHECK(loop.run().has_value());  // idle loop: a clean no-op run
}

TEST_CASE("a post made from inside a post runs on the same run()") {
  // The wake async never holds the loop open: without run() flushing the
  // chain again, the inner post would strand on an otherwise-idle loop
  // until some LATER run() happened to pick it up.
  EventLoop loop;
  std::atomic<int> order{0};
  std::atomic<int> inner_code{0};  // 0 = the inner post was accepted
  REQUIRE(loop.post([&] {
    order += 1;
    if (const auto s = loop.post([&] { order += 10; }); !s.has_value()) {
      inner_code = s.error().code;  // no asserts inside loop callbacks
    }
  }).has_value());
  REQUIRE(loop.run().has_value());
  CHECK(inner_code == 0);
  CHECK(order == 11);
}

TEST_CASE("run_once also flushes a post chain that posts again") {
  EventLoop loop;
  std::atomic<int> order{0};
  REQUIRE(loop.post([&] {
    order += 1;
    (void)loop.post([&] { order += 10; });  // outcome checked by `order`
  }).has_value());
  REQUIRE(loop.run_once().has_value());
  CHECK(order == 11);
}

TEST_CASE("post from another thread while run() is in flight") {
  EventLoop loop;
  std::atomic<int> from_other{0};
  std::atomic<int> post_failed{0};  // no asserts inside the helper thread
  // A 500ms timer holds the loop open long enough for the helper thread
  // to land its post; the posted work then stops the loop.
  REQUIRE(loop.start_timer(500, [] {}).has_value());
  std::thread helper([&] {
    std::this_thread::sleep_for(milliseconds(10));
    if (!loop.post([&] {
          from_other = 1;
          loop.stop();
        }).has_value()) {
      post_failed = 1;
    }
  });
  const auto run_result = loop.run();
  helper.join();
  REQUIRE(run_result.has_value());
  CHECK(post_failed == 0);
  CHECK(from_other == 1);
}

TEST_CASE("stop() from another thread ends run()") {
  EventLoop loop;
  std::atomic<int> fired{0};
  REQUIRE(loop.start_timer(500, [&] { fired = 1; }).has_value());
  std::thread helper([&] {
    std::this_thread::sleep_for(milliseconds(10));
    loop.stop();
  });
  const auto run_result = loop.run();
  helper.join();
  REQUIRE(run_result.has_value());
  CHECK(fired == 0);  // stopped well before the timer could fire
}

TEST_CASE("timers fire in order and run() returns when work drains") {
  EventLoop loop;
  std::vector<int> order;  // loop thread == test thread: no sync needed
  REQUIRE(loop.start_timer(10, [&] { order.push_back(1); }).has_value());
  REQUIRE(loop.start_timer(50, [&] { order.push_back(2); }).has_value());
  REQUIRE(loop.run().has_value());  // returns by itself: no work left
  CHECK(order.size() == 2);
  CHECK(order[0] == 1);
  CHECK(order[1] == 2);
}

TEST_CASE("cancel_timer prevents the fire and releases the loop") {
  EventLoop loop;
  std::atomic<int> kept{0}, cancelled{0};
  const auto doomed = loop.start_timer(300, [&] { cancelled = 1; });
  REQUIRE(doomed.has_value());
  REQUIRE(loop.start_timer(10, [&] { kept = 1; }).has_value());
  REQUIRE(loop.cancel_timer(doomed.value()).has_value());
  REQUIRE(loop.run().has_value());  // the cancelled timer no longer holds it
  CHECK(kept == 1);
  CHECK(cancelled == 0);
}

TEST_CASE("cancel misuse: fired, unknown and null handles; null callbacks") {
  EventLoop loop;
  std::atomic<int> fired{0};
  const auto t = loop.start_timer(5, [&] { fired = 1; });
  REQUIRE(t.has_value());
  REQUIRE(loop.run().has_value());  // the timer fired; the loop drained
  CHECK(fired == 1);
  CHECK(code_of(loop.cancel_timer(t.value())) ==
        static_cast<std::int32_t>(NetError::invalid_handle));
  CHECK(code_of(loop.cancel_timer(ws::net::TimerHandle{999})) ==
        static_cast<std::int32_t>(NetError::invalid_handle));
  CHECK(code_of(loop.cancel_timer(ws::net::TimerHandle{})) ==
        static_cast<std::int32_t>(NetError::invalid_argument));
  CHECK(code_of(loop.post(nullptr)) ==
        static_cast<std::int32_t>(NetError::invalid_argument));
  CHECK(!loop.start_timer(10, nullptr).has_value());
}

TEST_CASE("run() is not re-entrant from inside a callback") {
  EventLoop loop;
  std::atomic<std::int32_t> inner_code{0};
  const auto t = loop.start_timer(5, [&] {
    if (const auto s = loop.run(); !s.has_value()) {
      inner_code = s.error().code;
    }
  });
  REQUIRE(t.has_value());
  REQUIRE(loop.run().has_value());
  CHECK(inner_code == static_cast<std::int32_t>(NetError::loop_stopped));
}

TEST_CASE("run_once turns the loop until an event lands") {
  EventLoop loop;
  std::atomic<int> fired{0};
  REQUIRE(loop.start_timer(5, [&] { fired = 1; }).has_value());
  REQUIRE(loop.run_once().has_value());
  CHECK(fired == 1);
  REQUIRE(loop.run().has_value());  // drained: returns immediately
}

TEST_CASE("repeating is re-arming from the callback") {
  EventLoop loop;
  std::atomic<int> count{0};
  std::function<void()> arm;
  arm = [&] {
    count += 1;
    if (count < 3) {
      (void)loop.start_timer(1, arm);  // outcome checked by `count` below
    } else {
      loop.stop();
    }
  };
  REQUIRE(loop.start_timer(1, arm).has_value());
  REQUIRE(loop.run().has_value());
  CHECK(count == 3);
}

TEST_CASE("teardown with live timers and queued posts is safe") {
  {  // pending timer + queued post, never run
    EventLoop loop;
    REQUIRE(loop.start_timer(60000, [] {}).has_value());
    REQUIRE(loop.post([] {}).has_value());
  }
  {  // one turn taken, long timer still pending at destruction
    EventLoop loop;
    std::atomic<int> never{0};
    // run_once blocks until an event completes: the 1ms timer is the
    // event it waits for (a lone 60000ms timer would sleep the suite
    // for a minute), while the long one stays pending for the dtor.
    REQUIRE(loop.start_timer(1, [] {}).has_value());
    REQUIRE(loop.start_timer(60000, [&] { never = 1; }).has_value());
    REQUIRE(loop.run_once().has_value());
    CHECK(never == 0);
  }
  {  // a loop that never ran at all
    EventLoop loop;
    REQUIRE(loop.start_timer(1000, [] {}).has_value());
  }
}
