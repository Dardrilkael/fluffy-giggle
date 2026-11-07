// apps/circsim_editor - the side panels' headless gate. The Properties
// panel's editing state is the story: the analysis list's remove act
// (nothing selected must remove nothing), the directives block's
// split/join round-trip (a row cleared to empty must survive a
// reload), the field rows' "##" identity (a param labeled "value" is
// its own widget, not the built-in value row), and the Editor-level
// contracts the app's run delivery rides (refresh_problems' snapshot
// version, document_changed's selection reset). The Problems panel's
// walk (Prev/Next, the F8 pair, the severity filter) is driven the
// same way: the pure core, the act that jumps through the row-click
// path, and the buttons' wiring. The Palette's draw
// seam gets its first assertions here too: the tile grid records
// through palette_draw_canvas the way the plot and the schematic
// canvas record through theirs.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "../src/editor.hpp"
#include "../src/side_panels.hpp"

#include <array>
#include <string>
#include <utility>
#include <vector>

#include <ws/settings/filesystem.hpp>
#include <ws/ui/canvas/canvas2d.hpp>
#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/layout_cursor.hpp>
#include <ws/ui/gui/theme.hpp>
#include <ws/ui/renderer2d.hpp>

#include "ui_gui_stub.hpp"

namespace {

namespace gui = ws::ui::gui;
namespace canvas = ws::ui::canvas;
using editor::Editor;
using editor::SchematicDocument;

// A .csym layer with the collision case the field-identity tests need:
// a Text param whose DISPLAY label is "Value" - the same label the
// Properties panel's built-in value row wears (capitalized since the
// patch).
constexpr const char* kCollideCsym = R"json({
    "format": "circsim-symbols",
    "version": 1,
    "symbols": [
        {
            "key": "widget",
            "name": "Widget",
            "spice": "Q",
            "ref": "W",
            "pins": [],
            "value": true,
            "params": [
                {"key": "temp", "type": "text", "label": "Value",
                 "default": ""}
            ],
            "draw": []
        }
    ]
})json";

// The bench: an Editor over an untitled document, one drawn frame per
// call, and a mouse parked off-panel (no button, no scroll) so the
// widgets register and lay out without a pointer claim.
struct Bench {
    ws::settings::StdFileSystem fs;
    SchematicDocument doc{std::string{}, fs};
    Editor ed;
    gui::GuiContext gui;
    canvas::CommandListCanvas clc;

    Bench() {
        ed.doc = &doc;
    }

    // Draws the Properties panel once. The post-draw hook runs between
    // the panel body and end_frame - the window the focus-navigation
    // tests need (a Tab queued there resolves against this frame's
    // focusable registrations).
    template <typename Hook>
    void draw_properties(Hook&& after_body) {
        gui.begin_frame(ws::input::InputFrame{});
        gui::LayoutCursor layout;
        (void)layout.begin_panel(gui::Rect{0.0f, 0.0f, 360.0f, 640.0f},
                                 12.0f, 8.0f);
        const gui::ChromeTheme theme = gui::kDefaultChrome;
        editor::draw_properties_panel(ed, gui, layout, theme);
        after_body();
        layout.end_panel();
        gui.end_frame();
    }

    void draw_properties() {
        draw_properties([] {});
    }

    // Draws the Palette panel once (the seam test's surface).
    void draw_palette() {
        draw_palette(gui::kDefaultChrome);
    }

    // The frame-fed spelling (the keyboard cases): the stock chrome
    // with the frame's pointer/keys, and the post-draw hook between
    // the body and end_frame - the focus window the Enter/Esc cases
    // need.
    template <typename Hook>
    void draw_palette(const ws::input::InputFrame& frame, Hook&& after_body) {
        gui.begin_frame(frame);
        gui::LayoutCursor layout;
        (void)layout.begin_panel(gui::Rect{0.0f, 0.0f, 200.0f, 640.0f},
                                 12.0f, 8.0f);
        editor::draw_palette_panel(ed, gui, layout, gui::kDefaultChrome);
        after_body();
        layout.end_panel();
        gui.end_frame();
    }

    // Arms the palette's first match for `filter` the way the search
    // box's Enter does: focus the box (one frame), set the filter,
    // press Enter.
    void arm_first(const char* filter) {
        draw_palette(ws::input::InputFrame{},
                     [&] { gui.set_focus(gui.ids().id("Search symbols")); });
        std::snprintf(ed.palette_filter, sizeof ed.palette_filter, "%s",
                      filter);
        ws::input::InputFrame enter;
        enter.key_pressed[static_cast<std::size_t>(ws::input::keys::kEnter)] =
            true;
        draw_palette(enter, [] {});
    }

    // The themed spelling: the typography cases pin the heading's face
    // through the r2d stub the whole panel draws past.
    void draw_palette(const gui::ChromeTheme& theme) {
        gui.begin_frame(ws::input::InputFrame{});
        gui::LayoutCursor layout;
        (void)layout.begin_panel(gui::Rect{0.0f, 0.0f, 200.0f, 640.0f},
                                 12.0f, 8.0f);
        editor::draw_palette_panel(ed, gui, layout, theme);
        layout.end_panel();
        gui.end_frame();
    }

    // Draws the Problems panel once (the stock fontless chrome - the
    // mono cases install ed.font_mono themselves). The optional frame
    // feeds the pointer/keys; the post-draw hook runs between the
    // panel body and end_frame (the focus cases' window).
    template <typename Hook>
    void draw_problems(const ws::input::InputFrame& frame, Hook&& after_body) {
        gui.begin_frame(frame);
        gui::LayoutCursor layout;
        (void)layout.begin_panel(gui::Rect{0.0f, 0.0f, 480.0f, 240.0f},
                                 12.0f, 8.0f);
        const gui::ChromeTheme theme = gui::kDefaultChrome;
        editor::draw_problems_panel(ed, gui, layout, theme);
        after_body();
        layout.end_panel();
        gui.end_frame();
    }

    void draw_problems() {
        draw_problems(ws::input::InputFrame{}, [] {});
    }

    // The frame-less hook spelling (the properties rig's own pair): a
    // parked-mouse frame with the post-draw hook between the body and
    // end_frame.
    template <typename Hook>
    void draw_problems(Hook&& after_body) {
        draw_problems(ws::input::InputFrame{}, std::forward<Hook>(after_body));
    }

    // A full click at (x, y): the press frame, then the release
    // frame - the two-frame click the widgets want (the gui suite's
    // own snap shape). `draw` receives the frame to feed.
    template <typename Draw>
    void click_at(Draw&& draw, float x, float y) {
        ws::input::InputFrame press;
        press.mouse_present = true;
        press.mouse_x = x;
        press.mouse_y = y;
        press.mouse_down[0] = true;
        press.mouse_pressed[0] = true;
        draw(press);
        ws::input::InputFrame release;
        release.mouse_present = true;
        release.mouse_x = x;
        release.mouse_y = y;
        release.mouse_released[0] = true;
        draw(release);
    }

    void click_problems(float x, float y) {
        click_at(
            [this](const ws::input::InputFrame& frame) {
                draw_problems(frame, [] {});
            },
            x, y);
    }

    // Draws the Netlist panel once in its own rect (the Properties
    // panel's Netlist tab draws the same body under its own scope).
    template <typename Hook>
    void draw_netlist(const ws::input::InputFrame& frame, Hook&& after_body) {
        gui.begin_frame(frame);
        gui::LayoutCursor layout;
        (void)layout.begin_panel(gui::Rect{0.0f, 0.0f, 480.0f, 240.0f},
                                 12.0f, 8.0f);
        const gui::ChromeTheme theme = gui::kDefaultChrome;
        editor::draw_netlist_panel(ed, gui, layout, theme);
        after_body();
        layout.end_panel();
        gui.end_frame();
    }

    void draw_netlist() {
        draw_netlist(ws::input::InputFrame{}, [] {});
    }

    void click_netlist(float x, float y) {
        click_at(
            [this](const ws::input::InputFrame& frame) {
                draw_netlist(frame, [] {});
            },
            x, y);
    }

    // A selected part of the given symbol key, placed straight into
    // the model (the tests that need the undo machinery use the
    // push_* API instead).
    std::uint32_t select_part(const char* symbol_key) {
        ws::circsim::Part p =
            ws::circsim::Part::make(symbol_key, "W1", "1", {0.0, 0.0});
        p.id = doc.schem.alloc_id();
        doc.schem.parts_mut().push_back(p);
        ed.sel.kind = editor::Selection::Kind::Part;
        ed.sel.id = p.id;
        return p.id;
    }

    // One placed part under `ref` (NOT selected - the jump tests want
    // the selection to move) plus the diags pointing at it, delivered
    // as the panel's own check: the walk's landing spot and its list.
    std::uint32_t place_with_findings(const char* ref,
                                      std::vector<ws::circsim::Diag> diags) {
        ws::circsim::Part p =
            ws::circsim::Part::make("res", ref, "1k", {0.0, 0.0});
        p.id = doc.schem.alloc_id();
        doc.schem.parts_mut().push_back(p);
        for (ws::circsim::Diag& d : diags) {
            d.where = ref;
        }
        ed.refresh_problems(false, std::move(diags), "erc",
                            doc.schem.version());
        return p.id;
    }
};

}  // namespace

// --- the directives split/join (the round-trip that keeps empty lines) ------

TEST_CASE("the directives split keeps a trailing empty line") {
    using editor::split_directive_lines;
    // SEPARATOR semantics: the lines BETWEEN the newlines.
    CHECK(split_directive_lines("") ==
          std::vector<std::string>{""});
    CHECK(split_directive_lines("a") ==
          std::vector<std::string>{"a"});
    CHECK(split_directive_lines("a\n") ==
          std::vector<std::string>{"a", ""});
    CHECK(split_directive_lines("a\n\n") ==
          std::vector<std::string>{"a", "", ""});
    CHECK(split_directive_lines("a\nb") ==
          std::vector<std::string>{"a", "b"});
    CHECK(split_directive_lines("\n") ==
          std::vector<std::string>{"", ""});
    CHECK(split_directive_lines("\na") ==
          std::vector<std::string>{"", "a"});
    CHECK(split_directive_lines("a\n\nb") ==
          std::vector<std::string>{"a", "", "b"});
}

TEST_CASE("the directives join keeps a final empty line's newline") {
    using editor::join_directive_lines;
    CHECK(join_directive_lines({""}) == "");
    CHECK(join_directive_lines({"a"}) == "a");
    CHECK(join_directive_lines({"a", ""}) == "a\n");
    CHECK(join_directive_lines({"a", "", ""}) == "a\n\n");
    CHECK(join_directive_lines({"a", "b"}) == "a\nb");
    CHECK(join_directive_lines({"", ""}) == "\n");
    CHECK(join_directive_lines({"a", "", "b"}) == "a\n\nb");
}

TEST_CASE("the directives round-trip preserves every row") {
    using editor::join_directive_lines;
    using editor::split_directive_lines;
    const std::vector<std::vector<std::string>> cases = {
        {""},
        {"a"},
        {"a", ""},   // the regression: the cleared-to-empty last row
        {"a", "", ""},
        {"", ""},
        {"a", "b"},
        {"a", "", "b"},
        {".model d dn", ".include library.cir", ""},
    };
    for (const std::vector<std::string>& lines : cases) {
        CHECK(split_directive_lines(join_directive_lines(lines)) == lines);
    }
    // And from the text side: every text is its own split's join.
    const std::vector<std::string> texts = {
        "", "a", "a\n", "a\n\n", "a\nb", "\n", "\na", "a\n\nb",
    };
    for (const std::string& text : texts) {
        CHECK(join_directive_lines(split_directive_lines(text)) == text);
    }
}

TEST_CASE("a directives block with a trailing newline lays out two rows") {
    Bench b;
    b.doc.schem.directives = ".model d dn\n";
    b.ed.properties_tab = 2;  // Simulation tab
    b.draw_properties();
    // The field rows the panel minted: directives.0 AND directives.1 -
    // the old split dropped the empty second row, so a cleared line
    // came back as nothing on reload.
    CHECK(b.ed.fields.count("directives.0") == 1u);
    CHECK(b.ed.fields.count("directives.1") == 1u);
    // And the row it loaded was the empty one.
    CHECK(std::string{b.ed.fields.at("directives.1").buf.data()} == "");
}

// --- the analysis list's remove act ------------------------------------------

TEST_CASE("removing with no analysis selected removes nothing") {
    Bench b;
    editor::push_add_analysis(b.doc, ws::circsim::AcSpec{});
    editor::push_add_analysis(b.doc, ws::circsim::DcSpec{});
    REQUIRE(b.doc.schem.analysis.entries.size() == 2u);
    const std::size_t undo_depth = b.doc.undo().undo_count();

    b.ed.analysis_index = -1;  // nothing selected
    CHECK(editor::remove_selected_analysis(b.ed) == false);
    CHECK(b.doc.schem.analysis.entries.size() == 2u);
    CHECK(b.doc.undo().undo_count() == undo_depth);  // no edit landed
    CHECK(b.ed.analysis_index == -1);

    // A stale index past a shrunken list is as selectionless as -1:
    // the old clamp would have deleted the LAST entry here too.
    b.ed.analysis_index = 5;
    CHECK(editor::remove_selected_analysis(b.ed) == false);
    CHECK(b.doc.schem.analysis.entries.size() == 2u);
    CHECK(b.doc.undo().undo_count() == undo_depth);
}

TEST_CASE("removing a selected analysis lands the edit and drops the selection") {
    Bench b;
    editor::push_add_analysis(b.doc, ws::circsim::AcSpec{});
    editor::push_add_analysis(b.doc, ws::circsim::DcSpec{});
    REQUIRE(b.doc.schem.analysis.entries.size() == 2u);

    b.ed.analysis_index = 1;  // the .dc
    CHECK(editor::remove_selected_analysis(b.ed) == true);
    CHECK(b.doc.schem.analysis.entries.size() == 1u);
    CHECK(b.doc.undo().can_undo() == true);
    CHECK(b.ed.analysis_index == -1);
    CHECK(b.ed.field_stamp == 2u);  // the epoch bumped: fields reload
}

// --- document_changed's selection reset ---------------------------------------

TEST_CASE("a document switch resets the analysis selection") {
    Bench b;
    editor::push_add_analysis(b.doc, ws::circsim::AcSpec{});
    editor::push_add_analysis(b.doc, ws::circsim::DcSpec{});
    b.ed.analysis_index = 1;
    // The switch: the editor re-points at (here) nothing.
    b.ed.doc = nullptr;
    b.ed.document_changed();
    CHECK(b.ed.analysis_index == -1);
}

// --- refresh_problems' snapshot version ----------------------------------------

TEST_CASE("refresh_problems keys to the version it is handed, not the live one") {
    Bench b;
    const std::uint64_t submitted = b.doc.schem.version();
    b.doc.schem.bump();
    b.doc.schem.bump();  // two edits landed "while the job ran"
    REQUIRE(b.doc.schem.version() == submitted + 2u);

    // The delivered-run shape: the version the job carried at submit.
    b.ed.refresh_problems(true, {}, "run", submitted);
    CHECK(b.ed.problems.has == true);
    CHECK(b.ed.problems.from_run == true);
    CHECK(b.ed.problems.version == submitted);  // honestly stale
    CHECK(b.ed.problems.header == std::string("run - 0 errors, 0 warnings"));

    // The synchronous ERC shape: the live version.
    b.ed.run_erc();
    CHECK(b.ed.problems.from_run == false);
    CHECK(b.ed.problems.version == b.doc.schem.version());
}

// --- the problems walk (Prev/Next and the F8 pair) -----------------------------

namespace {

// One diag, severity and where the case wants (the message is
// constant - the walk never reads it).
ws::circsim::Diag diag_of(ws::circsim::Severity severity, const char* where) {
    ws::circsim::Diag d;
    d.severity = severity;
    d.code = "test.diag";
    d.message = "a finding";
    d.where = where;
    return d;
}

}  // namespace

TEST_CASE("the walk's pure core wraps at the ends and skips refusals") {
    using editor::step_problem_index;
    const auto all = [](std::size_t) { return true; };
    const auto odds = [](std::size_t i) { return i % 2 == 1; };
    const auto none = [](std::size_t) { return false; };

    // Nothing under the cursor yet: forward starts at the first,
    // backward at the last.
    CHECK(step_problem_index(-1, 4, true, all) == 0);
    CHECK(step_problem_index(-1, 4, false, all) == 3);
    // Plain steps, both ways.
    CHECK(step_problem_index(1, 4, true, all) == 2);
    CHECK(step_problem_index(2, 4, false, all) == 1);
    // The ends cycle.
    CHECK(step_problem_index(3, 4, true, all) == 0);
    CHECK(step_problem_index(0, 4, false, all) == 3);
    // The lone row cycles to itself.
    CHECK(step_problem_index(0, 1, true, all) == 0);
    // The walk skips indices the predicate refuses.
    CHECK(step_problem_index(-1, 4, true, odds) == 1);
    CHECK(step_problem_index(1, 4, true, odds) == 3);
    CHECK(step_problem_index(3, 4, true, odds) == 1);  // wrapped, 0 skipped
    CHECK(step_problem_index(2, 4, false, odds) == 1);
    // Nothing passes / nothing to walk: -1, never a garbage index.
    CHECK(step_problem_index(-1, 4, true, none) == -1);
    CHECK(step_problem_index(1, 4, false, none) == -1);
    CHECK(step_problem_index(-1, 0, true, all) == -1);
    // A stale cursor past a shrunken list degrades to the wrap math.
    CHECK(step_problem_index(9, 3, true, all) == 0);
    CHECK(step_problem_index(9, 3, false, all) == 2);
}

TEST_CASE("the severity filter's predicate answers per severity") {
    using editor::problems_filter_shows;
    using ws::circsim::Severity;
    CHECK(problems_filter_shows(Severity::Error, false, true, true) == false);
    CHECK(problems_filter_shows(Severity::Error, true, false, false) == true);
    CHECK(problems_filter_shows(Severity::Warning, true, false, true) ==
          false);
    CHECK(problems_filter_shows(Severity::Warning, false, true, false) ==
          true);
    CHECK(problems_filter_shows(Severity::Note, true, true, false) == false);
    CHECK(problems_filter_shows(Severity::Note, false, false, true) == true);
}

TEST_CASE("the walk's enablement follows the filter, not the raw count") {
    Bench b;
    // No check yet: not stepable, whatever the toggles say.
    CHECK(editor::problems_any_visible(b.ed) == false);

    (void)b.place_with_findings(
        "R1", {diag_of(ws::circsim::Severity::Error, "R1"),
               diag_of(ws::circsim::Severity::Warning, "R1")});
    CHECK(editor::problems_any_visible(b.ed) == true);

    // Hide the errors: the warning still stands.
    b.ed.problems_show_error = false;
    CHECK(editor::problems_any_visible(b.ed) == true);

    // Hide everything: the buttons' dead face and the F8 commands'
    // grey answer the same no.
    b.ed.problems_show_warning = false;
    CHECK(editor::problems_any_visible(b.ed) == false);

    // An empty check is as stepable as none.
    b.ed.refresh_problems(false, {}, "erc", b.doc.schem.version());
    CHECK(editor::problems_any_visible(b.ed) == false);
}

TEST_CASE("step_problem lands the cursor and jumps through the row-click path") {
    Bench b;
    const std::uint32_t id = b.place_with_findings(
        "R1", {diag_of(ws::circsim::Severity::Error, "R1"),
               diag_of(ws::circsim::Severity::Warning, "R1"),
               diag_of(ws::circsim::Severity::Note, "R1")});
    REQUIRE(b.ed.problems.has);
    REQUIRE(b.ed.problems_cursor == -1);  // the delivery reset it

    // Nothing under the cursor: forward starts at the first finding -
    // and the jump rode the row-click path (the selection moved).
    CHECK(editor::step_problem(b.ed, true) == true);
    CHECK(b.ed.problems_cursor == 0);
    CHECK(b.ed.sel.is(editor::Selection::Kind::Part, id));
    CHECK(editor::step_problem(b.ed, true) == true);
    CHECK(b.ed.problems_cursor == 1);
    CHECK(editor::step_problem(b.ed, false) == true);
    CHECK(b.ed.problems_cursor == 0);
    // The ends cycle.
    CHECK(editor::step_problem(b.ed, false) == true);
    CHECK(b.ed.problems_cursor == 2);
    CHECK(editor::step_problem(b.ed, true) == true);
    CHECK(b.ed.problems_cursor == 0);  // wrapped forward

    // The filter hides everything: nowhere to land.
    b.ed.problems_show_error = false;
    b.ed.problems_show_warning = false;
    b.ed.problems_show_note = false;
    b.ed.problems_cursor = -1;
    CHECK(editor::step_problem(b.ed, true) == false);
    CHECK(b.ed.problems_cursor == -1);
    // And it skips what it hides: errors hidden, the walk steps
    // warning -> note, never landing on the error at 0.
    b.ed.problems_show_warning = true;
    b.ed.problems_show_note = true;
    CHECK(editor::step_problem(b.ed, true) == true);
    CHECK(b.ed.problems_cursor == 1);
    CHECK(editor::step_problem(b.ed, true) == true);
    CHECK(b.ed.problems_cursor == 2);

    // No check yet (a fresh document's load report is the document's,
    // not the cursor's): the walk refuses.
    Bench fresh;
    CHECK(editor::step_problem(fresh.ed, true) == false);
}

TEST_CASE("a replaced check resets the problems cursor") {
    Bench b;
    b.ed.problems_cursor = 3;
    b.ed.refresh_problems(false, {}, "erc", b.doc.schem.version());
    CHECK(b.ed.problems_cursor == -1);
    // And the document switch's own reset.
    b.ed.problems_cursor = 1;
    b.ed.doc = nullptr;
    b.ed.document_changed();
    CHECK(b.ed.problems_cursor == -1);
}

TEST_CASE("the Prev and Next buttons ride the same walk") {
    Bench b;
    (void)b.place_with_findings(
        "R1", {diag_of(ws::circsim::Severity::Error, "R1"),
               diag_of(ws::circsim::Severity::Warning, "R1")});

    // The button row: [Re-check 120] [Prev 56] [Next 56] at the
    // panel's 12 px pad, 8 px gaps, 28 px control height - Next at
    // x 204..260, Prev at x 140..196.
    b.click_problems(232.0f, 26.0f);  // Next
    CHECK(b.ed.problems_cursor == 0);
    b.click_problems(168.0f, 26.0f);  // Prev
    CHECK(b.ed.problems_cursor == 1);  // wrapped back to the last
    b.click_problems(232.0f, 26.0f);  // Next again
    CHECK(b.ed.problems_cursor == 0);  // wrapped forward

    // Dead without a check: the pair never fires.
    Bench fresh;
    fresh.click_problems(232.0f, 26.0f);
    fresh.click_problems(168.0f, 26.0f);
    CHECK(fresh.ed.problems_cursor == -1);
}

// --- the severity filter's row ---------------------------------------------------

TEST_CASE("the filter's checkboxes flip their toggles; Show all restores") {
    Bench b;
    (void)b.place_with_findings(
        "R1", {diag_of(ws::circsim::Severity::Error, "R1"),
               diag_of(ws::circsim::Severity::Warning, "R1"),
               diag_of(ws::circsim::Severity::Note, "R1")});

    // The keyboard's click: focus the checkbox by its "###" spelling
    // (the count in the display relabels with the findings; the
    // identity does not), then Enter.
    const auto focus = [&](const char* label) {
        b.draw_problems(
            [&] { b.gui.set_focus(b.gui.ids().id(label)); });
    };
    const auto press_enter = [&]() {
        ws::input::InputFrame enter;
        enter.key_pressed[static_cast<std::size_t>(
            ws::input::keys::kEnter)] = true;
        b.draw_problems(enter, [] {});
    };

    focus("Errors 1###problems.error");
    press_enter();
    CHECK(b.ed.problems_show_error == false);
    CHECK(b.ed.problems_show_warning == true);
    CHECK(b.ed.problems_show_note == true);

    focus("Warnings 1###problems.warning");
    press_enter();
    CHECK(b.ed.problems_show_warning == false);

    focus("Notes 1###problems.note");
    press_enter();
    CHECK(b.ed.problems_show_note == false);

    // Everything hidden now: the one-click way back.
    focus("Show all");
    press_enter();
    CHECK(b.ed.problems_show_error == true);
    CHECK(b.ed.problems_show_warning == true);
    CHECK(b.ed.problems_show_note == true);
}

TEST_CASE("a hidden row never renumbers the rows that stay") {
    Bench b;
    const std::uint32_t id = b.place_with_findings(
        "R1", {diag_of(ws::circsim::Severity::Error, "R1"),
               diag_of(ws::circsim::Severity::Warning, "R1"),
               diag_of(ws::circsim::Severity::Note, "R1")});
    // Hide the error: the rows that draw are the warning's and the
    // note's, keyed by their ORIGINAL diag indices - a click on the
    // first drawn row lands the cursor on 1 (the warning's own slot in
    // ed.problems), never on 0 (the slot a renumbered list would give
    // it). The id scope AND the line lookup both ride that index: the
    // row's label is lines[1] rendered, so the focus proves both.
    b.ed.problems_show_error = false;
    const auto row_id = [](gui::GuiContext& g, int at, const char* label) {
        g.ids().push(at);
        const gui::WidgetId row = g.ids().id(label);
        g.ids().pop();
        return row;
    };
    b.draw_problems([&] {
        b.gui.set_focus(row_id(b.gui, 1, "[W] R1: warning: a finding"));
    });
    ws::input::InputFrame enter;
    enter.key_pressed[static_cast<std::size_t>(ws::input::keys::kEnter)] =
        true;
    b.draw_problems(enter, [] {});
    CHECK(b.ed.problems_cursor == 1);
    CHECK(b.ed.sel.is(editor::Selection::Kind::Part, id));
}

// --- the field rows' "##" identity ----------------------------------------------

TEST_CASE("a param labeled Value is its own field, not the built-in value row") {
    Bench b;
    std::vector<ws::circsim::Diag> diags;
    REQUIRE(b.ed.library.load_text(kCollideCsym, "", diags).has_value());
    const std::uint32_t id = b.select_part("widget");
    const std::string part = "p" + std::to_string(id);

    // Frame 1: the panel registers its focusables in draw order -
    // Reference, the BUILT-IN Value row, then the "Value"-labeled param row.
    // A Tab queued after the body resolves against that list, so the
    // ids it lands on are the panel's own field identity.
    b.draw_properties([&] {
        b.gui.set_focus(b.gui.ids().id(("Reference##" + part + ".ref").c_str()));
        b.gui.nav_next();  // Tab off the Reference row
    });

    // Frame 2: the focus must sit on the built-in Value field alone -
    // the param sharing its display label is a different widget, and
    // the tracked flag answers for the focused field.
    b.draw_properties();
    CHECK(b.gui.is_focused(
        b.gui.ids().id(("Value##" + part + ".value").c_str())));
    CHECK(!b.gui.is_focused(
        b.gui.ids().id(("Value##" + part + ".param.temp").c_str())));
    CHECK(b.ed.text_field_focused == true);

    // Tab again: the NEXT focusable is the param row - two rows, two
    // stops, where the shared-label bug fused them into one.
    b.draw_properties([&] {
        b.gui.nav_next();
    });
    b.draw_properties();
    CHECK(b.gui.is_focused(
        b.gui.ids().id(("Value##" + part + ".param.temp").c_str())));
    CHECK(!b.gui.is_focused(
        b.gui.ids().id(("Value##" + part + ".value").c_str())));
    CHECK(b.ed.text_field_focused == true);

    // Focus gone (cleared): the tracked flag quiets.
    b.draw_properties([&] {
        b.gui.set_focus(b.gui.ids().id("no-such-widget"));
    });
    b.draw_properties();
    CHECK(b.ed.text_field_focused == false);
}

TEST_CASE("two params sharing a display label keep separate buffer rows") {
    Bench b;
    // The built-in resistor already carries distinct labels; layer a
    // second passive whose param repeats the built-in "ref" label.
    constexpr const char* kTwinsCsym = R"json({
        "format": "circsim-symbols",
        "version": 1,
        "symbols": [
            {
                "key": "twin",
                "name": "Twin",
                "spice": "U",
                "ref": "U",
                "pins": [],
                "value": true,
                "params": [
                    {"key": "one", "type": "text", "label": "ref",
                     "default": ""},
                    {"key": "two", "type": "text", "label": "ref",
                     "default": ""}
                ],
                "draw": []
            }
        ]
    })json";
    std::vector<ws::circsim::Diag> diags;
    REQUIRE(b.ed.library.load_text(kTwinsCsym, "", diags).has_value());
    const std::uint32_t id = b.select_part("twin");
    const std::string part = "p" + std::to_string(id);

    b.draw_properties();
    // Two param rows, one display label: both live, both keyed.
    CHECK(b.ed.fields.count(part + ".param.one") == 1u);
    CHECK(b.ed.fields.count(part + ".param.two") == 1u);
}

// --- the Palette's draw seam -----------------------------------------------------

TEST_CASE("the palette's tiles draw through the palette_draw_canvas seam") {
    Bench b;
    b.ed.palette_canvas = &b.clc;
    b.draw_palette();
    // The tile grid: one tessellated rounded-fill per tile face (the
    // stock palette's 13 built-ins), recorded through the injected
    // canvas (a fill_rounded_rect lands as a fill_polygon command).
    CHECK(b.clc.count(canvas::CanvasCmdKind::fill_polygon) >= 13u);
    // The pictures ride the same surface: the symbol strokes are
    // polyline commands on the seam's stream.
    CHECK(b.clc.count(canvas::CanvasCmdKind::stroke_polyline) >= 1u);
}

// --- the palette's recent history and the Esc clear ------------------------------

TEST_CASE("arming notes the symbol recent: editor state, capped at six") {
    Bench b;
    // The search box's Enter path, the same one a user rides: each
    // filter below matches its built-in first (the palette suite's own
    // fuzzy cases).
    b.arm_first("r");
    CHECK(b.ed.place_symbol == "r");
    CHECK(b.ed.palette_recent == std::vector<std::string>{"r"});

    b.arm_first("c");
    b.arm_first("l");
    b.arm_first("v");
    b.arm_first("d");
    b.arm_first("gnd");
    // Six distinct arms: newest first, the cap exactly full.
    CHECK(b.ed.palette_recent ==
          std::vector<std::string>({"gnd", "d", "v", "l", "c", "r"}));

    // Re-arming an existing key moves it to the front without growing.
    b.arm_first("r");
    CHECK(b.ed.palette_recent ==
          std::vector<std::string>({"r", "gnd", "d", "v", "l", "c"}));

    // A seventh distinct key pushes the oldest off the tail.
    b.arm_first("vcc");
    CHECK(b.ed.palette_recent ==
          std::vector<std::string>({"vcc", "r", "gnd", "d", "v", "l"}));

    // Editor state, not the process-wide static it replaces: a second
    // editor's history starts empty (the file-static list shared every
    // editor in the process - the app's and a test bench's alike).
    Bench other;
    CHECK(other.ed.palette_recent.empty());
}

TEST_CASE("Esc in the search box clears the filter") {
    Bench b;
    b.ed.palette_canvas = &b.clc;

    // Focus the box, then a filter that matches exactly one built-in
    // ("r" - the resistor, whose zigzag is a stroke, so one tile face
    // is the whole canvas fill story).
    b.draw_palette(ws::input::InputFrame{}, [&] {
        b.gui.set_focus(b.gui.ids().id("Search symbols"));
    });
    std::snprintf(b.ed.palette_filter, sizeof b.ed.palette_filter, "r");
    b.clc.cmds.clear();
    b.draw_palette();
    CHECK(b.clc.count(canvas::CanvasCmdKind::fill_polygon) == 1u);

    // Esc: the filter clears (the field's own Esc clears a selection;
    // the panel's rule is the whole filter)...
    ws::input::InputFrame esc;
    esc.key_pressed[static_cast<std::size_t>(ws::input::keys::kEscape)] =
        true;
    b.draw_palette(esc, [] {});
    CHECK(b.ed.palette_filter[0] == '\0');

    // ...and the next frame browses the whole palette again - every
    // built-in's tile face back on the seam.
    b.clc.cmds.clear();
    b.draw_palette();
    CHECK(b.clc.count(canvas::CanvasCmdKind::fill_polygon) >= 13u);
}

// --- the toast pair's stable message slots (the About hazard) ------------------

TEST_CASE("two toasts in quick succession keep both messages' bytes live") {
    Bench b;
    gui::Toasts toasts;
    b.ed.toasts = &toasts;

    // The About shape: the same act fires twice inside the first
    // toast's four-second life. The first message's bytes must stay
    // put - the editor's toast pair gives every toast its own slot in
    // toast_text_ (the deque never moves its elements), where the
    // app-owned single buffer this replaces re-pointed under the
    // still-live first message.
    b.ed.toast("circsim editor - the ws::circsim stage-3 schematic "
               "editor over ws::ui::shell",
               gui::ToastSeverity::info);
    REQUIRE(toasts.count() == 1);
    REQUIRE(b.ed.toast_text_.size() == 1u);
    const char* first = b.ed.toast_text_.front().c_str();

    b.ed.toast("a second about, four seconds early",
               gui::ToastSeverity::info);
    CHECK(toasts.count() == 2);
    CHECK(b.ed.toast_text_.size() == 2u);
    // The first message is exactly what it was, at exactly the address
    // it had - no re-point, no reuse, no dangling.
    CHECK(std::string{first} ==
          "circsim editor - the ws::circsim stage-3 schematic "
          "editor over ws::ui::shell");
    CHECK(b.ed.toast_text_.front().c_str() == first);
    CHECK(std::string{b.ed.toast_text_.back().c_str()} ==
          "a second about, four seconds early");
}

// --- the typography pass: the hierarchy rung, the mono data face ------------------

namespace {

// The synthetic faces the typography cases install: a strong table at
// the heading size (18 px against the 16 px body, the app's own bake
// pair) and a mono table at the data size. The fill ctor copies the
// table, so the arrays fill before the Font members construct.
using GlyphTable = std::array<ws::ui::r2d::Font::Glyph, 95>;

GlyphTable glyph_table(float advance) {
    GlyphTable glyphs{};
    for (ws::ui::r2d::Font::Glyph& g : glyphs) {
        g.advance = advance;
    }
    return glyphs;
}

}  // namespace

TEST_CASE("the palette's tools header draws through the strong face") {
    Bench b;
    b.ed.palette_canvas = &b.clc;

    const GlyphTable strong_glyphs = glyph_table(9.0f);
    const ws::ui::r2d::Font strong(18.0f, 23.0f, strong_glyphs.data(), 95);
    gui::ChromeTheme theme = gui::kDefaultChrome;  // fontless body rows
    theme.font_strong = &strong;

    ws::ui::gui_test::stub.reset();
    b.draw_palette(theme);

    // The ONE text the whole palette draws: the "tools" heading, through
    // the strong face at its own baked size - every other row (the tool
    // selectables, the search placeholder, the group headers, the hint
    // line) is fontless chrome and answers in bars, so the seam's count
    // is the heading's own proof.
    CHECK(ws::ui::gui_test::stub.texts == 1u);
    CHECK(ws::ui::gui_test::stub.last_text_font == &strong);

    // And without a strong face the heading degrades to a bar like
    // every other row - one text fewer, no crash, the one-font rule.
    ws::ui::gui_test::stub.reset();
    b.draw_palette();  // the stock fontless theme
    CHECK(ws::ui::gui_test::stub.texts == 0u);
}

TEST_CASE("the problems rows wear the mono face, the chrome stays body") {
    Bench b;
    std::vector<ws::circsim::Diag> diags;
    ws::circsim::Diag floating;
    floating.severity = ws::circsim::Severity::Error;
    floating.code = "erc.floating";
    floating.message = "output pin floats";
    floating.where = "R1";
    diags.push_back(floating);
    ws::circsim::Diag dup;
    dup.severity = ws::circsim::Severity::Warning;
    dup.code = "erc.dupref";
    dup.message = "duplicate refdes";
    dup.where = "R1";
    diags.push_back(dup);
    b.ed.refresh_problems(false, std::move(diags), "erc",
                          b.doc.schem.version());
    REQUIRE(b.ed.problems.has);

    const GlyphTable mono_glyphs = glyph_table(8.0f);
    ws::ui::r2d::Font mono(15.0f, 19.0f, mono_glyphs.data(), 95);
    b.ed.font_mono = &mono;

    ws::ui::gui_test::stub.reset();
    b.draw_problems();

    // The two findings drew through the mono face (the data rows - the
    // [E]/[W] prefixes, the refdeses, the columns), and nothing else
    // drew text at all: the button, the header and the counts are
    // fontless chrome in this bench.
    CHECK(ws::ui::gui_test::stub.texts == 2u);
    CHECK(ws::ui::gui_test::stub.last_text_font == &mono);

    // The fontless ladder: no mono face installed, the rows fall back
    // to the theme's face - null here, so bars, and the panel still
    // draws (never a deref on the way down).
    b.ed.font_mono = nullptr;
    ws::ui::gui_test::stub.reset();
    b.draw_problems();
    CHECK(ws::ui::gui_test::stub.texts == 0u);
}

TEST_CASE("the filter hides rows from the draw, not findings from the counts") {
    Bench b;
    (void)b.place_with_findings(
        "R1", {diag_of(ws::circsim::Severity::Error, "R1"),
               diag_of(ws::circsim::Severity::Warning, "R1"),
               diag_of(ws::circsim::Severity::Note, "R1")});
    const GlyphTable mono_glyphs = glyph_table(8.0f);
    ws::ui::r2d::Font mono(15.0f, 19.0f, mono_glyphs.data(), 95);
    b.ed.font_mono = &mono;

    // All three rows draw through the mono face; the filter row's
    // checkboxes and the buttons are fontless chrome (no text).
    ws::ui::gui_test::stub.reset();
    b.draw_problems();
    CHECK(ws::ui::gui_test::stub.texts == 3u);

    // Hide the errors: the error's row stops drawing (2 texts), and
    // hiding everything stops them all (0) - the counts the labels
    // carry are Editor state, already covered above.
    b.ed.problems_show_error = false;
    ws::ui::gui_test::stub.reset();
    b.draw_problems();
    CHECK(ws::ui::gui_test::stub.texts == 2u);

    b.ed.problems_show_warning = false;
    b.ed.problems_show_note = false;
    ws::ui::gui_test::stub.reset();
    b.draw_problems();
    CHECK(ws::ui::gui_test::stub.texts == 0u);
}

TEST_CASE("the netlist tab honors the mono face and survives a fontless theme") {
    Bench b;
    b.ed.properties_tab = 1;  // the right panel's Netlist tab
    b.ed.netlist.has = true;
    b.ed.netlist.version = b.doc.schem.version();
    b.ed.netlist.lines = {"* the deck", "V1 in 0 dc 5", "R1 in out 1k",
                          ".end"};

    // The fontless frame FIRST (the regression): no mono face and no
    // body font - the deck still draws, as plain rows, where the old
    // path deref'd *theme.font unconditionally.
    ws::ui::gui_test::stub.reset();
    b.draw_properties();
    CHECK(ws::ui::gui_test::stub.texts == 0u);

    // With the mono face installed: every deck line answers through it
    // (now with line numbers too: 4 line numbers + 4 deck lines = 8 texts),
    // and only the deck - the tab strip, the re-export button and the
    // "the deck" label stay fontless chrome.
    const GlyphTable mono_glyphs = glyph_table(8.0f);
    ws::ui::r2d::Font mono(15.0f, 19.0f, mono_glyphs.data(), 95);
    b.ed.font_mono = &mono;
    ws::ui::gui_test::stub.reset();
    b.draw_properties();
    CHECK(ws::ui::gui_test::stub.texts == 8u);
    CHECK(ws::ui::gui_test::stub.last_text_font == &mono);
}

// --- the netlist Copy button -----------------------------------------------------

TEST_CASE("the deck join is the rows with single newlines between") {
    using editor::join_deck_lines;
    CHECK(join_deck_lines({}) == "");
    CHECK(join_deck_lines({"* the deck"}) == "* the deck");
    CHECK(join_deck_lines({"* the deck", "V1 in 0 dc 5", ".end"}) ==
          "* the deck\nV1 in 0 dc 5\n.end");
    // The split's own round trip: whatever refresh_netlist split, the
    // join hands the clipboard the export's exact bytes.
    const std::vector<std::string> deck = {"* rc step", "V1 in 0 dc 5",
                                           "R1 in out 1k", ".end"};
    CHECK(join_deck_lines(deck) ==
          "* rc step\nV1 in 0 dc 5\nR1 in out 1k\n.end");
}

TEST_CASE("the Copy button lands the shown deck on the clipboard seam") {
    Bench b;
    b.ed.netlist.has = true;
    b.ed.netlist.version = b.doc.schem.version();
    b.ed.netlist.lines = {"* the deck", "V1 in 0 dc 5", "R1 in out 1k",
                          ".end"};

    // The windowing layer's own seam, the way install_clipboard wires
    // it (the headless bench's gui starts with none).
    std::string copied;
    std::string status;
    b.ed.set_status = [&](std::string text) { status = std::move(text); };
    b.gui.set_clipboard(
        [] { return nullptr; },
        [&](const char* text) { copied = text; });
    REQUIRE(b.gui.has_clipboard() == true);

    // The button row: [Re-export 120][Copy 64] at the 12 px pad - Copy
    // spans x 140..204.
    b.click_netlist(172.0f, 26.0f);
    CHECK(copied == "* the deck\nV1 in 0 dc 5\nR1 in out 1k\n.end");
    CHECK(status == "Copied 4 lines");

    // A stale deck copies as shown - the status line says what it is.
    b.doc.schem.bump();
    b.click_netlist(172.0f, 26.0f);
    CHECK(copied == "* the deck\nV1 in 0 dc 5\nR1 in out 1k\n.end");
    CHECK(status == "Copied 4 lines (stale)");

    // No clipboard installed (the Android shape): the honest failure
    // status, never "Copied" over a silent drop.
    b.gui.set_clipboard(nullptr, nullptr);
    CHECK(b.gui.has_clipboard() == false);
    copied.clear();
    b.click_netlist(172.0f, 26.0f);
    CHECK(copied.empty());
    CHECK(status == "copy failed - the clipboard is unavailable");

    // Not exported yet: the button is dead and the seam stays untouched.
    Bench fresh;
    std::string fresh_copied;
    fresh.gui.set_clipboard(
        [] { return nullptr; },
        [&](const char* text) { fresh_copied = text; });
    fresh.click_netlist(172.0f, 26.0f);
    CHECK(fresh_copied.empty());
}
