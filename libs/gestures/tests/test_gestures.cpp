// ws::gestures - the arbiter suite. The contracts, each named after
// the failure mode it exists to prevent:
//   * THE FIRST FINGER OF A PINCH IS NEVER A CLICK (the deferred
//     press: the click verdict can only be spent at a clean lift);
//   * a pinch answers zoom steps measured from the last EMITTED frame
//     (compounding exactness), with the two-finger pan riding along;
//   * slop: a wander under the threshold stays a tap candidate, a
//     crossing arms a drag whose from-point is the crossing itself;
//   * a tap's window: a slow lift is a long-press, not a late click;
//   * an OS-cancelled contact never clicks;
//   * determinism: the same frame stream answers the same actions.
// Every frame is hand-built (the test_ui_tap precedent - the InputFrame
// is the seam, and the platform layers are its only other writers).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <ws/gestures/arbiter.hpp>

#include <cmath>

using ws::gestures::GesturePolicy;
using ws::gestures::GestureState;
using ws::gestures::TouchArbiter;
using ws::input::InputFrame;
using ws::input::PointerId;

namespace {

// A scripted finger stream: each step builds ONE frame (the shell's
// contract - one InputFrame per pump iteration) carrying the table's
// edges exactly as the platform would: a down frame, quiet move frames
// (touch_move updates position; no edge), a lift frame (released, kept
// through the frame by ws::input's table rule).
struct Script {
    InputFrame frame;  // the accumulator: down/move/up applied onto it
    bool first = true;

    // One frame where finger `id` lands at (x, y).
    InputFrame down(PointerId id, float x, float y) {
        InputFrame f = frame;
        f.touch_down(id, x, y);
        frame = f;  // the contact rides held state from now on
        first = false;
        return f;
    }
    // One frame where finger `id` moved to (x, y) - a quiet move.
    InputFrame move(PointerId id, float x, float y) {
        InputFrame f = frame;
        f.touch_move(id, x, y);
        frame = f;
        return f;
    }
    // One frame where finger `id` lifts at (x, y).
    InputFrame up(PointerId id, float x, float y,
                  bool cancelled = false) {
        InputFrame f = frame;
        f.touch_up(id, x, y, cancelled);
        frame = f;
        return f;
    }
    // One quiet frame (no changes; dt settable).
    InputFrame wait(float dt_s) {
        InputFrame f = frame;
        f.dt_seconds = dt_s;
        frame = f;
        return f;
    }
    // The next frame starts fresh edges (next_frame drops lifted
    // contacts): the script's own pump.
    InputFrame pump() {
        frame.next_frame();
        return frame;
    }
};

// The classic pinch script: two fingers land, spread apart, lift.
// `spread` answers the two zoom frames' factors total.
struct PinchRun {
    TouchArbiter arb;
    std::vector<ws::gestures::GestureActions> frames;
};

}  // namespace

TEST_CASE("a plain tap: press inside the window, lift inside the window, no move") {
    TouchArbiter arb;
    Script s;
    // Frame 1: the finger lands.
    ws::gestures::GestureActions a1 = arb.pass(s.down(PointerId{1}, 100.0f, 100.0f));
    CHECK(arb.state() == GestureState::Touching);
    CHECK(!a1.has_tap);            // the click NEVER lands at press
    CHECK(!a1.has_zoom);
    CHECK(!a1.has_drag);
    CHECK(arb.holding());          // the surface must suppress its own click reading

    // Frame 2 (a fast lift, dt tiny): the deferred click lands at LIFT.
    InputFrame lift = s.up(PointerId{1}, 100.0f, 100.0f);
    ws::gestures::GestureActions a2 = arb.pass(lift);
    CHECK(a2.has_tap);
    CHECK(a2.tap.x == 100.0f);
    CHECK(a2.tap.y == 100.0f);
    CHECK(arb.state() == GestureState::Idle);
    CHECK(!arb.holding());
}

TEST_CASE("THE FIRST FINGER OF A PINCH IS NEVER A CLICK") {
    // The exact Android failure the arbiter exists for: the first
    // finger lands (a mouse-shaped canvas would arm a tool on that
    // press - the bridge mirrors it as button 0), the second finger
    // lands, the pinch runs, both lift. NO tap may ever be emitted,
    // at any frame, for either finger.
    TouchArbiter arb;
    Script s;
    // Finger 1 lands where a button sits.
    ws::gestures::GestureActions f1 = arb.pass(s.down(PointerId{1}, 200.0f, 200.0f));
    CHECK(!f1.has_tap);  // certainly not at its press
    CHECK(arb.state() == GestureState::Touching);

    // Finger 2 lands: the gesture becomes a pinch - and the verdict
    // for finger 1 (still down, no lift) can no longer ever be a tap.
    ws::gestures::GestureActions f2 = arb.pass(s.down(PointerId{2}, 300.0f, 200.0f));
    CHECK(!f2.has_tap);
    CHECK(arb.state() == GestureState::Pinch);

    // The pinch runs (spread + drift).
    s.pump();
    ws::gestures::GestureActions f3 = arb.pass(s.move(PointerId{1}, 180.0f, 190.0f));
    s.pump();
    ws::gestures::GestureActions f4 = arb.pass(s.move(PointerId{2}, 320.0f, 210.0f));
    (void)f3;
    (void)f4;

    // Both lift (finger 1 first, finger 2 after) - no tap, ever.
    s.pump();
    ws::gestures::GestureActions l1 = arb.pass(s.up(PointerId{1}, 180.0f, 190.0f));
    CHECK(!l1.has_tap);
    s.pump();
    ws::gestures::GestureActions l2 = arb.pass(s.up(PointerId{2}, 320.0f, 210.0f));
    CHECK(!l2.has_tap);
    CHECK(!l2.has_tap);
    CHECK(arb.state() == GestureState::Idle);
}

TEST_CASE("a pinch answers compounding zoom steps with the pan riding along") {
    TouchArbiter arb;
    Script s;
    arb.pass(s.down(PointerId{1}, 100.0f, 100.0f));
    arb.pass(s.down(PointerId{2}, 200.0f, 100.0f));
    // The anchor frame: span 100 px, mid (150, 100). The first pinch
    // frame may emit nothing (it anchors); the anchor is span/mid now.

    // Frame: both fingers move out 10% and the mid drifts +5px in x.
    s.pump();
    ws::gestures::GestureActions z = arb.pass(s.move(PointerId{2}, 210.0f, 100.0f));
    // span went 100 -> 110: factor 1.1 (the ratio gate 0.02 passes).
    CHECK(z.has_zoom);
    CHECK(z.zoom.factor == doctest::Approx(110.0 / 100.0).epsilon(1e-6));
    CHECK(z.zoom.anchor_x == doctest::Approx(155.0f));  // fingers (100..210): mid 155
    CHECK(z.zoom.anchor_y == doctest::Approx(100.0f));
    // The mid drifted 150 -> 155: a two-finger pan of 5px.
    CHECK(z.has_pan);
    CHECK(z.pan.dx == doctest::Approx(5.0f));
    CHECK(z.pan.dy == doctest::Approx(0.0f));

    // Next frame: another 10% out (110 -> 121). The factor is measured
    // from the last EMITTED frame (the re-anchor), so it answers
    // 1.1 again - NOT 1.21 (the total). Compounding is the consumer's
    // job (apply each step); the arbiter must not double-count.
    s.pump();
    ws::gestures::GestureActions z2 = arb.pass(s.move(PointerId{2}, 221.0f, 100.0f));
    CHECK(z2.has_zoom);
    CHECK(z2.zoom.factor == doctest::Approx(121.0 / 110.0).epsilon(1e-6));

    // And the anchor follows the CURRENT mid (160 -> 165.5? no: the
    // fingers sit at 100 and 221 -> mid 160.5, drifted from 155).
    CHECK(z2.zoom.anchor_x == doctest::Approx(160.5f));
    CHECK(z2.has_pan);
    CHECK(z2.pan.dx == doctest::Approx(5.5f));
}

TEST_CASE("a jitter pinch (under the ratio gate) emits no zoom - but a mid drift still pans") {
    TouchArbiter arb;
    Script s;
    arb.pass(s.down(PointerId{1}, 100.0f, 100.0f));
    arb.pass(s.down(PointerId{2}, 200.0f, 100.0f));
    s.pump();
    // A 1px tremble on a 100px span: ratio 1.01 < the 0.02 gate, so no
    // zoom. The mid drifts 0.5px - under the pan's own 1e-3 dead zone is
    // false... the mid IS 0.5px moved, the dead zone is 1e-3: it pans a
    // hair. The zoom and pan are INDEPENDENT contracts now: the span
    // gate guards the zoom alone, the mid drift pans however small
    // (that independence is what makes a pure translation pan).
    ws::gestures::GestureActions j = arb.pass(s.move(PointerId{2}, 201.0f, 100.0f));
    CHECK(!j.has_zoom);
    CHECK(j.has_pan);
    CHECK(j.pan.dx == doctest::Approx(0.5f));
}

TEST_CASE("slop: a wander under the threshold stays a tap; a crossing arms a drag") {
    TouchArbiter arb;
    GesturePolicy p;  // defaults: slop 24px
    arb.policy = p;
    Script s;
    arb.pass(s.down(PointerId{7}, 50.0f, 50.0f));
    // A 10px wander (under 24): still Touching, still a tap candidate.
    s.pump();
    ws::gestures::GestureActions w = arb.pass(s.move(PointerId{7}, 60.0f, 50.0f));
    CHECK(arb.state() == GestureState::Touching);
    CHECK(!w.has_drag);
    // ... and it may still lift as a tap (the slop wander never armed).
    s.pump();
    ws::gestures::GestureActions lift = arb.pass(s.up(PointerId{7}, 60.0f, 50.0f));
    CHECK(lift.has_tap);
    CHECK(lift.tap.x == 60.0f);  // the tap lands where the finger lifted
    CHECK(arb.state() == GestureState::Idle);

    // A 40px move (past 24): Drag, and the from-point is the CROSSING.
    arb.reset();
    Script s2;
    arb.pass(s2.down(PointerId{8}, 50.0f, 50.0f));
    s2.pump();
    ws::gestures::GestureActions d = arb.pass(s2.move(PointerId{8}, 90.0f, 50.0f));
    CHECK(arb.state() == GestureState::Drag);
    CHECK(d.has_drag);
    CHECK(d.drag.pointer == PointerId{8});
    CHECK(d.drag.x == 90.0f);
    // The crossing: 24px along the 50->90 direction = (74, 50).
    CHECK(d.drag.from_x == doctest::Approx(74.0f).epsilon(1e-4));
    CHECK(d.drag.from_y == doctest::Approx(50.0f).epsilon(1e-4));
    // A drag's lift is NOT a click (the slop crossing spent the verdict).
    s2.pump();
    ws::gestures::GestureActions dl = arb.pass(s2.up(PointerId{8}, 90.0f, 50.0f));
    CHECK(!dl.has_tap);
}

TEST_CASE("the tap window: a slow stationary lift is a long-press, not a late click") {
    TouchArbiter arb;
    Script s;
    arb.pass(s.down(PointerId{1}, 100.0f, 100.0f));
    // Hold 0.4s in two 0.2s frames (the window is 0.35): the crossing
    // frame announces the long-press once, then quiet.
    s.pump();
    ws::gestures::GestureActions h1 = arb.pass(s.wait(0.2f));
    CHECK(!h1.long_press);
    CHECK(arb.state() == GestureState::Touching);
    s.pump();
    ws::gestures::GestureActions h2 = arb.pass(s.wait(0.2f));
    CHECK(h2.long_press);              // announced at the crossing...
    CHECK(arb.state() == GestureState::LongPress);
    s.pump();
    ws::gestures::GestureActions h3 = arb.pass(s.wait(0.05f));
    CHECK(!h3.long_press);             // ...exactly once
    // The late lift: NOT a tap.
    s.pump();
    ws::gestures::GestureActions late = arb.pass(s.up(PointerId{1}, 100.0f, 100.0f));
    CHECK(!late.has_tap);
    CHECK(late.cancelled == false);
    CHECK(arb.state() == GestureState::Idle);
}

TEST_CASE("an OS-cancelled contact never clicks") {
    TouchArbiter arb;
    Script s;
    arb.pass(s.down(PointerId{3}, 100.0f, 100.0f));
    s.pump();
    ws::gestures::GestureActions c = arb.pass(s.up(PointerId{3}, 100.0f, 100.0f, true));
    CHECK(!c.has_tap);
    CHECK(c.cancelled);
    CHECK(arb.state() == GestureState::Idle);
}

TEST_CASE("a pinch cancelled mid-flight never resolves into anything") {
    TouchArbiter arb;
    Script s;
    arb.pass(s.down(PointerId{1}, 100.0f, 100.0f));
    arb.pass(s.down(PointerId{2}, 200.0f, 100.0f));
    s.pump();
    arb.pass(s.move(PointerId{2}, 210.0f, 100.0f));  // a zoom frame
    s.pump();
    ws::gestures::GestureActions c = arb.pass(s.up(PointerId{2}, 210.0f, 100.0f, true));
    CHECK(!c.has_tap);
    // One finger remains: the pinch is over; the survivor may yet drag
    // (it is still live), but its drag folds from where it IS.
    CHECK(arb.state() != GestureState::Idle);
}

TEST_CASE("the one-finger pan policy: the drag becomes a pan stream") {
    TouchArbiter arb;
    arb.policy.one_finger_pan = true;
    Script s;
    arb.pass(s.down(PointerId{9}, 100.0f, 100.0f));
    s.pump();
    ws::gestures::GestureActions d = arb.pass(s.move(PointerId{9}, 140.0f, 100.0f));
    CHECK(d.has_drag);              // the drag fact stands...
    CHECK(d.has_pan);               // ...and the surface pans by the step
    CHECK(d.pan.dx == doctest::Approx(40.0f));  // press->now (the first step
                                                // pans the whole press move -
                                                // the drag arms at the slop
                                                // crossing, the pan rides along)
    // The next move answers ONLY its own step (the reference advanced).
    s.pump();
    ws::gestures::GestureActions d2 = arb.pass(s.move(PointerId{9}, 170.0f, 100.0f));
    CHECK(d2.has_pan);
    CHECK(d2.pan.dx == doctest::Approx(30.0f));
}

TEST_CASE("determinism: the same stream, twice, answers the same actions") {
    const auto run = [] {
        TouchArbiter arb;
        Script s;
        std::vector<double> zooms;
        arb.pass(s.down(PointerId{1}, 100.0f, 100.0f));
        arb.pass(s.down(PointerId{2}, 200.0f, 100.0f));
        s.pump();
        zooms.push_back(arb.pass(s.move(PointerId{2}, 210.0f, 100.0f)).zoom.factor);
        s.pump();
        zooms.push_back(arb.pass(s.move(PointerId{2}, 220.0f, 100.0f)).zoom.factor);
        s.pump();
        zooms.push_back(arb.pass(s.up(PointerId{1}, 100.0f, 100.0f)).has_tap ? -1.0 : 0.0);
        s.pump();
        zooms.push_back(arb.pass(s.up(PointerId{2}, 220.0f, 100.0f)).has_tap ? -1.0 : 0.0);
        return zooms;
    };
    const std::vector<double> a = run();
    const std::vector<double> b = run();
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        CHECK(a[i] == doctest::Approx(b[i]).epsilon(1e-12));
    }
}

TEST_CASE("a three-finger circus: the two oldest are the pinch, the third is noise") {
    TouchArbiter arb;
    Script s;
    arb.pass(s.down(PointerId{1}, 100.0f, 100.0f));
    arb.pass(s.down(PointerId{2}, 200.0f, 100.0f));
    s.pump();
    arb.pass(s.move(PointerId{2}, 210.0f, 100.0f));
    // A third finger lands mid-pinch: the pinch keeps its two oldest
    // (the zoom answer does not jump when the noise finger arrives).
    s.pump();
    const ws::gestures::GestureActions n =
        arb.pass(s.down(PointerId{3}, 400.0f, 400.0f));
    CHECK(arb.state() == GestureState::Pinch);
    CHECK(!n.has_tap);  // the noise finger is no click either
    // And the pinch still answers zoom against its OWN anchor:
    s.pump();
    ws::gestures::GestureActions z = arb.pass(s.move(PointerId{1}, 90.0f, 100.0f));
    CHECK(z.has_zoom);
    // span: fingers 1,2 at (90,100)-(210,100) -> 120 vs anchor 110:
    CHECK(z.zoom.factor == doctest::Approx(120.0 / 110.0).epsilon(1e-6));
}

TEST_CASE("reset: a mid-gesture reset forgets everything, idempotently") {
    TouchArbiter arb;
    Script s;
    arb.pass(s.down(PointerId{1}, 100.0f, 100.0f));
    arb.pass(s.down(PointerId{2}, 200.0f, 100.0f));
    REQUIRE(arb.state() == GestureState::Pinch);
    arb.reset();
    CHECK(arb.state() == GestureState::Idle);
    CHECK(arb.contact_count() == 0);
    arb.reset();  // idempotent
    // The next pass with a stale frame (the contacts still in the
    // table) treats them as fresh presses: no carry, no zoom.
    ws::gestures::GestureActions a = arb.pass(s.frame);
    CHECK(!a.has_zoom);
    CHECK(!a.has_tap);
}
