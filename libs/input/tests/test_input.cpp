#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <cstdint>

#include <ws/input/event.hpp>
#include <ws/input/input.hpp>

// The input family's own suite, moved with it from test_ui.cpp when the
// family promoted to libs/input: the event types + the InputFrame
// snapshot, all headless - no window, no GPU, no platform TU. The
// gui-side adoption cases (GuiContext::begin_frame copying the snapshot)
// stayed with the ui suite.

// --- event system ---------------------------------------------------------

TEST_CASE("every event carries its own type and starts unhandled") {
    using ws::input::EventType;
    CHECK(ws::input::WindowCloseEvent{}.type() == EventType::WindowClose);
    CHECK(ws::input::WindowCloseEvent{}.static_type() == EventType::WindowClose);
    CHECK(ws::input::WindowResizeEvent{640, 480}.type() == EventType::WindowResize);
    CHECK(ws::input::KeyPressedEvent{'D', false}.type() == EventType::KeyPressed);
    CHECK(ws::input::KeyReleasedEvent{'D'}.type() == EventType::KeyReleased);
    CHECK(ws::input::MouseButtonPressedEvent{0}.type() == EventType::MouseButtonPressed);
    CHECK(ws::input::MouseButtonReleasedEvent{1}.type() == EventType::MouseButtonReleased);
    CHECK(ws::input::MouseMovedEvent{1.0f, 2.0f}.type() == EventType::MouseMoved);
    CHECK(ws::input::MouseScrolledEvent{0.0f, -1.0f}.type() == EventType::MouseScrolled);
    CHECK(ws::input::TouchPressedEvent{7, 1.0f, 2.0f}.type() ==
          EventType::TouchPressed);
    CHECK(ws::input::TouchMovedEvent{7, 1.0f, 2.0f}.type() == EventType::TouchMoved);
    CHECK(ws::input::TouchReleasedEvent{7, 1.0f, 2.0f, true}.type() ==
          EventType::TouchReleased);
    CHECK(ws::input::TextInputEvent{U'x'}.type() == EventType::TextInput);

    ws::input::WindowResizeEvent resize{800, 600};
    CHECK(resize.handled == false);
    CHECK(resize.width == 800);
    CHECK(resize.height == 600);
    ws::input::KeyPressedEvent key{'D', true};
    CHECK(key.keycode == 'D');
    CHECK(key.repeat == true);
    ws::input::TouchReleasedEvent touch{7, 1.0f, 2.0f, true};
    CHECK(touch.pointer_id == 7);
    CHECK(touch.x == 1.0f);
    CHECK(touch.y == 2.0f);
    CHECK(touch.cancelled == true);
    ws::input::TextInputEvent glyph{U'w'};
    CHECK(glyph.codepoint == U'w');
}

TEST_CASE("dispatcher fires only on the matching type and honors the return") {
    ws::input::WindowResizeEvent event{800, 600};
    ws::input::EventDispatcher dispatcher{event};

    bool called = false;
    CHECK(dispatcher.dispatch<ws::input::KeyPressedEvent>(
              [&](const ws::input::KeyPressedEvent&) {
                  called = true;
                  return true;
              }) == false);
    CHECK(called == false);  // wrong type: handler must not fire

    CHECK(dispatcher.dispatch<ws::input::WindowResizeEvent>(
              [&](const ws::input::WindowResizeEvent& e) {
                  called = true;
                  CHECK(e.width == 800);
                  return false;  // declines: event stays unhandled
              }) == false);
    CHECK(called);
    CHECK(event.handled == false);

    CHECK(dispatcher.dispatch<ws::input::WindowResizeEvent>(
              [](const ws::input::WindowResizeEvent&) { return true; }) == true);
    CHECK(event.handled == true);
}

TEST_CASE("dispatcher skips events the shell pre-marked handled") {
    ws::input::MouseMovedEvent event{3.0f, 4.0f};
    event.handled = true;  // e.g. ImGui captured the mouse over a widget
    ws::input::EventDispatcher dispatcher{event};

    bool called = false;
    CHECK(dispatcher.dispatch<ws::input::MouseMovedEvent>(
              [&](const ws::input::MouseMovedEvent&) {
                  called = true;
                  return true;
              }) == false);
    CHECK(called == false);
}

// --- InputFrame: one frame's snapshot ---------------------------------------

TEST_CASE("InputFrame::next_frame clears edges and keeps held state") {
    using namespace ws::input;
    InputFrame frame;
    frame.mouse_x = 100.0f;
    frame.mouse_y = 50.0f;
    frame.mouse_present = true;
    frame.mouse_down[0] = true;
    frame.mouse_pressed[0] = true;
    frame.mouse_released[1] = true;
    frame.scroll_dx = 1.5f;
    frame.scroll_dy = -2.5f;
    frame.key_down['D'] = true;
    frame.key_pressed['D'] = true;
    frame.text_input.push_back(U'w');

    frame.next_frame();

    // Held state and position survive the frame boundary... presence is
    // held state too - the platform re-establishes it per event, and a
    // frame boundary must not flicker the mouse out of the pointer list.
    CHECK(frame.mouse_x == 100.0f);
    CHECK(frame.mouse_y == 50.0f);
    CHECK(frame.mouse_present == true);
    CHECK(frame.mouse_down[0] == true);
    CHECK(frame.key_down['D'] == true);
    // ...the per-frame edges do not.
    CHECK(frame.mouse_pressed[0] == false);
    CHECK(frame.mouse_released[1] == false);
    CHECK(frame.scroll_dx == 0.0f);
    CHECK(frame.scroll_dy == 0.0f);
    CHECK(frame.key_pressed['D'] == false);
    CHECK(frame.text_input.empty());
}

TEST_CASE("InputFrame: the frame clock survives next_frame - the shell's write is the refresh") {
    using namespace ws::input;
    InputFrame frame;
    CHECK(frame.dt_seconds == 0.0f);  // a hand-built frame starts at 0

    frame.dt_seconds = 1.0f / 120.0f;
    frame.next_frame();
    // next_frame clears EDGES; the clock is the SHELL's to refresh (its
    // next write, once per pump) - a frame boundary between writes must
    // not zero it.
    CHECK(frame.dt_seconds == doctest::Approx(1.0f / 120.0f));
    // (The gui half of this case - GuiContext::begin_frame adopting dt
    // whole - stayed with the ui suite: the public-surface case there.)
}

TEST_CASE("frame_input is drop-safe without a running app") {
    // Headless: the shell never installed an accumulator, so the public
    // getter answers an empty snapshot instead of dangling.
    const ws::input::InputFrame& frame = ws::input::frame_input();
    CHECK(frame.mouse_x == 0.0f);
    CHECK(frame.mouse_y == 0.0f);
    CHECK(frame.mouse_down[0] == false);
    CHECK(frame.mouse_pressed[0] == false);
    CHECK(frame.key_pressed['D'] == false);
    CHECK(frame.scroll_dy == 0.0f);
    CHECK(frame.dt_seconds == 0.0f);  // no shell ran: the clock reads 0
    CHECK(frame.text_input.empty());
    CHECK(frame.touch_count == 0);
    CHECK(frame.bridged_touch() == ws::input::kNoPointer);
    // An empty frame never saw a mouse: presence defaults false, so the
    // pointer list is EMPTY - the touch-only shape. The mouse joins only
    // when the platform says one exists.
    std::array<ws::input::PointerView,
               ws::input::InputFrame::kMaxPointers>
        pointers{};
    CHECK(frame.gather_pointers(pointers) == 0);
    CHECK(pointers[0].id == ws::input::kNoPointer);
    CHECK(!frame.pointer_pos(ws::input::kMousePointerId, nullptr, nullptr));
}

// --- InputFrame touch table (multi-touch groundwork) -------------------------

TEST_CASE("touch table: down/move/up track a contact through its frames") {
    using namespace ws::input;
    InputFrame frame;

    // Contact 17 lands: entry 0, held with a press edge.
    CHECK(frame.touch_down(PointerId{17}, 30.0f, 40.0f) == 0);
    CHECK(frame.touch_count == 1);
    CHECK(frame.touches[0].id == PointerId{17});
    CHECK(frame.touches[0].down);
    CHECK(frame.touches[0].pressed);
    CHECK(!frame.touches[0].released);
    CHECK(frame.touches[0].x == 30.0f);
    CHECK(frame.touches[0].y == 40.0f);

    // ...moves: position only, no new edges (the press edge from the down
    // above still belongs to this frame - edges clear at next_frame()).
    frame.touch_move(PointerId{17}, 55.0f, 65.0f);
    CHECK(frame.touches[0].x == 55.0f);
    CHECK(frame.touches[0].y == 65.0f);
    CHECK(frame.touches[0].down);
    CHECK(!frame.touches[0].released);

    // ...lifts: release edge, but the entry stays for this frame - a widget
    // must still be able to see who lifted.
    frame.touch_up(PointerId{17}, 60.0f, 70.0f, false);
    CHECK(frame.touch_count == 1);
    CHECK(!frame.touches[0].down);
    CHECK(frame.touches[0].released);
    CHECK(!frame.touches[0].cancelled);
    CHECK(frame.touches[0].x == 60.0f);

    // ...and leaves at the frame boundary.
    frame.next_frame();
    CHECK(frame.touch_count == 0);
}

TEST_CASE("touch table: insertion order survives releases and frame edges") {
    using namespace ws::input;
    InputFrame frame;
    frame.touch_down(PointerId{7}, 0.0f, 0.0f);
    frame.touch_down(PointerId{3}, 1.0f, 1.0f);
    frame.touch_down(PointerId{9}, 2.0f, 2.0f);
    CHECK(frame.touch_count == 3);
    CHECK(frame.touches[1].id == PointerId{3});

    // The middle contact lifts; the frame boundary compacts the table
    // without reordering the survivors.
    frame.touch_up(PointerId{3}, 1.0f, 1.0f, false);
    frame.next_frame();
    CHECK(frame.touch_count == 2);
    CHECK(frame.touches[0].id == PointerId{7});
    CHECK(frame.touches[1].id == PointerId{9});
    CHECK(!frame.touches[0].pressed);  // edges cleared on the survivors
    CHECK(!frame.touches[1].pressed);

    // A new contact appends after them.
    CHECK(frame.touch_down(PointerId{5}, 3.0f, 3.0f) == 2);
    CHECK(frame.touches[2].id == PointerId{5});
}

TEST_CASE("touch table: a repeated down for the same id updates in place") {
    using namespace ws::input;
    InputFrame frame;
    CHECK(frame.touch_down(PointerId{7}, 10.0f, 10.0f) == 0);
    // One down per contact is the platform contract; a stray repeat must
    // not duplicate the entry.
    CHECK(frame.touch_down(PointerId{7}, 20.0f, 20.0f) == 0);
    CHECK(frame.touch_count == 1);
    CHECK(frame.touches[0].x == 20.0f);
    CHECK(frame.touches[0].pressed);
}

TEST_CASE("touch table: a same-frame id reuse keeps the lift's release edge") {
    using namespace ws::input;
    InputFrame frame;
    CHECK(frame.touch_down(PointerId{4}, 10.0f, 10.0f) == 0);
    frame.touch_up(PointerId{4}, 12.0f, 12.0f, false);
    // The platform hands id 4 to a new contact before next_frame() -
    // Android recycles low ids fast. The lift's edge must survive the
    // arrival: the slot reads pressed+released with down, up-then-down,
    // the flags the gui needs to report the lift while the new contact
    // claims fresh. (The defect: the arrival erased the release.)
    CHECK(frame.touch_down(PointerId{4}, 30.0f, 30.0f) == 0);
    CHECK(frame.touch_count == 1);
    CHECK(frame.touches[0].id == PointerId{4});
    CHECK(frame.touches[0].down);
    CHECK(frame.touches[0].pressed);
    CHECK(frame.touches[0].released);
    CHECK(frame.touches[0].x == 30.0f);  // position is the new contact's:
    CHECK(frame.touches[0].y == 30.0f);  // last-write, like the mouse's

    // The bridge mirrors the reuse order-exactly: the lift mirrored its
    // release, the new contact re-bridges and mirrors a fresh press - and
    // never cleared the mirrored release (the mouse's own rule, which the
    // touch side now matches).
    CHECK(frame.bridged_touch() == PointerId{4});
    CHECK(frame.mouse_down[0]);
    CHECK(frame.mouse_pressed[0]);
    CHECK(frame.mouse_released[0]);

    // The frame boundary clears every edge; the survivor stays.
    frame.next_frame();
    CHECK(frame.touch_count == 1);
    CHECK(frame.touches[0].down);
    CHECK(!frame.touches[0].pressed);
    CHECK(!frame.touches[0].released);
}

TEST_CASE("touch table: a recycled slot carries no stale release into a new contact") {
    using namespace ws::input;
    InputFrame frame;
    frame.touch_down(PointerId{1}, 0.0f, 0.0f);
    frame.touch_down(PointerId{2}, 1.0f, 1.0f);
    frame.touch_down(PointerId{3}, 2.0f, 2.0f);
    // 1 and 2 lift (2 cancelled); 3 survives. The boundary compacts 3
    // into slot 0 and leaves 2's released+cancelled bytes sitting in
    // slot 1, above the count - the next arrival is created straight on
    // top of them, and must not inherit either edge.
    frame.touch_up(PointerId{1}, 0.0f, 0.0f, false);
    frame.touch_up(PointerId{2}, 1.0f, 1.0f, true);
    frame.next_frame();
    CHECK(frame.touch_count == 1);

    CHECK(frame.touch_down(PointerId{9}, 5.0f, 5.0f) == 1);
    CHECK(frame.touches[1].id == PointerId{9});
    CHECK(frame.touches[1].down);
    CHECK(frame.touches[1].pressed);
    CHECK(!frame.touches[1].released);   // slot 1 held 2's released bytes
    CHECK(!frame.touches[1].cancelled);  // ...and its cancelled bytes
}

TEST_CASE("primary-touch bridge: the first contact mirrors the mouse") {
    using namespace ws::input;
    InputFrame frame;

    // The first contact of the gesture is the primary: it mirrors the mouse
    // so single-finger use and ImGui keep working unchanged.
    frame.touch_down(PointerId{7}, 10.0f, 20.0f);
    CHECK(frame.bridged_touch() == PointerId{7});
    CHECK(frame.mouse_x == 10.0f);
    CHECK(frame.mouse_y == 20.0f);
    CHECK(frame.mouse_down[0]);
    CHECK(frame.mouse_pressed[0]);

    // A second finger joins: tracked, never mirrored.
    frame.touch_down(PointerId{9}, 80.0f, 90.0f);
    CHECK(frame.bridged_touch() == PointerId{7});
    CHECK(frame.mouse_x == 10.0f);

    // Each finger's moves mirror only its own.
    frame.touch_move(PointerId{9}, 85.0f, 95.0f);
    CHECK(frame.mouse_x == 10.0f);
    frame.touch_move(PointerId{7}, 15.0f, 25.0f);
    CHECK(frame.mouse_x == 15.0f);
    CHECK(frame.mouse_y == 25.0f);

    // The secondary lifting changes nothing for the mouse.
    frame.touch_up(PointerId{9}, 85.0f, 95.0f, false);
    CHECK(frame.mouse_down[0]);
    CHECK(!frame.mouse_released[0]);
    CHECK(frame.bridged_touch() == PointerId{7});

    // The primary lifting releases the mouse.
    frame.touch_up(PointerId{7}, 15.0f, 25.0f, false);
    CHECK(!frame.mouse_down[0]);
    CHECK(frame.mouse_released[0]);
    CHECK(frame.bridged_touch() == kNoPointer);
}

TEST_CASE("bridge: no promotion - a freed mouse stays free until a new gesture") {
    using namespace ws::input;
    InputFrame frame;
    frame.touch_down(PointerId{7}, 10.0f, 20.0f);
    frame.touch_down(PointerId{9}, 30.0f, 40.0f);

    // The primary lifts while the secondary holds.
    frame.touch_up(PointerId{7}, 10.0f, 20.0f, false);
    CHECK(frame.bridged_touch() == kNoPointer);
    CHECK(!frame.mouse_down[0]);

    // The surviving finger neither owns nor moves the mouse.
    frame.touch_move(PointerId{9}, 35.0f, 45.0f);
    CHECK(frame.mouse_x == 10.0f);
    CHECK(frame.mouse_released[0]);  // still released - no re-press

    // The NEXT contact starts a fresh gesture and bridges.
    frame.touch_down(PointerId{4}, 60.0f, 70.0f);
    CHECK(frame.bridged_touch() == PointerId{4});
    CHECK(frame.mouse_down[0]);
    CHECK(frame.mouse_pressed[0]);
    CHECK(frame.mouse_x == 60.0f);
}

TEST_CASE("cancelled contact: release edge with cancelled set, bridge lifts") {
    using namespace ws::input;
    InputFrame frame;
    frame.touch_down(PointerId{2}, 5.0f, 6.0f);
    frame.touch_up(PointerId{2}, 5.0f, 6.0f, true);

    CHECK(frame.touches[0].cancelled);
    CHECK(frame.touches[0].released);
    CHECK(!frame.touches[0].down);
    // A cancelled gesture is "never happened": the mouse reads as released,
    // and the bridge frees.
    CHECK(!frame.mouse_down[0]);
    CHECK(frame.mouse_released[0]);
    CHECK(frame.bridged_touch() == kNoPointer);
}

TEST_CASE("touch cap: the ninth contact drops, the table stays intact") {
    using namespace ws::input;
    InputFrame frame;
    for (std::uint32_t id = 1; id <= static_cast<std::uint32_t>(
                                     InputFrame::kMaxTouches);
         ++id) {
        CHECK(frame.touch_down(PointerId{id}, static_cast<float>(id), 0.0f) >=
              0);
    }
    CHECK(frame.touch_count == InputFrame::kMaxTouches);
    CHECK(frame.bridged_touch() == PointerId{1});  // the first contact bridges

    // The 9th finger: rejected with no side effects anywhere.
    CHECK(frame.touch_down(PointerId{9}, 99.0f, 99.0f) == -1);
    CHECK(frame.touch_count == InputFrame::kMaxTouches);
    CHECK(frame.bridged_touch() == PointerId{1});
    CHECK(frame.mouse_x == 1.0f);       // the dropped contact moved nothing
    CHECK(!frame.mouse_released[0]);   // ...and lifted nothing
    CHECK(frame.touches[InputFrame::kMaxTouches - 1].id ==
          PointerId{InputFrame::kMaxTouches});
}

TEST_CASE("gather_pointers: mouse is slot 0, touches follow in table order") {
    using namespace ws::input;
    InputFrame frame;
    frame.touch_down(PointerId{7}, 40.0f, 50.0f);
    frame.touch_down(PointerId{9}, 60.0f, 70.0f);
    frame.touch_up(PointerId{9}, 65.0f, 75.0f, false);  // 9 is in its release frame
    // The platform's bridge would have overwritten the mirrored mouse with
    // the touch; writing the mouse fields directly is the desktop shape.
    // Either way slot 0 reports whatever the mouse state says - and the
    // desktop shape means a present mouse.
    frame.mouse_x = 12.0f;
    frame.mouse_y = 34.0f;
    frame.mouse_present = true;
    frame.mouse_down[0] = true;
    frame.mouse_pressed[0] = true;

    std::array<PointerView, InputFrame::kMaxPointers> pointers{};
    const int count = frame.gather_pointers(pointers);
    CHECK(count == 3);
    CHECK(pointers[0].id == kMousePointerId);
    CHECK(pointers[0].x == 12.0f);
    CHECK(pointers[0].y == 34.0f);
    CHECK(pointers[0].down);
    CHECK(pointers[0].pressed);
    CHECK(pointers[1].id == PointerId{7});
    CHECK(pointers[1].x == 40.0f);
    CHECK(pointers[1].down);
    CHECK(pointers[2].id == PointerId{9});
    CHECK(!pointers[2].down);
    CHECK(pointers[2].released);
    CHECK(pointers[2].x == 65.0f);
    CHECK(pointers[3].id == kNoPointer);  // past the count: the sentinel

    // pointer_pos reaches the mouse and each touch - including one in its
    // release frame - and misses ids the frame never held.
    float x = 0.0f;
    float y = 0.0f;
    CHECK(frame.pointer_pos(kMousePointerId, &x, &y));
    CHECK(x == 12.0f);
    CHECK(y == 34.0f);
    CHECK(frame.pointer_pos(PointerId{7}, &x, &y));
    CHECK(x == 40.0f);
    CHECK(y == 50.0f);
    CHECK(frame.pointer_pos(PointerId{9}, &x, &y));
    CHECK(x == 65.0f);
    CHECK(y == 75.0f);
    CHECK(frame.pointer_pos(PointerId{7}, nullptr, nullptr));  // pure existence check
    CHECK(!frame.pointer_pos(PointerId{1234}, &x, &y));
}

TEST_CASE("gather_pointers: an absent mouse is not a pointer - touches only") {
    using namespace ws::input;
    // The touch-only shape: the bridge mirrors the finger into the raw
    // mouse fields, but presence is false, so the mouse claims nothing.
    // Whatever mouse_x/y hold - a finger's position, or the (0,0) default
    // - the pointer list is exactly the touches.
    InputFrame frame;
    frame.touch_down(PointerId{7}, 40.0f, 50.0f);
    CHECK(frame.mouse_x == 40.0f);  // the bridge mirrored the finger...
    CHECK(!frame.mouse_present);    // ...but the platform never saw a mouse

    std::array<PointerView, InputFrame::kMaxPointers> pointers{};
    CHECK(frame.gather_pointers(pointers) == 1);
    CHECK(pointers[0].id == PointerId{7});  // slot 0 is the TOUCH
    CHECK(pointers[0].x == 40.0f);
    CHECK(pointers[0].pressed);
    CHECK(pointers[1].id == kNoPointer);
    float x = 0.0f;
    float y = 0.0f;
    CHECK(!frame.pointer_pos(kMousePointerId, &x, &y));
    CHECK(!frame.pointer_pos(kMousePointerId, nullptr, nullptr));
    CHECK(frame.pointer_pos(PointerId{7}, &x, &y));
    CHECK(x == 40.0f);
    CHECK(y == 50.0f);

    // The mouse joins the list the moment the platform reports it - the
    // Bluetooth-mouse-plugged-in-while-touching shape: the raw fields
    // still mirror the finger (either-writer), and now they count.
    frame.mouse_present = true;
    CHECK(frame.gather_pointers(pointers) == 2);
    CHECK(pointers[0].id == kMousePointerId);
    CHECK(pointers[0].x == 40.0f);  // the finger's mirror, as written
    CHECK(pointers[1].id == PointerId{7});
    CHECK(frame.pointer_pos(kMousePointerId, &x, &y));
    CHECK(x == 40.0f);
}
