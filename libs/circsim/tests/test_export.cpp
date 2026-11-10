// The ws::spice export: golden decks, the back-map, the analysis grammar,
// and the full path (schematic -> deck -> run) checked against closed-form
// answers.

#include <cmath>
#include <clocale>
#include <optional>
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
#include <ws/circsim/models.hpp>
#include <ws/circsim/nets.hpp>
#include <ws/circsim/plot.hpp>  // format_eng (the locale proof)
#include <ws/circsim/text.hpp>  // fmt_num / parse_plain_number
#include <ws/spice/deck.hpp>
#include <ws/spice/parse.hpp>
#include <ws/spice/run.hpp>

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
using ws::circsim::Wire;

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

// --- the orphan devices (stage 24) -------------------------------------------------
//
// The shared shape: a source on the left below the axis (never a straight
// wire over its own opposite pin), the device under test on the axis, the
// load to the right, ground wherever a return needs one.

// The first part carrying `symbol`, mutable through find_part.
Part* first_with_symbol(Schematic& s, std::string_view symbol) {
    for (const Part& p : s.parts()) {
        if (p.symbol == symbol) {
            return s.find_part(p.id);
        }
    }
    return nullptr;
}

Schematic vcvs_stage() {
    Schematic s;
    Part* v = s.add_part(Part::make("v", "V1", "1", {0.0, -4.0}));
    Part* e = s.add_part(Part::make("e", "E1", "10", {12.0, 0.0}));
    Part* r = s.add_part(Part::make("r", "R1", "1k", {24.0, 1.0}));
    s.add_part(Part::make("gnd", "", "", {3.0, -3.0}));  // the sense return's pickup
    Part* g2 = s.add_part(Part::make("gnd", "", "", {17.0, -4.0}));
    Part* g3 = s.add_part(Part::make("gnd", "", "", {27.0, -3.0}));
    // The sense pair: in+ up and over, in- across and up (the gnd on the
    // vertical leg grounds the return).
    s.add_wire(Wire::endpoints(PinRef{v->id, 0}, PinRef{e->id, 2}))
        ->waypoints = {Pos{-3.0, 1.0}};
    s.add_wire(Wire::endpoints(PinRef{v->id, 1}, PinRef{e->id, 3}))
        ->waypoints = {Pos{3.0, -1.0}};
    // The output pair: out+ straight into the load, out- and the load's
    // far end each down to their own ground.
    s.add_wire(Wire::endpoints(PinRef{e->id, 0}, PinRef{r->id, 0}));
    s.add_wire(Wire::endpoints(PinRef{e->id, 1}, PinRef{g2->id, 0}));
    s.add_wire(Wire::endpoints(PinRef{r->id, 1}, PinRef{g3->id, 0}));
    Label* in = s.add_label({});
    in->pos = Pos{2.0, 1.0};
    in->name = "in";
    Label* out = s.add_label({});
    out->pos = Pos{19.0, 1.0};
    out->name = "out";
    s.analysis.entries.push_back(OpSpec{});
    return s;
}

TEST_CASE("the VCVS decks its four nodes and solves at its gain") {
    const Schematic s = vcvs_stage();
    const std::vector<ws::circsim::Diag> erc = ws::circsim::run_erc(s);
    CHECK(erc.empty());
    const ws::circsim::NetBuild nets = ws::circsim::build_nets(s);
    const auto deck = ws::circsim::export_deck(s, nets.nets);
    REQUIRE(deck.has_value());
    CHECK(deck->text ==
          "circsim export\n"
          "E1 out 0 in 0 10\n"
          "R1 out 0 1k\n"
          "V1 in 0 1\n"
          ".op\n"
          ".end\n");
    const ws::circsim::RunOutcome run = ws::circsim::run_schematic(s);
    REQUIRE(run.run.has_value());
    const auto v_out = run.run->results.tables[0].signal("v(out)");
    REQUIRE(v_out.size() == 1);
    CHECK(v_out[0] == doctest::Approx(10.0).epsilon(1e-9));  // 1 V * 10
}

TEST_CASE("the VCCS decks the same shape and solves at its gm") {
    Schematic s = vcvs_stage();
    Part* e = first_with_symbol(s, "e");
    REQUIRE(e != nullptr);
    e->symbol = "g";
    e->ref = "G1";
    e->value = "1m";
    const ws::circsim::NetBuild nets = ws::circsim::build_nets(s);
    const auto deck = ws::circsim::export_deck(s, nets.nets);
    REQUIRE(deck.has_value());
    CHECK(deck->text ==
          "circsim export\n"
          "G1 out 0 in 0 1m\n"
          "R1 out 0 1k\n"
          "V1 in 0 1\n"
          ".op\n"
          ".end\n");
    const ws::circsim::RunOutcome run = ws::circsim::run_schematic(s);
    REQUIRE(run.run.has_value());
    const auto v_out = run.run->results.tables[0].signal("v(out)");
    REQUIRE(v_out.size() == 1);
    // ngspice's G convention: the current gm*v(in) flows from out+
    // THROUGH the source to out-, so the load sees -1 mA.
    CHECK(v_out[0] == doctest::Approx(-1.0).epsilon(1e-9));
}

Schematic current_controlled(bool f_not_h) {
    // V1 5 V over R1 1k sources 5 mA; the controlled device reads
    // i(V1) and drives R2 1k from its own output node. The source sits
    // BELOW the axis (a straight pin wire must never pass over its own
    // opposite pin).
    Schematic s;
    Part* v = s.add_part(Part::make("v", "V1", "5", {0.0, -4.0}));
    Part* r1 = s.add_part(Part::make("r", "R1", "1k", {10.0, 0.0}));
    Part* d = s.add_part(Part::make(f_not_h ? "f" : "h", f_not_h ? "F1" : "H1",
                                    "", {20.0, 0.0}));
    d->mirror = true;  // p faces the load: no straight wire over own pins
    d->params["vs"] = "V1";
    d->params[f_not_h ? "gain" : "r"] = f_not_h ? "2" : "100";
    Part* r2 = s.add_part(Part::make("r", "R2", "1k", {30.0, 0.0}));
    s.add_part(Part::make("gnd", "", "", {8.0, -4.0}));  // the return's pickup
    Part* g2 = s.add_part(Part::make("gnd", "", "", {17.0, -3.0}));
    Part* g3 = s.add_part(Part::make("gnd", "", "", {33.0, -3.0}));
    // The measuring loop: V1 up onto the axis into R1, back under it.
    s.add_wire(Wire::endpoints(PinRef{v->id, 0}, PinRef{r1->id, 0}))
        ->waypoints = {Pos{-3.0, 0.0}};
    s.add_wire(Wire::endpoints(PinRef{r1->id, 1}, PinRef{v->id, 1}))
        ->waypoints = {Pos{13.0, -4.0}};
    // The output: the device's p on to the load, its n down to ground
    // (the F's current then flows out into the node; the H is a source
    // of r*i(V1) volts across the same pair).
    s.add_wire(Wire::endpoints(PinRef{d->id, 0}, PinRef{r2->id, 0}));
    s.add_wire(Wire::endpoints(PinRef{d->id, 1}, PinRef{g2->id, 0}));
    s.add_wire(Wire::endpoints(PinRef{r2->id, 1}, PinRef{g3->id, 0}));
    Label* in = s.add_label({});
    in->pos = Pos{2.0, 0.0};
    in->name = "in";
    Label* out = s.add_label({});
    out->pos = Pos{25.0, 0.0};
    out->name = "out";
    s.analysis.entries.push_back(OpSpec{});
    return s;
}

TEST_CASE("the CCCS names its controlling source before its gain") {
    const Schematic s = current_controlled(true);
    const std::vector<ws::circsim::Diag> erc = ws::circsim::run_erc(s);
    CHECK(erc.empty());
    const ws::circsim::NetBuild nets = ws::circsim::build_nets(s);
    const auto deck = ws::circsim::export_deck(s, nets.nets);
    REQUIRE(deck.has_value());
    CHECK(deck->text ==
          "circsim export\n"
          "F1 out 0 V1 2\n"
          "R1 in 0 1k\n"
          "R2 out 0 1k\n"
          "V1 in 0 5\n"
          ".op\n"
          ".end\n");
    const ws::circsim::RunOutcome run = ws::circsim::run_schematic(s);
    REQUIRE(run.run.has_value());
    const auto v_out = run.run->results.tables[0].signal("v(out)");
    REQUIRE(v_out.size() == 1);
    // 2 * 5 mA through 1 k.
    CHECK(v_out[0] == doctest::Approx(10.0).epsilon(1e-9));
}

TEST_CASE("the CCVS names its controlling source before its r") {
    const Schematic s = current_controlled(false);
    const ws::circsim::NetBuild nets = ws::circsim::build_nets(s);
    const auto deck = ws::circsim::export_deck(s, nets.nets);
    REQUIRE(deck.has_value());
    CHECK(deck->text ==
          "circsim export\n"
          "H1 out 0 V1 100\n"
          "R1 in 0 1k\n"
          "R2 out 0 1k\n"
          "V1 in 0 5\n"
          ".op\n"
          ".end\n");
    const ws::circsim::RunOutcome run = ws::circsim::run_schematic(s);
    REQUIRE(run.run.has_value());
    const auto v_out = run.run->results.tables[0].signal("v(out)");
    REQUIRE(v_out.size() == 1);
    // v(out+) - v(out-) = r * i(V1), and a delivering source's branch
    // current is negative in the SPICE convention: -0.5 V.
    CHECK(v_out[0] == doctest::Approx(-0.5).epsilon(1e-9));
}

// The behavioral source stage: V1 biases a real "in" node over R0, and
// the B source (mirrored, so its p faces the load) drives R1. The
// expression references the net NAMES - no sense pins needed.
Schematic b_stage() {
    Schematic s;
    Part* v = s.add_part(Part::make("v", "V1", "1", {0.0, -4.0}));
    Part* r0 = s.add_part(Part::make("r", "R0", "10k", {4.0, 0.0}));
    Part* b = s.add_part(Part::make("b", "B1", "", {12.0, 0.0}));
    b->mirror = true;  // p faces the load
    Part* r1 = s.add_part(Part::make("r", "R1", "1k", {22.0, 0.0}));
    Part* g1 = s.add_part(Part::make("gnd", "", "", {7.0, -3.0}));
    Part* g2 = s.add_part(Part::make("gnd", "", "", {3.0, -7.0}));
    Part* g3 = s.add_part(Part::make("gnd", "", "", {25.0, -3.0}));
    Part* g4 = s.add_part(Part::make("gnd", "", "", {9.0, -3.0}));
    // V1.p up onto the axis into R0 (the in node), its return grounded.
    s.add_wire(Wire::endpoints(PinRef{v->id, 0}, PinRef{r0->id, 0}))
        ->waypoints = {Pos{-3.0, 0.0}};
    s.add_wire(Wire::endpoints(PinRef{r0->id, 1}, PinRef{g1->id, 0}));
    s.add_wire(Wire::endpoints(PinRef{v->id, 1}, PinRef{g2->id, 0}));
    // The B source's output: p on to the load, n down to ground.
    s.add_wire(Wire::endpoints(PinRef{b->id, 0}, PinRef{r1->id, 0}));
    s.add_wire(Wire::endpoints(PinRef{b->id, 1}, PinRef{g4->id, 0}));
    s.add_wire(Wire::endpoints(PinRef{r1->id, 1}, PinRef{g3->id, 0}));
    Label* in = s.add_label({});
    in->pos = Pos{0.0, 0.0};
    in->name = "in";
    Label* out = s.add_label({});
    out->pos = Pos{17.0, 0.0};
    out->name = "out";
    s.analysis.entries.push_back(OpSpec{});
    return s;
}

TEST_CASE("the behavioral source carries its V= or I= expression") {
    SUBCASE("the V= form") {
        Schematic s = b_stage();
        Part* b = first_with_symbol(s, "b");
        REQUIRE(b != nullptr);
        b->params["vexpr"] = "3*v(in)";
        const std::vector<ws::circsim::Diag> erc = ws::circsim::run_erc(s);
        CHECK(erc.empty());
        const ws::circsim::NetBuild nets = ws::circsim::build_nets(s);
        const auto deck = ws::circsim::export_deck(s, nets.nets);
        REQUIRE(deck.has_value());
        CHECK(deck->text ==
              "circsim export\n"
              "B1 out 0 V=3*v(in)\n"
              "R0 in 0 10k\n"
              "R1 out 0 1k\n"
              "V1 in 0 1\n"
              ".op\n"
              ".end\n");
        const ws::circsim::RunOutcome run = ws::circsim::run_schematic(s);
        REQUIRE(run.run.has_value());
        const auto v_out = run.run->results.tables[0].signal("v(out)");
        REQUIRE(v_out.size() == 1);
        CHECK(v_out[0] == doctest::Approx(3.0).epsilon(1e-9));
    }
    SUBCASE("the I= form") {
        Schematic s = b_stage();
        Part* b = first_with_symbol(s, "b");
        REQUIRE(b != nullptr);
        b->params["iexpr"] = "v(in)/1k";
        const ws::circsim::RunOutcome run = ws::circsim::run_schematic(s);
        REQUIRE(run.run.has_value());
        const auto v_out = run.run->results.tables[0].signal("v(out)");
        REQUIRE(v_out.size() == 1);
        // The I-form's current flows p -> n through the source, so the
        // 1 mA (a 1 k behavioral resistor) draws v(out) negative.
        CHECK(v_out[0] == doctest::Approx(-1.0).epsilon(1e-9));
    }
    SUBCASE("neither expression rejects loudly and maps back") {
        Schematic s = b_stage();
        const ws::circsim::RunOutcome run = ws::circsim::run_schematic(s);
        REQUIRE(!run.run.has_value());
        CHECK_FALSE(run.problems.empty());
    }
}

// The S switch: V1 5 V through the contact into R1 1k; V2 drives the
// control pair SINGLE-ENDED (the control port is an open sense - a
// floating drive loop has no DC path to ground and the graph check
// says so), and the .dc sweep over V2 draws the transfer curve.
Schematic vswitch_stage() {
    Schematic s;
    Part* v1 = s.add_part(Part::make("v", "V1", "5", {0.0, -4.0}));
    Part* sw = s.add_part(Part::make("s", "S1", "", {12.0, 0.0}));
    sw->params["model"] = "swmod";
    Part* r1 = s.add_part(Part::make("r", "R1", "1k", {26.0, 0.0}));
    Part* v2 = s.add_part(Part::make("v", "V2", "0", {4.0, -12.0}));
    Part* g1 = s.add_part(Part::make("gnd", "", "", {3.0, -7.0}));
    Part* g2 = s.add_part(Part::make("gnd", "", "", {29.0, -3.0}));
    Part* g3 = s.add_part(Part::make("gnd", "", "", {10.0, -7.0}));
    Part* g4 = s.add_part(Part::make("gnd", "", "", {7.0, -15.0}));
    // The switched path.
    s.add_wire(Wire::endpoints(PinRef{v1->id, 0}, PinRef{sw->id, 0}))
        ->waypoints = {Pos{-3.0, 0.0}};
    s.add_wire(Wire::endpoints(PinRef{sw->id, 1}, PinRef{r1->id, 0}));
    s.add_wire(Wire::endpoints(PinRef{v1->id, 1}, PinRef{g1->id, 0}));
    s.add_wire(Wire::endpoints(PinRef{r1->id, 1}, PinRef{g2->id, 0}));
    // The control drive: V2.p up into c+, both returns grounded.
    s.add_wire(Wire::endpoints(PinRef{v2->id, 0}, PinRef{sw->id, 2}))
        ->waypoints = {Pos{1.0, -8.0}, Pos{14.0, -8.0}};
    s.add_wire(Wire::endpoints(PinRef{v2->id, 1}, PinRef{g4->id, 0}));
    s.add_wire(Wire::endpoints(PinRef{sw->id, 3}, PinRef{g3->id, 0}));
    Label* in = s.add_label({});
    in->pos = Pos{2.0, 0.0};
    in->name = "in";
    Label* out = s.add_label({});
    out->pos = Pos{20.0, 0.0};
    out->name = "out";
    Label* ctl = s.add_label({});
    ctl->pos = Pos{8.0, -8.0};
    ctl->name = "ctl";
    s.directives = ".model swmod SW(Ron=10 Roff=1meg Vt=2 Vh=0)";
    s.analysis.entries.push_back(DcSpec{.source = "V2", .start = 0.0,
                                        .stop = 5.0, .step = 0.5});
    return s;
}

TEST_CASE("the voltage switch decks p n c+ c- and its model") {
    const Schematic s = vswitch_stage();
    const std::vector<ws::circsim::Diag> erc = ws::circsim::run_erc(s);
    CHECK(erc.empty());
    const ws::circsim::NetBuild nets = ws::circsim::build_nets(s);
    const auto deck = ws::circsim::export_deck(s, nets.nets);
    REQUIRE(deck.has_value());
    CHECK(deck->text ==
          "circsim export\n"
          ".model swmod SW(Ron=10 Roff=1meg Vt=2 Vh=0)\n"
          "R1 out 0 1k\n"
          "S1 in out ctl 0 swmod\n"
          "V1 in 0 5\n"
          "V2 ctl 0 0\n"
          ".dc V2 0 5 0.5\n"
          ".end\n");

    // The transfer curve: below Vt the contact is Roff (the 1k/1meg
    // divider, ~5 mV), above it Ron (5 V over 1010).
    const ws::circsim::RunOutcome run = ws::circsim::run_schematic(s);
    REQUIRE(run.run.has_value());
    const ws::spice::Table& t = run.run->results.tables[0];
    const auto v_out = t.signal("v(out)");
    const auto v2 = t.axis(0);
    REQUIRE(v_out.size() == v2.size());
    REQUIRE(v_out.size() == 11);
    CHECK(v_out[0] < 1e-2);   // V2 = 0: open (the Roff divider ~5 mV)
    CHECK(v_out[2] < 1e-2);   // V2 = 1: still open (Vt = 2)
    CHECK(v_out[10] == doctest::Approx(5.0 * 1000.0 / 1010.0).epsilon(1e-3));
}

TEST_CASE("the current switch decks its controlling source and model") {
    // The idiomatic W circuit: a DEDICATED 0 V source carries the
    // measured current (its branch current is positive in the loop's
    // flow direction - a delivering source's own i() is negative), and
    // W1 (It = 2 m, so 5 mA closes it) connects V2 5 V through R2 1k.
    Schematic s;
    Part* v1 = s.add_part(Part::make("v", "V1", "5", {0.0, -4.0}));
    Part* r1 = s.add_part(Part::make("r", "R1", "1k", {10.0, 0.0}));
    Part* vs = s.add_part(Part::make("v", "Vs", "0", {20.0, 0.0}));
    Part* w = s.add_part(Part::make("w", "W1", "", {28.0, 0.0}));
    w->params["vs"] = "Vs";
    w->params["model"] = "swmod";
    Part* r2 = s.add_part(Part::make("r", "R2", "1k", {36.0, 0.0}));
    Part* v2 = s.add_part(Part::make("v", "V2", "5", {0.0, -16.0}));
    Part* g1 = s.add_part(Part::make("gnd", "", "", {23.0, -3.0}));
    Part* g2 = s.add_part(Part::make("gnd", "", "", {39.0, -3.0}));
    Part* g3 = s.add_part(Part::make("gnd", "", "", {3.0, -19.0}));
    // The measuring loop: V1 up onto the axis, through R1, through the
    // 0 V sense source, down to ground.
    s.add_wire(Wire::endpoints(PinRef{v1->id, 0}, PinRef{r1->id, 0}))
        ->waypoints = {Pos{-3.0, 0.0}};
    s.add_wire(Wire::endpoints(PinRef{r1->id, 1}, PinRef{vs->id, 0}));
    s.add_wire(Wire::endpoints(PinRef{vs->id, 1}, PinRef{g1->id, 0}));
    s.add_wire(Wire::endpoints(PinRef{v1->id, 1}, PinRef{g3->id, 0}));
    // The switched branch: V2.p up from below, across to W1.p, W1.n on
    // to the load.
    s.add_wire(Wire::endpoints(PinRef{v2->id, 0}, PinRef{w->id, 0}))
        ->waypoints = {Pos{-3.0, -12.0}, Pos{25.0, -12.0}};
    s.add_wire(Wire::endpoints(PinRef{w->id, 1}, PinRef{r2->id, 0}));
    s.add_wire(Wire::endpoints(PinRef{r2->id, 1}, PinRef{g2->id, 0}));
    s.add_wire(Wire::endpoints(PinRef{v2->id, 1}, PinRef{g3->id, 0}));
    Label* in = s.add_label({});
    in->pos = Pos{2.0, 0.0};
    in->name = "in";
    Label* sens = s.add_label({});
    sens->pos = Pos{15.0, 0.0};
    sens->name = "sens";
    Label* out = s.add_label({});
    out->pos = Pos{32.0, 0.0};
    out->name = "out";
    Label* bus = s.add_label({});
    bus->pos = Pos{10.0, -12.0};
    bus->name = "bus";
    s.directives = ".model swmod SW(Ron=10 Roff=1meg It=2m Ih=0)";
    s.analysis.entries.push_back(OpSpec{});

    const std::vector<ws::circsim::Diag> erc = ws::circsim::run_erc(s);
    CHECK(erc.empty());
    const ws::circsim::NetBuild nets = ws::circsim::build_nets(s);
    const auto deck = ws::circsim::export_deck(s, nets.nets);
    REQUIRE(deck.has_value());
    CHECK(deck->text ==
          "circsim export\n"
          ".model swmod SW(Ron=10 Roff=1meg It=2m Ih=0)\n"
          "R1 in sens 1k\n"
          "R2 out 0 1k\n"
          "V1 in 0 5\n"
          "V2 bus 0 5\n"
          "Vs sens 0 0\n"
          "W1 bus out Vs swmod\n"
          ".op\n"
          ".end\n");

    const ws::circsim::RunOutcome run = ws::circsim::run_schematic(s);
    REQUIRE(run.run.has_value());
    const auto v_out = run.run->results.tables[0].signal("v(out)");
    REQUIRE(v_out.size() == 1);
    CHECK(v_out[0] == doctest::Approx(5.0 * 1000.0 / 1010.0).epsilon(1e-3));
}

// Two discrete inductors + the pinless k part, AC-driven: the loaded
// secondary answers k*sqrt(L2/L1) times the primary voltage.
Schematic coupled_stage() {
    Schematic s;
    Part* v = s.add_part(Part::make("v", "V1", "0", {0.0, -4.0}));
    v->params["ac"] = "1";
    Part* rs = s.add_part(Part::make("r", "Rs", "10", {4.0, 0.0}));
    Part* l1 = s.add_part(Part::make("l", "L1", "1m", {11.0, 0.0}));
    Part* l2 = s.add_part(Part::make("l", "L2", "10m", {24.0, 0.0}));
    Part* k = s.add_part(Part::make("k", "K1", "", {17.0, -6.0}));
    k->params["l1"] = "L1";
    k->params["l2"] = "L2";
    k->params["k"] = "0.9";
    Part* rl = s.add_part(Part::make("r", "Rl", "100k", {24.0, -8.0}));
    s.add_part(Part::make("gnd", "", "", {8.0, -4.0}));  // the primary return's pickup
    Part* g2 = s.add_part(Part::make("gnd", "", "", {27.0, -3.0}));
    Part* g3 = s.add_part(Part::make("gnd", "", "", {27.0, -11.0}));
    // The primary: V1.p up onto the axis, through Rs into L1.a (the
    // series R keeps V1 off the inductor - a 0-ohm V-L loop is the
    // graph check's own rejection); the return runs under into L1.b.
    s.add_wire(Wire::endpoints(PinRef{v->id, 0}, PinRef{rs->id, 0}))
        ->waypoints = {Pos{-3.0, 0.0}};
    s.add_wire(Wire::endpoints(PinRef{rs->id, 1}, PinRef{l1->id, 0}));
    s.add_wire(Wire::endpoints(PinRef{v->id, 1}, PinRef{l1->id, 1}))
        ->waypoints = {Pos{14.0, -4.0}};
    // The secondary: L2.a is the sec node into the load, L2.b grounds.
    s.add_wire(Wire::endpoints(PinRef{l2->id, 0}, PinRef{rl->id, 0}));
    s.add_wire(Wire::endpoints(PinRef{l2->id, 1}, PinRef{g2->id, 0}));
    s.add_wire(Wire::endpoints(PinRef{rl->id, 1}, PinRef{g3->id, 0}));
    Label* pri = s.add_label({});
    pri->pos = Pos{8.0, 0.0};
    pri->name = "pri";
    Label* sec = s.add_label({});
    sec->pos = Pos{21.0, 0.0};
    sec->name = "sec";
    AcSpec ac;
    ac.sweep = AcSpec::Sweep::Dec;
    ac.n = 5;
    ac.fstart = 1e3;
    ac.fstop = 100e3;
    s.analysis.entries.push_back(ac);
    return s;
}

TEST_CASE("the coupling part decks K and solves the mutual ratio") {
    const Schematic s = coupled_stage();
    const std::vector<ws::circsim::Diag> erc = ws::circsim::run_erc(s);
    CHECK(erc.empty());
    const ws::circsim::NetBuild nets = ws::circsim::build_nets(s);
    const auto deck = ws::circsim::export_deck(s, nets.nets);
    REQUIRE(deck.has_value());
    CHECK(deck->text ==
          "circsim export\n"
          "K1 L1 L2 0.9\n"
          "L1 pri 0 1m\n"
          "L2 sec 0 10m\n"
          "Rl sec 0 100k\n"
          "Rs N001 pri 10\n"
          "V1 N001 0 0 AC 1\n"
          ".ac dec 5 1000 100000\n"
          ".end\n");

    const ws::circsim::RunOutcome run = ws::circsim::run_schematic(s);
    REQUIRE(run.run.has_value());
    const ws::spice::Table& t = run.run->results.tables[0];
    const auto pri_re = t.signal("re(v(pri))");
    const auto pri_im = t.signal("im(v(pri))");
    const auto sec_re = t.signal("re(v(sec))");
    const auto sec_im = t.signal("im(v(sec))");
    REQUIRE(pri_re.size() == 11);
    REQUIRE(sec_re.size() == 11);
    // v(sec)/v(pri) = k*sqrt(L2/L1) = 0.9*sqrt(10) at every point (the
    // 100k load draws it down by w*L2/100k, under 0.3% across the band).
    const double want = 0.9 * std::sqrt(10.0);
    for (std::size_t i = 0; i < pri_re.size(); ++i) {
        const double pri_mag = std::hypot(pri_re[i], pri_im[i]);
        const double sec_mag = std::hypot(sec_re[i], sec_im[i]);
        REQUIRE(pri_mag > 1e-9);
        CHECK(sec_mag / pri_mag == doctest::Approx(want).epsilon(4e-3));
    }
}

TEST_CASE("the transformer part decks L + L + K and solves its ratio") {
    Schematic s;
    Part* v = s.add_part(Part::make("v", "V1", "0", {0.0, -4.0}));
    v->params["ac"] = "1";
    Part* rs = s.add_part(Part::make("r", "Rs", "10", {5.0, 0.0}));
    Part* x = s.add_part(Part::make("xfmr", "K1", "", {17.0, 0.0}));
    x->params["lp"] = "1m";
    x->params["ls"] = "4m";
    x->params["k"] = "0.99";
    Part* rl = s.add_part(Part::make("r", "Rl", "100k", {30.0, -6.0}));
    s.add_part(Part::make("gnd", "", "", {8.0, -2.0}));  // the primary return's pickup
    Part* g2 = s.add_part(Part::make("gnd", "", "", {22.0, -5.0}));
    Part* g3 = s.add_part(Part::make("gnd", "", "", {33.0, -9.0}));
    // The primary: V1.p up onto the axis, through Rs into p+ (the
    // series R keeps V1 off the winding - a 0-ohm V-L loop is the
    // graph check's own rejection); the return into p-.
    s.add_wire(Wire::endpoints(PinRef{v->id, 0}, PinRef{rs->id, 0}))
        ->waypoints = {Pos{-3.0, 0.0}};
    s.add_wire(Wire::endpoints(PinRef{rs->id, 1}, PinRef{x->id, 0}))
        ->waypoints = {Pos{8.0, 2.0}};
    s.add_wire(Wire::endpoints(PinRef{v->id, 1}, PinRef{x->id, 1}))
        ->waypoints = {Pos{3.0, -2.0}};
    // The secondary: s- grounded, s+ down into the load.
    s.add_wire(Wire::endpoints(PinRef{x->id, 3}, PinRef{g2->id, 0}));
    s.add_wire(Wire::endpoints(PinRef{x->id, 2}, PinRef{rl->id, 0}))
        ->waypoints = {Pos{27.0, 2.0}};
    s.add_wire(Wire::endpoints(PinRef{rl->id, 1}, PinRef{g3->id, 0}));
    Label* pri = s.add_label({});
    pri->pos = Pos{10.0, 2.0};
    pri->name = "pri";
    Label* sec = s.add_label({});
    sec->pos = Pos{24.0, 2.0};
    sec->name = "sec";
    AcSpec ac;
    ac.sweep = AcSpec::Sweep::Dec;
    ac.n = 5;
    ac.fstart = 1e3;
    ac.fstop = 100e3;
    s.analysis.entries.push_back(ac);

    const std::vector<ws::circsim::Diag> erc = ws::circsim::run_erc(s);
    CHECK(erc.empty());
    const ws::circsim::NetBuild nets = ws::circsim::build_nets(s);
    const auto deck = ws::circsim::export_deck(s, nets.nets);
    REQUIRE(deck.has_value());
    // One part, three cards: the inductors derive their names from the
    // part's ref (K1 -> L1p/L1s), and the coupling names its siblings.
    CHECK(deck->text ==
          "circsim export\n"
          "L1p pri 0 1m\n"
          "L1s sec 0 4m\n"
          "K1 L1p L1s 0.99\n"
          "Rl sec 0 100k\n"
          "Rs N001 pri 10\n"
          "V1 N001 0 0 AC 1\n"
          ".ac dec 5 1000 100000\n"
          ".end\n");
    // All three element names resolve onto the one part.
    REQUIRE(deck->element_part.count("K1") != 0);
    REQUIRE(deck->element_part.count("L1p") != 0);
    REQUIRE(deck->element_part.count("L1s") != 0);
    CHECK(deck->element_part.at("K1") == x->id);
    CHECK(deck->element_part.at("L1p") == x->id);
    CHECK(deck->element_part.at("L1s") == x->id);

    const ws::circsim::RunOutcome run = ws::circsim::run_schematic(s);
    REQUIRE(run.run.has_value());
    const ws::spice::Table& t = run.run->results.tables[0];
    const auto pri_re = t.signal("re(v(pri))");
    const auto pri_im = t.signal("im(v(pri))");
    const auto sec_re = t.signal("re(v(sec))");
    const auto sec_im = t.signal("im(v(sec))");
    REQUIRE(pri_re.size() == 11);
    // v(sec)/v(pri) = k*sqrt(ls/lp) = 0.99*2 (the load draws it down
    // by w*ls/100k, under 0.3% across the band).
    const double want = 0.99 * 2.0;
    for (std::size_t i = 0; i < pri_re.size(); ++i) {
        const double pri_mag = std::hypot(pri_re[i], pri_im[i]);
        const double sec_mag = std::hypot(sec_re[i], sec_im[i]);
        REQUIRE(pri_mag > 1e-9);
        CHECK(sec_mag / pri_mag == doctest::Approx(want).epsilon(4e-3));
    }
}

TEST_CASE("two transformers and a plain inductor never collide on names") {
    // Two K refs and a user inductor named like a derived one: the
    // dedup appends _2 and the coupling still names its true siblings.
    Schematic s;
    Part* v = s.add_part(Part::make("v", "V1", "0", {0.0, 0.0}));
    v->params["ac"] = "1";
    Part* x1 = s.add_part(Part::make("xfmr", "K1", "", {17.0, 0.0}));
    x1->params["lp"] = "1m";
    x1->params["ls"] = "4m";
    x1->params["k"] = "0.99";
    Part* x2 = s.add_part(Part::make("xfmr", "K1", "", {17.0, -20.0}));
    x2->params["lp"] = "1m";
    x2->params["ls"] = "4m";
    x2->params["k"] = "0.99";
    Part* lx = s.add_part(Part::make("l", "L1p", "1m", {40.0, 0.0}));
    (void)lx;
    (void)v;
    const ws::circsim::NetBuild nets = ws::circsim::build_nets(s);
    const auto deck = ws::circsim::export_deck(s, nets.nets);
    REQUIRE(deck.has_value());
    // K1 (first by id) claims K1/L1p/L1s; the second transformer's
    // names collide and shift by one suffix; the plain inductor named
    // L1p finds L1p and L1p_2 both taken and lands on L1p_3.
    CHECK(deck->text.find("K1 L1p L1s 0.99\n") != std::string::npos);
    CHECK(deck->text.find("K1_2 L1p_2 L1s_2 0.99\n") != std::string::npos);
    CHECK(deck->text.find("\nL1p_3 N") != std::string::npos);
}

// --- the hardening batch: value validation, locale-free numbers ----------------

TEST_CASE("invalid analysis values are rejected at the parser") {
    // The user's table, one row per line: every one of these used to
    // parse into a spec the engine would only reject (or accept
    // wrongly) at run time.
    CHECK_FALSE(ws::circsim::parse_analysis_spec("ac dec 0 1 100k").has_value());
    CHECK_FALSE(ws::circsim::parse_analysis_spec("ac dec 1.5 1 100k")
                    .has_value());
    CHECK_FALSE(ws::circsim::parse_analysis_spec("tran 0 0").has_value());
    CHECK_FALSE(ws::circsim::parse_analysis_spec("dc V1 5 5 0").has_value());
    CHECK_FALSE(ws::circsim::parse_analysis_spec("noise v( ) V1 dec 10 1 1k")
                    .has_value());
    CHECK_FALSE(ws::circsim::parse_analysis_spec("tran 1u 1m uic uic")
                    .has_value());
    // Direction: the step must point toward the stop.
    CHECK_FALSE(ws::circsim::parse_analysis_spec("dc V1 0 5 -1").has_value());
    CHECK_FALSE(ws::circsim::parse_analysis_spec("dc V1 5 0 1").has_value());
    // Frequencies: 0 < fstart <= fstop, both sweeps.
    CHECK_FALSE(ws::circsim::parse_analysis_spec("ac dec 10 0 1k").has_value());
    CHECK_FALSE(ws::circsim::parse_analysis_spec("ac dec 10 100k 1")
                    .has_value());
    CHECK_FALSE(ws::circsim::parse_analysis_spec("noise out V1 dec 10 1k 1")
                    .has_value());
    // tran: tstart in [0, tstop), tmax > 0.
    CHECK_FALSE(ws::circsim::parse_analysis_spec("tran 1u 1m 2m").has_value());
    CHECK_FALSE(ws::circsim::parse_analysis_spec("tran 1u 1m -1m").has_value());
    CHECK_FALSE(ws::circsim::parse_analysis_spec("tran 1u 1m 0 0")
                    .has_value());

    // And the edge cases that stay legal:
    // - tstep 0 = the tstop/100 card convention;
    // - a dc point sweep (start == stop, nonzero step);
    // - fstart == fstop (a single-frequency ac point);
    // - a bare noise node without the v(...) wrapper.
    const auto tran_auto = ws::circsim::parse_analysis_spec("tran 0 1m");
    REQUIRE(tran_auto.has_value());
    const auto* ta = std::get_if<TranSpec>(&*tran_auto);
    REQUIRE(ta != nullptr);
    CHECK(ta->tstep == 0.0);
    CHECK(ta->tstop == 1e-3);

    const auto dc_point = ws::circsim::parse_analysis_spec("dc V1 5 5 0.1");
    CHECK(dc_point.has_value());

    const auto ac_point = ws::circsim::parse_analysis_spec("ac lin 1 1k 1k");
    CHECK(ac_point.has_value());

    const auto noise_bare_node =
        ws::circsim::parse_analysis_spec("noise out V1 dec 10 1 100k");
    REQUIRE(noise_bare_node.has_value());
    const auto* nb = std::get_if<NoiseSpec>(&*noise_bare_node);
    REQUIRE(nb != nullptr);
    CHECK(nb->output == "out");

    // The shared validator behind both doors, on the spec values
    // themselves (the .csim loader's path).
    using ws::circsim::analysis_spec_problem;
    CHECK(analysis_spec_problem(ws::circsim::AnalysisSpec{OpSpec{}}) == std::nullopt);
    DcSpec bad_dc;
    bad_dc.source = "V1";
    bad_dc.start = 0.0;
    bad_dc.stop = 5.0;
    bad_dc.step = 0.0;
    CHECK(analysis_spec_problem(ws::circsim::AnalysisSpec{bad_dc}).has_value());
    NoiseSpec empty_out;  // defaults: valid numbers, no output node
    CHECK(analysis_spec_problem(ws::circsim::AnalysisSpec{empty_out}).has_value());
    TranSpec zero_stop;
    zero_stop.tstop = 0.0;
    CHECK(analysis_spec_problem(ws::circsim::AnalysisSpec{zero_stop}).has_value());
    AcSpec tiny_n;
    tiny_n.n = 0;
    CHECK(analysis_spec_problem(ws::circsim::AnalysisSpec{tiny_n}).has_value());
}

TEST_CASE("number text is locale-independent") {
    using ws::circsim::fmt_num;
    using ws::circsim::parse_plain_number;
    // The "C"-locale baseline (also the exact historical %.12g output,
    // which the golden decks pin).
    CHECK(fmt_num(0.1) == "0.1");
    CHECK(fmt_num(-0.000123) == "-0.000123");
    CHECK(fmt_num(1e-5) == "1e-05");
    CHECK(fmt_num(1e+21) == "1e+21");
    CHECK(fmt_num(100000.0) == "100000");
    CHECK(fmt_num(0.0) == "0");
    // Round trips through the parse (the std::atof replacement).
    for (const double v : {0.1, -2.5, 1e-7, 999.9, 123456.0}) {
        const auto back = parse_plain_number(fmt_num(v));
        REQUIRE(back.has_value());
        CHECK(*back == v);
    }
    CHECK_FALSE(parse_plain_number("").has_value());
    CHECK_FALSE(parse_plain_number("0.1 junk").has_value());
    CHECK(parse_plain_number("1e+05").value_or(0.0) == 1e5);

    // The point of the exercise: under a comma-decimal LC_NUMERIC the
    // old snprintf("%.12g") spells "0,1" and quietly corrupts decks.
    // Prove it when the runner has such a locale installed; skip
    // (loudly) when it does not.
    const char* tried[] = {"de_DE.UTF-8", "de_DE",   "fr_FR.UTF-8",
                           "fr_FR",       "it_IT.UTF-8", "it_IT"};
    const char* won = nullptr;
    for (const char* name : tried) {
        if (std::setlocale(LC_NUMERIC, name) != nullptr) {
            won = name;
            break;
        }
    }
    if (won == nullptr) {
        MESSAGE("no comma-decimal locale installed; the LC_NUMERIC proof "
                "is skipped (the 'C' baseline above still stands)");
        return;
    }
    MESSAGE("proving LC_NUMERIC independence under '", won, "'");
    CHECK(fmt_num(0.1) == "0.1");
    CHECK(fmt_num(4.7).find(',') == std::string::npos);
    // The deck itself: a schematic whose temp and analysis spell
    // decimals must export decimal-pointed text.
    Schematic s = divider();
    s.temp = 0.1;
    const ws::circsim::NetBuild nets = ws::circsim::build_nets(s);
    const auto deck = ws::circsim::export_deck(s, nets.nets);
    REQUIRE(deck.has_value());
    CHECK(deck->text.find(".options temp=0.1\n") != std::string::npos);
    CHECK(ws::circsim::format_eng(0.001) == "1m");
    CHECK(ws::circsim::format_eng(4.7) == "4.7");
    std::setlocale(LC_NUMERIC, "C");
}

// --- the built-in model library (stage 25) -----------------------------------
//
// A semiconductor part's `model` param names a card; when the name is one
// of the library's, export appends the card's text and the user never
// hand-writes the directives line. The shared shape below is the LED test's
// diode stage, minus the directives block.

namespace {

// 5 V over 1k into a diode: ~4.4 mA forward conduction for the 1N4148
// card (Vf ~0.6 V at that current).
Schematic library_diode_stage(const std::string& model) {
    Schematic s;
    Part* v = s.add_part(Part::make("v", "V1", "5", {0.0, -4.0}));
    Part* r = s.add_part(Part::make("r", "R1", "1k", {6.0, 0.0}));
    Part* d = s.add_part(Part::make("d", "D1", "", {14.0, 0.0}));
    d->params["model"] = model;
    Part* g = s.add_part(Part::make("gnd", "", "", {7.0, -6.0}));
    s.add_wire(ws::circsim::Wire::endpoints(PinRef{v->id, 0}, PinRef{r->id, 0}));
    s.add_wire(ws::circsim::Wire::endpoints(PinRef{r->id, 1}, PinRef{d->id, 0}));
    s.add_wire(ws::circsim::Wire::endpoints(PinRef{d->id, 1}, PinRef{g->id, 0}));
    s.add_wire(ws::circsim::Wire::endpoints(PinRef{g->id, 0}, PinRef{v->id, 1}));
    s.analysis.entries.push_back(OpSpec{});
    return s;
}

// The non-inverting stage the opamp_amp example rides: 1 V into in+,
// Rg 1k to ground, Rf 9k around - gain 1 + Rf/Rg = 10, solved by the
// ua741 card's linear macromodel (Avol 2e5, Ri 2meg, Ro 75; the sense
// supplies ride V2/V3 exactly like the shipped example).
Schematic opamp_stage() {
    Schematic s;
    Part* v1 = s.add_part(Part::make("v", "V1", "1", {-5.0, 2.0}));
    v1->rot = 180;
    Part* x = s.add_part(Part::make("opamp", "X1", "", {14.0, 0.0}));
    x->params["model"] = "ua741";
    Part* rf = s.add_part(Part::make("r", "Rf", "9k", {14.0, 8.0}));
    Part* rg = s.add_part(Part::make("r", "Rg", "1k", {9.0, -8.0}));
    rg->rot = 90;
    Part* v2 = s.add_part(Part::make("v", "V2", "12", {14.0, 11.0}));
    v2->rot = 90;
    Part* v3 = s.add_part(Part::make("v", "V3", "12", {14.0, -9.0}));
    v3->rot = 90;
    Part* g1 = s.add_part(Part::make("gnd", "", "", {-8.0, 0.0}));
    Part* g2 = s.add_part(Part::make("gnd", "", "", {9.0, -13.0}));
    Part* g3 = s.add_part(Part::make("gnd", "", "", {14.0, 16.0}));
    Part* g4 = s.add_part(Part::make("gnd", "", "", {14.0, -14.0}));
    s.add_wire(ws::circsim::Wire::endpoints(PinRef{v1->id, 0}, PinRef{x->id, 1}));
    s.add_wire(ws::circsim::Wire::endpoints(PinRef{x->id, 2}, PinRef{rg->id, 1}));
    s.add_wire(ws::circsim::Wire::endpoints(PinRef{rg->id, 0}, PinRef{g2->id, 0}));
    Wire* fb = s.add_wire(
        ws::circsim::Wire::endpoints(PinRef{rf->id, 0}, PinRef{x->id, 2}));
    fb->waypoints = {Pos{11.0, 6.0}, Pos{7.0, 6.0}, Pos{7.0, -2.0}};
    s.add_wire(ws::circsim::Wire::endpoints(PinRef{x->id, 0}, PinRef{rf->id, 1}))
        ->waypoints = {Pos{19.0, 8.0}};
    s.add_wire(ws::circsim::Wire::endpoints(PinRef{x->id, 3}, PinRef{v2->id, 0}));
    s.add_wire(ws::circsim::Wire::endpoints(PinRef{v2->id, 1}, PinRef{g3->id, 0}));
    s.add_wire(ws::circsim::Wire::endpoints(PinRef{x->id, 4}, PinRef{v3->id, 1}));
    s.add_wire(ws::circsim::Wire::endpoints(PinRef{v3->id, 0}, PinRef{g4->id, 0}));
    s.add_wire(ws::circsim::Wire::endpoints(PinRef{v1->id, 1}, PinRef{g1->id, 0}));
    Label* out = s.add_label({});
    out->pos = Pos{19.0, 2.0};
    out->name = "out";
    Label* tap = s.add_label({});
    tap->pos = Pos{9.0, -3.0};  // on the in- lead: the feedback node
    tap->name = "fb";
    s.analysis.entries.push_back(OpSpec{});
    return s;
}

// Runs deck text through ws::spice exactly as the CLI does (the sweep's
// engine - the card table's every entry must survive the real path).
ws::core::Result<ws::spice::RunOutput> run_spice_text(const std::string& deck) {
    const auto parsed =
        ws::spice::read_deck("lib.cir", deck, ws::spice::ParseOptions{});
    if (!parsed) {
        return ws::core::unexpected<ws::core::Error>(parsed.error());
    }
    std::vector<ws::spice::Warning> warnings;
    const auto ast = ws::spice::build_deck_ast(*parsed, warnings);
    if (!ast) {
        return ws::core::unexpected<ws::core::Error>(ast.error());
    }
    return ws::spice::run_deck(*ast, ws::spice::RunSettings{}, warnings);
}

// ASCII ci equality / upper (util.hpp's lower is private to the lib's
// TUs; the tests carry their own tiny fold).
bool ci_equal(const std::string& a, const std::string& b) {
    return !ws::circsim::CiLess{}(a, b) && !ws::circsim::CiLess{}(b, a);
}

std::string ci_upper(std::string s) {
    for (char& c : s) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return s;
}

}  // namespace

TEST_CASE("a part naming a library card decks it with no directives") {
    Schematic s = library_diode_stage("1n4148");
    const std::vector<ws::circsim::Diag> erc = ws::circsim::run_erc(s);
    CHECK(erc.empty());
    const ws::circsim::NetBuild nets = ws::circsim::build_nets(s);
    const auto deck = ws::circsim::export_deck(s, nets.nets);
    REQUIRE(deck.has_value());
    CHECK(deck->text ==
          "circsim export\n"
          ".model 1n4148 D(Is=4.35n N=1.6666 Rs=0.8456 Tt=4.4n"
          " Cjo=1.1p M=0.23 Vj=0.75 Bv=100 Ibv=100u)\n"
          "D1 N002 0 1n4148\n"
          "R1 N001 N002 1k\n"
          "V1 N001 0 5\n"
          ".op\n"
          ".end\n");
    // The card line carries the Library origin, attributed to the part
    // that named it (a bad card locates onto the pick that brought it
    // in); line 2 is the .model card, one under the title.
    REQUIRE(deck->line_origins.size() > 2);
    CHECK(deck->line_origins[1].kind ==
          ws::circsim::SpiceExport::Origin::Kind::Library);
    const Part* d1 = first_with_symbol(s, "d");
    REQUIRE(d1 != nullptr);
    CHECK(deck->line_origins[1].id == d1->id);

    // The run: ~4.4 mA forward (Vf ~0.6 V at that current, per the
    // card's own desc), the loop invariant against the source probe.
    const ws::circsim::RunOutcome out = ws::circsim::run_schematic(s);
    REQUIRE(out.run.has_value());
    REQUIRE(out.run->results.tables.size() == 1);
    const ws::spice::Table& t = out.run->results.tables.front();
    const auto v_d = t.signal("v(N002)");
    const auto i_v = t.signal("i(V1)");
    const auto i_d = t.signal("i(D1)");
    REQUIRE(v_d.size() == 1);
    CHECK(v_d[0] > 0.5);
    CHECK(v_d[0] < 0.7);
    CHECK(i_v[0] < -4.0e-3);
    CHECK(i_v[0] > -4.6e-3);
    CHECK(i_d[0] == doctest::Approx(-i_v[0]).epsilon(1e-9));
}

TEST_CASE("the directives' own card wins over the library") {
    // The escape hatch stays an override: a name the user defined in the
    // directives block is skipped by the library's emission, so exactly
    // ONE card of that name rides the deck - the user's.
    Schematic s = library_diode_stage("1n4148");
    s.directives = ".model 1n4148 D(Is=1e-12 N=1)";
    const ws::circsim::NetBuild nets = ws::circsim::build_nets(s);
    const auto deck = ws::circsim::export_deck(s, nets.nets);
    REQUIRE(deck.has_value());
    CHECK(deck->text ==
          "circsim export\n"
          ".model 1n4148 D(Is=1e-12 N=1)\n"
          "D1 N002 0 1n4148\n"
          "R1 N001 N002 1k\n"
          "V1 N001 0 5\n"
          ".op\n"
          ".end\n");
    // The directives line carries the Directives origin, not Library's.
    CHECK(deck->line_origins[1].kind ==
          ws::circsim::SpiceExport::Origin::Kind::Directives);
}

TEST_CASE("a wrong-kind library name still decks, and the engine maps it back") {
    // Emission is by NAME, not kind (ERC flagged the mismatch already);
    // ws::spice's own type check rejects the pairing, and the Library
    // line's attribution lands the diagnostic on the part that named
    // the card - the picker's pick, not a mystery deck line.
    Schematic s = library_diode_stage("2n3904");
    const ws::circsim::NetBuild nets = ws::circsim::build_nets(s);
    const auto deck = ws::circsim::export_deck(s, nets.nets);
    REQUIRE(deck.has_value());
    CHECK(deck->text.find(".model 2n3904 NPN(") != std::string::npos);
    CHECK(deck->text.find("D1 N002 0 2n3904\n") != std::string::npos);

    const ws::circsim::RunOutcome out = ws::circsim::run_schematic(s);
    CHECK_FALSE(out.run.has_value());
    bool mapped = false;
    for (const ws::circsim::Diag& d : out.problems) {
        if (d.code == "circsim.run.failed" && d.where == "D1") {
            mapped = true;
        }
    }
    CHECK(mapped);
}

TEST_CASE("the op amp decks its subckt card and solves at its gain") {
    const Schematic s = opamp_stage();
    const std::vector<ws::circsim::Diag> erc = ws::circsim::run_erc(s);
    CHECK(erc.empty());
    const ws::circsim::NetBuild nets = ws::circsim::build_nets(s);
    const auto deck = ws::circsim::export_deck(s, nets.nets);
    REQUIRE(deck.has_value());
    // The multi-line .subckt card rides whole, seven lines under the
    // title; every line carries the Library origin.
    CHECK(deck->text.substr(0, deck->text.find("Rf ")) ==
          "circsim export\n"
          ".subckt ua741 out inp inn vp vm\n"
          "Ri inp inn 2meg\n"
          "Eg g1 0 inp inn 1\n"
          "Rg g1 g2 1meg\n"
          "Cg g2 0 31.8n\n"
          "Eo b1 0 g2 0 2e5\n"
          "Ro b1 out 75\n"
          ".ends\n");
    for (std::size_t i = 1; i <= 7; ++i) {
        CHECK(deck->line_origins[i].kind ==
              ws::circsim::SpiceExport::Origin::Kind::Library);
    }
    // The instance card: out in+ in- v+ v- then the subckt name.
    const std::size_t x_at = deck->text.find("X1 out ");
    CHECK(x_at != std::string::npos);
    const std::string x_line = deck->text.substr(
        x_at, deck->text.find('\n', x_at) - x_at);
    CHECK(x_line.find(" fb ") != std::string::npos);
    CHECK(x_line.substr(x_line.size() - 6) == " ua741");

    // The closed form: Vout = 10*Avol/(10+Avol) (the finite-gain
    // answer for 1+Rf/Rg = 10), the feedback node rides the divider
    // Vout/10, and the input draws only the Ri leakage.
    const ws::circsim::RunOutcome out = ws::circsim::run_schematic(s);
    REQUIRE(out.run.has_value());
    REQUIRE(out.run->results.tables.size() == 1);
    const ws::spice::Table& t = out.run->results.tables.front();
    const auto v_out = t.signal("v(out)");
    const auto v_fb = t.signal("v(fb)");
    const auto i_v1 = t.signal("i(V1)");
    REQUIRE(v_out.size() == 1);
    const double avol = 2.0e5;
    CHECK(v_out[0] == doctest::Approx(10.0 * avol / (10.0 + avol)).epsilon(1e-5));
    CHECK(v_fb[0] == doctest::Approx(v_out[0] / 10.0).epsilon(1e-6));
    CHECK(std::abs(i_v1[0]) < 1e-9);
}

TEST_CASE("the model library table is well-formed and every card runs") {
    using ws::circsim::builtin_models;
    using ws::circsim::builtin_models_of_kind;
    using ws::circsim::find_builtin_model;

    // The four requested families all carry cards, and so do the
    // JFETs and switches the shelf grew.
    for (const char* kind :
         {"d", "npn", "pnp", "nmos", "pmos", "njf", "pjf", "sw", "opamp"}) {
        CHECK_FALSE(builtin_models_of_kind(kind).empty());
    }
    CHECK(builtin_models_of_kind("no-such-kind").empty());

    std::map<std::string, int> seen;  // ci names are unique
    for (const ws::circsim::ModelCard& c : builtin_models()) {
        ++seen[c.name];
        // The lookup answers for every card, ci.
        const ws::circsim::ModelCard* found = find_builtin_model(c.name);
        REQUIRE(found != nullptr);
        CHECK(found->name == c.name);
        CHECK(find_builtin_model(ci_upper(c.name)) != nullptr);
        // The shelf of its own kind carries it.
        bool on_shelf = false;
        for (const ws::circsim::ModelCard* k : builtin_models_of_kind(c.kind)) {
            if (k->name == c.name) {
                on_shelf = true;
            }
        }
        CHECK(on_shelf);
        // The card text is a .model of its own TYPE (or an opamp
        // .subckt): the kind a part must have to name it.
        if (c.kind == "opamp") {
            CHECK(c.text.rfind(".subckt ", 0) == 0);
        } else {
            const std::string head = ".model " + c.name + " ";
            CHECK(c.text.rfind(head, 0) == 0);
            const std::size_t type_at = c.text.find('(', head.size());
            REQUIRE(type_at != std::string::npos);
            const std::string type = c.text.substr(head.size(), type_at - head.size());
            CHECK(ci_equal(type, c.kind));
        }
    }
    for (const auto& [name, count] : seen) {
        CHECK(count == 1);
    }

    // THE PROMISE: every card runs through its own device - a minimal
    // but honest stage per kind, straight through ws::spice's public
    // path. A card the engine cannot back never ships.
    const auto instance = [](const std::string& kind,
                             const std::string& name) {
        if (kind == "d") {
            return "V1 a 0 5\nR1 a b 1k\nD1 b 0 " + name + "\n";
        }
        if (kind == "npn" || kind == "pnp") {
            return "V1 a 0 5\nR1 a b 1k\nQ1 b b 0 " + name + "\n";
        }
        if (kind == "nmos" || kind == "pmos") {
            return "V1 a 0 5\nR1 a b 1k\nM1 b b 0 0 " + name + "\n";
        }
        if (kind == "njf" || kind == "pjf") {
            return "V1 a 0 5\nR1 a b 1k\nJ1 b 0 0 " + name + "\n";
        }
        if (kind == "sw") {
            return "V1 a 0 5\nR1 a b 1k\nS1 b 0 a 0 " + name + "\n";
        }
        return "V1 in 0 1\nX1 out in out 12 0 " + name + "\n";  // opamp
    };
    for (const ws::circsim::ModelCard& c : builtin_models()) {
        const std::string deck =
            "card sweep\n" + instance(c.kind, c.name) + c.text + "\n.op\n.end";
        const auto run = run_spice_text(deck);
        REQUIRE_MESSAGE(run.has_value(),
                        (c.name + ": " +
                         (run.has_value() ? std::string()
                                          : run.error().message))
                            .c_str());
        REQUIRE(run->results.tables.size() == 1);
    }
}
