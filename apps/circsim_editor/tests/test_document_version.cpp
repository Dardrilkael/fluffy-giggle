// apps/circsim_editor - the document layer's version contract, gated
// headlessly. Schematic::version() is what every cache keys on: results
// are "current" while their stamp equals it, the Problems list and the
// Netlist view rebuild when it moves. So an undoable edit that changes
// what the export emits MUST move it on apply, on undo and on redo - an
// edit that forgets leaves a stale table marked current.
//
// Pure document code: no gui, no editor. The directives and temperature
// rows both reach the exported deck (the directive text verbatim, `temp`
// as an .options card), which is what makes a missed bump visible.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "../src/document.hpp"

#include <cstdint>
#include <string>

#include <ws/circsim/export.hpp>
#include <ws/circsim/model.hpp>
#include <ws/settings/filesystem.hpp>

namespace {

using editor::SchematicDocument;

template <typename F>
std::uint64_t moves_version(SchematicDocument& doc, const char* what, F&& step) {
    const std::uint64_t before = doc.schem.version();
    step();
    CHECK_MESSAGE(doc.schem.version() != before,
                  what << " left the schematic version at " << before);
    return doc.schem.version();
}

}  // namespace

TEST_CASE("directives edits bump the version on apply, undo and redo") {
    ws::settings::StdFileSystem fs;
    SchematicDocument doc{std::string{}, fs};

    moves_version(doc, "push_directives", [&] {
        editor::push_directives(doc, ".param gain=2");
    });
    CHECK(doc.schem.directives == ".param gain=2");

    moves_version(doc, "undo of directives",
                  [&] { CHECK(doc.undo().undo()); });
    CHECK(doc.schem.directives.empty());

    moves_version(doc, "redo of directives",
                  [&] { CHECK(doc.undo().redo()); });
    CHECK(doc.schem.directives == ".param gain=2");
}

TEST_CASE("temperature edits bump the version on apply, undo and redo") {
    ws::settings::StdFileSystem fs;
    SchematicDocument doc{std::string{}, fs};

    moves_version(doc, "push_set_temp",
                  [&] { editor::push_set_temp(doc, 85.0); });
    REQUIRE(doc.schem.temp.has_value());
    CHECK(*doc.schem.temp == 85.0);

    moves_version(doc, "undo of temperature",
                  [&] { CHECK(doc.undo().undo()); });
    CHECK_FALSE(doc.schem.temp.has_value());

    moves_version(doc, "redo of temperature",
                  [&] { CHECK(doc.undo().redo()); });
    REQUIRE(doc.schem.temp.has_value());
    CHECK(*doc.schem.temp == 85.0);
}

TEST_CASE("typing coalesces into one step but every keystroke still moves the version") {
    ws::settings::StdFileSystem fs;
    SchematicDocument doc{std::string{}, fs};
    const std::size_t steps_before = doc.undo().undo_count();

    moves_version(doc, "first keystroke", [&] { editor::push_set_temp(doc, 1.0); });
    moves_version(doc, "second keystroke", [&] { editor::push_set_temp(doc, 12.0); });
    CHECK(doc.undo().undo_count() == steps_before + 1u);

    moves_version(doc, "undo of the merged step", [&] { CHECK(doc.undo().undo()); });
    CHECK_FALSE(doc.schem.temp.has_value());

    moves_version(doc, "first directive keystroke", [&] { editor::push_directives(doc, ".p"); });
    moves_version(doc, "second directive keystroke", [&] { editor::push_directives(doc, ".param"); });
    moves_version(doc, "undo of the merged directives", [&] { CHECK(doc.undo().undo()); });
    CHECK(doc.schem.directives.empty());
}

TEST_CASE("a directives or temperature edit changes the exported deck") {
    ws::settings::StdFileSystem fs;
    SchematicDocument doc{std::string{}, fs};
    const ws::circsim::NetBuild nets = ws::circsim::build_nets(doc.schem);

    const auto plain = ws::circsim::export_deck(doc.schem, nets.nets);
    REQUIRE(plain.has_value());

    editor::push_directives(doc, ".param gain=2");
    editor::push_set_temp(doc, 85.0);
    const auto edited = ws::circsim::export_deck(doc.schem, nets.nets);
    REQUIRE(edited.has_value());

    CHECK(edited->text != plain->text);
    CHECK(edited->text.find(".param gain=2") != std::string::npos);
    CHECK(edited->text.find(".options temp=85") != std::string::npos);
}
