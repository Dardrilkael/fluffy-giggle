// apps/circsim_editor - document deletion/undo regression tests.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "document.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include <ws/circsim/export.hpp>
#include <ws/circsim/model.hpp>
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
}  // namespace

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
