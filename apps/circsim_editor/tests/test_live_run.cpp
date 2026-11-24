// apps/circsim_editor - the live edit loop's policy gate.
//
// Editor.hpp's LiveRun core driven with a scripted clock - the struct's
// whole design is that the app FEEDS now_s and answers busy, so a test
// is just a stricter feeder. The cases walk the loop's contract:
//
//   * off: edits never arm, ticks never fire (the batch act stays).
//   * the debounce window: an edit arms a countdown; ticks inside the
//     window do not fire; the first tick at/after the deadline does,
//     exactly once.
//   * burst collapse: a second edit pushes the deadline out, so a
//     burst of commits joins one run - never one run per keystroke.
//   * busy coalesce: an edit while a run is in flight arms `pending`
//     instead of a countdown; ticks stay quiet while it rides.
//   * delivery re-queue: delivered() answers true exactly once for a
//     pending, and false forever after (no chain: a run with no new
//     edit never re-runs).
//   * the race: a countdown that expires while a run is in flight
//     converts to pending instead of firing (one worker, never two).
//   * typing settle: a focused text field pauses the countdown (the
//     field's own commit re-arms from zero when it lands).
//   * the kill switches: off() clears everything (no straggler run),
//     reset() additionally rewinds the clock state (a document switch).
//   * enable-mid-burst: arming auto ON with a countdown already
//     running does not fire it retroactively; only the next edit arms.
//   * settle_s == 0: the next tick fires (frame-rate collapse).
//
// The integration half: a real SchematicDocument + the app's exact
// listener wiring (push -> edited(busy)) and a real Editor's
// document_changed() reset - proving the policy rides the undo
// contract the app wires, not a parallel edit notification.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "../src/editor.hpp"
#include "../src/document.hpp"

#include <cstdint>

#include <ws/settings/filesystem.hpp>

namespace {

using editor::Editor;
using editor::SchematicDocument;

// The scripted feeder: `advance` moves the clock and answers the
// frame's questions exactly the way CircsimApp::on_update does.
struct Clock {
    editor::Editor::LiveRun& live;
    bool busy = false;
    int fired = 0;  // submit_run() calls the tick site makes

    explicit Clock(editor::Editor::LiveRun& l) : live(l) {}

    void advance(double dt) {
        live.now_s += dt;
        if (live.tick(busy)) {
            ++fired;
        }
    }
};

struct PolicyFixture {
    editor::Editor ed;
    Clock clock{ed.live};

    PolicyFixture() { ed.live.enabled = true; }
};

}  // namespace

TEST_CASE("auto off: edits never arm and ticks never fire") {
    Editor ed;
    CHECK_FALSE(ed.live.enabled);
    ed.live.edited(false);
    CHECK_FALSE(ed.live.dirty);
    CHECK_FALSE(ed.live.pending);
    ed.live.now_s = 10.0;
    CHECK_FALSE(ed.live.tick(false));
    CHECK_FALSE(ed.live.delivered());
}

TEST_CASE("an edit arms a countdown that fires once, at the deadline") {
    PolicyFixture f;
    f.ed.live.now_s = 1.0;
    f.ed.live.edited(false);
    CHECK(f.ed.live.dirty);

    // Inside the window: quiet, whatever the frame rate.
    f.clock.advance(0.25);
    CHECK(f.ed.live.dirty);
    f.clock.advance(0.20);  // 0.45 < 0.5
    CHECK(f.clock.fired == 0);

    // At/past the deadline: fires exactly once...
    f.clock.advance(0.06);  // 0.51 >= 0.5
    CHECK(f.clock.fired == 1);
    CHECK_FALSE(f.ed.live.dirty);

    // ...and the next frame is quiet (the fire consumed the edit).
    f.clock.advance(0.5);
    CHECK(f.clock.fired == 1);
    CHECK_FALSE(f.ed.live.delivered());
}

TEST_CASE("a burst collapses into one run: each edit pushes the deadline") {
    PolicyFixture f;
    f.ed.live.now_s = 0.0;
    f.ed.live.edited(false);  // deadline 0.5

    // Typing cadence: an edit every 0.2s never lets the window close.
    for (int i = 0; i < 10; ++i) {
        f.clock.advance(0.2);
        f.ed.live.edited(false);  // pushes the deadline out again
    }
    CHECK(f.clock.fired == 0);
    CHECK(f.ed.live.dirty);

    // The burst ends; the LAST edit's window is the one that fires.
    f.clock.advance(0.5);
    CHECK(f.clock.fired == 1);
}

TEST_CASE("an edit while a run is in flight arms pending, not a countdown") {
    PolicyFixture f;
    f.ed.live.now_s = 0.0;
    f.ed.live.edited(true /* busy */);
    CHECK(f.ed.live.pending);
    CHECK_FALSE(f.ed.live.dirty);

    // No deadline rides a pending: time passing must not fire anything.
    f.clock.busy = true;
    f.clock.advance(10.0);
    CHECK(f.clock.fired == 0);
    CHECK(f.ed.live.pending);

    // The delivery answers true exactly once...
    CHECK(f.ed.live.delivered());
    CHECK_FALSE(f.ed.live.pending);
    // ...and never chains: a second delivery with no new edit is quiet.
    CHECK_FALSE(f.ed.live.delivered());
}

TEST_CASE("a countdown that expires mid-flight converts to pending, not a fire") {
    PolicyFixture f;
    f.ed.live.now_s = 0.0;
    f.ed.live.edited(false);  // deadline 0.5

    // The run starts before the window closes; it is still in flight
    // when the deadline passes.
    f.clock.busy = true;
    f.clock.advance(0.6);
    CHECK(f.clock.fired == 0);       // never two workers
    CHECK_FALSE(f.ed.live.dirty);    // the countdown was consumed...
    CHECK(f.ed.live.pending);        // ...into the one queued re-run

    CHECK(f.ed.live.delivered());  // the re-submit the app owes
}

TEST_CASE("a failed run still re-runs: delivered() carries no opinion") {
    PolicyFixture f;
    f.ed.live.edited(true);
    // (The app's deliver_run treats error and success identically for
    // the re-queue: the next edit is often exactly the fix. The struct
    // reflects that by answering delivered() with nothing about the
    // run's outcome in hand.)
    CHECK(f.ed.live.delivered());
}

TEST_CASE("a focused text field settles the countdown; its commit re-arms") {
    PolicyFixture f;
    f.ed.live.now_s = 0.0;
    f.ed.live.edited(false);

    // Typing is in flight: the field takes focus mid-countdown.
    f.clock.advance(0.3);
    f.ed.live.settle();
    CHECK_FALSE(f.ed.live.dirty);

    // The window the field paused through does not fire anything...
    f.clock.advance(2.0);
    CHECK(f.clock.fired == 0);

    // ...and the field's own commit (an undo push like any other edit)
    // re-arms from zero.
    f.ed.live.edited(false);
    f.clock.advance(0.5);
    CHECK(f.clock.fired == 1);
}

TEST_CASE("turning auto off mid-burst kills the straggler run") {
    PolicyFixture f;
    f.ed.live.now_s = 0.0;
    f.ed.live.edited(false);
    f.ed.live.edited(true);  // and one edit landed in flight

    f.ed.live.off();
    CHECK_FALSE(f.ed.live.dirty);
    CHECK_FALSE(f.ed.live.pending);
    f.clock.advance(10.0);
    CHECK(f.clock.fired == 0);
    CHECK_FALSE(f.ed.live.delivered());  // no straggler re-run either
}

TEST_CASE("arming auto on does not fire a countdown that predates it") {
    Editor ed;  // starts disabled
    ed.live.now_s = 0.0;
    ed.live.edited(false);  // auto is off: a no-op
    CHECK_FALSE(ed.live.dirty);

    ed.live.enabled = true;  // the toggle lands (the app calls off() too)
    ed.live.off();
    ed.live.now_s = 5.0;
    CHECK_FALSE(ed.live.tick(false));  // nothing was armed
}

TEST_CASE("document_changed resets the loop; enabled survives it") {
    PolicyFixture f;
    f.ed.live.edited(false);
    f.ed.live.edited(true);
    CHECK(f.ed.live.pending);

    f.ed.document_changed();
    CHECK_FALSE(f.ed.live.dirty);
    CHECK_FALSE(f.ed.live.pending);
    CHECK(f.ed.live.enabled);  // a view preference, not document state
    CHECK(f.ed.live.now_s == 0.0);
}

TEST_CASE("settle_s = 0 collapses to the next frame") {
    PolicyFixture f;
    f.ed.live.settle_s = 0.0;
    f.ed.live.now_s = 7.0;
    f.ed.live.edited(false);
    f.clock.advance(0.001);  // any frame after the edit
    CHECK(f.clock.fired == 1);
}

// --- the integration half: the real document, the real listener ------
//
// The app's exact wiring: an undo-stack listener whose body answers
// edited(run_job_.busy()). The busy answer is the app's run_job_; a
// test stands in with a bool the "job site" owns.

TEST_CASE("a real edit command arms the loop through the undo listener") {
    ws::settings::StdFileSystem fs;
    SchematicDocument doc{std::string{}, fs};
    Editor ed;
    ed.doc = &doc;
    ed.live.enabled = true;

    bool busy = false;
    const auto listener = doc.undo().add_listener([&ed, &busy] {
        ed.live.edited(busy);
    });
    REQUIRE(listener.has_value());

    // A real edit: placing a part is one push, one listener fire, one
    // arming - exactly the act a user's hand performs.
    ws::circsim::Part part;
    part.symbol = "r";
    part.ref = "R1";
    part.pos = {0.0, 0.0};
    editor::push_place_part(doc, part);

    CHECK(ed.live.dirty);
    CHECK(ed.live.pending == false);

    // The countdown the frame loop would fire.
    ed.live.now_s = 1.0;
    CHECK(ed.live.tick(false));
    CHECK_FALSE(ed.live.dirty);

    // An edit while the (stand-in) run is in flight coalesces.
    busy = true;
    ws::circsim::Part second;
    second.symbol = "c";
    second.ref = "C1";
    second.pos = {100.0, 0.0};
    editor::push_place_part(doc, second);
    CHECK(ed.live.pending);
    CHECK_FALSE(ed.live.dirty);

    // The delivery re-queues once, never chains. (The stand-in run is
    // now idle - `busy` dies with the delivery the way the app's
    // run_job_.busy() answer does.)
    CHECK(ed.live.delivered());
    CHECK_FALSE(ed.live.delivered());
    busy = false;

    // Undo is an edit too (the run that lands after it must reflect
    // the undone drawing) - the SAME listener answers for it, and an
    // idle stack arms the countdown, not a pending.
    (void)doc.undo().undo();
    CHECK(ed.live.dirty);
    CHECK_FALSE(ed.live.pending);
}

TEST_CASE("document_changed resets a document's loop state (the switch)") {
    ws::settings::StdFileSystem fs;
    SchematicDocument doc{std::string{}, fs};
    Editor ed;
    ed.doc = &doc;
    ed.live.enabled = true;

    const auto listener = doc.undo().add_listener([&ed] {
        ed.live.edited(false);
    });
    REQUIRE(listener.has_value());

    ws::circsim::Part part;
    part.symbol = "r";
    part.ref = "R1";
    part.pos = {0.0, 0.0};
    editor::push_place_part(doc, part);
    CHECK(ed.live.dirty);

    // The app's watch_active_document re-points the editor at the new
    // document and calls document_changed(); the armed countdown for
    // the OLD one must die with it.
    ed.document_changed();
    CHECK_FALSE(ed.live.dirty);

    (void)doc.undo().remove_listener(*listener);
}
