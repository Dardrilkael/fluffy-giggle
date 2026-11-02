// The ws::spice export: golden decks, the back-map, the analysis grammar,
// and the full path (schematic -> deck -> run) checked against closed-form
// answers.

#include <cmath>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include <ws/circsim/export.hpp>
#include <ws/circsim/nets.hpp>

using ws::circsim::AcSpec;
using ws::circsim::DcSpec;
using ws::circsim::Label;
using ws::circsim::OpSpec;
using ws::circsim::Part;
using ws::circsim::PinRef;
using ws::circsim::Pos;
using ws::circsim::Schematic;
using ws::circsim::TranSpec;

namespace {

// The stage-1 divider: 5 V over 10k + 10k, ground, an "out" label. The
// source sits BELOW the resistor axis so no straight pin wire passes over
// the source's own opposite pin (V pins are only 2 units apart).
Schematic divider() {
    Schematic s;
    Part* v = s.add_part(Part::make("v", "V1", "5", {0.0, -4.0}));
    Part* r1 = s.add_part(Part::make("r", "R1", "10k", {6.0, 0.0}));
    Part* r2 = s.add_part(Part::make("r", "R2", "10k", {14.0, 0.0}));
    Part* g = s.add_part(Part::make("gnd", "", "", {7.0, -6.0}));
    s.add_wire(ws::circsim::Wire::endpoints(PinRef{v->id, 0}, PinRef{r1->id, 0}));
    s.add_wire(ws::circsim::Wire::endpoints(PinRef{r1->id, 1}, PinRef{r2->id, 0}));
    s.add_wire(ws::circsim::Wire::endpoints(PinRef{r2->id, 1}, PinRef{g->id, 0}));
    s.add_wire(ws::circsim::Wire::endpoints(PinRef{g->id, 0}, PinRef{v->id, 1}));
    Label* out = s.add_label({});
    out->pos = Pos{10.0, 0.0};
    out->name = "out";
    return s;
}

Schematic rc_step() {
    Schematic s;
    Part* v = s.add_part(Part::make("v", "V1", "0", {0.0, -4.0}));
    v->params["wave"] = "PULSE(0 5 1n 1n 20m 20m)";
    Part* r = s.add_part(Part::make("r", "R1", "1k", {6.0, 0.0}));
    Part* c = s.add_part(Part::make("c", "C1", "1u", {14.0, 0.0}));
    Part* g = s.add_part(Part::make("gnd", "", "", {7.0, -6.0}));
    s.add_wire(ws::circsim::Wire::endpoints(PinRef{v->id, 0}, PinRef{r->id, 0}));
    s.add_wire(ws::circsim::Wire::endpoints(PinRef{r->id, 1}, PinRef{c->id, 0}));
    s.add_wire(ws::circsim::Wire::endpoints(PinRef{c->id, 1}, PinRef{g->id, 0}));
    s.add_wire(ws::circsim::Wire::endpoints(PinRef{g->id, 0}, PinRef{v->id, 1}));
    Label* out = s.add_label({});
    out->pos = Pos{10.0, 0.0};
    out->name = "out";
    s.analysis.entries.push_back(
        TranSpec{.tstep = 1e-4, .tstop = 1e-2});
    return s;
}

}  // namespace

TEST_CASE("the divider exports its golden deck") {
    Schematic s = divider();
    const ws::circsim::NetBuild nets = ws::circsim::build_nets(s);
    s.analysis.entries.push_back(OpSpec{});
    const auto deck = ws::circsim::export_deck(s, nets.nets);
    REQUIRE(deck.has_value());
    CHECK(deck->text ==
          "circsim export\n"
          "R1 N001 out 10k\n"
          "R2 out 0 10k\n"
          "V1 N001 0 5\n"
          ".op\n"
          ".end\n");
}

TEST_CASE("an empty analysis setup defaults to one .op on the run path") {
    const Schematic s = divider();  // no analysis entries
    const ws::circsim::RunOutcome out = ws::circsim::run_schematic(s);
    REQUIRE(out.run.has_value());
    CHECK(out.map.text.find(".op\n") != std::string::npos);
}

TEST_CASE("the RC step exports its waveform through the wave param") {
    const Schematic s = rc_step();
    const ws::circsim::NetBuild nets = ws::circsim::build_nets(s);
    const auto deck = ws::circsim::export_deck(s, nets.nets);
    REQUIRE(deck.has_value());
    CHECK(deck->text ==
          "circsim export\n"
          "C1 out 0 1u\n"
          "R1 N001 out 1k\n"
          "V1 N001 0 0 PULSE(0 5 1n 1n 20m 20m)\n"
          ".tran 0.0001 0.01\n"
          ".end\n");
}

TEST_CASE("directives ride verbatim ahead of the cards") {
    Schematic s = divider();
    s.directives = ".model qmod NPN(Is=1e-14 Bf=100)";
    Part* q = s.add_part(Part::make("npn", "Q1", "", {30.0, 0.0}));
    q->params["model"] = "qmod";
    const ws::circsim::NetBuild nets = ws::circsim::build_nets(s);
    const auto deck = ws::circsim::export_deck(s, nets.nets);
    REQUIRE(deck.has_value());
    // The unconnected BJT pins each own a floating single-pin net
    // (N002/N003/N004, entity order c-b-e) - spice's own floating-node
    // check maps back through them; ERC flagged the pins already.
    const std::string expected =
        "circsim export\n"
        ".model qmod NPN(Is=1e-14 Bf=100)\n"
        "Q1 N002 N003 N004 qmod\n"
        "R1 N001 out 10k\n"
        "R2 out 0 10k\n"
        "V1 N001 0 5\n"
        ".end\n";
    CHECK(deck->text == expected);
    // The back-map ties the directives line and the Q1 card.
    REQUIRE(deck->line_origins.size() == 7);
    CHECK(deck->line_origins[1].kind ==
          ws::circsim::SpiceExport::Origin::Kind::Directives);
    CHECK(deck->line_origins[2].kind ==
          ws::circsim::SpiceExport::Origin::Kind::Part);
    CHECK(deck->line_origins[2].id == q->id);
}

TEST_CASE("refs without the symbol's letter mangle deterministically") {
    Schematic s;
    Part* r = s.add_part(Part::make("r", "Load", "1k", {0.0, 0.0}));
    const ws::circsim::NetBuild nets = ws::circsim::build_nets(s);
    const auto deck = ws::circsim::export_deck(s, nets.nets);
    REQUIRE(deck.has_value());
    CHECK(deck->text.find("R_Load") != std::string::npos);
    CHECK(deck->element_part.find("R_Load") != deck->element_part.end());
    CHECK(deck->element_part.at("R_Load") == r->id);
}

TEST_CASE("node tokens sanitize; collisions fail the export") {
    Schematic s;
    s.add_part(Part::make("v", "V1", "5", {0.0, -4.0}));
    Part* r1 = s.add_part(Part::make("r", "R1", "1k", {6.0, 0.0}));
    Part* r2 = s.add_part(Part::make("r", "R2", "1k", {14.0, 0.0}));
    s.add_part(Part::make("gnd", "", "", {7.0, -6.0}));
    s.add_wire(ws::circsim::Wire::endpoints(PinRef{r1->id, 1}, PinRef{r2->id, 0}));
    Label* a = s.add_label({});
    a->pos = Pos{10.0, 0.0};
    a->name = "in+out";  // -> "inout"
    const ws::circsim::NetBuild nets = ws::circsim::build_nets(s);
    const auto deck = ws::circsim::export_deck(s, nets.nets);
    REQUIRE(deck.has_value());
    CHECK(deck->text.find("inout") != std::string::npos);

    // A label spelling "5" sanitizes to n5.
    Schematic t;
    Part* v = t.add_part(Part::make("v", "V1", "5", {0.0, -4.0}));
    Part* r = t.add_part(Part::make("r", "R1", "1k", {6.0, 0.0}));
    Part* g = t.add_part(Part::make("gnd", "", "", {7.0, -6.0}));
    t.add_wire(ws::circsim::Wire::endpoints(PinRef{v->id, 0}, PinRef{r->id, 0}));
    t.add_wire(ws::circsim::Wire::endpoints(PinRef{r->id, 1}, PinRef{g->id, 0}));
    t.add_wire(ws::circsim::Wire::endpoints(PinRef{g->id, 0}, PinRef{v->id, 1}));
    Label* five = t.add_label({});
    five->pos = Pos{3.0, 0.0};
    five->name = "5";
    const ws::circsim::NetBuild nets2 = ws::circsim::build_nets(t);
    const auto deck2 = ws::circsim::export_deck(t, nets2.nets);
    REQUIRE(deck2.has_value());
    CHECK(deck2->text.find("n5") != std::string::npos);

    // "a+b" and "ab" both sanitize to ab: the export refuses.
    Schematic u;
    Part* ru = u.add_part(Part::make("r", "R1", "1k", {0.0, 0.0}));
    Label* x = u.add_label({});
    x->pos = Pos{-3.0, 0.0};
    x->name = "a+b";
    Label* y = u.add_label({});
    y->pos = Pos{3.0, 0.0};
    y->name = "ab";
    const ws::circsim::NetBuild nets3 = ws::circsim::build_nets(u);
    const auto failed = ws::circsim::export_deck(u, nets3.nets);
    REQUIRE_FALSE(failed.has_value());
    (void)ru;
}

TEST_CASE("the analysis card spellings") {
    using ws::circsim::analysis_card;
    CHECK(analysis_card(OpSpec{}) == ".op");
    CHECK(analysis_card(DcSpec{.source = "V1", .start = 0.0, .stop = 5.0,
                               .step = 0.1}) == ".dc V1 0 5 0.1");
    CHECK(analysis_card(TranSpec{.tstep = 1e-4, .tstop = 1e-2}) ==
          ".tran 0.0001 0.01");
    CHECK(analysis_card(TranSpec{.tstep = 0.0, .tstop = 1.0}) ==
          ".tran 0.01 1");  // tstep 0 -> tstop/100
    CHECK(analysis_card(TranSpec{.tstep = 1e-4, .tstop = 1e-2,
                                 .tstart = 0.0, .tmax = 1e-3, .uic = true}) ==
          ".tran 0.0001 0.01 0 0.001 UIC");
    CHECK(analysis_card(AcSpec{.sweep = AcSpec::Sweep::Dec, .n = 10,
                               .fstart = 1.0, .fstop = 1e6}) ==
          ".ac dec 10 1 1000000");
}

TEST_CASE("the --analysis grammar parses (and rejects)") {
    const auto op = ws::circsim::parse_analysis_spec("op");
    REQUIRE(op.has_value());
    CHECK(std::holds_alternative<OpSpec>(*op));

    const auto dc = ws::circsim::parse_analysis_spec("dc V1 0 5 100m");
    REQUIRE(dc.has_value());
    const auto* dcs = std::get_if<DcSpec>(&*dc);
    REQUIRE(dcs != nullptr);
    CHECK(dcs->source == "V1");
    CHECK(dcs->stop == 5.0);
    CHECK(dcs->step == 0.1);

    const auto tran = ws::circsim::parse_analysis_spec("tran 1u 1m uic");
    REQUIRE(tran.has_value());
    const auto* ts = std::get_if<TranSpec>(&*tran);
    REQUIRE(ts != nullptr);
    CHECK(ts->tstep == 1e-6);
    CHECK(ts->tstop == 1e-3);
    CHECK(ts->uic);

    const auto ac = ws::circsim::parse_analysis_spec("ac oct 20 10 100k");
    REQUIRE(ac.has_value());
    const auto* as = std::get_if<AcSpec>(&*ac);
    REQUIRE(as != nullptr);
    CHECK(as->sweep == AcSpec::Sweep::Oct);
    CHECK(as->fstop == 1e5);

    CHECK_FALSE(ws::circsim::parse_analysis_spec("").has_value());
    CHECK_FALSE(ws::circsim::parse_analysis_spec("noise 1 10").has_value());
    CHECK_FALSE(ws::circsim::parse_analysis_spec("dc V1 zero five")
                    .has_value());
}

TEST_CASE("an analysis override replaces the stored setup") {
    Schematic s = divider();
    s.analysis.entries.push_back(OpSpec{});
    std::vector<ws::circsim::AnalysisSpec> override_;
    override_.push_back(DcSpec{.source = "V1", .start = 0.0, .stop = 5.0,
                               .step = 1.0});
    const ws::circsim::RunOutcome out =
        ws::circsim::run_schematic(s, &override_);
    REQUIRE(out.run.has_value());
    CHECK(out.map.text.find(".dc V1 0 5 1\n") != std::string::npos);
    CHECK(out.map.text.find(".op") == std::string::npos);
    REQUIRE(out.run->results.tables.size() == 1);
    CHECK(out.run->results.tables[0].analysis ==
          ws::spice::AnalysisKind::Dc);
}

TEST_CASE("the divider solves to its analytic midpoint") {
    const Schematic s = divider();
    const ws::circsim::RunOutcome out = ws::circsim::run_schematic(s);
    REQUIRE(out.run.has_value());
    REQUIRE(out.run->results.tables.size() == 1);
    const auto v_out = out.run->results.tables[0].signal("v(out)");
    REQUIRE(v_out.size() == 1);
    CHECK(v_out[0] == doctest::Approx(2.5).epsilon(1e-9));
}

TEST_CASE("the RC step response tracks 5*(1-exp(-t/tau))") {
    const Schematic s = rc_step();
    const ws::circsim::RunOutcome out = ws::circsim::run_schematic(s);
    REQUIRE(out.run.has_value());
    REQUIRE(out.run->results.tables.size() == 1);
    const ws::spice::Table& t = out.run->results.tables[0];
    const auto time = t.axis(0);  // the .tran sweep axis
    const auto v_out = t.signal("v(out)");
    REQUIRE(time.size() >= 10);
    REQUIRE(v_out.size() == time.size());
    const double tau = 1e3 * 1e-6;  // 1k * 1u
    const auto near = [&](double target, double window) {
        std::size_t best = 0;
        double best_d = 1e9;
        for (std::size_t k = 0; k < time.size(); ++k) {
            const double d = std::fabs(time[k] - target);
            if (d < best_d) {
                best_d = d;
                best = k;
            }
        }
        if (best_d > window) {
            return -1.0;
        }
        return v_out[best];
    };
    const double v1 = near(3e-3, 5e-4);
    REQUIRE(v1 >= 0.0);
    CHECK(v1 ==
          doctest::Approx(5.0 * (1.0 - std::exp(-3e-3 / tau))).epsilon(2e-3));
    const double v2 = near(1e-2, 5e-4);
    REQUIRE(v2 >= 0.0);
    CHECK(v2 ==
          doctest::Approx(5.0 * (1.0 - std::exp(-1e-2 / tau))).epsilon(2e-3));
}

TEST_CASE("simulation failures map back onto parts and directives") {
    SUBCASE("a voltage-source loop names its parts") {
        Schematic s;
        Part* v1 = s.add_part(Part::make("v", "V1", "5", {0.0, -4.0}));
        Part* v2 = s.add_part(Part::make("v", "V2", "3", {10.0, 0.0}));
        s.add_part(Part::make("gnd", "", "", {5.0, -4.0}));
        s.add_wire(ws::circsim::Wire::endpoints(PinRef{v1->id, 0}, PinRef{v2->id, 0}));
        s.add_wire(ws::circsim::Wire::endpoints(PinRef{v1->id, 1}, PinRef{v2->id, 1}));
        const ws::circsim::RunOutcome out = ws::circsim::run_schematic(s);
        CHECK_FALSE(out.run.has_value());
        bool mapped = false;
        for (const ws::circsim::Diag& d : out.problems) {
            if (d.code == "circsim.run.failed" &&
                (d.where == "V1" || d.where == "V2")) {
                mapped = true;
            }
        }
        CHECK(mapped);
    }
    SUBCASE("a bad directives line points at the directives block") {
        Schematic s = divider();
        s.directives = ".model qmod NPN(Is=1e-14 Bf=100)\n.lib nowhere.lib";
        const ws::circsim::RunOutcome out = ws::circsim::run_schematic(s);
        CHECK_FALSE(out.run.has_value());
        bool mapped = false;
        for (const ws::circsim::Diag& d : out.problems) {
            if (d.code == "circsim.run.failed" && d.where == "directives") {
                mapped = true;
            }
        }
        CHECK(mapped);
    }
}

TEST_CASE("directives can .include through the injected resolver") {
    Schematic s = divider();
    s.directives = ".include models.inc";
    s.analysis.entries.push_back(OpSpec{});
    ws::spice::IncludeResolver resolver = [](const std::string& path) {
        if (path == "models.inc") {
            return ws::core::Result<std::string>(
                std::string(".param rl=10k\n"));
        }
        return ws::core::Result<std::string>(
            ws::core::unexpected<ws::core::Error>(
                ws::core::make_error(1, "circsim",
                                     "no such include '" + path + "'")));
    };
    const ws::circsim::RunOutcome out =
        ws::circsim::run_schematic(s, nullptr, resolver);
    REQUIRE(out.run.has_value());
    // The include landed: rl is defined (the run solves; the divider never
    // references it, but the scope carried it).
    CHECK(out.run->results.tables.size() == 1);
}

TEST_CASE("results_json round-trips through the settings json parser") {
    const Schematic s = divider();
    const ws::circsim::RunOutcome out = ws::circsim::run_schematic(s);
    REQUIRE(out.run.has_value());
    const std::string json = ws::circsim::results_json(*out.run);
    const auto parsed = ws::settings::json::parse(json);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->is_object());
    const auto* tables = parsed->as_object()->find("tables");
    REQUIRE(tables != nullptr);
    REQUIRE(tables->is_array());
    REQUIRE(tables->as_array()->size() == 1);
    const auto& columns =
        *tables->as_array()->at(0).as_object()->find("columns");
    REQUIRE(columns.is_array());
    // .op has no sweep column; the default save list is every node
    // voltage (N001, out - ground never appears) plus i(V1).
    CHECK(columns.as_array()->size() == 3);
}

TEST_CASE("the export back-map answers for every element and node") {
    Schematic s = divider();
    s.analysis.entries.push_back(OpSpec{});
    const ws::circsim::NetBuild nets = ws::circsim::build_nets(s);
    const auto deck = ws::circsim::export_deck(s, nets.nets);
    REQUIRE(deck.has_value());
    REQUIRE(deck->element_part.size() == 3);
    REQUIRE(deck->node_net.size() == 3);  // N001, out, 0
    const ws::circsim::Net* out_net = nets.nets.by_name("out");
    REQUIRE(out_net != nullptr);
    CHECK(deck->node_net.at("out") == out_net->id);
    const ws::circsim::Net* ground = nets.nets.by_name("0");
    REQUIRE(ground != nullptr);
    CHECK(deck->node_net.at("0") == ground->id);
}
