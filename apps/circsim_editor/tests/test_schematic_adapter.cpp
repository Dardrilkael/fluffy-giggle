// apps/circsim_editor - the schematic adapter's lift/landing contract.
//
// The adapter translates the persisted model (junction `taps`, no pin
// exit directions) into the ws::sch graph and back. These tests pin the
// three places that translation used to lose information:
//   * a wire carrying SEVERAL taps (the lift must cut it at every one,
//     and the landing must merge the whole chain back);
//   * a dragged tap junction (the persisted path must run through the
//     junction's LANDING, not its press position);
//   * the pin exit directions (a re-routed wire must leave its pin the
//     way the pin faces, not back through the part body).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "../src/schematic_adapter.hpp"

#include <cmath>
#include <vector>

#include <ws/circsim/library.hpp>
#include <ws/circsim/model.hpp>
#include <ws/circsim/symbols.hpp>
#include <ws/schematic/model.hpp>

namespace {

using ws::circsim::Junction;
using ws::circsim::Part;
using ws::circsim::PinRef;
using ws::circsim::Pos;
using ws::circsim::Schematic;
using ws::circsim::Wire;

constexpr double kEps = 1e-9;

// Two resistors on one row, R1.pin1 wired to R2.pin0 by one wire.
struct Rig {
    Schematic s;
    const ws::circsim::SymbolDef* r = nullptr;
    std::uint32_t r1 = 0;
    std::uint32_t r2 = 0;
    std::uint32_t wire = 0;

    Rig() {
        r = ws::circsim::builtin_library().find("r");
        r1 = s.add_part(Part::make("r", "R1", "1k", {0, 0}))->id;
        r2 = s.add_part(Part::make("r", "R2", "1k", {10, 0}))->id;
        Wire w;
        w.a = PinRef{r1, 1};
        w.b = PinRef{r2, 0};
        wire = s.add_wire(w)->id;
    }
    std::uint32_t tap(Pos at) {
        Junction j;
        j.pos = at;
        j.taps.push_back(wire);
        return s.add_junction(j)->id;
    }
    Pos pin(std::uint32_t part, std::uint32_t i, Pos at) const {
        Part probe = *s.find_part(part);
        probe.pos = at;
        return ws::circsim::pin_pos(*r, probe, i);
    }
};

bool orthogonal(const std::vector<Pos>& path) {
    for (std::size_t i = 1; i < path.size(); ++i) {
        const bool h = std::fabs(path[i].y - path[i - 1].y) <= kEps;
        const bool v = std::fabs(path[i].x - path[i - 1].x) <= kEps;
        if (!h && !v) {
            return false;
        }
    }
    return true;
}

bool on_path(const std::vector<Pos>& path, Pos p) {
    for (std::size_t i = 1; i < path.size(); ++i) {
        const Pos a = path[i - 1];
        const Pos b = path[i];
        if (std::fabs((b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x)) >
            kEps) {
            continue;
        }
        if (p.x >= std::min(a.x, b.x) - kEps &&
            p.x <= std::max(a.x, b.x) + kEps &&
            p.y >= std::min(a.y, b.y) - kEps &&
            p.y <= std::max(a.y, b.y) + kEps) {
            return true;
        }
    }
    return false;
}

std::vector<Pos> full_path(Pos a, const std::vector<Pos>& mids, Pos b) {
    std::vector<Pos> out{a};
    out.insert(out.end(), mids.begin(), mids.end());
    out.push_back(b);
    return out;
}

}  // namespace

TEST_CASE("a wire with several taps is cut at every one of them") {
    // Both orders: the far tap first leaves the near one on the piece that
    // still carries the original id; the near one first leaves the far tap
    // on a NEW piece. Each used to lose a tap or the whole re-route.
    for (const bool far_first : {false, true}) {
        CAPTURE(far_first);
        Rig g;
        const Pos t_far{6, 0};
        const Pos t_near{4, 0};
        if (far_first) {
            g.tap(t_far);
            g.tap(t_near);
        } else {
            g.tap(t_near);
            g.tap(t_far);
        }
        editor::SchematicAdapter ad;
        ad.build(g.s, ws::circsim::builtin_library());
        CHECK(ad.doc.wires.size() == 3u);  // one wire, two cuts

        const Pos to{10, 4};  // R2 up: the wire must bend
        const ws::sch::Overlay ov =
            ws::sch::resolve_move(ad.doc, ad.part_session(g.r2, to));
        const auto landing = ad.landing(ov);
        REQUIRE(landing.wires.count(g.wire) == 1u);

        const std::vector<Pos> path =
            full_path(g.pin(g.r1, 1, {0, 0}), landing.wires.at(g.wire),
                      g.pin(g.r2, 0, to));
        CHECK(orthogonal(path));
        CHECK(on_path(path, t_near));  // both T-connections survive
        CHECK(on_path(path, t_far));
    }
}

TEST_CASE("a dragged tap junction's landing is on the persisted path") {
    Rig g;
    const std::uint32_t j = g.tap({5, 0});
    editor::SchematicAdapter ad;
    ad.build(g.s, ws::circsim::builtin_library());

    const Pos to{5, 3};
    const ws::sch::Overlay ov =
        ws::sch::resolve_move(ad.doc, ad.junction_session(j, to));
    const auto landing = ad.landing(ov);
    REQUIRE(landing.junctions.count(j) == 1u);
    CHECK(landing.junctions.at(j).y == doctest::Approx(3.0));
    REQUIRE(landing.wires.count(g.wire) == 1u);

    const std::vector<Pos> path =
        full_path(g.pin(g.r1, 1, {0, 0}), landing.wires.at(g.wire),
                  g.pin(g.r2, 0, {10, 0}));
    CHECK(orthogonal(path));       // used to keep the OLD spot: a diagonal
    CHECK(on_path(path, to));      // and the dot ended up off the wire
}

TEST_CASE("a moved part's wire leaves the pin the way the pin faces") {
    Rig g;
    editor::SchematicAdapter ad;
    ad.build(g.s, ws::circsim::builtin_library());
    // The view carries the pins' exit directions (R1.pin1 faces east,
    // R2.pin0 faces west for the built-in resistor).
    REQUIRE(ad.doc.wires.size() == 1u);
    CHECK(ad.doc.wires[0].a_dir == 0);
    CHECK(ad.doc.wires[0].b_dir == 180);

    // R2 lands with its pin0 WEST of R1.pin1 on another row: a free-ended
    // L ran straight back through R1's body.
    const Pos to{1, 4};
    const ws::sch::Overlay ov =
        ws::sch::resolve_move(ad.doc, ad.part_session(g.r2, to));
    const auto landing = ad.landing(ov);
    REQUIRE(landing.wires.count(g.wire) == 1u);
    const Pos a = g.pin(g.r1, 1, {0, 0});
    const std::vector<Pos> path =
        full_path(a, landing.wires.at(g.wire), g.pin(g.r2, 0, to));
    CHECK(orthogonal(path));
    REQUIRE(path.size() >= 3u);
    CHECK(path[1].x >= a.x - kEps);  // first leg heads east, not west
}

TEST_CASE("a press-pose build equals building with the entities parked") {
    // drag_overlay() used to write the press positions INTO the live
    // schematic, build, and write the live ones back. The pose argument
    // must give the identical view without touching the schematic.
    Rig g;
    const std::uint32_t j = g.tap({5, 0});
    const Pos press_part{10, 0};
    const Pos press_junction{5, 0};
    // The "live" state the canvas keeps drawing: both moved.
    g.s.find_part(g.r2)->pos = {10, 4};
    for (Junction& jn : g.s.junctions_mut()) {
        if (jn.id == j) {
            jn.pos = {5, 3};
        }
    }

    editor::SchematicAdapter posed;
    editor::SchematicAdapter::PressPose pose;
    pose.parts[g.r2] = press_part;
    pose.junctions[j] = press_junction;
    posed.build(g.s, ws::circsim::builtin_library(), &pose);
    CHECK(g.s.find_part(g.r2)->pos.y == doctest::Approx(4.0));  // untouched

    // The old way: park, build, restore.
    g.s.find_part(g.r2)->pos = press_part;
    for (Junction& jn : g.s.junctions_mut()) {
        if (jn.id == j) {
            jn.pos = press_junction;
        }
    }
    editor::SchematicAdapter parked;
    parked.build(g.s, ws::circsim::builtin_library());

    REQUIRE(posed.doc.nodes.size() == parked.doc.nodes.size());
    for (std::size_t i = 0; i < posed.doc.nodes.size(); ++i) {
        CHECK(posed.doc.nodes[i].id == parked.doc.nodes[i].id);
        CHECK(posed.doc.nodes[i].pos.x == doctest::Approx(parked.doc.nodes[i].pos.x));
        CHECK(posed.doc.nodes[i].pos.y == doctest::Approx(parked.doc.nodes[i].pos.y));
    }
    CHECK(posed.doc.wires.size() == parked.doc.wires.size());
}

TEST_CASE("a junction tapping two wires still ends up as one node") {
    // The tap re-bind is deferred to one pass: a junction that taps two
    // wires remaps old -> split1 -> split2 and every wire must land on
    // the final node.
    Schematic s;
    const auto* r = ws::circsim::builtin_library().find("r");
    REQUIRE(r != nullptr);
    Part* p1 = s.add_part(Part::make("r", "R1", "1k", {0, 0}));
    Part* p2 = s.add_part(Part::make("r", "R2", "1k", {10, 0}));
    Part* p3 = s.add_part(Part::make("r", "R3", "1k", {0, 6}));
    Part* p4 = s.add_part(Part::make("r", "R4", "1k", {10, 6}));
    Wire w1;
    w1.a = PinRef{p1->id, 1};
    w1.b = PinRef{p2->id, 0};
    Wire w2;
    w2.a = PinRef{p3->id, 1};
    w2.b = PinRef{p4->id, 0};
    const std::uint32_t id1 = s.add_wire(w1)->id;
    const std::uint32_t id2 = s.add_wire(w2)->id;
    Junction j;
    j.pos = {5, 0};
    j.taps = {id1};
    s.add_junction(j);
    Junction k;
    k.pos = {5, 6};
    k.taps = {id2};
    s.add_junction(k);
    editor::SchematicAdapter ad;
    ad.build(s, ws::circsim::builtin_library());
    CHECK(ad.doc.wires.size() == 4u);  // two wires, one cut each
    // Every wire end resolves to a live node.
    for (const auto& w : ad.doc.wires) {
        CHECK(ad.doc.find_node(w.a) != nullptr);
        CHECK(ad.doc.find_node(w.b) != nullptr);
    }
}
