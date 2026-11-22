// The idle-frame pacing policy (src/app/frame_pacer.hpp): pure data in, a
// render/skip decision out. The app loop that consumes it needs Vulkan and
// a window; the policy does not, so every rule is pinned here by hand.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "app/frame_pacer.hpp"

using ws::ui::FrameActivity;
using ws::ui::FramePacer;
using ws::ui::FramePacerConfig;

namespace {

FramePacerConfig enabled(double heartbeat = 0.1, int settle = 3) {
    FramePacerConfig c;
    c.enabled = true;
    c.heartbeat_seconds = heartbeat;
    c.settle_frames = settle;
    return c;
}

constexpr FrameActivity kIdle{};

FrameActivity only(bool FrameActivity::* const flag) {
    FrameActivity a;
    a.*flag = true;
    return a;
}

}  // namespace

TEST_CASE("pacer: disabled renders every iteration, never idle") {
    FramePacer p;  // the default config is disabled
    double t = 0.0;
    for (int i = 0; i < 100; ++i, t += 0.001) {
        CHECK(p.should_render(kIdle, t));
        CHECK_FALSE(p.idle());
    }
    CHECK(p.idle_wait(t) == 0.0);
}

TEST_CASE("pacer: the first iteration always renders") {
    FramePacer p(enabled());
    CHECK(p.should_render(kIdle, 5.0));
    CHECK_FALSE(p.idle());
}

TEST_CASE("pacer: an idle loop skips until the heartbeat is due") {
    FramePacer p(enabled(0.1, 0));
    CHECK(p.should_render(kIdle, 0.0));
    CHECK_FALSE(p.should_render(kIdle, 0.01));
    CHECK(p.idle());
    CHECK_FALSE(p.should_render(kIdle, 0.099));
    CHECK(p.should_render(kIdle, 0.1));  // due: a heartbeat frame
    CHECK_FALSE(p.idle());
    CHECK_FALSE(p.should_render(kIdle, 0.15));  // the clock restarted
    CHECK(p.should_render(kIdle, 0.2));
}

TEST_CASE("pacer: every kind of activity renders") {
    bool FrameActivity::* const flags[] = {
        &FrameActivity::events, &FrameActivity::jobs,
        &FrameActivity::input_held, &FrameActivity::app_wants,
        &FrameActivity::surface_dirty};
    for (bool FrameActivity::* const flag : flags) {
        FramePacer p(enabled(10.0, 0));  // a heartbeat far away
        CHECK(p.should_render(kIdle, 0.0));
        CHECK_FALSE(p.should_render(kIdle, 0.001));
        CHECK(p.should_render(only(flag), 0.002));
        CHECK_FALSE(p.idle());
    }
}

TEST_CASE("pacer: a burst of activity owes exactly settle_frames more") {
    FramePacer p(enabled(10.0, 3));
    double t = 0.0;
    CHECK(p.should_render(kIdle, t += 0.001));  // the first frame
    CHECK_FALSE(p.should_render(kIdle, t += 0.001));

    CHECK(p.should_render(only(&FrameActivity::events), t += 0.001));
    // The three settle frames, active or not...
    CHECK(p.should_render(kIdle, t += 0.001));
    CHECK(p.should_render(kIdle, t += 0.001));
    CHECK(p.should_render(kIdle, t += 0.001));
    // ...then idle again.
    CHECK_FALSE(p.should_render(kIdle, t += 0.001));
    CHECK(p.idle());
    CHECK_FALSE(p.should_render(kIdle, t += 0.001));
}

TEST_CASE("pacer: activity during settling restarts the count") {
    FramePacer p(enabled(10.0, 3));
    double t = 0.0;
    CHECK(p.should_render(kIdle, t += 0.001));
    CHECK(p.should_render(only(&FrameActivity::events), t += 0.001));
    CHECK(p.should_render(kIdle, t += 0.001));  // settle 3 -> 2
    CHECK(p.should_render(only(&FrameActivity::jobs), t += 0.001));  // back to 3
    CHECK(p.should_render(kIdle, t += 0.001));
    CHECK(p.should_render(kIdle, t += 0.001));
    CHECK(p.should_render(kIdle, t += 0.001));
    CHECK_FALSE(p.should_render(kIdle, t += 0.001));
}

TEST_CASE("pacer: sustained activity renders every iteration") {
    // A held button (a drag), the app's animation: no skipping at all.
    FramePacer p(enabled(10.0, 3));
    double t = 0.0;
    for (int i = 0; i < 200; ++i) {
        CHECK(p.should_render(only(&FrameActivity::input_held), t += 0.001));
    }
}

TEST_CASE("pacer: idle_wait counts down to the heartbeat, never zero") {
    FramePacer p(enabled(0.1, 0));
    CHECK(p.idle_wait(0.0) == 0.0);  // nothing rendered yet: no wait
    CHECK(p.should_render(kIdle, 1.0));
    CHECK_FALSE(p.should_render(kIdle, 1.03));
    CHECK(p.idle_wait(1.03) == doctest::Approx(0.07));
    CHECK(p.idle_wait(1.0999) == doctest::Approx(0.001));  // floor: no spin
    CHECK(p.idle_wait(1.5) == doctest::Approx(0.001));      // overdue: floor
    CHECK(p.idle_wait(1.0) == doctest::Approx(0.1));        // right after a frame
}

TEST_CASE("pacer: a second of idle at 1 kHz renders about ten frames") {
    // The point of the whole thing: ~1000 loop iterations, ~10 frames.
    FramePacer p(enabled(0.1, 0));
    int rendered = 0;
    for (int i = 0; i < 1000; ++i) {
        if (p.should_render(kIdle, 0.001 * i)) {
            ++rendered;
        }
    }
    CHECK(rendered >= 10);
    CHECK(rendered <= 11);
}

TEST_CASE("pacer: a settle frame is not a heartbeat (and vice versa)") {
    // The settle count is spent only by settle frames; a heartbeat render
    // in the middle of an idle stretch leaves it at zero.
    FramePacer p(enabled(0.1, 2));
    CHECK(p.should_render(kIdle, 0.0));
    CHECK(p.should_render(only(&FrameActivity::events), 0.01));
    CHECK(p.should_render(kIdle, 0.02));
    CHECK(p.should_render(kIdle, 0.03));
    CHECK_FALSE(p.should_render(kIdle, 0.04));  // settled
    CHECK(p.should_render(kIdle, 0.14));        // a heartbeat
    CHECK_FALSE(p.should_render(kIdle, 0.15));  // not a new burst
}
