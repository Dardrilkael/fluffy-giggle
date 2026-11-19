// apps/circsim_editor - document deletion/undo regression tests.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "../src/document.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include <ws/circsim/export.hpp>
#include <ws/circsim/model.hpp>
#include <ws/circsim/symbols.hpp>
#include <ws/settings/filesystem.hpp>

namespace {
using ws::circsim::Junction;
using ws::circsim::Label;
using ws::circsim::Part;
using ws::circsim::Wire;

struct DocumentFixture {
    ws::settings::StdFileSystem fs;
    editor::SchematicDocument doc{std::string{}, fs};
};

template <typename Container>
std::vector<std::uint32_t> entity_ids(const Container& entities) {
    std::vector<std::uint32_t> result;
    result.reserve(entities.size());
    for (const auto& entity : entities) result.push_back(entity.id);
    return result;
}
    using editor::SchematicDocument;

    template <typename F>
    void check_version_bump(SchematicDocument& doc, const char* operation, F&& run) {
        const std::uint64_t before = doc.schem.version();
        run();
        CHECK_MESSAGE(doc.schem.version() > before,
                      operation << " did not advance Schematic::version()");
    }
}  // namespace

TEST_CASE("directives edits bump the schematic version on apply, undo and redo") {
    DocumentFixture f;

    check_version_bump(f.doc, "directives apply", [&] {
        editor::push_directives(f.doc, ".param gain=2");
    });
    CHECK(f.doc.schem.directives == ".param gain=2");

    check_version_bump(f.doc, "directives undo", [&] {
        (void)f.doc.undo().undo();
    });
    CHECK(f.doc.schem.directives.empty());

    check_version_bump(f.doc, "directives redo", [&] {
        (void)f.doc.undo().redo();
    });
    CHECK(f.doc.schem.directives == ".param gain=2");
}

TEST_CASE("temperature edits bump the schematic version on apply, undo and redo") {
    DocumentFixture f;

    check_version_bump(f.doc, "temperature apply", [&] {
        editor::push_set_temp(f.doc, 85.0);
    });
    REQUIRE(f.doc.schem.temp.has_value());
    CHECK(*f.doc.schem.temp == 85.0);

    check_version_bump(f.doc, "temperature undo", [&] {
        (void)f.doc.undo().undo();
    });
    CHECK_FALSE(f.doc.schem.temp.has_value());

    check_version_bump(f.doc, "temperature redo", [&] {
        (void)f.doc.undo().redo();
    });
    REQUIRE(f.doc.schem.temp.has_value());
    CHECK(*f.doc.schem.temp == 85.0);
}

TEST_CASE("directives and temperature typing coalesce without skipping version bumps") {
    DocumentFixture f;
    const std::size_t initial_undo_count = f.doc.undo().undo_count();

    check_version_bump(f.doc, "first temperature keystroke", [&] {
        editor::push_set_temp(f.doc, 1.0);
    });
    check_version_bump(f.doc, "second temperature keystroke", [&] {
        editor::push_set_temp(f.doc, 12.0);
    });
    CHECK(f.doc.undo().undo_count() == initial_undo_count + 1u);

    check_version_bump(f.doc, "undo merged temperature edit", [&] {
        (void)f.doc.undo().undo();
    });
    CHECK_FALSE(f.doc.schem.temp.has_value());

    check_version_bump(f.doc, "first directives keystroke", [&] {
        editor::push_directives(f.doc, ".p");
    });
    check_version_bump(f.doc, "second directives keystroke", [&] {
        editor::push_directives(f.doc, ".param");
    });

    check_version_bump(f.doc, "undo merged directives edit", [&] {
        (void)f.doc.undo().undo();
    });
    CHECK(f.doc.schem.directives.empty());
}

TEST_CASE("directives and temperature edits are reflected in the exported deck") {
    DocumentFixture f;
    const ws::circsim::NetBuild nets = ws::circsim::build_nets(f.doc.schem);
    const auto original = ws::circsim::export_deck(f.doc.schem, nets.nets);
    REQUIRE(original.has_value());

    editor::push_directives(f.doc, ".param gain=2");
    editor::push_set_temp(f.doc, 85.0);

    const auto edited = ws::circsim::export_deck(f.doc.schem, nets.nets);
    REQUIRE(edited.has_value());
    CHECK(edited->text != original->text);
    CHECK(edited->text.find(".param gain=2") != std::string::npos);
    CHECK(edited->text.find(".options temp=85") != std::string::npos);
}

TEST_CASE("delete-set undo restores exact entity ordering") {
    DocumentFixture f;
    auto& s = f.doc.schem;
    std::vector<std::uint32_t> part_ids, junction_ids, wire_ids, label_ids;

    for (int i = 0; i < 4; ++i) {
        const Part* p = s.add_part(Part::make(
            "r", "R" + std::to_string(i + 1), "1k",
            {static_cast<double>(i * 4), 0.0}));
        REQUIRE(p != nullptr);
        part_ids.push_back(p->id);

        Junction j;
        j.pos = {static_cast<double>(i * 2), 2.0};
        Junction* jp = s.add_junction(j);
        REQUIRE(jp != nullptr);
        junction_ids.push_back(jp->id);

        Label l;
        l.pos = {static_cast<double>(i * 2), 4.0};
        l.name = "N" + std::to_string(i + 1);
        Label* lp = s.add_label(l);
        REQUIRE(lp != nullptr);
        label_ids.push_back(lp->id);
    }

    // The removed middle wires share J3. It must be captured only once;
    // J2 and J4 remain connected to the two surviving wires.
    const auto add_wire = [&](std::uint32_t a, std::uint32_t b) {
        Wire* w = s.add_wire(Wire::endpoints(
            ws::circsim::JunctionRef{a}, ws::circsim::JunctionRef{b}));
        REQUIRE(w != nullptr);
        wire_ids.push_back(w->id);
    };
    add_wire(junction_ids[0], junction_ids[1]);
    add_wire(junction_ids[1], junction_ids[2]);
    add_wire(junction_ids[2], junction_ids[3]);
    add_wire(junction_ids[3], junction_ids[0]);

    const std::string before = ws::circsim::save_csim(s);
    CHECK(editor::push_delete_set(
        f.doc, {part_ids[1], part_ids[3]},
        {wire_ids[1], wire_ids[2]}, {label_ids[1], label_ids[3]}));
    CHECK(s.parts().size() == 2u);
    CHECK(s.wires().size() == 2u);
    CHECK(s.junctions().size() == 3u);
    CHECK(s.labels().size() == 2u);

    REQUIRE(f.doc.undo().can_undo());
    (void)f.doc.undo().undo();
    CHECK(entity_ids(s.parts()) == part_ids);
    CHECK(entity_ids(s.wires()) == wire_ids);
    CHECK(entity_ids(s.junctions()) == junction_ids);
    CHECK(entity_ids(s.labels()) == label_ids);
    CHECK(ws::circsim::save_csim(s) == before);
}

TEST_CASE("single delete undo restores the original deque position") {
    SUBCASE("part and cascaded entities") {
        DocumentFixture f;
        auto& s = f.doc.schem;
        std::vector<std::uint32_t> parts, junctions, wires;
        for (int i = 0; i < 3; ++i) {
            const Part* p = s.add_part(Part::make(
                "r", "R" + std::to_string(i + 1), "1k",
                {static_cast<double>(i * 4), 0.0}));
            REQUIRE(p != nullptr);
            parts.push_back(p->id);
        }
        for (int i = 0; i < 3; ++i) {
            Junction j;
            j.pos = {static_cast<double>(i * 2), 2.0};
            Junction* jp = s.add_junction(j);
            REQUIRE(jp != nullptr);
            junctions.push_back(jp->id);
        }
        const auto add_wire = [&](ws::circsim::WireEndpoint a,
                                  ws::circsim::WireEndpoint b) {
            Wire* w = s.add_wire(Wire::endpoints(a, b));
            REQUIRE(w != nullptr);
            wires.push_back(w->id);
        };
        add_wire(ws::circsim::PinRef{parts[0], 0},
                 ws::circsim::PinRef{parts[2], 0});
        add_wire(ws::circsim::PinRef{parts[1], 0},
                 ws::circsim::JunctionRef{junctions[1]});
        add_wire(ws::circsim::PinRef{parts[2], 1},
                 ws::circsim::JunctionRef{junctions[2]});

        editor::push_delete_part(f.doc, parts[1]);
        REQUIRE(s.parts().size() == 2u);
        REQUIRE(s.wires().size() == 2u);
        REQUIRE(s.junctions().size() == 2u);
        (void)f.doc.undo().undo();
        CHECK(entity_ids(s.parts()) == parts);
        CHECK(entity_ids(s.wires()) == wires);
        CHECK(entity_ids(s.junctions()) == junctions);
    }

    SUBCASE("wire and its orphaned junctions") {
        DocumentFixture f;
        auto& s = f.doc.schem;
        std::vector<std::uint32_t> junctions, wires;
        for (int i = 0; i < 4; ++i) {
            Junction j;
            j.pos = {static_cast<double>(i * 2), 0.0};
            Junction* jp = s.add_junction(j);
            REQUIRE(jp != nullptr);
            junctions.push_back(jp->id);
        }
        const std::uint32_t a[] = {junctions[0], junctions[2], junctions[0]};
        const std::uint32_t b[] = {junctions[1], junctions[3], junctions[1]};
        for (std::size_t i = 0; i < 3u; ++i) {
            Wire* w = s.add_wire(Wire::endpoints(
                ws::circsim::JunctionRef{a[i]},
                ws::circsim::JunctionRef{b[i]}));
            REQUIRE(w != nullptr);
            wires.push_back(w->id);
        }
        editor::push_delete_wire(f.doc, wires[1]);
        REQUIRE(s.wires().size() == 2u);
        REQUIRE(s.junctions().size() == 2u);
        (void)f.doc.undo().undo();
        CHECK(entity_ids(s.wires()) == wires);
        CHECK(entity_ids(s.junctions()) == junctions);
    }

    SUBCASE("label") {
        DocumentFixture f;
        auto& s = f.doc.schem;
        std::vector<std::uint32_t> labels;
        for (int i = 0; i < 3; ++i) {
            Label l;
            l.pos = {static_cast<double>(i * 2), 0.0};
            l.name = "NET" + std::to_string(i + 1);
            Label* lp = s.add_label(l);
            REQUIRE(lp != nullptr);
            labels.push_back(lp->id);
        }
        editor::push_delete_label(f.doc, labels[1]);
        REQUIRE(s.labels().size() == 2u);
        (void)f.doc.undo().undo();
        CHECK(entity_ids(s.labels()) == labels);
    }
}
// --- a move never creates connections ------------------------------------------

TEST_CASE("a move landing a pin on a wire stays inert (no minted dots)") {
    // The tip-crossing rule at the document layer: the move command
    // lands the part whose pins ride an unrelated wire's interior,
    // and the sheet gains nothing - no junction, no binding. The
    // touch stays ERC's pin_on_wire warning until the author wires
    // it.
    DocumentFixture f;
    auto& s = f.doc.schem;
    const ws::circsim::SymbolDef* r =
        ws::circsim::find_symbol("r");
    REQUIRE(r != nullptr);

    // R1 at (0, 10): pin0 (-3, 10), pin1 (3, 10).
    Part* p1 = s.add_part(
        Part::make("r", "R1", "1k", {0.0, 10.0}));
    REQUIRE(p1 != nullptr);

    // Net A: J1 -- W1 -- R1.pin0 (the authored connection).
    Junction* j1 = s.add_junction({});
    REQUIRE(j1 != nullptr);
    j1->pos = {-13.0, 10.0};
    Wire* w1 = s.add_wire({});
    REQUIRE(w1 != nullptr);
    w1->a = ws::circsim::JunctionRef{j1->id};
    w1->b = ws::circsim::PinRef{p1->id, 0};
    w1->ortho = true;

    // Net B: the unrelated horizontal wire at y = 0.
    Junction* b1 = s.add_junction({});
    REQUIRE(b1 != nullptr);
    b1->pos = {-20.0, 0.0};
    Junction* b2 = s.add_junction({});
    REQUIRE(b2 != nullptr);
    b2->pos = {20.0, 0.0};
    Wire* w2 = s.add_wire({});
    REQUIRE(w2 != nullptr);
    w2->a = ws::circsim::JunctionRef{b1->id};
    w2->b = ws::circsim::JunctionRef{b2->id};
    w2->ortho = true;

    p1->pos = {0.0, 0.0};
    s.bump();
    p1->pos = {0.0, 10.0};
    s.bump();

    // The move command: routes for the attached W1, no dots minted.
    const std::vector<ws::circsim::WireRoute> no_routes;
    const std::vector<editor::GluedJunctionMove> no_glue;
    editor::push_move_part(f.doc, p1->id, {0.0, 0.0},
                            {}, {});
    REQUIRE(s.junctions().size() == 3u);  // j1, b1, b2 - nothing minted
    CHECK(s.find_part(p1->id)->pos.y == 0.0);

    // The wire's net does not carry the part's pins.
    {
        const ws::circsim::NetBuild nets =
            ws::circsim::build_nets(s, &ws::circsim::builtin_library());
        const ws::circsim::Net* wn = nets.nets.net_of_wire(w2->id);
        REQUIRE(wn != nullptr);
        bool pin_in = false;
        for (const ws::circsim::PinRef& pr : wn->pins) {
            if (pr.part == p1->id) {
                pin_in = true;
            }
        }
        CHECK(!pin_in);
    }

    // One undo returns the sheet to exactly what it was.
    (void)f.doc.undo().undo();
    CHECK(s.junctions().size() == 3u);
    CHECK(s.find_part(p1->id)->pos.y == 10.0);
}
