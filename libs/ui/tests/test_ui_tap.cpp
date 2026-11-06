// The tap-defect suite (the M7 leftover; docs/PLAN_UI_TAP_DEFECT.md): the
// click floor. A same-frame tap - one frame carrying both the press and
// the release edges - landing where two overlapping widgets of one layer
// meet clicks exactly ONE of them, the first hit-tested claimant; a
// strictly higher stratum may still claim above the spent floor (the
// combo's one-frame open-and-pick), and the same-frame id reuse's new
// contact (still down) claims as ever. Every case drives the real gui
// headless - the same purity class as test_ui_gui.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <ws/ui/gui/gui_context.hpp>
#include <ws/input/input.hpp>

namespace {

using ws::input::InputFrame;
using ws::input::PointerId;
using ws::ui::gui::GuiContext;
using ws::ui::gui::HitShape;
using ws::ui::gui::HitTest;
using ws::ui::gui::Layer;
using ws::ui::gui::Rect;
using ws::ui::gui::WidgetId;

// The mouse's one-frame tap: press and release edges on button 0, the
// contact already over - the gesture the event pump batches on touch and
// the one the defect rides. (The field shape mirrors test_ui_gui's
// snap(); the mouse is present because the platform said so.)
InputFrame tap(float x, float y) {
    InputFrame frame;
    frame.mouse_x = x;
    frame.mouse_y = y;
    frame.mouse_present = true;
    frame.mouse_pressed[0] = true;
    frame.mouse_released[0] = true;
    return frame;
}

// Two fingers, each a one-frame tap (down then up inside the frame). The
// mouse is absent, so the frame's pointers are exactly the two touches in
// first-contact order - f1 slot 0, f2 slot 1.
InputFrame two_finger_taps(const PointerId& f1, float x1, float y1,
                           const PointerId& f2, float x2, float y2) {
    InputFrame frame;
    frame.touch_down(f1, x1, y1);
    frame.touch_up(f1, x1, y1, false);
    frame.touch_down(f2, x2, y2);
    frame.touch_up(f2, x2, y2, false);
    return frame;
}

}  // namespace

TEST_CASE("the defect: a one-frame tap on two overlapping widgets clicks one") {
    GuiContext gui;
    const WidgetId a = gui.ids().id("under");
    const WidgetId b = gui.ids().id("over");
    const Rect ra{0.0f, 0.0f, 100.0f, 100.0f};
    const Rect rb{50.0f, 0.0f, 100.0f, 100.0f};  // overlaps A over x 50..100

    // A is hit-tested first: it claims, publishes the click, and the
    // publication raises its pointer's click floor to the stratum. B,
    // hit-tested after at the same stratum, may not claim at or below it
    // - exactly one click leaves the pair.
    gui.begin_frame(tap(75.0f, 50.0f));
    const HitTest ha = gui.hit_test(a, ra);
    const HitTest hb = gui.hit_test(b, rb);
    gui.end_frame();
    CHECK(ha.clicked);
    CHECK(!hb.clicked);
    CHECK(!hb.pressed);
    CHECK(!hb.released);
}

TEST_CASE("the first hit-tested claimant wins, whichever widget that is") {
    GuiContext gui;
    const WidgetId a = gui.ids().id("under");
    const WidgetId b = gui.ids().id("over");
    const Rect ra{0.0f, 0.0f, 100.0f, 100.0f};
    const Rect rb{50.0f, 0.0f, 100.0f, 100.0f};

    // The same scene with the call order reversed: B runs first and is
    // the claimant - call order decides within a stratum, and the answer
    // stays deterministic per app.
    gui.begin_frame(tap(75.0f, 50.0f));
    const HitTest hb = gui.hit_test(b, rb);
    const HitTest ha = gui.hit_test(a, ra);
    gui.end_frame();
    CHECK(hb.clicked);
    CHECK(!ha.clicked);
}

TEST_CASE("the cascade: a tap may publish at base and again strictly above") {
    GuiContext gui;
    const WidgetId a = gui.ids().id("base");
    const WidgetId c = gui.ids().id("popup");
    const Rect ra{0.0f, 0.0f, 100.0f, 100.0f};
    const Rect rc{50.0f, 0.0f, 100.0f, 100.0f};

    // The M19 shape: the base widget's publication raises the floor to
    // 10, and the popup-scope widget claims at 20, strictly above it, and
    // publishes too - the combo's one-frame open-and-pick stays legal.
    gui.begin_frame(tap(75.0f, 50.0f));
    const HitTest ha = gui.hit_test(a, ra);
    gui.begin_layer(Layer::popup);
    const HitTest hc = gui.hit_test(c, rc);
    gui.end_layer();
    gui.end_frame();
    CHECK(ha.clicked);
    CHECK(hc.clicked);
}

TEST_CASE("the floor is per stratum: spent at 20, the base layer claims no more") {
    GuiContext gui;
    const WidgetId a = gui.ids().id("base");
    const WidgetId c = gui.ids().id("popup");
    const WidgetId d = gui.ids().id("late-base");
    const Rect ra{0.0f, 0.0f, 100.0f, 100.0f};
    const Rect rc{50.0f, 0.0f, 100.0f, 100.0f};
    const Rect rd{25.0f, 0.0f, 100.0f, 100.0f};

    gui.begin_frame(tap(75.0f, 50.0f));
    const HitTest ha = gui.hit_test(a, ra);  // publishes: floor 10
    gui.begin_layer(Layer::popup);
    const HitTest hc = gui.hit_test(c, rc);  // publishes: floor 20
    gui.end_layer();
    const HitTest hd = gui.hit_test(d, rd);  // 10 <= 20: refused outright
    gui.end_frame();
    CHECK(ha.clicked);
    CHECK(hc.clicked);
    CHECK(!hd.clicked);
    CHECK(!hd.pressed);
}

TEST_CASE("the reuse: the new contact claims, the lift's click folds to the landing") {
    GuiContext gui;
    const WidgetId a = gui.ids().id("lift-owner");
    const WidgetId b = gui.ids().id("landing");
    const Rect ra{0.0f, 0.0f, 100.0f, 100.0f};
    const Rect rb{150.0f, 0.0f, 100.0f, 100.0f};  // the landing is off A
    const PointerId finger{1};

    InputFrame frame;
    (void)frame.touch_down(finger, 40.0f, 50.0f);  // the press, inside A
    gui.begin_frame(frame);
    const HitTest pa = gui.hit_test(a, ra);
    (void)gui.hit_test(b, rb);
    gui.end_frame();
    CHECK(pa.pressed);

    frame.next_frame();
    // The same-frame id reuse: the finger lifts off A and the platform
    // hands its id to a new contact on B before next_frame() - up then
    // down in one frame, the position the new contact's. The frame
    // snapshot folds the lift and the landing to the LANDING's verdict:
    // A ends its drag (released) with no click - and publishes nothing,
    // so no floor rises; B takes the new contact's press through its own
    // claim, view.down exempting it from the floor.
    frame.touch_up(finger, 190.0f, 50.0f, false);
    (void)frame.touch_down(finger, 190.0f, 50.0f);
    gui.begin_frame(frame);
    const HitTest la = gui.hit_test(a, ra);
    const HitTest lb = gui.hit_test(b, rb);
    gui.end_frame();
    CHECK(la.released);
    CHECK(!la.clicked);
    CHECK(lb.pressed);
    CHECK(!lb.clicked);

    frame.next_frame();  // the reuse entry survives, edges cleared
    frame.touch_up(finger, 190.0f, 50.0f, false);  // the new contact lifts
    gui.begin_frame(frame);
    const HitTest done = gui.hit_test(b, rb);
    (void)gui.hit_test(a, ra);
    gui.end_frame();
    CHECK(done.clicked);
}

TEST_CASE("the reuse's re-claimed press is stealable - the floor does not close it") {
    GuiContext gui;
    const WidgetId a = gui.ids().id("reused");
    const WidgetId b = gui.ids().id("late-comer");
    const Rect ra{0.0f, 0.0f, 100.0f, 100.0f};
    const Rect rb{50.0f, 0.0f, 100.0f, 100.0f};  // overlaps A over x 50..100
    const PointerId finger{7};

    InputFrame frame;
    (void)frame.touch_down(finger, 75.0f, 50.0f);  // the press on A, inside both
    gui.begin_frame(frame);
    const HitTest pa = gui.hit_test(a, ra);  // B is not running yet
    gui.end_frame();
    CHECK(pa.pressed);

    frame.next_frame();
    // The reuse ON A - lift and new contact both inside it. A publishes
    // the lift's click and re-claims the new contact fresh; B, a widget
    // that appeared under the finger this frame at the same stratum,
    // steals that fresh press (M11's documented contract - view.down
    // exempts the floor wholesale).
    frame.touch_up(finger, 75.0f, 50.0f, false);
    (void)frame.touch_down(finger, 75.0f, 50.0f);
    gui.begin_frame(frame);
    const HitTest ca = gui.hit_test(a, ra);
    const HitTest sb = gui.hit_test(b, rb);
    gui.end_frame();
    CHECK(ca.clicked);   // the lift's click: exactly one
    CHECK(sb.pressed);   // B owns the new contact
    CHECK(!sb.clicked);  // its click waits for its own release

    frame.next_frame();
    frame.touch_up(finger, 75.0f, 50.0f, false);
    gui.begin_frame(frame);
    const HitTest fin = gui.hit_test(b, rb);
    (void)gui.hit_test(a, ra);
    gui.end_frame();
    CHECK(fin.clicked);
}

TEST_CASE("the floor is per pointer: two fingers, two pairs, one click each") {
    GuiContext gui;
    const WidgetId a1 = gui.ids().id("p1-under");
    const WidgetId b1 = gui.ids().id("p1-over");
    const WidgetId a2 = gui.ids().id("p2-under");
    const WidgetId b2 = gui.ids().id("p2-over");
    const Rect ra1{0.0f, 0.0f, 100.0f, 100.0f};
    const Rect rb1{50.0f, 0.0f, 100.0f, 100.0f};
    const Rect ra2{300.0f, 0.0f, 100.0f, 100.0f};
    const Rect rb2{350.0f, 0.0f, 100.0f, 100.0f};
    const PointerId f1{11};
    const PointerId f2{12};

    // Crossed on purpose: finger 1 taps the SECOND pair, finger 2 the
    // first - each pair resolves on its own finger's track and floor.
    gui.begin_frame(two_finger_taps(f1, 375.0f, 50.0f, f2, 75.0f, 50.0f));
    const HitTest ha1 = gui.hit_test(a1, ra1);
    const HitTest hb1 = gui.hit_test(b1, rb1);
    const HitTest ha2 = gui.hit_test(a2, ra2);
    const HitTest hb2 = gui.hit_test(b2, rb2);
    gui.end_frame();
    CHECK(ha1.clicked);
    CHECK(ha1.pointer == f2);
    CHECK(!hb1.clicked);
    CHECK(ha2.clicked);
    CHECK(ha2.pointer == f1);
    CHECK(!hb2.clicked);
}

TEST_CASE("the floor is a this-frame rule: the next tap clicks again") {
    GuiContext gui;
    const WidgetId a = gui.ids().id("first");
    const WidgetId b = gui.ids().id("second");
    const Rect ra{0.0f, 0.0f, 100.0f, 100.0f};
    const Rect rb{50.0f, 0.0f, 100.0f, 100.0f};  // under the same point

    gui.begin_frame(tap(75.0f, 50.0f));
    const HitTest first = gui.hit_test(a, ra);
    gui.end_frame();
    CHECK(first.clicked);

    // The track carries across frames (the same pointer id), so without
    // the begin_frame reset the stale floor would refuse B here.
    gui.begin_frame(tap(75.0f, 50.0f));
    const HitTest second = gui.hit_test(b, rb);
    gui.end_frame();
    CHECK(second.clicked);
}

TEST_CASE("a shape-missed release publishes nothing and raises no floor") {
    GuiContext gui;
    const WidgetId a = gui.ids().id("shaped");
    const WidgetId b = gui.ids().id("under");
    const Rect ra{0.0f, 0.0f, 100.0f, 100.0f};
    const Rect rb{0.0f, 0.0f, 100.0f, 100.0f};

    // A claims only the square's inscribed circle: the tap at (5, 5) is
    // inside the claimable rect but outside the shape - no claim, no
    // publication, no floor. B, the plain rect over the same point, must
    // still take the tap.
    HitShape circle;
    circle.kind = HitShape::Kind::circle;
    gui.begin_frame(tap(5.0f, 5.0f));
    const HitTest ma = gui.hit_test(a, ra, circle);
    const HitTest mb = gui.hit_test(b, rb);
    gui.end_frame();
    CHECK(!ma.clicked);
    CHECK(!ma.pressed);
    CHECK(mb.clicked);
}
