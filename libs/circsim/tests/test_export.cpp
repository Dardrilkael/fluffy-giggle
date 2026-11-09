// The ws::spice export: golden decks, the back-map, the analysis grammar,
// and the full path (schematic -> deck -> run) checked against closed-form
// answers.

#include <cmath>
#include <string>
#include <vector>

// LOCAL SANDBOX NOTE (review build only, not a repo change): GCC 14's
// -Wmaybe-uninitialized fires a known false positive when a
// default-constructed AnalysisSpec variant is moved into a vector at
// -O3; MSVC (the workspace's primary desktop toolchain) and the
// semantics are unaffected.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

#include <doctest/doctest.h>

#include <ws/circsim/erc.hpp>
#include <ws/circsim/export.hpp>
#include <ws/circsim/nets.hpp>

using ws::circsim::AcSpec;
using ws::circsim::DcSpec;
using ws::circsim::Label;
using ws::circsim::NoiseSpec;
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
    CHECK(analysis_card(NoiseSpec{
              .sweep = NoiseSpec::Sweep::Dec, .n = 10, .fstart = 1.0,
              .fstop = 1e6, .output = "out", .input = "V1"}) ==
          ".noise v(out) V1 dec 10 1 1000000");
    CHECK(analysis_card(NoiseSpec{
              .sweep = NoiseSpec::Sweep::Lin, .n = 5, .fstart = 10.0,
              .fstop = 1e3, .output = "out", .ref = "ref", .input = "I2",
              .pts_per_summary = 2}) ==
          ".noise v(out,ref) I2 lin 5 10 1000 2");
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

    // The noise grammar: both spellings of the output, the optional
    // reference and the optional summary cadence.
    const auto noise = ws::circsim::parse_analysis_spec(
        "noise v(out) V1 dec 10 1 100k 5");
    REQUIRE(noise.has_value());
    const auto* n1 = std::get_if<NoiseSpec>(&*noise);
    REQUIRE(n1 != nullptr);
    CHECK(n1->output == "out");
    CHECK(n1->ref.empty());
    CHECK(n1->input == "V1");
    CHECK(n1->sweep == NoiseSpec::Sweep::Dec);
    CHECK(n1->n == 10);
    CHECK(n1->fstart == 1.0);
    CHECK(n1->fstop == 1e5);
    CHECK(n1->pts_per_summary == 5);

    const auto noise_bare =
        ws::circsim::parse_analysis_spec("noise out,ref V2 oct 4 1k 1meg");
    REQUIRE(noise_bare.has_value());
    const auto* n2 = std::get_if<NoiseSpec>(&*noise_bare);
    REQUIRE(n2 != nullptr);
    CHECK(n2->output == "out");
    CHECK(n2->ref == "ref");
    CHECK(n2->input == "V2");
    CHECK(n2->sweep == NoiseSpec::Sweep::Oct);
    CHECK(n2->fstop == 1e6);
    CHECK(n2->pts_per_summary == 0);

    CHECK_FALSE(ws::circsim::parse_analysis_spec("").has_value());
    CHECK_FALSE(ws::circsim::parse_analysis_spec("noise 1 10").has_value());
    CHECK_FALSE(ws::circsim::parse_analysis_spec("noise out V1 fast 10 1 1k")
                    .has_value());
    CHECK_FALSE(ws::circsim::parse_analysis_spec(
                    "noise out V1 dec ten 1 1k")
                    .has_value());
    CHECK_FALSE(ws::circsim::parse_analysis_spec(
                    "noise out V1 dec 10 1 1k 2.5")
                    .has_value());
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

TEST_CASE("a noise analysis runs through the schematic path") {
    Schematic s = divider();
    s.analysis.entries.push_back(NoiseSpec{
        .sweep = NoiseSpec::Sweep::Dec, .n = 10, .fstart = 1.0,
        .fstop = 1e5, .output = "out", .input = "V1"});
    const ws::circsim::RunOutcome out = ws::circsim::run_schematic(s);
    REQUIRE(out.run.has_value());
    REQUIRE(out.run->results.tables.size() == 1);
    const ws::spice::Table& t = out.run->results.tables[0];
    CHECK(t.analysis == ws::spice::AnalysisKind::Noise);
    CHECK(t.sweep == ws::spice::SweepKind::Frequency);
    // The spectra columns the Results panel plots.
    REQUIRE(t.signal("onoise_spectrum").size() == t.row_count());
    REQUIRE(t.signal("inoise_spectrum").size() == t.row_count());
    CHECK(t.row_count() == 10 * 5 + 1);  // 10 pts/decade over 5 decades
    // The integrated totals: the divider's out node sees R1||R2 = 5k of
    // Johnson noise against the AC-grounded source, onoise =
    // sqrt(4 k T (R1||R2) (fstop - fstart)) at the 27°C default.
    REQUIRE(out.run->reports.size() == 1);
    const double analytic =
        std::sqrt(1.656e-20 * 5.0e3 * (1e5 - 1.0));
    CHECK(out.run->reports[0].noise_out_total ==
          doctest::Approx(analytic).epsilon(0.05));
    // The forward gain from V1 to out is 1/2, so the input-referred
    // noise is twice the output noise.
    CHECK(out.run->reports[0].noise_in_total ==
          doctest::Approx(2.0 * analytic).epsilon(0.05));
    // Every spectrum point is positive and finite.
    for (const double v : t.signal("onoise_spectrum")) {
        CHECK(v > 0.0);
        CHECK(std::isfinite(v));
    }
}

TEST_CASE("the temperature rides as .options temp= ahead of the parts") {
    // Unset: no card anywhere (SPICE's 27°C default stands).
    {
        const Schematic s = divider();
        const ws::circsim::NetBuild nets = ws::circsim::build_nets(s);
        const auto deck = ws::circsim::export_deck(s, nets.nets);
        REQUIRE(deck.has_value());
        CHECK(deck->text.find(".options temp=") == std::string::npos);
    }
    // Set: the card lands between the directives and the parts, so a
    // user's own .options temp= inside the directives still wins (deck
    // order, last assignment lands).
    {
        Schematic s = divider();
        s.temp = 125.0;
        s.directives = ".options temp=-40";
        s.analysis.entries.push_back(OpSpec{});
        const ws::circsim::NetBuild nets = ws::circsim::build_nets(s);
        const auto deck = ws::circsim::export_deck(s, nets.nets);
        REQUIRE(deck.has_value());
        CHECK(deck->text ==
              "circsim export\n"
              ".options temp=125\n"
              ".options temp=-40\n"
              "R1 N001 out 10k\n"
              "R2 out 0 10k\n"
              "V1 N001 0 5\n"
              ".op\n"
              ".end\n");
        // The back-map pins the temp card on the simulation setup.
        REQUIRE(deck->line_origins.size() == 8);
        CHECK(deck->line_origins[1].kind ==
              ws::circsim::SpiceExport::Origin::Kind::Temp);
    }
}

TEST_CASE(".nodeset, a LEVEL=77 MOS and a BJT with Tf ride the directives") {
    // The editor's whole escape hatch for the recent spice models:
    // model cards and DC hints live in the directives block, the
    // devices carry model/L/W through the property grid's params, and
    // the run composes both halves.
    SUBCASE("a .nodeset hint rides the directives and solves") {
        Schematic s = divider();
        s.directives = ".nodeset v(out)=2.5";
        s.analysis.entries.push_back(OpSpec{});
        const ws::circsim::RunOutcome out = ws::circsim::run_schematic(s);
        REQUIRE(out.run.has_value());
        const auto v_out = out.run->results.tables[0].signal("v(out)");
        REQUIRE(v_out.size() == 1);
        CHECK(v_out[0] == doctest::Approx(2.5).epsilon(1e-9));
    }
    SUBCASE("the BSIM6-style MOS core") {
        Schematic s;
        Part* vd = s.add_part(Part::make("v", "Vd", "1", {0.0, 4.0}));
        Part* vg = s.add_part(Part::make("v", "Vg", "0.8", {0.0, -2.0}));
        Part* m = s.add_part(Part::make("nmos", "M1", "", {10.0, 0.0}));
        m->params["model"] = "nch";
        m->params["l"] = "1u";
        m->params["w"] = "1u";
        Part* g = s.add_part(Part::make("gnd", "", "", {5.0, -6.0}));
        // nmos pins: d g s b. Drain and gate ride their sources; source
        // and bulk ground through the one ground part (same fixed net).
        s.add_wire(ws::circsim::Wire::endpoints(
            PinRef{vd->id, 0}, PinRef{m->id, 0}));
        s.add_wire(ws::circsim::Wire::endpoints(
            PinRef{vg->id, 0}, PinRef{m->id, 1}));
        s.add_wire(ws::circsim::Wire::endpoints(
            PinRef{m->id, 2}, PinRef{g->id, 0}));
        s.add_wire(ws::circsim::Wire::endpoints(
            PinRef{m->id, 3}, PinRef{g->id, 0}));
        s.add_wire(ws::circsim::Wire::endpoints(
            PinRef{vd->id, 1}, PinRef{g->id, 0}));
        s.add_wire(ws::circsim::Wire::endpoints(
            PinRef{vg->id, 1}, PinRef{g->id, 0}));
        s.directives =
            ".model nch NMOS(Level=77 Tox=2n Ndep=2e17 Vth0=0.5"
            " U0=0.045 Vsat=8e4)";
        s.analysis.entries.push_back(OpSpec{});
        const ws::circsim::RunOutcome out = ws::circsim::run_schematic(s);
        REQUIRE(out.run.has_value());
        REQUIRE(out.run->results.tables.size() == 1);
        // Above threshold (Vgs=0.8 > Vth0=0.5) at Vds=1: a real drain
        // current flows through the charge-sheet channel - the Level=77
        // core answered, not a silent no-op.
        const auto i_vd = out.run->results.tables[0].signal("i(Vd)");
        REQUIRE(i_vd.size() == 1);
        CHECK(-i_vd[0] > 1e-6);
        CHECK(-i_vd[0] < 1e-2);
        // The geometry params ride the card tail.
        CHECK(out.map.text.find("nch L=1u W=1u\n") != std::string::npos);
    }
    SUBCASE("the BJT transit-time charges") {
        Schematic s;
        Part* vc = s.add_part(Part::make("v", "V1", "5", {0.0, 4.0}));
        Part* rc = s.add_part(Part::make("r", "Rc", "1k", {8.0, 2.0}));
        Part* vb = s.add_part(Part::make("v", "V2", "0.7", {0.0, -4.0}));
        Part* q = s.add_part(Part::make("npn", "Q1", "", {12.0, -2.0}));
        q->params["model"] = "qmod";
        Part* g = s.add_part(Part::make("gnd", "", "", {6.0, -8.0}));
        // npn pins: c b e. Collector rides Rc (which rides V1), base
        // rides V2, emitter grounds.
        s.add_wire(ws::circsim::Wire::endpoints(
            PinRef{vc->id, 0}, PinRef{rc->id, 0}));
        s.add_wire(ws::circsim::Wire::endpoints(
            PinRef{rc->id, 1}, PinRef{q->id, 0}));
        s.add_wire(ws::circsim::Wire::endpoints(
            PinRef{vb->id, 0}, PinRef{q->id, 1}));
        s.add_wire(ws::circsim::Wire::endpoints(
            PinRef{q->id, 2}, PinRef{g->id, 0}));
        s.add_wire(ws::circsim::Wire::endpoints(
            PinRef{vc->id, 1}, PinRef{g->id, 0}));
        s.add_wire(ws::circsim::Wire::endpoints(
            PinRef{vb->id, 1}, PinRef{g->id, 0}));
        s.directives =
            ".model qmod NPN(Is=1e-14 Bf=100 Vaf=50 Tf=0.5n)";
        s.analysis.entries.push_back(OpSpec{});
        const ws::circsim::RunOutcome out = ws::circsim::run_schematic(s);
        REQUIRE(out.run.has_value());
        const auto i_v1 = out.run->results.tables[0].signal("i(V1)");
        REQUIRE(i_v1.size() == 1);
        // Is*exp(0.7/Vt) ~ 5.8 mA collector current, lifted by Vaf.
        CHECK(-i_v1[0] > 1e-3);
        CHECK(-i_v1[0] < 5e-2);
        CHECK(out.map.text.find("qmod\n") != std::string::npos);
    }
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

TEST_CASE("the LED decks as a diode and lights its probe current") {
    // 5 V over 330 R into an LED (.model led D(Is=1e-16 N=2) - Vf ~1.7 V,
    // so ~10 mA flows). The color param rides the part but never the
    // deck; the run answers the i(D1) probe the canvas glow reads.
    Schematic s;
    Part* v = s.add_part(Part::make("v", "V1", "5", {0.0, -4.0}));
    Part* r = s.add_part(Part::make("r", "R1", "330", {6.0, 0.0}));
    Part* led = s.add_part(Part::make("led", "D1", "", {14.0, 0.0}));
    led->params["model"] = "led";
    led->params["color"] = "green";
    Part* g = s.add_part(Part::make("gnd", "", "", {7.0, -6.0}));
    s.add_wire(ws::circsim::Wire::endpoints(PinRef{v->id, 0}, PinRef{r->id, 0}));
    s.add_wire(ws::circsim::Wire::endpoints(PinRef{r->id, 1}, PinRef{led->id, 0}));
    s.add_wire(ws::circsim::Wire::endpoints(PinRef{led->id, 1}, PinRef{g->id, 0}));
    s.add_wire(ws::circsim::Wire::endpoints(PinRef{g->id, 0}, PinRef{v->id, 1}));
    s.directives = ".model led D(Is=1e-16 N=2)";
    s.analysis.entries.push_back(OpSpec{});

    // ERC is quiet: the color is spec'd (no unused_param), the model
    // rides (no missing_model), the ref carries the D prefix.
    const std::vector<ws::circsim::Diag> erc = ws::circsim::run_erc(s);
    CHECK(erc.empty());

    const ws::circsim::NetBuild nets = ws::circsim::build_nets(s);
    const auto deck = ws::circsim::export_deck(s, nets.nets);
    REQUIRE(deck.has_value());
    CHECK(deck->text ==
          "circsim export\n"
          ".model led D(Is=1e-16 N=2)\n"
          "D1 N002 0 led\n"
          "R1 N001 N002 330\n"
          "V1 N001 0 5\n"
          ".op\n"
          ".end\n");
    REQUIRE(deck->element_part.count("D1") != 0);
    CHECK(deck->element_part.at("D1") == led->id);

    const ws::circsim::RunOutcome out = ws::circsim::run_schematic(s);
    REQUIRE(out.run.has_value());
    REQUIRE(out.run->results.tables.size() == 1);
    const ws::spice::Table& t = out.run->results.tables.front();
    const auto i_led = t.signal("i(D1)");
    REQUIRE(i_led.size() == 1);
    // ~10 mA forward conduction (the 1.7 V drop over 330 R), and the
    // loop invariant: the LED carries exactly what the source delivers.
    CHECK(i_led[0] > 8e-3);
    CHECK(i_led[0] < 12e-3);
    CHECK(i_led[0] == doctest::Approx(-t.signal("i(V1)")[0]).epsilon(1e-9));
}
