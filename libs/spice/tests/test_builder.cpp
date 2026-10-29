// The builder/Simulator/netlist-writer gate (DESIGN.md §2.1 rev, §10.1):
// the C++ front end IS the elaborator's second client - these tests pin
// that. No DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here - test_spice.cpp owns
// doctest's single implementation TU for this binary.

#include <cmath>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include <ws/spice/analysis.hpp>
#include <ws/spice/builder.hpp>
#include <ws/spice/circuit.hpp>
#include <ws/spice/netlist.hpp>
#include <ws/spice/parse.hpp>
#include <ws/spice/run.hpp>
#include <ws/spice/simulator.hpp>
#include <ws/spice/units.hpp>

using namespace ws::spice::literals;
using ws::spice::AcAnalysis;
using ws::spice::AcSpec;
using ws::spice::ExpSource;
using ws::spice::Pulse;
using ws::spice::Pwl;
using ws::spice::Sin;
using ws::spice::Analysis;
using ws::spice::CircuitBuilder;
using ws::spice::DcAnalysis;
using ws::spice::OpAnalysis;
using ws::spice::Simulator;
using ws::spice::Table;
using ws::spice::TranAnalysis;
using ws::spice::gnd;

namespace {

doctest::Approx approx(double v) { return doctest::Approx(v).epsilon(1e-9); }

// Runs deck text through the netlist front end (the equivalence oracle).
ws::core::Result<ws::spice::RunOutput> run_deck_text(const std::string& deck) {
    const ws::core::Result<ws::spice::Deck> parsed =
        ws::spice::read_deck("t.cir", deck, ws::spice::ParseOptions{});
    if (!parsed) {
        return ws::core::unexpected(parsed.error());
    }
    std::vector<ws::spice::Warning> w;
    const ws::core::Result<ws::spice::DeckAst> ast = ws::spice::build_deck_ast(*parsed, w);
    if (!ast) {
        return ws::core::unexpected(ast.error());
    }
    return ws::spice::run_deck(*ast, ws::spice::RunSettings{}, w);
}

// Elaborates deck text to a Circuit (for round-trip tests).
ws::core::Result<ws::spice::Circuit> circuit_from_deck(const std::string& deck) {
    const ws::core::Result<ws::spice::Deck> parsed =
        ws::spice::read_deck("t.cir", deck, ws::spice::ParseOptions{});
    if (!parsed) {
        return ws::core::unexpected(parsed.error());
    }
    std::vector<ws::spice::Warning> w;
    const ws::core::Result<ws::spice::DeckAst> ast = ws::spice::build_deck_ast(*parsed, w);
    if (!ast) {
        return ws::core::unexpected(ast.error());
    }
    return ws::spice::elaborate_deck(*ast, ws::spice::kDefaultTemp, w);
}

// Two signal columns agree elementwise within `eps` (and have equal
// lengths).
bool columns_agree(std::span<const double> a, std::span<const double> b, double eps) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::abs(a[i] - b[i]) > eps) {
            return false;
        }
    }
    return true;
}

}  // namespace

// --- the review's sketch, verbatim in spirit --------------------------------------

TEST_CASE("builder: the review's RC example end to end") {
    CircuitBuilder c;
    auto in = c.node("in"), out = c.node("out");
    auto R1 = c.resistor("R1", in, out, 10_k);
    (void)c.capacitor("C1", out, gnd, 100_n);
    (void)c.vsource("V1", in, gnd, Pulse{.v2 = 5, .tr = 1_n, .pw = 1_ms});

    auto circuit = c.finish();
    REQUIRE(circuit);
    auto sim = Simulator::create(std::move(*circuit));
    REQUIRE(sim);
    auto res = sim->run(TranAnalysis{.tstop = 5_ms});
    REQUIRE(res);
    CHECK(res->row_count() > 10);

    // Handle-based change: re-stamps values only, same pattern.
    REQUIRE(sim->set(R1.value, 22_k));
    auto again = sim->run(TranAnalysis{.tstop = 5_ms});
    REQUIRE(again);
    CHECK(again->row_count() == res->row_count());
    // tau = 22k * 100n = 2.2us now: the 1ms plateau sample settles slower
    // but reaches 5 V either way; discriminate via an early point.
    const double t_early = 0.5e-3;
    std::size_t i_early = 0;
    for (std::size_t i = 0; i < again->axis().size(); ++i) {
        if (std::abs(again->axis()[i] - t_early) <
            std::abs(again->axis()[i_early] - t_early)) {
            i_early = i;
        }
    }
    CHECK(again->signal("v(out)")[i_early] < res->signal("v(out)")[i_early]);
}

// --- builder <-> netlist equivalence (the free cross-check) -----------------------

TEST_CASE("equivalence: builder circuit vs its written netlist") {
    CircuitBuilder c;
    auto in = c.node("in"), out = c.node("out");
    (void)c.resistor("R1", in, out, 10_k);
    (void)c.capacitor("C1", out, gnd, 100_n);
    (void)c.vsource("V1", in, gnd, Pulse{.v2 = 5, .tr = 1_n, .pw = 1_ms});
    auto circuit = c.finish();
    REQUIRE(circuit);

    auto sim = Simulator::create(std::move(*circuit));
    REQUIRE(sim);
    auto built = sim->run(TranAnalysis{.tstop = 5_ms});
    REQUIRE(built);

    // Write the SAME circuit (pre-move state would need a second build;
    // rebuild for the writer) and run it through the netlist front end.
    CircuitBuilder c2;
    auto in2 = c2.node("in"), out2 = c2.node("out");
    (void)c2.resistor("R1", in2, out2, 10_k);
    (void)c2.capacitor("C1", out2, gnd, 100_n);
    (void)c2.vsource("V1", in2, gnd, Pulse{.v2 = 5, .tr = 1_n, .pw = 1_ms});
    auto circuit2 = c2.finish();
    REQUIRE(circuit2);
    auto bound = ws::spice::RunState::bind(*circuit2);
    REQUIRE(bound);
    const auto netlist = ws::spice::write_netlist(*circuit2, *bound);
    REQUIRE(netlist);
    // The writer emits a terminating .end; splice the analysis in front of
    // it (a deck handed to a reference simulator carries its own analyses).
    std::string deck_text = *netlist;
    const std::size_t end_at = deck_text.rfind(".end\n");
    REQUIRE(end_at != std::string::npos);
    deck_text.insert(end_at, ".tran 50u 5m\n");
    const auto deck_run = run_deck_text(deck_text);
    REQUIRE(deck_run);
    REQUIRE(deck_run->results.tables.size() == 1);

    // Same time grid (identical cards -> identical stepping) and same
    // v(out) to the last bit of the CSV precision.
    const Table& net = deck_run->results.tables[0];
    REQUIRE(net.row_count() == built->row_count());
    CHECK(columns_agree(net.signal("v(out)"), built->signal("v(out)"), 1e-9));
    CHECK(columns_agree(net.signal("v(in)"), built->signal("v(in)"), 1e-9));
}

TEST_CASE("equivalence: deck-built circuit round-trips through the writer") {
    const std::string deck =
        "round trip\n"
        ".param rl=2k\n"
        "V1 in 0 DC 3 AC 1\n"
        "R1 in out 1k\n"
        "C1 out 0 1u\n"
        "L1 out 0 1m Rser=1\n"
        "E1 dup 0 out 0 2\n"
        "G1 dup 0 in 0 0.5\n"
        "F1 0 sink V1 3\n"
        "R2 sink 0 100\n"
        "H1 hx 0 V1 50\n"
        "R3 hx 0 1k\n"
        "S1 in sw ctl 0 swm\n"
        "R4 sw 0 10k\n"
        "Vc ctl 0 PWL(0 0 1u 1m)\n"
        ".model swm SW(Ron=1 Roff=1meg Vt=0.5)\n";
    auto circuit = circuit_from_deck(deck);
    REQUIRE(circuit);
    auto bound = ws::spice::RunState::bind(*circuit);
    REQUIRE(bound);
    const auto netlist = ws::spice::write_netlist(*circuit, *bound);
    REQUIRE(netlist);

    // The written deck must re-parse (title line prepended) and elaborate.
    auto reparsed = circuit_from_deck(*netlist);
    REQUIRE(reparsed);
    CHECK(reparsed->devices.size() == circuit->devices.size());
    CHECK(reparsed->node_count() == circuit->node_count());

    // And solve to the same operating point.
    auto sim_a = Simulator::create(std::move(*circuit));
    REQUIRE(sim_a);
    auto sim_b = Simulator::create(std::move(*reparsed));
    REQUIRE(sim_b);
    auto op_a = sim_a->run(OpAnalysis{});
    auto op_b = sim_b->run(OpAnalysis{});
    REQUIRE(op_a);
    REQUIRE(op_b);
    for (const ws::spice::Signal& sig : op_a->signals) {
        const std::span<const double> other = op_b->signal(sig.name);
        REQUIRE(other.size() == 1);
        CHECK(sig.data[0] == doctest::Approx(other[0]).epsilon(1e-9));
    }
}

TEST_CASE("builder: subckt-free flatten still validates topology") {
    // A floating node through capacitors only: the §6.6 check fires from
    // the builder path exactly as from the deck path.
    CircuitBuilder c;
    auto a = c.node("a");
    (void)c.vsource("V1", a, gnd, 5.0);
    (void)c.capacitor("C1", a, c.node("float"), 1_n);
    auto circuit = c.finish();
    REQUIRE(!circuit);
    CHECK(circuit.error().message.find("no DC path to ground") != std::string::npos);
}

TEST_CASE("builder: handle set() drives the stamped value") {
    CircuitBuilder c;
    auto in = c.node("in"), mid = c.node("mid");
    auto R1 = c.resistor("R1", in, mid, 1_k);
    auto R2 = c.resistor("R2", mid, gnd, 1_k);
    (void)c.vsource("V1", in, gnd, 4.0);
    auto circuit = c.finish();
    REQUIRE(circuit);
    auto sim = Simulator::create(std::move(*circuit));
    REQUIRE(sim);

    auto op = sim->run(OpAnalysis{});
    REQUIRE(op);
    CHECK(op->signal("v(mid)")[0] == approx(2.0));

    // Value-only change: pattern untouched, answers move.
    REQUIRE(sim->set(R1.value, 3_k));
    op = sim->run(OpAnalysis{});
    REQUIRE(op);
    CHECK(op->signal("v(mid)")[0] == approx(1.0));  // 3k:1k divider

    // .param-scoped values rebind on demand (the .step mechanism).
    REQUIRE(sim->set(R2.param("value"), 3_k));
    op = sim->run(OpAnalysis{});
    REQUIRE(op);
    CHECK(op->signal("v(mid)")[0] == approx(2.0));

    // A bad handle names itself.
    const auto bad = sim->set({"R9", "value"}, 1.0);
    REQUIRE(!bad);
    CHECK(bad.error().message.find("R9") != std::string::npos);
}

TEST_CASE("builder: ExprVal devices rebind through the scope") {
    CircuitBuilder c;
    c.param("rl", "2k");
    c.param("vdd", "5");
    auto in = c.node("in");
    auto Rl = c.resistor("Rl", in, gnd, ws::spice::ExprVal{"rl"});
    (void)c.vsource("V1", in, gnd, ws::spice::ExprVal{"vdd"});
    auto circuit = c.finish();
    REQUIRE(circuit);
    auto sim = Simulator::create(std::move(*circuit));
    REQUIRE(sim);

    // Scope values: v = 5 * 2k / (2k) ... Rl=2k to ground, so v(in)=5.
    auto op = sim->run(OpAnalysis{});
    REQUIRE(op);
    CHECK(op->signal("v(in)")[0] == approx(5.0));

    // A .step-style override: rl -> 4k halves the current, not the
    // voltage; rebind with an overridden table and re-run.
    ws::spice::ParamTable scope;
    ws::spice::ParamDef def;
    def.name.text = "rl";
    def.expr = ws::spice::ExprProgram::constant(4e3);
    scope.define(def);
    ws::spice::ParamDef vdef;
    vdef.name.text = "vdd";
    vdef.expr = ws::spice::ExprProgram::constant(5.0);
    scope.define(vdef);
    REQUIRE(sim->rebind(&scope));
    op = sim->run(OpAnalysis{});
    REQUIRE(op);
    CHECK(op->signal("v(in)")[0] == approx(5.0));
    CHECK(op->signal("i(V1)")[0] == approx(-5.0 / 4e3));

    // The handle still addresses the same slot post-rebind.
    REQUIRE(sim->set(Rl.value, 1_k));
    op = sim->run(OpAnalysis{});
    REQUIRE(op);
    CHECK(op->signal("i(V1)")[0] == approx(-5.0 / 1e3));
}

TEST_CASE("simulator: set_source pins over the waveform, release restores") {
    CircuitBuilder c;
    auto in = c.node("in"), out = c.node("out");
    (void)c.resistor("R1", in, out, 1_k);
    (void)c.resistor("R2", out, gnd, 1_k);
    (void)c.vsource("V1", in, gnd, Pulse{.v1 = 0, .v2 = 5, .td = 1_ms, .tr = 1_u, .pw = 10_ms});
    auto circuit = c.finish();
    REQUIRE(circuit);
    auto sim = Simulator::create(std::move(*circuit));
    REQUIRE(sim);

    // Unpinned: the waveform drives; at t < td the source is 0.
    auto early = sim->run(TranAnalysis{.tstop = 100_u});
    REQUIRE(early);
    CHECK(early->signal("v(in)")[0] == approx(0.0));

    // Pinned: the DC value overrides the waveform.
    REQUIRE(sim->set_source("V1", 2.5));
    auto pinned = sim->run(TranAnalysis{.tstop = 100_u});
    REQUIRE(pinned);
    CHECK(pinned->signal("v(in)")[0] == approx(2.5));

    // Released: the waveform drives again.
    REQUIRE(sim->release_source("V1"));
    auto released = sim->run(TranAnalysis{.tstop = 100_u});
    REQUIRE(released);
    CHECK(released->signal("v(in)")[0] == approx(0.0));
}

TEST_CASE("simulator: snapshot/restore rewinds a run") {
    CircuitBuilder c;
    auto in = c.node("in"), out = c.node("out");
    (void)c.resistor("R1", in, out, 1_k);
    (void)c.capacitor("C1", out, gnd, 1_u);
    (void)c.vsource("V1", in, gnd, 5.0);
    auto circuit = c.finish();
    REQUIRE(circuit);
    auto sim = Simulator::create(std::move(*circuit));
    REQUIRE(sim);

    const ws::spice::RunState snap = sim->snapshot();
    auto a = sim->run(TranAnalysis{.tstop = 1_ms});
    REQUIRE(a);
    auto b = sim->run(TranAnalysis{.tstop = 1_ms});
    REQUIRE(b);
    // Deterministic: two runs from the same state agree bit-for-bit.
    REQUIRE(a->row_count() == b->row_count());
    CHECK(columns_agree(a->signal("v(out)"), b->signal("v(out)"), 0.0));

    // Restore the pristine state and verify a third run still matches.
    sim->restore(snap);
    auto c2 = sim->run(TranAnalysis{.tstop = 1_ms});
    REQUIRE(c2);
    CHECK(columns_agree(a->signal("v(out)"), c2->signal("v(out)"), 0.0));
}

TEST_CASE("simulator: Dc/Ac aggregates match the card front end") {
    CircuitBuilder c;
    auto in = c.node("in"), out = c.node("out");
    (void)c.vsource("V1", in, gnd, 1.0, ws::spice::AcSpec{1.0, 0.0});
    (void)c.resistor("R1", in, out, 1_k);
    (void)c.capacitor("C1", out, gnd, 1_u);
    auto circuit = c.finish();
    REQUIRE(circuit);
    auto sim = Simulator::create(std::move(*circuit));
    REQUIRE(sim);

    auto dc = sim->run(DcAnalysis{.source = "V1", .start = 0.0, .stop = 2.0, .step = 1.0});
    REQUIRE(dc);
    REQUIRE(dc->row_count() == 3);
    CHECK(dc->signal("v(in)")[2] == approx(2.0));

    auto ac = sim->run(AcAnalysis{.n = 2, .fstart = 1_k, .fstop = 100_k});
    REQUIRE(ac);
    // RC pole at ~159 Hz: |v(out)| at 1 kHz = 1/sqrt(1 + (2pi*1e3)^2*
    // (1e-3)^2)... compute against the closed form.
    const double w = 2.0 * 3.14159265358979 * 1e3;
    const double x = w * 1e-3;
    const double expect_re = 1.0 / (1.0 + x * x);
    CHECK(ac->signal("re(v(out))")[0] == doctest::Approx(expect_re).epsilon(1e-6));
}

TEST_CASE("units: the SPICE scale factors as literals") {
    using namespace ws::spice::literals;
    CHECK(1.5_k == approx(1.5e3));
    CHECK(10_k == approx(10e3));
    CHECK(2_meg == approx(2e6));
    CHECK(100_n == approx(100e-9));
    CHECK(5_ms == approx(5e-3));
    CHECK(1_u == approx(1e-6));
    CHECK(22_k == approx(22e3));
    CHECK(3_T == approx(3e12));
    CHECK(7_f == approx(7e-15));
    CHECK(47_p == approx(47e-12));
    CHECK(1_G == approx(1e9));
}

// --- the steppable transient (§6 rev) ---------------------------------------------

TEST_CASE("transient: advance_to marches in whole steps to the target") {
    CircuitBuilder c;
    auto in = c.node("in"), out = c.node("out");
    (void)c.resistor("R1", in, out, 1_k);
    (void)c.capacitor("C1", out, gnd, 1_u);
    (void)c.vsource("V1", in, gnd, Pulse{.v1 = 0, .v2 = 5, .td = 100_u, .tr = 1_u,
                                         .pw = 10_ms});
    auto circuit = c.finish();
    REQUIRE(circuit);
    auto sim = Simulator::create(std::move(*circuit));
    REQUIRE(sim);

    auto run = sim->begin_transient(TranAnalysis{.tstep = 10_u, .tstop = 1_ms});
    REQUIRE(run);
    CHECK(run->now() == approx(0.0));
    CHECK(!run->done());

    // Advance in ten 100us chunks (integer counters: accumulated floats
    // drift below `now` and freeze the march). A step is never SPLIT to
    // land on the target exactly, so now() may overshoot by up to one
    // whole step (dtmax = tstep = 10us here).
    for (int k = 1; k <= 10; ++k) {
        const double t = static_cast<double>(k) * 100_u;
        REQUIRE(run->advance_to(t));
        CHECK(run->now() <= t + 10_u + 1e-12);
        CHECK(run->now() > t - 30_u);
    }
    CHECK(run->done());

    // The stepped table equals a one-shot run's table bit-for-bit: same
    // card, same stepping logic, only the driving loop differs.
    auto oneshot = sim->run(TranAnalysis{.tstep = 10_u, .tstop = 1_ms});
    REQUIRE(oneshot);
    const Table stepped = run->table();
    REQUIRE(stepped.row_count() == oneshot->row_count());
    CHECK(columns_agree(stepped.signal("v(out)"), oneshot->signal("v(out)"), 0.0));
    CHECK(columns_agree(stepped.signal("v(in)"), oneshot->signal("v(in)"), 0.0));
}

TEST_CASE("transient: stop token cancels promptly and resumably") {
    CircuitBuilder c;
    auto in = c.node("in"), out = c.node("out");
    (void)c.resistor("R1", in, out, 1_k);
    (void)c.capacitor("C1", out, gnd, 1_u);
    (void)c.vsource("V1", in, gnd, Pulse{.v1 = 0, .v2 = 5, .td = 100_u, .tr = 1_u,
                                         .pw = 10_ms});
    auto circuit = c.finish();
    REQUIRE(circuit);
    auto sim = Simulator::create(std::move(*circuit));
    REQUIRE(sim);

    auto run = sim->begin_transient(TranAnalysis{.tstep = 10_u, .tstop = 1_ms});
    REQUIRE(run);

    ws::spice::StopToken token;
    run->set_stop_token(token);
    REQUIRE(run->advance_to(200_u));
    token.request_stop();
    REQUIRE(run->advance_to(1_ms));
    CHECK(run->cancelled());
    CHECK(!run->done());
    CHECK(run->now() < 400_u);  // stopped soon after the request

    // Resume to the end: the run continues from the same state and lands
    // on the SAME trajectory a one-shot run takes (bit-for-bit).
    ws::spice::StopToken fresh;
    run->set_stop_token(fresh);
    REQUIRE(run->advance_to(1_ms));
    CHECK(run->done());
    auto oneshot = sim->run(TranAnalysis{.tstep = 10_u, .tstop = 1_ms});
    REQUIRE(oneshot);
    const Table resumed = run->table();
    REQUIRE(resumed.row_count() == oneshot->row_count());
    CHECK(columns_agree(resumed.signal("v(out)"), oneshot->signal("v(out)"), 0.0));
}

TEST_CASE("transient: closed-loop set_source between steps") {
    // A bang-bang controller around 2 V: set_source() between advance_to
    // calls drives the very next step - the embedded-control pattern.
    CircuitBuilder c;
    auto in = c.node("in"), out = c.node("out");
    (void)c.resistor("R1", in, out, 100.0);
    (void)c.capacitor("C1", out, gnd, 1_u);
    (void)c.vsource("V1", in, gnd, 0.0);
    auto circuit = c.finish();
    REQUIRE(circuit);
    auto sim = Simulator::create(std::move(*circuit));
    REQUIRE(sim);

    auto run = sim->begin_transient(TranAnalysis{.tstep = 10_u, .tstop = 2_ms});
    REQUIRE(run);
    const auto node_id = [&sim](const char* name) {
        for (std::size_t i = 0; i < sim->circuit().node_count(); ++i) {
            if (sim->circuit().node_names[i] == name) {
                return i;
            }
        }
        return std::size_t{0};
    };
    const std::size_t out_id = node_id("out");
    int flips = 0;
    bool high = false;
    for (int k = 1; k <= 40; ++k) {
        const double t = static_cast<double>(k) * 50_u;
        const bool want = run->solution()[out_id] < 2.0;
        if (want != high) {
            ++flips;
            high = want;
        }
        REQUIRE(sim->set_source("V1", high ? 5.0 : 0.0));
        REQUIRE(run->advance_to(t));
        if (run->done()) {
            break;
        }
    }
    // The controller switched repeatedly and regulates around 2 V.
    CHECK(flips > 5);
    const double vfinal = run->solution()[out_id];
    CHECK(vfinal < 5.0);
    CHECK(vfinal > 0.5);
}

TEST_CASE("transient: progress callback observes every accepted step") {
    CircuitBuilder c;
    auto in = c.node("in"), out = c.node("out");
    (void)c.resistor("R1", in, out, 1_k);
    (void)c.capacitor("C1", out, gnd, 1_u);
    (void)c.vsource("V1", in, gnd, 5.0);
    auto circuit = c.finish();
    REQUIRE(circuit);
    auto sim = Simulator::create(std::move(*circuit));
    REQUIRE(sim);

    auto run = sim->begin_transient(TranAnalysis{.tstep = 10_u, .tstop = 500_u});
    REQUIRE(run);

    std::vector<double> seen;
    run->set_progress([&](double now, double tstop) {
        seen.push_back(now);
        CHECK(tstop == approx(500e-6));
    });
    REQUIRE(run->advance_to(500_u));
    CHECK(run->done());
    // Every accepted sample (including t=0's row? no - accepts only) and
    // monotonic times.
    REQUIRE(seen.size() >= 5);
    for (std::size_t i = 1; i < seen.size(); ++i) {
        CHECK(seen[i] > seen[i - 1]);
    }
    CHECK(seen.back() == approx(500e-6));
}
