// The M1 end-to-end suite (DESIGN.md §10.1 rows for .op/.dc, §10.4
// parse-error goldens). Decks run through the public API exactly as the
// CLI does: read_deck -> build_deck_ast -> run_deck; closed-form answers
// at 1e-9 relative. No DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here -
// test_spice.cpp owns doctest's single implementation TU for this binary.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include <ws/spice/circuit.hpp>
#include <ws/spice/deck.hpp>
#include <ws/spice/error.hpp>
#include <ws/spice/netlist.hpp>
#include <ws/spice/parse.hpp>
#include <ws/spice/state.hpp>
#include <ws/spice/lint.hpp>
#include <ws/spice/raw.hpp>
#include <ws/spice/results.hpp>
#include <ws/spice/measure.hpp>
#include <ws/spice/run.hpp>

using ws::spice::build_deck_ast;
using ws::spice::read_deck;
using ws::spice::run_deck;
using ws::spice::RunOutput;
using ws::spice::RunSettings;
using ws::spice::Table;
using ws::spice::Warning;

namespace {

// Runs deck text end to end (the CLI's path minus argv). Returns the
// RunOutput or the propagated error; `warnings` (optional) collects §9.3
// warnings from every stage; `settings` overrides the defaults (the
// fixed_dt test hook reaches tests this way).
ws::core::Result<RunOutput> run_text(const std::string& deck,
                                     std::vector<Warning>* warnings = nullptr,
                                     const RunSettings& settings = RunSettings{}) {
    std::vector<Warning> local;
    std::vector<Warning>& w = warnings ? *warnings : local;
    const ws::core::Result<ws::spice::Deck> parsed =
        read_deck("t.cir", deck, ws::spice::ParseOptions{});
    if (!parsed) {
        return ws::core::unexpected<ws::core::Error>(parsed.error());
    }
    const ws::core::Result<ws::spice::DeckAst> ast = build_deck_ast(*parsed, w);
    if (!ast) {
        return ws::core::unexpected<ws::core::Error>(ast.error());
    }
    return run_deck(*ast, settings, w);
}

// The rendered error message of a failed run.
std::string error_of(const ws::core::Result<RunOutput>& r) {
    REQUIRE(!r);
    return r.error().message;
}

// Column index of `name` in the table (sweep columns excluded).
std::size_t col_of(const Table& t, const std::string& name) {
    const auto idx = t.signal_index(name);
    if (idx.has_value()) {
        return *idx;
    }
    REQUIRE_MESSAGE(false, ("missing column " + name).c_str());
    return 0;
}

doctest::Approx approx(double v) { return doctest::Approx(v).epsilon(1e-9); }

// The table's signal names, in order (the old `columns` member).
std::vector<std::string> signal_names(const Table& t) {
    std::vector<std::string> out;
    out.reserve(t.signals.size());
    for (const ws::spice::Signal& s : t.signals) {
        out.push_back(s.name);
    }
    return out;
}

}  // namespace

// --- §10.1: analytic .op rows --------------------------------------------------

TEST_CASE(".op: resistor divider against the closed form") {
    const auto run = run_text(
        "divider\n"
        "V1 in 0 DC 5\n"
        "R1 in mid 1k\n"
        "R2 mid 0 2k\n"
        ".op\n");
    REQUIRE(run);
    REQUIRE(run->results.tables.size() == 1);
    const Table& t = run->results.tables[0];
    CHECK(t.analysis == ws::spice::AnalysisKind::Op);
    CHECK(t.sweep == ws::spice::SweepKind::None);
    CHECK(signal_names(t) == std::vector<std::string>{"v(in)", "v(mid)", "i(V1)"});
    REQUIRE(t.rows().size() == 1);
    CHECK(t.rows()[0][col_of(t, "v(in)")] == approx(5.0));
    CHECK(t.rows()[0][col_of(t, "v(mid)")] == approx(5.0 * 2.0 / 3.0));
    CHECK(t.rows()[0][col_of(t, "i(V1)")] == approx(-5.0 / 3000.0));

    // Two linear Newton iterations: solve, then a confirming pass.
    CHECK(run->reports.size() == 1);
    CHECK(run->reports[0].analysis == ".op");
    CHECK(run->reports[0].newton_iterations == 2);
    CHECK(run->reports[0].lu_factorizations == 2);
}

TEST_CASE(".op: controlled sources E F G H against the closed form") {
    const auto run = run_text(
        "controlled sources\n"
        "V1 a 0 1\n"
        "R1 a b 1k\n"
        "Rload b 0 2k\n"
        "E1 c 0 b 0 3\n"
        "R2 c d 500\n"
        "G1 d 0 a 0 0.5\n"
        "H1 e 0 V1 100\n"
        "R3 e 0 1k\n"
        "F1 0 f V1 2\n"
        "R4 f 0 100\n"
        ".op\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];

    const double i_v1 = -1.0 / 3000.0;  // -(R1 + Rload) path
    CHECK(t.rows()[0][col_of(t, "v(a)")] == approx(1.0));
    CHECK(t.rows()[0][col_of(t, "v(b)")] == approx(2.0 / 3.0));
    CHECK(t.rows()[0][col_of(t, "i(V1)")] == approx(i_v1));
    // E: v(c) = 3 * v(b).
    CHECK(t.rows()[0][col_of(t, "v(c)")] == approx(2.0));
    // G: draws 0.5*v(a) out of d through 500 Ohm.
    CHECK(t.rows()[0][col_of(t, "v(d)")] == approx(2.0 - 0.5 * 1.0 * 500.0));
    // H: v(e) = 100 * i(V1).
    CHECK(t.rows()[0][col_of(t, "v(e)")] == approx(100.0 * i_v1));
    // F: v(f) = 2 * i(V1) * R4.
    CHECK(t.rows()[0][col_of(t, "v(f)")] == approx(2.0 * i_v1 * 100.0));
    CHECK(t.rows()[0][col_of(t, "i(E1)")] == approx(-0.5));
    CHECK(t.rows()[0][col_of(t, "i(H1)")] == approx(-100.0 * i_v1 / 1000.0));
}

TEST_CASE(".op: I source orientation follows ngspice (positive: N+ to N-)") {
    // "I1 0 out 1m" charges 'out' positive through R.
    const auto run = run_text("i orientation\nI1 0 out 1m\nR1 out 0 1k\n.op\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    CHECK(t.rows()[0][col_of(t, "v(out)")] == approx(1.0));
}

TEST_CASE(".op: subckt flattening, scoped names and parameter overrides") {
    const auto run = run_text(
        "amp stage\n"
        ".param rgain=2k\n"
        ".subckt divider n1 n2 n3 params: rtop=1k rbot=1k\n"
        "Rt n1 nm {rtop}\n"
        "Rb nm n2 {rbot}\n"
        ".ends\n"
        "X1 in out 0 divider params: rtop=3k rbot=1k\n"
        "V1 in 0 DC 4\n"
        "R2 out 0 5k\n"
        ".op\n"
        ".end\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    // Display names keep the instance scoping (§0.7/§8.4).
    CHECK(signal_names(t) == std::vector<std::string>{
                               "v(in)", "v(X1:nm)", "v(out)", "i(V1)"});
    // Closed form: v(nm) = 4 * (Rb||R2) / (Rt + Rb||R2) with Rt=3k (the
    // override), Rb=1k, R2=5k.
    CHECK(t.rows()[0][col_of(t, "v(in)")] == approx(4.0));
    CHECK(t.rows()[0][col_of(t, "v(X1:nm)")] == approx(8.0 / 3.0));
    CHECK(t.rows()[0][col_of(t, "v(out)")] == approx(20.0 / 9.0));
    CHECK(t.rows()[0][col_of(t, "i(V1)")] == approx(-4.0 / 9000.0));
}

TEST_CASE(".op: nested subckt instances chain prefixes") {
    const auto run = run_text(
        "nested subckts\n"
        ".subckt r n1 n2\n"
        "Ri n1 n2 1k\n"
        ".ends\n"
        ".subckt pair n1 n2\n"
        "Xa n1 mid r\n"
        "Xb mid n2 r\n"
        ".ends\n"
        "X1 a 0 pair\n"
        "V1 a 0 2\n"
        ".op\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    // 2k total: i(V1) = -1mA; internal node X1:mid sits at 1 V.
    CHECK(t.rows()[0][col_of(t, "v(X1:mid)")] == approx(1.0));
    CHECK(t.rows()[0][col_of(t, "i(V1)")] == approx(-1e-3));
}

// --- §7.3: .dc ----------------------------------------------------------------

TEST_CASE(".dc: point grid is exact, values are start + k*step") {
    const auto run = run_text(
        "sweep\n"
        "V1 in 0 DC 5\n"
        "R1 in mid 1k\n"
        "R2 mid 0 2k\n"
        ".dc V1 0 1 0.3\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    CHECK(t.analysis == ws::spice::AnalysisKind::Dc);
    CHECK(t.sweep == ws::spice::SweepKind::Swept);
    CHECK(t.sweep_names == std::vector<std::string>{"V1"});
    // floor(1/0.3 + 1e-9) + 1 = 4 points: 0, 0.3, 0.6, 0.9 - no float
    // drift, no phantom endpoint.
    REQUIRE(t.rows().size() == 4);
    for (std::size_t k = 0; k < t.rows().size(); ++k) {
        CHECK(t.rows()[k][0] == approx(0.3 * static_cast<double>(k)));
        CHECK(t.rows()[k][col_of(t, "v(mid)") + 1] ==
              approx(0.3 * static_cast<double>(k) * 2.0 / 3.0));
    }
}

TEST_CASE(".dc: negative direction and single-point sweeps") {
    const auto back = run_text(
        "down\nV1 in 0 5\nR1 in 0 1k\n.dc V1 5 0 -2.5\n");
    REQUIRE(back);
    REQUIRE(back->results.tables[0].rows().size() == 3);
    CHECK(back->results.tables[0].rows()[2][0] == approx(0.0));

    const auto one = run_text("single\nV1 in 0 5\nR1 in 0 1k\n.dc V1 3 3 1\n");
    REQUIRE(one);
    REQUIRE(one->results.tables[0].rows().size() == 1);
    CHECK(one->results.tables[0].rows()[0][0] == approx(3.0));
}

TEST_CASE(".dc: two nested sweeps, first source innermost") {
    const auto run = run_text(
        "nested\n"
        "V1 in 0 DC 5\n"
        "R1 in mid 1k\n"
        "R2 mid 0 2k\n"
        "I1 mid 0 DC 0\n"
        ".dc V1 0 4 2 I1 0 1m 1m\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    CHECK(t.sweep_names == std::vector<std::string>{"V1", "I1"});
    // 3 inner points x 2 outer points = 6 rows. Inner (V1) cycles
    // fastest; the outer I1 draws current OUT of mid (ngspice
    // orientation): at V1=4, I1=1m: v(mid) = 4*2/3 - 1m*666.7.
    REQUIRE(t.rows().size() == 6);
    const std::size_t last = 5;
    CHECK(t.rows()[last][0] == approx(4.0));
    CHECK(t.rows()[last][1] == approx(1e-3));
    CHECK(t.rows()[last][col_of(t, "v(mid)") + 2] == approx(2.0));
}

TEST_CASE(".dc: temp sweep is accepted (M1 devices carry no temp dependence)") {
    const auto run = run_text(
        "temp\nV1 in 0 5\nR1 in 0 1k\n.dc temp 0 100 50\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    REQUIRE(t.rows().size() == 3);
    for (const auto& row : t.rows()) {
        CHECK(row[col_of(t, "v(in)") + 1] == approx(5.0));
    }

    // Every temp point re-elaborates (§7.7) - INCLUDING the first (the
    // pre-sweep elaboration ran at the 27 C default, not the sweep start)
    // - so three fresh kernels: 2 Newton iterations cold, 1 per warm
    // point. Both report totals must accumulate across the kernel
    // swaps, not read the last kernel's counter.
    REQUIRE(run->reports.size() == 1);
    CHECK(run->reports[0].analysis == ".dc");
    CHECK(run->reports[0].newton_iterations == 4);
    CHECK(run->reports[0].lu_factorizations == 4);
}

TEST_CASE(".dc: non-temp sweep reports stay one kernel deep") {
    const auto run = run_text(
        "sweep\nV1 in 0 5\nR1 in 0 1k\n.dc V1 0 4 2\n");
    REQUIRE(run);
    REQUIRE(run->reports.size() == 1);
    // No re-elaboration: one kernel throughout. Point 1 (V1=0) solves to
    // all zeros, so the cold start converges in one iteration; points 2
    // and 3 move the solution and take solve+confirm each: 1 + 2 + 2.
    CHECK(run->reports[0].newton_iterations == 5);
    CHECK(run->reports[0].lu_factorizations == 5);
}

TEST_CASE("a convergence failure stays a convergence error, never 'singular'") {
    // itl1=1 starves EVERY Newton pass: a linear circuit needs its second
    // (confirming) iteration, so plain Newton, each gmin/source stage and
    // the PTC confirm all fail - the full ladder diagnostic, still a
    // spice Convergence error reaching the caller untouched (the old
    // wrap_solver_error re-branded every solve failure as spice.singular
    // and double-rendered the "error: [.op]" prefix).
    const auto run = run_text(
        "conv\nV1 in 0 5\nR1 in 0 1k\n.options itl1 1\n.op\n");
    REQUIRE(!run);
    CHECK(run.error().category == std::string("spice"));
    CHECK(run.error().code ==
          static_cast<std::int32_t>(ws::spice::ErrorKind::Convergence));
    CHECK(run.error().message.find("no convergence after newton, gmin stepping,"
                                   " source stepping and pseudo-transient"
                                   " continuation") != std::string::npos);
    CHECK(run.error().message.find("singular") == std::string::npos);
    CHECK(run.error().message.find("error: [.op] error:") == std::string::npos);

    // Same for a .dc point: itl2 caps the non-first points (itl1 only
    // gates point 1, whose V1=0 solution converges instantly).
    const auto dc = run_text(
        "convdc\nV1 in 0 5\nR1 in 0 1k\n.dc V1 0 4 2\n.options itl2 1\n");
    REQUIRE(!dc);
    CHECK(dc.error().code ==
          static_cast<std::int32_t>(ws::spice::ErrorKind::Convergence));
    CHECK(dc.error().message.find("error: [.dc ") != std::string::npos);
    CHECK(dc.error().message.find("error: [.dc ") ==
          dc.error().message.rfind("error: [.dc "));  // exactly one prefix
}

TEST_CASE(".dc: sweep expressions see .param") {
    const auto run = run_text(
        "param sweep\n"
        ".param vstart=1 vstop={vstart*3} vstep={vstart/2}\n"
        "V1 in 0 5\n"
        "R1 in 0 1k\n"
        ".dc V1 vstart vstop vstep\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    REQUIRE(t.rows().size() == 5);  // 1, 1.5, 2, 2.5, 3
    CHECK(t.rows()[0][0] == approx(1.0));
    CHECK(t.rows()[4][0] == approx(3.0));
}

TEST_CASE(".dc errors: step direction, unknown source, non-source target") {
    CHECK(error_of(run_text("z\nV1 a 0 1\nR1 a 0 1k\n.dc V1 0 1 0\n"))
              .find("step") != std::string::npos);
    CHECK(error_of(run_text("d\nV1 a 0 1\nR1 a 0 1k\n.dc V1 0 1 -0.5\n"))
              .find("must point toward") != std::string::npos);
    CHECK(error_of(run_text("u\nV1 a 0 1\nR1 a 0 1k\n.dc V9 0 1 0.5\n"))
              .find("unknown source 'V9'") != std::string::npos);
    CHECK(error_of(run_text("r\nV1 a 0 1\nR1 a 0 1k\n.dc R1 0 1 0.5\n"))
              .find("not a V or I source") != std::string::npos);
}

TEST_CASE(".dc errors: sweep range is validated before the point count") {
    // A count that overflows size_t (or just materializes a billion rows)
    // must be a semantic error, not a huge loop or UB.
    const std::string huge = error_of(run_text(
        "huge\nV1 a 0 1\nR1 a 0 1k\n.dc V1 0 1e308 1\n"));
    CHECK(huge.find("maximum is 1000000") != std::string::npos);

    // stop-start itself overflows double here; the guard fires before the
    // floor/cast arithmetic can produce inf.
    const std::string span = error_of(run_text(
        "span\nV1 a 0 1\nR1 a 0 1k\n.dc V1 -1e308 1e308 1\n"));
    CHECK(span.find("out of range") != std::string::npos);
}

TEST_CASE(".dc errors: nested temp and duplicate sources are rejected") {
    CHECK(error_of(run_text(
              "tt\nV1 a 0 1\nR1 a 0 1k\n.dc temp 0 10 5 temp 0 10 5\n"))
              .find("cannot nest") != std::string::npos);
    CHECK(error_of(run_text(
              "dd\nV1 a 0 1\nR1 a 0 1k\n.dc V1 0 1 0.5 V1 0 1 0.5\n"))
              .find("distinct source") != std::string::npos);
}

TEST_CASE("a deck with no analysis and no devices is an error, not silence") {
    const std::string msg = error_of(run_text("empty deck\n"));
    CHECK(msg.find("no devices to simulate") != std::string::npos);

    // The valid counterpart: devices but no analysis card still elaborates
    // and succeeds with no tables.
    const auto ok = run_text("devices only\nV1 a 0 1\nR1 a 0 1k\n");
    REQUIRE(ok);
    CHECK(ok->results.tables.empty());
}

TEST_CASE("run_deck rejects a malformed AnalysisCard instead of skipping it") {
    std::vector<Warning> warnings;
    const auto parsed = read_deck(
        "t.cir", "card\nV1 a 0 1\nR1 a 0 1k\n.op\n", ws::spice::ParseOptions{});
    REQUIRE(parsed);
    const auto ast = build_deck_ast(*parsed, warnings);
    REQUIRE(ast);

    // Neither .op nor .dc: silently skipping used to turn this into a
    // successful run with no tables.
    ws::spice::DeckAst neither = *ast;
    ws::spice::AnalysisCard empty_card;
    empty_card.loc = ws::spice::SourceLoc::make("t.cir", 4, 1);
    neither.analyses.push_back(empty_card);
    const auto r_neither = run_deck(neither, RunSettings{}, warnings);
    REQUIRE(!r_neither);
    CHECK(r_neither.error().message.find("neither .op nor .dc") !=
          std::string::npos);

    // Both at once: .op used to win silently.
    ws::spice::DeckAst both = *ast;
    both.analyses[0].dc = ws::spice::DcCard{};
    const auto r_both = run_deck(both, RunSettings{}, warnings);
    REQUIRE(!r_both);
    CHECK(r_both.error().message.find("both .op and .dc") != std::string::npos);
}

// --- §0.5/§7.8: CSV shape ------------------------------------------------------

TEST_CASE("CSV: header row, %.12g numbers, op-only writes one row") {
    const auto run = run_text(
        "divider\nV1 in 0 DC 5\nR1 in mid 1k\nR2 mid 0 2k\n.op\n");
    REQUIRE(run);
    CHECK(ws::spice::results_csv(run->results) ==
          "v(in),v(mid),i(V1)\n"
          "5,3.33333333333,-0.00166666666667\n");
}

TEST_CASE("CSV: .dc sweep header, header fields == row fields") {
    const auto run = run_text(
        "sweep\nV1 in 0 DC 5\nR1 in 0 1k\n.dc V1 0 1 1\n");
    REQUIRE(run);
    CHECK(ws::spice::results_csv(run->results) ==
          "V1,v(in),i(V1)\n"
          "0,0,0\n"
          "1,1,-0.001\n");
}

TEST_CASE("CSV: .op inside a multi-analysis run writes no section (§7.2)") {
    const auto run = run_text(
        "both\nV1 in 0 5\nR1 in 0 1k\n.op\n.dc V1 0 1 1\n");
    REQUIRE(run);
    REQUIRE(run->results.tables.size() == 2);  // both tables in Results
    const std::string csv = ws::spice::results_csv(run->results);
    // Exactly the .dc section: its sweep header, then two data rows -
    // the .op table is skipped per §7.2, and no blank separator line
    // appears.
    CHECK(csv ==
          "V1,v(in),i(V1)\n"
          "0,0,0\n"
          "1,1,-0.001\n");
}

TEST_CASE(".op: bare source value (no DC keyword) parses the same") {
    const auto run = run_text(
        "bare\nV1 in 0 5\nR1 in mid 1k\nR2 mid 0 2k\n.op\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    CHECK(signal_names(t) == std::vector<std::string>{"v(in)", "v(mid)", "i(V1)"});
    CHECK(t.rows()[0][col_of(t, "v(in)")] == approx(5.0));
    CHECK(t.rows()[0][col_of(t, "v(mid)")] == approx(5.0 * 2.0 / 3.0));
    CHECK(t.rows()[0][col_of(t, "i(V1)")] == approx(-5.0 / 3000.0));
}

// --- §9.3: warnings ------------------------------------------------------------

TEST_CASE("unknown .options keys warn (rendered as warnings), never fail") {
    std::vector<Warning> warnings;
    const auto run = run_text(
        "warn\nV1 in 0 5\nR1 in 0 1k\n.options foo=1 reltol=1e-6\n.op\n",
        &warnings);
    REQUIRE(run);
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].render() ==
          "t.cir:4:10: warning: ignored .options key 'foo'");
    CHECK(warnings[0].render().find("error") == std::string::npos);
}

TEST_CASE("a title line that looks like a card warns (the swallowed-device footgun)") {
    // "V1 in 0 5" as line 1 IS the title (§8.1, same as ngspice) - the
    // device is gone from the circuit. The warning names the mystery.
    std::vector<Warning> warnings;
    const auto run = run_text(
        "V1 in 0 5\nR1 in mid 1k\nR2 mid 0 2k\n.op\n", &warnings);
    REQUIRE(run);
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].render().rfind(
              "t.cir:1:1: warning: the title line looks like a card ('V1') but"
              " line 1 of a deck is always the title and is ignored",
              0) == 0);
    CHECK(warnings[0].render().find("hint:") != std::string::npos);
    // ...and the run itself still succeeds with the title-only V1 absent:
    const Table& t = run->results.tables[0];
    CHECK(signal_names(t) == std::vector<std::string>{"v(in)", "v(mid)"});

    // A dot-card title warns too.
    std::vector<Warning> dot_warnings;
    REQUIRE(run_text(".op\nV1 a 0 5\nR1 a 0 1k\n.op\n", &dot_warnings));
    REQUIRE(dot_warnings.size() == 1);
    CHECK(dot_warnings[0].render().find("'.op'") != std::string::npos);

    // Ordinary English titles never warn.
    std::vector<Warning> none;
    REQUIRE(run_text(
        "Voltage divider test\nV1 in 0 5\nR1 in 0 1k\n.op\n", &none));
    CHECK(none.empty());
    std::vector<Warning> none2;
    REQUIRE(run_text(
        "Running the show\nV1 in 0 5\nR1 in 0 1k\n.op\n", &none2));
    CHECK(none2.empty());
}

// --- §10.4: parse-error goldens - line:col prefixes cannot drift ---------------

TEST_CASE("parse-error goldens: file:line:col prefixes") {
    struct Golden {
        const char* deck;
        std::string prefix;      // must start the rendered error
        std::string contains;    // a case-specific snippet
    };
    const Golden goldens[] = {
        {".lib backlog\n* line 2 is a comment\n.lib foo\n",
         "t.cir:3:1: error: ", "card '.lib'"},
        {"bjt\nV1 a 0 5\nB1 a 0 0 qm\n",
         "t.cir:3:1: error: ", "expected V= or I="},
        {"arity\nV1 a 0 5\nR1 a 0\n",
         "t.cir:3:1: error: ", "needs a value"},
        {"bad waveform\nV1 a 0 PULSE(0)\nR1 a 0 1k\n.op\n",
         "t.cir:2:8: error: ", "PULSE needs at least"},
        {"sub\nX1 a b nosuch\nV1 a 0 5\nR1 a 0 1k\n.op\n",
         "t.cir:2:8: error: ", "unknown subcircuit"},
        {"dc arity\nV1 a 0 5\nR1 a 0 1k\n.dc V1 0 1\n",
         "t.cir:4:1: error: ", "'.dc' takes"},
        {"tran arity\nV1 a 0 5\nR1 a 0 1k\n.tran 1u\n",
         "t.cir:4:1: error: ", "'.tran' takes"},
        {"ac mode\nV1 a 0 5 AC 1\nR1 a 0 1k\nC1 a 0 1u\n.ac foo 10 1 1k\n",
         "t.cir:5:1: error: ", "not one of dec, oct, lin"},
        {"ic form\nV1 a 0 5\nR1 a 0 1k\n.ic a 5\n",
         "t.cir:4:5: error: ", "v(node)=value"},
        {"op in subckt\n.subckt s a\nRi a 0 1k\n.op\n.ends\n",
         "t.cir:4:1: error: ", "'.op' inside .subckt"},
        {"bad number\nV1 a 0 5\nR1 a 0 1.2.3\n.op\n",
         "t.cir:3:8: error: ", ""},
    };
    for (const auto& g : goldens) {
        CAPTURE(g.deck);
        const std::string msg = error_of(run_text(g.deck));
        CHECK_MESSAGE(msg.rfind(g.prefix, 0) == 0, msg);
        if (!g.contains.empty()) {
            CHECK_MESSAGE(msg.find(g.contains) != std::string::npos, msg);
        }
    }
}

// --- §6.6: topology goldens ----------------------------------------------------

TEST_CASE("topology: parallel voltage sources to ground are a named loop") {
    const std::string msg = error_of(run_text(
        "loop\nV1 a 0 5\nV2 a 0 3\nR1 a 0 1k\n.op\n"));
    CHECK(msg.rfind("error: voltage-source loop: V1, V2", 0) == 0);
    CHECK(msg.find("hint: add series resistance") != std::string::npos);
}

TEST_CASE("topology: a V loop between two non-ground nodes is named too") {
    const std::string msg = error_of(run_text(
        "loop2\nV1 a b 5\nV2 a b 3\nR1 a 0 1k\nR2 b 0 1k\n.op\n"));
    CHECK(msg.rfind("error: voltage-source loop: V1, V2", 0) == 0);
}

TEST_CASE("topology: floating node has no DC path to ground") {
    const std::string msg = error_of(run_text(
        "float\nI1 a 0 1m\nR1 a b 1k\n.op\n"));
    CHECK(msg.rfind("error: no DC path to ground from node 'a'", 0) == 0);
}

TEST_CASE("topology: an E output in a V loop counts as a constraint") {
    const std::string msg = error_of(run_text(
        "eloop\nV1 a b 5\nE1 a b c d 2\nRc c 0 1k\nRd d 0 1k\nR1 b 0 1k\n.op\n"));
    CHECK(msg.rfind("error: voltage-source loop", 0) == 0);
}

// --- §8.4/§8.5: parameters and expressions -------------------------------------

TEST_CASE(".param: lazy scope, cycles are named") {
    const std::string msg = error_of(run_text(
        "cycle\n.param a=b+1\n.param b=a\nV1 x 0 1\nR1 x 0 1k\n.op\n"));
    CHECK(msg.find("cycle") != std::string::npos);
}

TEST_CASE(".param: values flow into devices three ways") {
    const auto run = run_text(
        "params\n"
        ".param r1=2k v1=3\n"
        "V1 a 0 {v1}\n"
        "R1 a 0 r1\n"
        "R2 a b {r1/2}\n"
        "R3 b 0 r1\n"
        ".op\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    // R1 (2k) parallels the R2+R3 branch (1k+2k = 3k): 1.2k total.
    CHECK(t.rows()[0][col_of(t, "v(a)")] == approx(3.0));
    CHECK(t.rows()[0][col_of(t, "v(b)")] == approx(2.0));
    CHECK(t.rows()[0][col_of(t, "i(V1)")] == approx(-2.5e-3));
}

TEST_CASE("expression evaluator: operators, functions, ternary") {
    struct Case {
        std::string expr;
        double want;
    };
    const Case cases[] = {
        {"1+2*3", 7}, {"2**10", 1024}, {"(1+2)*3", 9},
        {"sqrt(16)+abs(-3)", 7}, {"min(3,5)+max(3,5)", 8},
        {"1 < 2 ? 10 : 20", 10}, {"!(1 < 2) ? 10 : 20", 20},
        {"sgn(-5)+floor(1.7)+ceil(1.2)", 2},
    };
    for (const auto& c : cases) {
        CAPTURE(c.expr);
        const auto prog = ws::spice::parse_expression(c.expr);
        REQUIRE(prog);
        const auto v = prog->evaluate({});
        REQUIRE(v);
        CHECK(*v == doctest::Approx(c.want));
    }
}

// --- M2 (§10.1 rows): .tran / .ac / C / L / K / switches --------------------------

namespace {

// The .tran row whose time is closest to `t` (by value: rows() is a
// materialized view).
std::vector<double> row_near(const Table& t, double when) {
    const std::vector<std::vector<double>> rows = t.rows();
    std::size_t best = 0;
    double best_diff = 1e30;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const double d = std::abs(rows[i][0] - when);
        if (d < best_diff) {
            best_diff = d;
            best = i;
        }
    }
    REQUIRE(best < rows.size());
    return rows[best];
}

constexpr double kPi = 3.14159265358979323846;

}  // namespace

TEST_CASE(".tran: RC step against 1-e^{-t/RC}, at defaults and reltol=1e-6") {
    const char* deck =
        "rc step\n"
        "V1 in 0 PULSE(0 1 1u 1n 1u 10m 10m)\n"
        "R1 in out 1k\n"
        "C1 out 0 1u\n"
        ".tran 10u 5m\n";
    // tau = 1 ms; the step lands at 1 us. Defaults gate: 1e-3 V.
    {
        const auto run = run_text(deck);
        REQUIRE(run);
        const Table& t = run->results.tables[0];
        CHECK(t.sweep_names == std::vector<std::string>{"time"});
        const auto& r1 = row_near(t, 1.001e-3);  // 1 tau after the edge
        CHECK(std::abs(r1[col_of(t, "v(out)") + 1] - 0.632120558828) < 1e-3);
        const auto& r3 = row_near(t, 3.001e-3);  // 3 tau
        CHECK(std::abs(r3[col_of(t, "v(out)") + 1] - 0.950212931632) < 1e-3);
    }
    // Tightened gate: reltol=1e-6, smaller tstep -> 1e-5 V.
    {
        RunSettings s;
        s.reltol = 1e-6;
        const auto run = run_text(
            "rc tight\n"
            "V1 in 0 PULSE(0 1 1u 1n 1u 10m 10m)\n"
            "R1 in out 1k\n"
            "C1 out 0 1u\n"
            ".tran 2u 5m\n"
            ".options reltol=1e-6\n",
            nullptr, s);
        REQUIRE(run);
        const Table& t = run->results.tables[0];
        const auto& r1 = row_near(t, 1.001e-3);
        CHECK(std::abs(r1[col_of(t, "v(out)") + 1] - 0.632120558828) < 1e-5);
    }
}

TEST_CASE(".tran: trap order 2 - RC error ratio ~4 on halving fixed dt") {
    // The test-only fixed-step hook: pure trapezoidal, no BE restarts, no
    // LTE rejection, so the halving study is meaningful (§10.1). The step
    // edge sits at 1 us with an OP before it (no UIC: a cold UIC start is
    // deliberately inconsistent per §6.5 and its O(h) kink would mask the
    // order); rows after the edge are 1.001us + k*dt (the 1 ns ramp
    // breakpoint lands first) and each compares against the closed form
    // from the ramp midpoint.
    const char* deck =
        "rc order\n"
        "V1 in 0 PULSE(0 1 1u 1n 1u 10m)\n"
        "R1 in out 1k\n"
        "C1 out 0 1u\n"
        ".tran 10u 5m\n";
    auto max_err = [&](double h) {
        RunSettings s;
        s.fixed_dt = h;
        const auto run = run_text(deck, nullptr, s);
        REQUIRE(run);
        const Table& t = run->results.tables[0];
        const std::size_t vcol = col_of(t, "v(out)") + 1;
        double worst = 0.0;
        for (const auto& row : t.rows()) {
            if (row[0] <= 1.0011e-6) {
                continue;  // pre-edge (all zeros) and the ramp step itself
            }
            const double want = 1.0 - std::exp(-(row[0] - 1.0005e-6) / 1e-3);
            worst = std::max(worst, std::abs(want - row[vcol]));
        }
        return worst;
    };
    const double e_coarse = max_err(20e-6);
    const double e_fine = max_err(10e-6);
    REQUIRE(e_fine > 0.0);
    const double ratio = e_coarse / e_fine;
    CHECK(ratio > 3.8);
    CHECK(ratio < 4.2);
}

TEST_CASE(".tran: series RLC step - ringing frequency and overshoot") {
    // R=10, L=1m, C=1u: zeta = 0.158, f_d = 4.968 kHz, overshoot 0.605.
    RunSettings s;
    s.fixed_dt = 0.2e-6;  // ~1000 steps per ring
    const auto run = run_text(
        "rlc\n"
        "V1 in 0 1\n"
        "R1 in mid 10\n"
        "L1 mid out 1m\n"
        "C1 out 0 1u\n"
        ".ic v(out)=0\n"
        ".tran 1u 1m UIC\n",
        nullptr, s);
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    const std::size_t vcol = col_of(t, "v(out)") + 1;

    // Peak (overshoot) within 1e-3 V of the closed form.
    double peak = 0.0;
    for (const auto& row : t.rows()) {
        peak = std::max(peak, row[vcol]);
    }
    const double zeta = 10.0 / 2.0 * std::sqrt(1e-6 / 1e-3);
    const double want_peak = 1.0 + std::exp(-kPi * zeta / std::sqrt(1 - zeta * zeta));
    CHECK(std::abs(peak - want_peak) < 1e-3);

    // Ringing frequency from the first two downward zero crossings of
    // v - 1, within 0.2%.
    std::vector<double> crossings;
    for (std::size_t i = 1; i < t.rows().size(); ++i) {
        const double a = t.rows()[i - 1][vcol] - 1.0;
        const double b = t.rows()[i][vcol] - 1.0;
        if (a > 0 && b <= 0) {
            crossings.push_back(t.rows()[i - 1][0] +
                                a / (a - b) * (t.rows()[i][0] - t.rows()[i - 1][0]));
            if (crossings.size() == 2) {
                break;
            }
        }
    }
    REQUIRE(crossings.size() == 2);
    const double period = crossings[1] - crossings[0];
    const double fd = 1.0 / (2 * kPi * std::sqrt(1e-9)) * std::sqrt(1 - zeta * zeta);
    CHECK(std::abs(1.0 / period - fd) / fd < 0.002);
}

TEST_CASE(".tran: undamped LC conserves amplitude over 50 periods") {
    // v(t) = cos(t) with L = C = 1; the trap companion is the Cayley
    // transform, so amplitude drift over 50 periods stays within 1%.
    RunSettings s;
    s.fixed_dt = 2 * kPi / 100.0;
    const auto run = run_text(
        "lc\n"
        "L1 out 0 1\n"
        "C1 out 0 1\n"
        ".ic v(out)=1\n"
        ".tran 0.1 400 UIC\n",
        nullptr, s);
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    const std::size_t vcol = col_of(t, "v(out)") + 1;
    double tail_peak = 0.0;
    for (const auto& row : t.rows()) {
        if (row[0] > 49.0 * 2 * kPi) {
            tail_peak = std::max(tail_peak, std::abs(row[vcol]));
        }
    }
    CHECK(tail_peak > 0.99);
    CHECK(tail_peak < 1.01);
}

TEST_CASE(".ac: RC low-pass re/im against the closed form, 1e-9 rel") {
    const auto run = run_text(
        "lp\n"
        "V1 in 0 DC 0 AC 1\n"
        "R1 in out 1k\n"
        "C1 out 0 1u\n"
        ".ac dec 4 1 1e6\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    CHECK(t.sweep_names == std::vector<std::string>{"frequency"});
    CHECK(signal_names(t) == std::vector<std::string>{"re(v(in))", "im(v(in))",
                                                "re(v(out))", "im(v(out))",
                                                "re(i(V1))", "im(i(V1))"});
    REQUIRE(t.rows().size() > 10);
    for (const auto& row : t.rows()) {
        const double w = 2 * kPi * row[0];
        const double rc = 1e-3;
        // v_out = 1/(1 + j w RC) = (1 - j w RC)/(1 + (w RC)^2)
        const double want_re = 1.0 / (1.0 + (w * rc) * (w * rc));
        const double want_im = -(w * rc) / (1.0 + (w * rc) * (w * rc));
        const double got_re = row[col_of(t, "re(v(out))") + 1];
        const double got_im = row[col_of(t, "im(v(out))") + 1];
        CHECK(got_re == doctest::Approx(want_re).epsilon(1e-9));
        CHECK(got_im == doctest::Approx(want_im).epsilon(1e-9));
    }
}

TEST_CASE(".ac: series RLC - |Y| curve, resonance and Q, 1e-9 rel") {
    // A fine linear grid around f0 = 5.033 kHz so the peak is resolved;
    // every row still checks against the closed form.
    const auto run = run_text(
        "rlc ac\n"
        "V1 in 0 DC 0 AC 1\n"
        "R1 in mid 10\n"
        "L1 mid out 1m\n"
        "C1 out 0 1u\n"
        ".ac lin 401 4k 6k\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    double best_f = 0.0;
    double best_g = 0.0;
    for (const auto& row : t.rows()) {
        const double w = 2 * kPi * row[0];
        const double x = w * 1e-3 - 1.0 / (w * 1e-6);
        // i(V1) carries the source convention: negative when delivering
        // (same as .op), so i(V1) = -Y*V = -(R - jX)/(R^2 + X^2).
        const double want_re = -10.0 / (100.0 + x * x);
        const double want_im = x / (100.0 + x * x);
        const double got_re = row[col_of(t, "re(i(V1))") + 1];
        const double got_im = row[col_of(t, "im(i(V1))") + 1];
        CHECK(got_re == doctest::Approx(want_re).epsilon(1e-9));
        CHECK(got_im == doctest::Approx(want_im).epsilon(1e-9));
        const double g = std::hypot(got_re, got_im);
        if (g > best_g) {
            best_g = g;
            best_f = row[0];
        }
    }
    // Peak conductance 1/R at f0 = 1/(2 pi sqrt(LC)); the grid is 5 Hz,
    // so the peak row sits within one cell of f0.
    CHECK(best_g == doctest::Approx(0.1).epsilon(1e-5));
    const double f0 = 1.0 / (2 * kPi * std::sqrt(1e-9));
    CHECK(std::abs(best_f - f0) < 10.0);
}

TEST_CASE(".ac: coupled inductors - voltage ratio ~ sqrt(L2/L1) as k -> 1") {
    const auto run = run_text(
        "xfmr\n"
        "V1 a 0 AC 1\n"
        "R1 a p 1\n"
        "L1 p 0 1m\n"
        "L2 b 0 4m\n"
        "K1 L1 L2 0.9999\n"
        "R2 b 0 100k\n"
        ".ac dec 1 10k 100k\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    for (const auto& row : t.rows()) {
        const double vb = std::hypot(row[col_of(t, "re(v(b))") + 1],
                                     row[col_of(t, "im(v(b))") + 1]);
        CHECK(vb == doctest::Approx(2.0).epsilon(1e-3));  // sqrt(4m/1m)
    }
}

TEST_CASE(".tran: switch + RC charge curve from the closing breakpoint") {
    // The switch closes when the control PWL ramp crosses Vt = 2 V
    // (t_c = 1.0004 us); from there v(out) follows the closed form.
    const auto run = run_text(
        "sw\n"
        "V1 in 0 5\n"
        "Vc ctl 0 PWL(0 0 1u 0 1.001u 5)\n"
        "S1 in mid ctl 0 swmod\n"
        "R1 mid out 1k\n"
        "C1 out 0 1u\n"
        "R2 out 0 1meg\n"
        ".model swmod SW(Ron=1 Roff=1meg Vt=2)\n"
        ".tran 1u 4m\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    const double tc = 1.0004e-6;
    const double rs = 1001.0;          // Ron + R1
    const double r2 = 1e6;
    const double final_v = 5 * r2 / (rs + r2);
    const double tau = (rs * r2 / (rs + r2)) * 1e-6;
    const double v0 = 5 * r2 / (1e6 + rs + r2);
    for (const double when : {0.5e-3, 1.5e-3, 3.0e-3}) {
        const auto& row = row_near(t, when);
        const double want = final_v + (v0 - final_v) * std::exp(-(row[0] - tc) / tau);
        CHECK(std::abs(row[col_of(t, "v(out)") + 1] - want) < 1e-3);
    }
}

TEST_CASE(".tran: W (current-controlled) switch flips on its sense current") {
    // The sense source Vs sits in series with the drive: current flows
    // INTO its + terminal (p -> n through the source), so i(Vs) reads
    // positive while the ramp rises (a source shown delivering reads
    // negative - the .op convention).
    const auto run = run_text(
        "wsw\n"
        "Vd d 0 PWL(0 0 1u 0 1.001u 5)\n"
        "R4 d x 1k\n"
        "Vs x 0 DC 0\n"
        "V1 in 0 5\n"
        "R1 in mid 1k\n"
        "W1 mid out Vs wmod\n"
        "C1 out 0 1u\n"
        "R2 out 0 1meg\n"
        ".model wmod SW(Ron=1 Roff=1meg It=2.5m Ih=0)\n"
        ".tran 1u 3m\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    const std::size_t vcol = col_of(t, "v(out)") + 1;
    // At the ramp start (1 us, switch still off): v(out) sits at the
    // Roff divider 5*R2/(Roff+R1+R2) ~ 2.499.
    const auto& before = row_near(t, 1.0e-6);
    CHECK(before[vcol] == doctest::Approx(2.49875).epsilon(1e-3));
    // After closing: charges toward 5 * R2/(1001 + R2) ~ 4.995 with
    // tau ~ 1 ms; at 2.9 ms it is past 4.8 V.
    const auto& late = row_near(t, 2.9e-3);
    CHECK(late[vcol] > 4.8);
}

TEST_CASE(".tran: SIN/PWL/EXP waveform sources evaluate at their breakpoints") {
    // SIN quarter-period peak: v(out) = sin(2*pi*1k*0.25m) = 1.
    {
        RunSettings s;
        s.fixed_dt = 1e-6;
        const auto run = run_text(
            "sin\nV1 out 0 SIN(0 1 1k)\nR1 out 0 1meg\n.tran 1u 1m\n", nullptr, s);
        REQUIRE(run);
        const Table& t = run->results.tables[0];
        const auto& row = row_near(t, 2.5e-4);
        CHECK(row[col_of(t, "v(out)") + 1] == doctest::Approx(1.0).epsilon(1e-9));
    }
    // PWL ramp: v = t/2m at t = 1.5m.
    {
        RunSettings s;
        s.fixed_dt = 1e-6;
        const auto run = run_text(
            "pwl\nV1 out 0 PWL(0 0 2m 1)\nR1 out 0 1meg\n.tran 1u 2m\n", nullptr, s);
        REQUIRE(run);
        const Table& t = run->results.tables[0];
        const auto& row = row_near(t, 1.5e-3);
        CHECK(row[col_of(t, "v(out)") + 1] == doctest::Approx(0.75).epsilon(1e-9));
    }
    // EXP single rise: TD2/TAU2 given explicitly (the ngspice default
    // TD2 = TD1+TSTEP would fold the pulse back within a step).
    {
        RunSettings s;
        s.fixed_dt = 1e-9;
        const auto run = run_text(
            "exp\nV1 out 0 EXP(0 1 1u 1u 1 1)\nR1 out 0 1meg\n.tran 1n 5u\n",
            nullptr, s);
        REQUIRE(run);
        const Table& t = run->results.tables[0];
        const auto& row = row_near(t, 2e-6);
        CHECK(row[col_of(t, "v(out)") + 1] ==
              doctest::Approx(1.0 - std::exp(-1.0)).epsilon(1e-6));
    }
}

TEST_CASE("M2 errors: no AC stimulus, bad K, L loops, floating caps") {
    // §7.4: a deck with .ac and no AC source is an error, never zeros.
    CHECK(error_of(run_text(
              "noac\nV1 in 0 1\nR1 in out 1k\nC1 out 0 1u\n.ac dec 10 1 1k\n"))
              .find("no AC stimulus") != std::string::npos);

    // |k| >= 1 is singular.
    CHECK(error_of(run_text(
              "kbad\nV1 a 0 1\nR1 a b 1\nL1 b 0 1m\nL2 c 0 1m\nR2 c 0 1k\n"
              "K1 L1 L2 1\n.tran 1u 1m\n"))
              .find("k = 1 is singular") != std::string::npos);

    // K names an unknown inductor.
    CHECK(error_of(run_text(
              "kbad2\nV1 a 0 1\nR1 a b 1\nL1 b 0 1m\nK1 L1 L9 0.5\n.tran 1u 1m\n"))
              .find("unknown inductor") != std::string::npos);

    // An inductor across a V source is a constraint loop (§6.6).
    CHECK(error_of(run_text(
              "vl\nV1 a 0 5\nL1 a 0 1m\nR1 a 0 1k\n.op\n"))
              .find("voltage-source/inductor loop") != std::string::npos);

    // Two parallel inductors: an inductor loop.
    CHECK(error_of(run_text(
              "ll\nV1 a b 5\nL1 a b 1m\nL2 a b 2m\nR1 b 0 1k\n.op\n"))
              .find("inductor loop") != std::string::npos);

    // A node whose only path is through a capacitor has no DC path (§6.6).
    CHECK(error_of(run_text(
              "floatc\nI1 a b 1m\nR1 a 0 1k\nC1 b 0 1u\n.op\n"))
              .find("no DC path to ground from node 'b'") != std::string::npos);

    // .ic names an unknown node.
    CHECK(error_of(run_text(
              "icx\nV1 a 0 1\nR1 a 0 1k\n.tran 1u 1m\n.ic v(nope)=1\n"))
              .find("unknown node 'nope'") != std::string::npos);

    // .ic cannot force ground.
    CHECK(error_of(run_text(
              "icg\nV1 a 0 1\nR1 a 0 1k\n.tran 1u 1m\n.ic v(0)=1\n"))
              .find("cannot force the ground node") != std::string::npos);

    // A switch model with Ron >= Roff is rejected.
    CHECK(error_of(run_text(
              "swbad\nV1 a 0 1\nS1 a b a 0 m1\nR1 b 0 1k\n.model m1 SW(Ron=1 Roff=0.5)\n"
              ".op\n"))
              .find("0 < Ron < Roff") != std::string::npos);

    // An unused .model warns (§9.3).
    std::vector<Warning> warnings;
    REQUIRE(run_text("unused\nV1 a 0 1\nR1 a 0 1k\n.op\n.model m1 SW\n",
                     &warnings));
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].render().find("unused .model") != std::string::npos);
}

TEST_CASE("CSV: .tran time column and .ac re()/im() pairs") {
    const auto run = run_text(
        "both\n"
        "V1 in 0 DC 0 AC 1\n"
        "R1 in out 1k\n"
        "C1 out 0 1u\n"
        ".tran 10u 50u\n"
        ".ac dec 1 1k 10k\n");
    REQUIRE(run);
    const std::string csv = ws::spice::results_csv(run->results);
    // Two sections joined by a blank line: time-first then frequency-first.
    CHECK(csv.find("time,v(in),v(out),i(V1)\n") == 0);
    CHECK(csv.find("\n\nfrequency,re(v(in)),im(v(in)),re(v(out)),im(v(out)),"
                   "re(i(V1)),im(i(V1))\n") != std::string::npos);
}

// --- §3.5 rev: the param/state arenas ------------------------------------------

TEST_CASE("rebind: two runs of one deck are independent and identical") {
    const std::string deck =
        "rc\nV1 in 0 PULSE(0 5 1u 1u 1u 10u)\nR1 in out 1k\nC1 out 0 1n\n.tran 1u 20u\n";
    const auto a = run_text(deck);
    const auto b = run_text(deck);
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE(a->results.tables.size() == 1);
    REQUIRE(b->results.tables.size() == 1);
    const Table& ta = a->results.tables[0];
    const Table& tb = b->results.tables[0];
    REQUIRE(ta.rows().size() == tb.rows().size());
    REQUIRE(ta.rows().size() > 5);
    for (std::size_t i = 0; i < ta.rows().size(); ++i) {
        REQUIRE(ta.rows()[i].size() == tb.rows()[i].size());
        for (std::size_t j = 0; j < ta.rows()[i].size(); ++j) {
            CHECK(ta.rows()[i][j] == tb.rows()[i][j]);  // bit-identical (§10.5)
        }
    }
}

TEST_CASE("rebind: a .dc sweep value never leaks into a later analysis") {
    // The sweep mutates V1's dc parameter slot in the .dc run's own state;
    // the .tran that follows rebinds from the deck scope, so its t=0 OP
    // sees the deck's DC value (PULSE(0 5 ...) -> waveform(0) = 0).
    const auto run = run_text(
        "leak\n"
        "V1 in 0 PULSE(0 5 1u 1u 1u 10u)\n"
        "R1 in out 1k\n"
        "C1 out 0 1n\n"
        ".dc V1 0 5 1\n"
        ".tran 1u 5u\n");
    REQUIRE(run);
    REQUIRE(run->results.tables.size() == 2);
    const Table& tran = run->results.tables[1];
    REQUIRE(tran.rows().size() >= 2);
    CHECK(tran.rows()[0][col_of(tran, "v(in)") + 1] == approx(0.0));
    // The waveform drives the transient: 0 at t=0 (no .dc residue of the
    // 5 V sweep endpoint), ramping through the PULSE, at the 5 V plateau
    // by t = TD+TR = 2u (tstop = 5u).
    for (const auto& row : tran.rows()) {
        CHECK(row[col_of(tran, "v(in)") + 1] <= 5.0 + 1e-9);
    }
    CHECK(tran.rows().back()[col_of(tran, "v(in)") + 1] == approx(5.0));
}

TEST_CASE("rebind: interleaved analyses keep their own dynamic state") {
    // .tran's C companion state must not survive into the .ac that follows
    // (a fresh arena per analysis - §3.5 rev's trivial reset_state()).
    const auto run = run_text(
        "interleave\n"
        "V1 in 0 DC 1 AC 1\n"
        "R1 in out 1k\n"
        "C1 out 0 1u\n"
        ".tran 10u 30u\n"
        ".ac dec 2 1k 1meg\n");
    REQUIRE(run);
    REQUIRE(run->results.tables.size() == 2);
    // The RC pole at 1/(2*pi*1k*1u) ~ 159 Hz: v(out)/v(in) = 1/(1 + jwRC)
    // at the first frequency point, unaffected by the transient's history
    // (verified against the same deck run .ac-only).
    const Table& ac = run->results.tables[1];
    const double f = ac.rows()[0][0];
    CHECK(f == approx(1000.0));
    const double w = 2.0 * 3.14159265358979 * f;
    const double x = w * 1e3 * 1e-6;  // wRC
    const double expect_re = 1.0 / (1.0 + x * x);
    const double expect_im = -x / (1.0 + x * x);
    CHECK(ac.rows()[0][col_of(ac, "re(v(out))") + 1] ==
          doctest::Approx(expect_re).epsilon(1e-6));
    CHECK(ac.rows()[0][col_of(ac, "im(v(out))") + 1] ==
          doctest::Approx(expect_im).epsilon(1e-6));
}

// --- §7.8 rev: columnar access ---------------------------------------------------

TEST_CASE("columnar: signals by name, contiguous, axis shared") {
    const auto run = run_text(
        "cols\n"
        "V1 in 0 PULSE(0 5 1u 1u 1u 10u)\n"
        "R1 in out 1k\n"
        "C1 out 0 1n\n"
        ".tran 1u 20u\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];

    // Signal by name: a contiguous span of row_count() samples.
    const std::span<const double> vout = t.signal("v(out)");
    REQUIRE(vout.size() == t.row_count());
    CHECK(t.row_count() > 5);
    // Contiguity: the span points straight into the column storage.
    const auto idx = t.signal_index("v(out)");
    REQUIRE(idx.has_value());
    CHECK(vout.data() == t.signals[*idx].data.data());
    // A missing name is an empty span, not an error.
    CHECK(t.signal("v(nope)").empty());

    // The time axis is the shared sweep column.
    const std::span<const double> time = t.axis();
    REQUIRE(time.size() == t.row_count());
    CHECK(time.front() == 0.0);
    CHECK(time.back() == doctest::Approx(20e-6).epsilon(1e-9));

    // rows()[i] agrees with the columnar view.
    for (std::size_t i : {std::size_t{0}, std::size_t{1}, t.row_count() - 1}) {
        const std::vector<double> row = t.row(i);
        REQUIRE(row.size() == 1 + t.signals.size());
        CHECK(row[0] == time[i]);
        CHECK(row[1 + *idx] == vout[i]);
    }
}

TEST_CASE("columnar: Results::operator[] finds the owning analysis") {
    const auto run = run_text(
        "multi\n"
        "V1 in 0 DC 1 AC 1\n"
        "R1 in out 1k\n"
        "C1 out 0 1u\n"
        ".tran 10u 20u\n"
        ".ac dec 1 1k 100k\n");
    REQUIRE(run);
    // v(out) exists only in the .tran table; re(v(out)) only in .ac.
    CHECK(!run->results["v(out)"].empty());
    CHECK(run->results["v(out)"].size() == run->results.tables[0].row_count());
    CHECK(!run->results["re(v(out))"].empty());
    CHECK(run->results["re(v(out))"].size() ==
          run->results.tables[1].row_count());
    CHECK(run->results["nope(x)"].empty());
}

// --- §5.2 rev: the convergence ladder (PTC promoted) ------------------------------

TEST_CASE("ladder: linear decks report the plain-newton stage") {
    // Plain Newton converges in two passes on every linear deck; the
    // ladder's later stages must never even run (their machinery is
    // exercised only through the failure paths below and, once diodes
    // land in M3, through real homotopy rescues).
    const auto run = run_text(
        "plain\nV1 in 0 5\nR1 in out 1k\nR2 out 0 2k\n.op\n");
    REQUIRE(run);
    REQUIRE(run->reports.size() == 1);
    CHECK(run->reports[0].stage == "newton");

    const auto dc = run_text(
        "plaindc\nV1 in 0 5\nR1 in out 1k\nR2 out 0 2k\n.dc V1 0 5 1\n");
    REQUIRE(dc);
    CHECK(dc->reports[0].stage == "newton");
}

TEST_CASE("ladder: gmin and source stepping are value-only (pattern-safe)") {
    // A gmin stage that ran on a converged deck cannot change the answer:
    // force the ladder's machinery by starving itl1 on a deck whose
    // gmin-shunted system still converges in the one allowed pass
    // (the shunt dominates the 1k conductance by ~7 orders).
    const auto run = run_text(
        "starved\nV1 in 0 5\nR1 in 0 1k\n.options itl1 1 gmin 1e-3\n.op\n");
    REQUIRE(!run);
    // The full ladder ran and failed - but the run must still be a
    // Convergence-class spice error naming all four stages.
    CHECK(run.error().category == std::string("spice"));
    CHECK(run.error().code ==
          static_cast<std::int32_t>(ws::spice::ErrorKind::Convergence));
    CHECK(run.error().message.find("pseudo-transient") != std::string::npos);

    // The same deck WITHOUT the starvation solves normally: the ladder
    // left no residue in the settings or the state.
    const auto ok = run_text(
        "healthy\nV1 in 0 5\nR1 in 0 1k\n.options gmin 1e-3\n.op\n");
    REQUIRE(ok);
    CHECK(ok->results.tables[0].signal("v(in)")[0] == approx(5.0));
}

TEST_CASE("ladder: a chattering self-holding switch is a clean failure") {
    // The relaxation-oscillation-at-DC circuit: no DC operating point
    // exists (the switch's two states each cross the other's threshold),
    // so the FULL ladder fails - the same verdict ngspice gives, never a
    // hang, never a silently wrong answer.
    const auto run = run_text(
        "latch\n"
        "V1 vcc 0 5\n"
        "R1 vcc out 1k\n"
        "S1 out 0 out 0 swm\n"
        ".model swm SW(Ron=100 Roff=1meg Vt=2 Vh=1)\n"
        ".op\n");
    REQUIRE(!run);
    CHECK(run.error().category == std::string("spice"));
    CHECK(run.error().code ==
          static_cast<std::int32_t>(ws::spice::ErrorKind::Convergence));
    CHECK(run.error().message.find("pseudo-transient continuation") !=
          std::string::npos);
}

// --- §7.6: .step families + Monte Carlo (§8.5 rev) --------------------------------

TEST_CASE(".step: param sweep produces a family table with step axes") {
    const auto run = run_text(
        "family\n"
        ".param rbot=1k\n"
        "V1 in 0 5\n"
        "R1 in out 1k\n"
        "R2 out 0 {rbot}\n"
        ".step param rbot 1k 3k 1k\n"
        ".op\n");
    REQUIRE(run);
    REQUIRE(run->results.tables.size() == 1);
    const Table& t = run->results.tables[0];
    // Axes: step, rbot (the .op's own axis is empty).
    CHECK(t.sweep_names == std::vector<std::string>{"step", "rbot"});
    REQUIRE(t.row_count() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(t.axis(0)[i] == static_cast<double>(i + 1));
        CHECK(t.axis(1)[i] == approx(1e3 * static_cast<double>(i + 1)));
        // Divider: v(out) = 5 * rbot / (1k + rbot).
        const double rb = t.axis(1)[i];
        CHECK(t.signal("v(out)")[i] == approx(5.0 * rb / (1e3 + rb)));
    }
}

TEST_CASE(".step: list form and nested cards, first outermost") {
    const auto run = run_text(
        "nested\n"
        "V1 in 0 {vin}\n"
        "R1 in out 1k\n"
        "R2 out 0 {rl}\n"
        ".step param rl list 1k 2k\n"
        ".step param vin 4 6 2\n"
        ".op\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    CHECK(t.sweep_names == std::vector<std::string>{"step", "rl", "vin"});
    REQUIRE(t.row_count() == 4);
    // First card outermost: rl changes slowest.
    CHECK(t.axis(1)[0] == approx(1e3));
    CHECK(t.axis(1)[1] == approx(1e3));
    CHECK(t.axis(1)[2] == approx(2e3));
    CHECK(t.axis(1)[3] == approx(2e3));
    CHECK(t.axis(2)[0] == approx(4.0));
    CHECK(t.axis(2)[1] == approx(6.0));
    for (std::size_t i = 0; i < 4; ++i) {
        const double rl = t.axis(1)[i];
        const double vin = t.axis(2)[i];
        CHECK(t.signal("v(out)")[i] == approx(vin * rl / (1e3 + rl)));
    }
}

TEST_CASE(".step: the source-value form patches the dc slot") {
    const auto run = run_text(
        "srcstep\n"
        "V1 in 0 5\n"
        "R1 in out 1k\n"
        "R2 out 0 1k\n"
        ".step V1 1 5 2\n"
        ".op\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    REQUIRE(t.row_count() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(t.signal("v(in)")[i] == approx(t.axis(1)[i]));
        CHECK(t.signal("v(out)")[i] == approx(t.axis(1)[i] / 2.0));
    }
}

TEST_CASE(".step: a temp step re-elaborates per point") {
    // M1/M2 devices are temp-invariant, so the VALUES stay identical -
    // what is pinned here is the family shape and that the run completes
    // through the re-elaboration path.
    const auto run = run_text(
        "tempstep\n"
        "V1 in 0 5\n"
        "R1 in out 1k\n"
        "R2 out 0 1k\n"
        ".step temp list 0 100\n"
        ".op\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    CHECK(t.sweep_names == std::vector<std::string>{"step", "temp"});
    REQUIRE(t.row_count() == 2);
    CHECK(t.signal("v(out)")[0] == approx(2.5));
    CHECK(t.signal("v(out)")[1] == approx(2.5));
}

TEST_CASE(".step: Monte Carlo - gauss/flat draw per point, seed reproduces") {
    const std::string deck =
        "mc\n"
        ".param rl=2k+gauss(200)\n"
        "V1 in 0 5\n"
        "R1 in out 1k\n"
        "R2 out 0 {rl}\n"
        ".step param run 1 40 1\n"
        ".op\n";
    auto a = run_text(deck);
    REQUIRE(a);
    auto b = run_text(deck);
    REQUIRE(b);
    const Table& ta = a->results.tables[0];
    const Table& tb = b->results.tables[0];
    REQUIRE(ta.row_count() == 40);
    REQUIRE(tb.row_count() == 40);
    // Deterministic: the recorded seed reproduces the family bit-for-bit.
    for (std::size_t i = 0; i < 40; ++i) {
        CHECK(ta.signal("v(out)")[i] == tb.signal("v(out)")[i]);
    }
    // And the draws actually varied (40 distinct values would be extreme
    // chance; a handful suffices).
    std::size_t distinct = 0;
    for (std::size_t i = 1; i < 40; ++i) {
        if (ta.signal("v(out)")[i] != ta.signal("v(out)")[0]) {
            ++distinct;
        }
    }
    CHECK(distinct > 10);

    // A different seed gives a different family.
    auto c = run_text("mc\n.options seed=7\n.param rl=2k+gauss(200)\n"
                      "V1 in 0 5\nR1 in out 1k\nR2 out 0 {rl}\n"
                      ".step param run 1 40 1\n.op\n");
    REQUIRE(c);
    const Table& tc = c->results.tables[0];
    std::size_t differing = 0;
    for (std::size_t i = 0; i < 40; ++i) {
        if (tc.signal("v(out)")[i] != ta.signal("v(out)")[i]) {
            ++differing;
        }
    }
    CHECK(differing > 10);
    CHECK(a->seed == 1);
    CHECK(c->seed == 7);
}

TEST_CASE(".step: gauss in a plain run draws ONCE, coherently across analyses") {
    // No .step cards: the family is the single default point, and the
    // draw happens once - both analyses of the deck see the SAME rl
    // (a per-analysis redraw would silently change the circuit between
    // the .op and the .tran that follows).
    const std::string deck =
        "nog\n"
        ".param rl=2k+gauss(200)\n"
        "V1 in 0 PULSE(0 5 1u 1u 1u 10u)\n"
        "R1 in out 1k\n"
        "R2 out 0 {rl}\n"
        "C1 out 0 1n\n"
        ".op\n"
        ".tran 1u 5u\n";
    const auto run = run_text(deck);
    REQUIRE(run);
    REQUIRE(run->results.tables.size() == 2);
    const Table& op = run->results.tables[0];
    const Table& tran = run->results.tables[1];
    CHECK(op.signal("v(out)")[0] == tran.signal("v(out)")[0]);
    // Deterministic under the recorded seed.
    const auto again = run_text(deck);
    REQUIRE(again);
    CHECK(op.signal("v(out)")[0] ==
          again->results.tables[0].signal("v(out)")[0]);
}

TEST_CASE(".step: a failing point aborts with the step context; keep-going lists") {
    // Point run=2 forces a zero-valued resistor (check_params rejects).
    const std::string deck =
        "failing\n"
        ".param rl=2k*(run!=2)\n"
        "V1 in 0 5\n"
        "R1 in out 1k\n"
        "R2 out 0 {rl}\n"
        ".step param run 1 3 1\n"
        ".op\n";
    const auto run = run_text(deck);
    REQUIRE(!run);
    CHECK(run.error().message.find("run=2") != std::string::npos);

    RunSettings s;
    s.keep_going = true;
    const auto kg = run_text(deck, nullptr, s);
    REQUIRE(!kg);
    // The failure list names the point AND the run kept the others.
    CHECK(kg.error().message.find("run=2") != std::string::npos);
}

TEST_CASE(".step: cartesian product over the cap fails cleanly, never throws") {
    // Two .step cards whose per-axis counts are within the 1M cap but
    // whose product exceeds it would reserve ~10^12 entries and abort
    // with std::bad_alloc (exit 134) before any analysis ran. The product
    // cap rejects the deck with a Semantic error naming the offending card.
    // Each card uses 0..2000 step 2 = 1,001 points (well within the 1M
    // per-axis cap); the product 1,001 x 1,001 = 1,002,001 > 1M trips the
    // cartesian product guard.
    const std::string deck =
        "huge\n"
        "V1 in 0 5\n"
        "R1 in out 1k\n"
        "R2 out 0 1k\n"
        ".step param a 0 2000 2\n"
        ".step param b 0 2000 2\n"
        ".op\n";
    const auto run = run_text(deck);
    REQUIRE(!run);
    CHECK(run.error().category == std::string{"spice"});
    CHECK(run.error().message.find("cartesian product") != std::string::npos);
}

// --- §0.4 rev: the measurement library + .meas -------------------------------------

TEST_CASE("measure: library functions over analytic signals") {
    namespace measure = ws::spice::measure;
    // A fine unit ramp: axis 0..1, sig = t (exact interpolation
    // everywhere; 101 samples keep the trapezoidal integrals close).
    std::vector<double> axis;
    std::vector<double> ramp;
    for (int k = 0; k <= 100; ++k) {
        axis.push_back(static_cast<double>(k) / 100.0);
        ramp.push_back(static_cast<double>(k) / 100.0);
    }
    CHECK(measure::value_at(axis, ramp, 0.625) == approx(0.625));
    CHECK(measure::average(axis, ramp) == approx(0.5));
    CHECK(measure::maximum(ramp) == approx(1.0));
    CHECK(measure::minimum(ramp) == approx(0.0));
    CHECK(measure::peak_to_peak(ramp) == approx(1.0));
    CHECK(measure::integral(axis, ramp) == approx(0.5).epsilon(1e-4));
    CHECK(measure::rms(axis, ramp) ==
          doctest::Approx(std::sqrt(1.0 / 3.0)).epsilon(1e-4));
    CHECK(measure::derivative_at(axis, ramp, 0.5) == approx(1.0));
    // Crossings: the ramp crosses 0.3 at t=0.3 (first), 0.3 again never.
    CHECK(measure::cross_time(axis, ramp, 0.3, measure::Dir::Rise) == approx(0.3));
    CHECK(!measure::cross_time(axis, ramp, 0.3, measure::Dir::Fall).has_value());
    CHECK(!measure::cross_time(axis, ramp, 1.5, measure::Dir::Rise).has_value());
    // rise_time of the ramp = 0.8 (10%..90%).
    CHECK(measure::rise_time(axis, ramp) == approx(0.8));
    CHECK(measure::fall_time(axis, ramp) == std::nullopt);  // never falls

    // A triangle: up 0..1, down 1..2 - fall_time and windowing.
    const std::vector<double> tax{0.0, 1.0, 2.0};
    const std::vector<double> tri{0.0, 1.0, 0.0};
    CHECK(measure::rise_time(tax, tri) == approx(0.8));
    CHECK(measure::fall_time(tax, tri) == approx(0.8));
    CHECK(measure::average(tax, tri) == approx(0.5));
    // Window [0, 1]: just the rising leg.
    CHECK(measure::average(tax, tri, measure::Window::between(0.0, 1.0)) ==
          approx(0.5));
    CHECK(measure::average(tax, tri, measure::Window::between(1.0, 2.0)) ==
          approx(0.5));
    // trig/targ between two signals: targ crosses 0.25s AFTER trig (the
    // propagation-delay convention: the target search starts at the
    // trigger's crossing).
    const std::vector<double> five{0.0, 0.25, 0.5, 0.75, 1.0};
    const std::vector<double> delayed{-0.25, 0.0, 0.25, 0.5, 0.75};
    CHECK(measure::trig_targ(five, five, 0.5, measure::Dir::Rise, 1, five, delayed,
                             0.5, measure::Dir::Rise, 1) == approx(0.25));
}

TEST_CASE(".meas: the §10.1 row - RC rise time = RC*ln(9)") {
    const auto run = run_text(
        "rc meas\n"
        "V1 in 0 PULSE(0 5 1u 1n 1u 10m)\n"
        "R1 in out 1k\n"
        "C1 out 0 1n\n"
        ".tran 10n 10u\n"
        ".meas tran rt trig v(out) val=0.5 rise=1 targ v(out) val=4.5 rise=1\n"
        ".meas tran rt1090 avg v(out) from=1.000001 to=1.000002\n");
    REQUIRE(run);
    REQUIRE(run->measurements.size() == 2);
    REQUIRE(run->measurements[0].value.has_value());
    CHECK(*run->measurements[0].value ==
          doctest::Approx(1e3 * 1e-9 * std::log(9.0)).epsilon(1e-3));
}

TEST_CASE(".meas: AVG/RMS/MAX/MIN/PP/WHEN/FIND over a known transient") {
    // Full-amplitude sine after t=1ms: rms = A/sqrt(2), avg ~ 0.
    const auto run = run_text(
        "sine meas\n"
        "V1 in 0 SIN(0 2 1k 1m)\n"
        "R1 in out 1k\n"
        ".tran 10u 6m\n"
        ".meas tran rms_out rms v(out) from=2m to=6m\n"
        ".meas tran max_out max v(out) from=2m to=6m\n"
        ".meas tran min_out min v(out) from=2m to=6m\n"
        ".meas tran pp_out pp v(out) from=2m to=6m\n"
        ".meas tran tzero when v(out)=1 rise=2\n");
    REQUIRE(run);
    REQUIRE(run->measurements.size() == 5);
    const auto& rms_m = run->measurements[0];
    REQUIRE(rms_m.value.has_value());
    CHECK(*rms_m.value == doctest::Approx(2.0 / std::sqrt(2.0)).epsilon(2e-3));
    REQUIRE(run->measurements[1].value.has_value());
    CHECK(*run->measurements[1].value == doctest::Approx(2.0).epsilon(1e-2));
    REQUIRE(run->measurements[2].value.has_value());
    CHECK(*run->measurements[2].value == doctest::Approx(-2.0).epsilon(1e-2));
    REQUIRE(run->measurements[3].value.has_value());
    CHECK(*run->measurements[3].value == doctest::Approx(4.0).epsilon(2e-2));
    // v(out)=1 (rise=2): the 2nd rising crossing - one period after the
    // first at t = 1ms + T/12-ish... just check it is in [2ms, 5ms].
    REQUIRE(run->measurements[4].value.has_value());
    CHECK(*run->measurements[4].value > 2e-3);
    CHECK(*run->measurements[4].value < 5e-3);
}

TEST_CASE(".meas: a failed measurement is a note, never a run failure") {
    const auto run = run_text(
        "nofire\n"
        "V1 in 0 5\n"
        "R1 in out 1k\n"
        ".tran 1u 10u\n"
        ".meas tran never when v(out)=99\n");
    REQUIRE(run);  // the run itself succeeded (§9.5)
    REQUIRE(run->measurements.size() == 1);
    CHECK(!run->measurements[0].value.has_value());
    CHECK(run->measurements[0].note.find("never crossed") != std::string::npos);
}

TEST_CASE(".meas: rise_time/fall_time as LIBRARY calls on a run's table") {
    // The C++-first story: the same numbers .meas reports, without text.
    const auto run = run_text(
        "lib\n"
        "V1 in 0 PULSE(0 5 1u 1n 1u 10m)\n"
        "R1 in out 1k\n"
        "C1 out 0 1n\n"
        ".tran 10n 10u\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    const auto rt = ws::spice::measure::rise_time(t.axis(), t.signal("v(out)"));
    REQUIRE(rt.has_value());
    CHECK(*rt == doctest::Approx(1e3 * 1e-9 * std::log(9.0)).epsilon(1e-3));
}

// --- §0.5 rev: .save/.print expressions + @dev[param] -------------------------------

TEST_CASE("save expressions: difference, power and a bound parameter") {
    const auto run = run_text(
        "pw\n"
        "V1 in 0 5\n"
        "R1 in out 1k\n"
        "R2 out 0 1k\n"
        ".op\n"
        ".print op v(out)-v(in) v(in)*i(V1) @R1[value] @V1[dc]\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    CHECK(signal_names(t) == std::vector<std::string>{
                                 "v(out)-v(in)", "v(in)*i(V1)", "@R1[value]",
                                 "@V1[dc]"});
    REQUIRE(t.row_count() == 1);
    // v(out) = 2.5, v(in) = 5, i(V1) = -2.5mA.
    CHECK(t.signal("v(out)-v(in)")[0] == approx(-2.5));
    CHECK(t.signal("v(in)*i(V1)")[0] == approx(5.0 * -2.5e-3));
    CHECK(t.signal("@R1[value]")[0] == approx(1e3));
    CHECK(t.signal("@V1[dc]")[0] == approx(5.0));
}

TEST_CASE("save expressions: transient math over the stored columns") {
    // PULSE starts at 1 V so the ratio never divides by zero (a 0/0 is a
    // domain error by §8.5, not a silent NaN).
    const auto run = run_text(
        "trig math\n"
        "V1 in 0 PULSE(1 5 1u 1n 1u 10m)\n"
        "R1 in out 1k\n"
        "C1 out 0 1n\n"
        ".tran 10n 10u\n"
        ".print tran time v(out) v(out)/v(in)\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    // The axis is the time column (no duplicate); the ratio is post-hoc.
    CHECK(signal_names(t) == std::vector<std::string>{"v(out)", "v(out)/v(in)"});
    REQUIRE(t.row_count() > 100);
    // Long after the edge: v(out)/v(in) -> 1 (the DC transfer).
    CHECK(t.signal("v(out)/v(in)").back() == doctest::Approx(1.0).epsilon(1e-3));
}

TEST_CASE("save expressions: differential v(a,b) and unknown names") {
    const auto run = run_text(
        "diff\n"
        "V1 a 0 3\n"
        "V2 b 0 2\n"
        "R1 a x 1k\n"
        "R2 x b 1k\n"
        ".op\n"
        ".print op v(a,b) v(x,b)\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    CHECK(t.signal("v(a,b)")[0] == approx(1.0));
    CHECK(t.signal("v(x,b)")[0] == approx(0.5));

    // An unknown node names itself in the error.
    const auto bad = run_text(
        "bad\nV1 a 0 3\nR1 a 0 1k\n.op\n.print op v(nope)\n");
    REQUIRE(!bad);
    CHECK(bad.error().message.find("v(nope)") != std::string::npos);

    // A model quantity that no device carries yet says so (M4).
    const auto gm = run_text(
        "gm\nV1 a 0 3\nR1 a 0 1k\n.op\n.print op @R1[gm]\n");
    REQUIRE(!gm);
    CHECK(gm.error().message.find("@r1[gm]") != std::string::npos);
}

TEST_CASE("save expressions: per-point @dev[param] in a stepped family") {
    const auto run = run_text(
        "familysave\n"
        ".param rl=1k\n"
        "V1 in 0 5\n"
        "R1 in out 1k\n"
        "R2 out 0 {rl}\n"
        ".step param rl 1k 3k 1k\n"
        ".op\n"
        ".print op v(out) @R2[value]\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    // Axes: step, rl; then the requested expressions.
    REQUIRE(t.row_count() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        const double rl = t.axis(1)[i];
        CHECK(t.signal("@R2[value]")[i] == approx(rl));
        CHECK(t.signal("v(out)")[i] == approx(5.0 * rl / (1e3 + rl)));
    }
}

// --- .raw (§7.8 rev) + the lint pass (§9.3 rev) ------------------------------------

TEST_CASE("raw: header shape and binary payload round-trip") {
    const auto run = run_text(
        "rawtr\n"
        "V1 in 0 5\n"
        "R1 in out 1k\n"
        "R2 out 0 1k\n"
        ".op\n");
    REQUIRE(run);
    const std::string raw = ws::spice::table_raw(run->results.tables[0]);
    // Header: title, plotname, real flags, counts, variables, binary mark.
    CHECK(raw.find("Title: * ws-spice\n") == 0);
    CHECK(raw.find("Plotname: Operating Point\n") != std::string::npos);
    CHECK(raw.find("Flags: real\n") != std::string::npos);
    CHECK(raw.find("No. Variables: 3\n") != std::string::npos);
    CHECK(raw.find("No. Points: 1\n") != std::string::npos);
    CHECK(raw.find("\t0\tv(in)\tvoltage\n") != std::string::npos);
    CHECK(raw.find("\t2\ti(V1)\tcurrent\n") != std::string::npos);
    CHECK(raw.find("Binary:\n") != std::string::npos);
    // Payload: 1 point x 3 doubles after the "Binary:\n" mark.
    const std::size_t mark = raw.find("Binary:\n") + std::strlen("Binary:\n");
    CHECK(raw.size() - mark == 3 * sizeof(double));
    double vals[3];
    std::memcpy(vals, raw.data() + mark, sizeof(vals));
    CHECK(vals[0] == doctest::Approx(5.0).epsilon(1e-9));
    CHECK(vals[1] == doctest::Approx(2.5).epsilon(1e-9));
    CHECK(vals[2] == doctest::Approx(-2.5e-3).epsilon(1e-9));
    // Deterministic (§10.5): no Date/Offset lines by default.
    CHECK(raw.find("Date:") == std::string::npos);
    CHECK(raw.find("Offset:") == std::string::npos);
}

TEST_CASE("raw: transient axis type and .ac complex merging") {
    const auto run = run_text(
        "rawac\n"
        "V1 in 0 DC 1 AC 1\n"
        "R1 in out 1k\n"
        "C1 out 0 1u\n"
        ".tran 10u 20u\n"
        ".ac dec 1 1k 10k\n");
    REQUIRE(run);
    const std::string tr = ws::spice::table_raw(run->results.tables[0]);
    CHECK(tr.find("Plotname: Transient Analysis\n") != std::string::npos);
    CHECK(tr.find("\t0\ttime\ttime\n") != std::string::npos);
    CHECK(tr.find("Flags: real\n") != std::string::npos);

    // .ac: re()/im() pairs merge into complex columns named v(out) etc.
    const std::string ac = ws::spice::table_raw(run->results.tables[1]);
    CHECK(ac.find("Plotname: AC Analysis\n") != std::string::npos);
    CHECK(ac.find("Flags: complex\n") != std::string::npos);
    CHECK(ac.find("\t2\tv(out)\tvoltage\n") != std::string::npos);
    CHECK(ac.find("re(") == std::string::npos);  // wrappers gone
    // Points x (axis + 3 signals) x 2 doubles.
    const std::size_t mark = ac.find("Binary:\n") + std::strlen("Binary:\n");
    CHECK((ac.size() - mark) % (2 * sizeof(double)) == 0);
}

TEST_CASE("lint: the M/milli and F/femto traps fire with hints") {
    std::vector<Warning> warnings;
    const auto parsed = read_deck("t.cir",
                                  "traps\n"
                                  "V1 in 0 5\n"
                                  "R1 in out 10M\n"
                                  "R2 out 0 2k\n"
                                  "C1 out 0 100F\n"
                                  "L1 in 0 1m\n"
                                  "R3 in 0 3meg\n"
                                  ".op\n",
                                  ws::spice::ParseOptions{});
    REQUIRE(parsed);
    const auto ast = build_deck_ast(*parsed, warnings);
    REQUIRE(ast);
    REQUIRE(warnings.empty());  // parse-stage warnings: none expected here

    ws::spice::lint_number_traps(*ast, warnings);
    REQUIRE(warnings.size() == 2);
    CHECK(warnings[0].diag.code == "spice.warn.milli_resistor");
    CHECK(warnings[0].diag.loc.line == 3);
    CHECK(warnings[0].render().find("milli") != std::string::npos);
    CHECK(warnings[0].render().find("MEG") != std::string::npos);
    CHECK(warnings[1].diag.code == "spice.warn.femto_cap");
    CHECK(warnings[1].diag.loc.line == 5);
    // 1m on an inductor (millihenry - the intended unit) and 3meg on a
    // resistor are NOT findings.
}

TEST_CASE("lint: lint_deck surfaces topology findings without simulating") {
    std::vector<Warning> warnings;
    const auto parsed = read_deck(
        "t.cir", "float\nV1 in 0 5\nC1 in 0 1n\nC2 in flt 1n\n.op\n",
        ws::spice::ParseOptions{});
    REQUIRE(parsed);
    const auto ast = build_deck_ast(*parsed, warnings);
    REQUIRE(ast);
    const ws::core::Status st = ws::spice::lint_deck(*ast, warnings);
    REQUIRE(!st);  // the §6.6 finding is a hard error, not a warning
    CHECK(st.error().message.find("no DC path to ground") != std::string::npos);
    CHECK(st.error().message.find("flt") != std::string::npos);
}

// --- §8.3 rev: the explicit compat mode ---------------------------------------------

TEST_CASE("compat: strict rejects the LTspice forms, naming the ngspice spelling") {
    const std::string deck =
        "st\n.param rl=2k\nV1 in 0 5\nR1 in out 1k\nR2 out 0 {rl}\n.op\n";
    // Default (ngspice): accepted, as ever.
    REQUIRE(run_text(deck));
    // strict: rejected with the fix spelled out.
    const auto strict = run_text("st\n.options compat=strict\n.param rl=2k\n"
                                 "V1 in 0 5\nR1 in out 1k\nR2 out 0 {rl}\n.op\n");
    REQUIRE(!strict);
    CHECK(strict.error().message.find("{rl") != std::string::npos);
    CHECK(strict.error().message.find("compat=ltspice") != std::string::npos);

    // ltspice: explicitly accepted.
    REQUIRE(run_text("st\n.options compat=ltspice\n.param rl=2k\nV1 in 0 5\n"
                     "R1 in out 1k\nR2 out 0 {rl}\n.op\n"));

    // Rser= and params: sections reject under strict too.
    const auto rser = run_text("st\n.options compat=strict\nV1 a b 5\n"
                               "L1 a b 1m Rser=1\nR1 b 0 1k\n.op\n");
    REQUIRE(!rser);
    CHECK(rser.error().message.find("Rser=") != std::string::npos);
    const auto par = run_text(
        "st\n.options compat=strict\n.subckt d a b params: r=1k\n"
        "Ri a b {r}\n.ends\nX1 x 0 d\nV1 x 0 5\n.op\n");
    REQUIRE(!par);
    CHECK(par.error().message.find("params:") != std::string::npos);

    // An unknown mode is itself a parse error.
    const auto bad = run_text("st\n.options compat=pspice\nV1 a 0 5\nR1 a 0 1k\n.op\n");
    REQUIRE(!bad);
    CHECK(bad.error().message.find("ngspice, ltspice or strict") != std::string::npos);
}

TEST_CASE(".meas: measures each .step point, never the step axis") {
    // Regression: the family table's axis(0) is the step NUMBER column;
    // .meas used it as the sweep axis and produced garbage (0s). Now the
    // measurement splits per point and sees time.
    const auto run = run_text(
        "fam meas\n"
        "V1 in 0 PULSE(1 5 1u 1n 1u 10m)\n"
        "R1 in out 1k\n"
        "R2 out 0 {rl}\n"
        "C1 out 0 1n\n"
        ".param rl=1k\n"
        ".step param rl 1k 3k 1k\n"
        ".tran 10n 10u\n"
        ".meas tran rt trig v(out) val=1.5 rise=1 targ v(out) val=2 rise=1\n");
    REQUIRE(run);
    // One measurement per step point, @-suffixed past the first.
    REQUIRE(run->measurements.size() == 3);
    CHECK(run->measurements[0].name == "rt");
    CHECK(run->measurements[1].name == "rt@2");
    CHECK(run->measurements[2].name == "rt@3");
    // Each is a real rise time against TIME (the step-axis bug produced
    // uniform zeros): positive, sane, and the points differ.
    for (const ws::spice::Measurement& m : run->measurements) {
        REQUIRE(m.value.has_value());
        CHECK(*m.value > 100e-9);
        CHECK(*m.value < 10e-6);
    }
    CHECK(*run->measurements[0].value != *run->measurements[1].value);
    CHECK(*run->measurements[1].value != *run->measurements[2].value);
}

// --- §10.1/§10.4 M3: the diode (Shockley + pnjlim + the ladder) -------------------

namespace {

// Boltzmann/q in V/K (matches devices_m3.cpp's kVtPerK).
constexpr double kVtPerK = 8.617333262145e-5;

// The thermal voltage at 27°C (300.15 K) - the default Tnom.
constexpr double kVt27 = (27.0 + 273.15) * kVtPerK;  // ≈ 0.02585 V

// Halley's method for the principal branch W0(z), z > 0. 50 iterations
// converges to machine precision for every z > 0 we feed it (the diode+R
// closed form lands at z ~ 10^70+). The reference for the §10.1 diode row.
double lambert_w0(double z) {
    if (z <= 0.0) {
        return 0.0;
    }
    // Initial guess: ln(z+1) for small z, ln(z) - ln(ln(z)) for large z.
    double w = (z > 1.0) ? std::log(z) - std::log(std::log(z)) : std::log(z + 1.0);
    for (int i = 0; i < 50; ++i) {
        const double e = std::exp(w);
        const double f = w * e - z;
        const double df = e * (w + 1.0);
        const double d2f = e * (w + 2.0);
        const double delta = 2.0 * df * df - f * d2f;
        if (delta == 0.0) {
            break;
        }
        const double step = 2.0 * f * df / delta;
        w -= step;
        if (std::abs(step) < 1e-15 * std::max(1.0, std::abs(w))) {
            break;
        }
    }
    return w;
}

// The diode + series R .op closed form (gmin = 0):
//   Vs = I*R + Vd,  I = Is*(exp(Vd/(n*Vt)) - 1)
//   => I = (n*Vt/R) * W0((Is*R/(n*Vt)) * exp((Vs+Is*R)/(n*Vt))) - Is
// Returns {Vd, I}.
struct DiodeOpRef {
    double vd;
    double i;
};
DiodeOpRef diode_series_r_op(double vs, double r, double is_val, double n) {
    const double B = n * kVt27;
    const double A = is_val * r;
    const double z_arg = (A / B) * std::exp((vs + A) / B);
    const double z = lambert_w0(z_arg);
    const double i = (B / r) * z - is_val;
    const double vd = vs - i * r;
    return {vd, i};
}

}  // namespace

TEST_CASE("M3 diode: .op current matches the Lambert-W closed form") {
    // §10.1 row: diode + series R, .op current vs Lambert-W at reltol=1e-9.
    // Is=1e-14, N=1, R=1k, Vs=5V. gmin=0 so the closed form is exact (the
    // default gmin=1e-12 adds ~1e-12 A leakage - well below the 1e-7 gate).
    RunSettings settings;
    settings.reltol = 1e-9;  // tighten past the 1e-3 default
    settings.gmin = 0.0;     // exact closed form
    const auto run = run_text(
        "diode op\n"
        "V1 in 0 DC 5\n"
        "R1 in out 1k\n"
        "D1 out 0 dx\n"
        ".model dx D(Is=1e-14 N=1)\n"
        ".op\n",
        nullptr, settings);
    REQUIRE(run);
    REQUIRE(run->results.tables.size() == 1);
    const Table& t = run->results.tables[0];
    const std::size_t col_v_out = col_of(t, "v(out)");
    const std::size_t col_i_v1 = col_of(t, "i(V1)");
    const double v_out = t.signals[col_v_out].data[0];
    const double i_v1 = t.signals[col_i_v1].data[0];
    // Vd = v(out) - 0 = v(out) (cathode is ground).
    const double vd = v_out;
    // i(V1) is negative when the source delivers (ngspice sign convention);
    // the diode current magnitude is |i(V1)|.
    const double i_mag = -i_v1;
    const auto [ref_vd, ref_i] = diode_series_r_op(5.0, 1000.0, 1e-14, 1.0);
    // §10.1 tolerance: 1e-7 rel at reltol=1e-9.
    CHECK(vd == doctest::Approx(ref_vd).epsilon(1e-7));
    CHECK(i_mag == doctest::Approx(ref_i).epsilon(1e-7));
}

TEST_CASE("M3 diode: .dc sweep tracks the closed form at every point") {
    // Sweep the source from 0.1V to 1.0V (10 points); at each, the diode
    // current matches Lambert-W. Below ~0.5V the current is tiny (sub-uA);
    // above, it grows exponentially. The 1e-6 rel gate covers both regimes.
    RunSettings settings;
    settings.reltol = 1e-9;
    settings.gmin = 0.0;
    const auto run = run_text(
        "diode dc\n"
        "V1 in 0 DC 0\n"
        "R1 in out 1k\n"
        "D1 out 0 dx\n"
        ".model dx D(Is=1e-14 N=1)\n"
        ".dc V1 0.1 1.0 0.1\n",
        nullptr, settings);
    REQUIRE(run);
    REQUIRE(run->results.tables.size() == 1);
    const Table& t = run->results.tables[0];
    const std::size_t col_i_v1 = col_of(t, "i(V1)");
    REQUIRE(t.signals[col_i_v1].data.size() == 10);
    for (std::size_t k = 0; k < 10; ++k) {
        const double vs = 0.1 + 0.1 * static_cast<double>(k);
        const auto [ref_vd, ref_i] = diode_series_r_op(vs, 1000.0, 1e-14, 1.0);
        const double i_mag = -t.signals[col_i_v1].data[k];
        // Skip the smallest points where abstol dominates (current ~ 1e-13 A
        // at Vs=0.1 - the 1e-12 abstol floor swamps the signal). Compare
        // only where the current is meaningfully above abstol.
        if (ref_i > 1e-9) {
            CHECK(i_mag == doctest::Approx(ref_i).epsilon(1e-6));
        }
    }
}

TEST_CASE("M3 diode: the i(D1) probe column rides every real analysis") {
    // §7.8's terminal-current probe: the save list grows an "i(D1)" column
    // the device itself evaluates (MNA never carries a diode branch). In a
    // series loop it must agree with the source's own branch current to
    // solver precision, Lambert-W to closed form, and survive the Rs>0
    // mid-node path.
    RunSettings settings;
    settings.reltol = 1e-9;
    settings.gmin = 0.0;
    SUBCASE(".op: Lambert-W, and the series loop's own i(V1)") {
        const auto run = run_text(
            "diode probe op\n"
            "V1 in 0 DC 5\n"
            "R1 in out 1k\n"
            "D1 out 0 dx\n"
            ".model dx D(Is=1e-14 N=1)\n"
            ".op\n",
            nullptr, settings);
        REQUIRE(run);
        const Table& t = run->results.tables[0];
        const auto i_d1 = t.signal("i(D1)");
        REQUIRE(i_d1.size() == 1);
        const auto [ref_vd, ref_i] = diode_series_r_op(5.0, 1000.0, 1e-14, 1.0);
        CHECK(i_d1[0] == doctest::Approx(ref_i).epsilon(1e-7));
        // One loop, one current: the probe and the source branch agree.
        CHECK(i_d1[0] == doctest::Approx(-t.signal("i(V1)")[0]).epsilon(1e-9));
    }
    SUBCASE(".op: Rs > 0 reads the series element, not the junction") {
        // With Rs=5 the honest probe is (v(a)-v(mid))*area/Rs - and the
        // loop invariant still pins it to i(V1).
        const auto run = run_text(
            "diode probe rs\n"
            "V1 in 0 DC 5\n"
            "R1 in out 1k\n"
            "D1 out 0 dx\n"
            ".model dx D(Is=1e-14 N=1 Rs=5)\n"
            ".op\n",
            nullptr, settings);
        REQUIRE(run);
        const Table& t = run->results.tables[0];
        const auto i_d1 = t.signal("i(D1)");
        REQUIRE(i_d1.size() == 1);
        CHECK(i_d1[0] > 0.0);  // forward conduction, anode -> cathode
        CHECK(i_d1[0] == doctest::Approx(-t.signal("i(V1)")[0]).epsilon(1e-9));
    }
    SUBCASE(".dc and .tran carry the column per sample") {
        const auto run = run_text(
            "diode probe sweep\n"
            "V1 in 0 DC 0\n"
            "R1 in out 1k\n"
            "D1 out 0 dx\n"
            ".model dx D(Is=1e-14 N=1)\n"
            ".dc V1 0.4 1.0 0.2\n"
            ".tran 1m 3m\n",
            nullptr, settings);
        REQUIRE(run);
        REQUIRE(run->results.tables.size() == 2);
        const Table& dc = run->results.tables[0];
        REQUIRE(dc.signal("i(D1)").size() == 4);
        for (std::size_t k = 0; k < 4; ++k) {
            const double vs = 0.4 + 0.2 * static_cast<double>(k);
            const auto [ref_vd, ref_i] = diode_series_r_op(vs, 1000.0, 1e-14, 1.0);
            if (ref_i > 1e-9) {
                CHECK(dc.signal("i(D1)")[k] ==
                      doctest::Approx(ref_i).epsilon(1e-6));
            }
        }
        const Table& tran = run->results.tables[1];
        const auto i_d1 = tran.signal("i(D1)");
        REQUIRE(i_d1.size() == tran.row_count());
        REQUIRE(i_d1.size() > 2);
        for (const double v : i_d1) {
            CHECK(std::isfinite(v));
        }
        // A DC-biased loop: every sample is the same operating point.
        CHECK(i_d1.front() == doctest::Approx(i_d1.back()).epsilon(1e-3));
    }
    SUBCASE(".ac stays re()/im() over the small-signal save list") {
        const auto run = run_text(
            "diode probe ac\n"
            "V1 in 0 DC 5 AC 1\n"
            "R1 in out 1k\n"
            "D1 out 0 dx\n"
            ".model dx D(Is=1e-14 N=1)\n"
            ".ac dec 2 1k 10k\n");
        REQUIRE(run);
        const Table& t = run->results.tables[0];
        CHECK(t.signal("i(D1)").empty());  // the probe opts out of .ac
        CHECK(!t.signal("re(v(out))").empty());
    }
}

TEST_CASE("M3 diode: pnjlim prevents exp() overflow on a stiff bias") {
    // Without pnjlim, the first Newton iterate from a cold start (x=0)
    // puts Vd ≈ Vs (the diode looks like a near-open at Vd=0, Gd ≈ 4e-13 S).
    // exp(100/0.026) = exp(3846) overflows to +inf, NaN-ing the solve.
    // With pnjlim, Vd is clamped to 2*Vt per iteration; the solve converges
    // in ~30-50 iterations to Vd ≈ 0.7V (the diode's forward drop). The
    // ladder's plain Newton stage handles this - gmin/source/PTC never
    // engage. This is the §5.3 limiter's reason for existing.
    RunSettings settings;
    settings.reltol = 1e-6;  // sane tolerance (the default 1e-3 also works)
    const auto run = run_text(
        "stiff diode\n"
        "V1 in 0 DC 100\n"
        "R1 in out 1k\n"
        "D1 out 0 dx\n"
        ".model dx D(Is=1e-14 N=1)\n"
        ".op\n",
        nullptr, settings);
    REQUIRE(run);
    REQUIRE(run->results.tables.size() == 1);
    const Table& t = run->results.tables[0];
    const double v_out = t.signals[col_of(t, "v(out)")].data[0];
    const double i_v1 = t.signals[col_of(t, "i(V1)")].data[0];
    // The diode clamps v(out) to its forward drop (~0.7V); the rest of the
    // 100V drops across R1, so i ≈ (100 - 0.7)/1000 ≈ 99.3 mA.
    CHECK(v_out < 1.0);   // forward drop, not 100V
    CHECK(v_out > 0.6);   // a real forward drop, not 0
    CHECK(-i_v1 == doctest::Approx((100.0 - v_out) / 1000.0).epsilon(1e-6));
    // The plain Newton stage converged (the ladder never needed gmin/PTC).
    REQUIRE(!run->reports.empty());
    CHECK(run->reports.back().stage == "newton");
}

TEST_CASE("M3 diode: half-wave rectifier .tran converges and stays sane") {
    // The §11 M3 ref-harness shape: a diode rectifier into a capacitive
    // filter. The diode conducts near the source peak (charging the cap to
    // ~Vpeak - Vd); elsewhere the diode is reverse-biased and the cap
    // discharges through the load (RC decay). This exercises the diode in
    // .tran over many timesteps, with the Newton loop converging per step.
    // Sanity gates: output is always ≥ -0.7 (a diode drop below ground at
    // worst, never -10V); peaks near Vpeak - Vd ≈ 4.3V; never exceeds Vpeak.
    RunSettings settings;
    settings.reltol = 1e-6;
    settings.itl4 = 50;  // the diode's pnjlim walk needs more than the default 10
    const auto run = run_text(
        "rectifier\n"
        "V1 in 0 SIN(0 5 1k)\n"
        "D1 in out dx\n"
        "C1 out 0 10u\n"
        "R1 out 0 1k\n"
        ".model dx D(Is=1e-14 N=1)\n"
        ".tran 10u 5m 0 50u\n",
        nullptr, settings);
    REQUIRE(run);
    REQUIRE(run->results.tables.size() == 1);
    const Table& t = run->results.tables[0];
    const std::size_t col_v_out = col_of(t, "v(out)");
    REQUIRE(t.signals[col_v_out].data.size() > 100);
    double v_min = 1e9, v_max = -1e9;
    for (const double v : t.signals[col_v_out].data) {
        v_min = std::min(v_min, v);
        v_max = std::max(v_max, v);
    }
    // The cap never charges above the source peak minus a diode drop.
    CHECK(v_max < 5.0);
    CHECK(v_max > 3.5);   // charges to within a diode drop of the 5V peak
    // The cap never goes strongly negative (the diode blocks reverse flow).
    CHECK(v_min > -1.0);  // at worst a diode drop below ground
    // And the run converged cleanly (no convergence diagnostic).
    REQUIRE(!run->reports.empty());
    CHECK(run->reports.back().stage.find("fail") == std::string::npos);
}

TEST_CASE("M3 diode: the Jacobian matches central finite differences") {
    // §10.4: every nonlinear device's load Jacobian vs central finite
    // differences over a bias grid. We bias the diode with a V source
    // directly across it (no series R), sweep Vd from 0.3 to 0.7V, and at
    // each point compare dI/dV (from two adjacent bias points) to the
    // analytical Gd = Is/(n*Vt)*exp(Vd/(n*Vt)). Tolerance 1e-5 rel (the
    // finite-difference step is 10mV - the truncation error dominates at
    // 1e-4 scale, not the solver's 1e-9).
    RunSettings settings;
    settings.reltol = 1e-9;
    settings.gmin = 0.0;
    const auto run = run_text(
        "diode gd\n"
        "Vd a 0 DC 0\n"
        "D1 a 0 dx\n"
        ".model dx D(Is=1e-14 N=1)\n"
        ".dc Vd 0.30 0.71 0.01\n",
        nullptr, settings);
    REQUIRE(run);
    REQUIRE(run->results.tables.size() == 1);
    const Table& t = run->results.tables[0];
    const std::size_t col_i = col_of(t, "i(Vd)");
    const std::size_t col_vd = col_of(t, "v(a)");
    REQUIRE(t.signals[col_i].data.size() > 10);
    // Central difference over the sweep axis (ΔV = 0.01V).
    for (std::size_t k = 1; k + 1 < t.signals[col_i].data.size(); ++k) {
        const double vd = t.signals[col_vd].data[k];
        // Skip points right at the vcrit knee where pnjlim may have clamped
        // (the linearization point would be Vd_lim, not Vd - the Jacobian
        // we measure is the EFFECTIVE one, not the analytical Gd at Vd).
        if (vd < 0.4 || vd > 0.65) {
            continue;
        }
        const double i_prev = -t.signals[col_i].data[k - 1];
        const double i_next = -t.signals[col_i].data[k + 1];
        const double dv = t.signals[col_vd].data[k + 1] - t.signals[col_vd].data[k - 1];
        const double di_dv = (i_next - i_prev) / dv;
        const double gd_analytical = 1e-14 / kVt27 * std::exp(vd / kVt27);
        CHECK(di_dv == doctest::Approx(gd_analytical).epsilon(1e-3));
    }
}

// --- §10.1/§10.4 M4a: the BJT (Ebers-Moll) ---------------------------------------

TEST_CASE("M4a BJT: forward-active .op matches Ebers-Moll closed form") {
    // §10.1 row: BJT forward-active bias, Ic/Ib/Vbe vs Ebers-Moll at 1e-4 rel.
    // NPN with Vbe=0.7V (forward-biased BE), Vc=5V (Vbc=-4.3V, reverse-biased
    // BC → forward-active region). Is=1e-16, Bf=100, Br=1, Nf=Nr=1.
    // Closed form (forward-active, Vbc << 0): Ic ≈ Is*exp(Vbe/Vt), Ib ≈ Ic/Bf.
    RunSettings settings;
    settings.reltol = 1e-9;
    settings.gmin = 0.0;
    const auto run = run_text(
        "bjt forward-active\n"
        "Vbe b 0 DC 0.7\n"
        "Vc c 0 DC 5\n"
        "Q1 c b 0 qn\n"
        ".model qn NPN(Is=1e-16 Bf=100 Br=1 Nf=1 Nr=1)\n"
        ".op\n",
        nullptr, settings);
    REQUIRE(run);
    REQUIRE(run->results.tables.size() == 1);
    const Table& t = run->results.tables[0];
    const double i_vc = t.signals[col_of(t, "i(Vc)")].data[0];   // delivers Ic
    const double i_vbe = t.signals[col_of(t, "i(Vbe)")].data[0]; // delivers Ib
    // Ebers-Moll forward-active closed form.
    const double vt = kVt27;
    const double is_val = 1e-16;
    const double bf = 100.0;
    const double af = bf / (bf + 1.0);
    const double i_be = is_val / af * (std::exp(0.7 / vt) - 1.0);
    const double i_bc = is_val / (1.0 / 2.0) * (std::exp((0.7 - 5.0) / vt) - 1.0);
    const double ref_ic = af * i_be - i_bc;   // into collector
    const double ref_ib = i_be * (1.0 - af) + i_bc * (1.0 - 1.0 / 2.0);
    // i(Vc) is negative when delivering (convention); |i(Vc)| ≈ Ic.
    // i(Vbe) is negative when delivering; |i(Vbe)| ≈ Ib.
    CHECK(-i_vc == doctest::Approx(ref_ic).epsilon(1e-4));
    CHECK(-i_vbe == doctest::Approx(ref_ib).epsilon(1e-4));
    // Sanity: in forward-active, Ic >> Ib and Ib ≈ Ic/Bf.
    CHECK(-i_vc > 1e-6);   // Ic is µA-scale
    CHECK(-i_vbe < -i_vc / 50.0);  // Ib << Ic
    CHECK(-i_vbe == doctest::Approx(-i_vc / bf).epsilon(1e-3));
}

TEST_CASE("M4a BJT: cutoff - both junctions off, Ic ≈ 0") {
    // Vbe=0, Vbc=0: both junctions at zero bias. Ibe=Ibc=0, so Ic=Ib=0
    // (only gmin leakage). The BJT is in cutoff.
    RunSettings settings;
    settings.reltol = 1e-9;
    const auto run = run_text(
        "bjt cutoff\n"
        "Vbe b 0 DC 0\n"
        "Vc c 0 DC 0\n"
        "Q1 c b 0 qn\n"
        ".model qn NPN(Is=1e-16 Bf=100)\n"
        ".op\n",
        nullptr, settings);
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    const double i_vc = t.signals[col_of(t, "i(Vc)")].data[0];
    const double i_vbe = t.signals[col_of(t, "i(Vbe)")].data[0];
    // With gmin=1e-12 (default), leakage is ~gmin*V = 0 (V=0). So Ic ≈ 0.
    CHECK(std::abs(i_vc) < 1e-9);
    CHECK(std::abs(i_vbe) < 1e-9);
}

TEST_CASE("M4a BJT: saturation - both junctions forward-biased") {
    // Vbe=0.8V, Vc=0.3V → Vbc=0.5V (both forward-biased → saturation).
    // In the transport model, Ic = Is*(exp(Vbe/Vt) - exp(Vbc/Vt)). With
    // voltage sources directly on the terminals (no collector resistor),
    // Ic is NOT dramatically lower than forward-active — the external
    // circuit doesn't limit it. But Ic IS less than the forward-active
    // value (Vbc=0 would give Is*exp(Vbe/Vt)); the exp(Vbc/Vt) term
    // subtracts. Sanity: Ic < forward-active, and Ib > Ic/Bf (the base
    // drives both junctions).
    RunSettings settings;
    settings.reltol = 1e-9;
    const auto run = run_text(
        "bjt saturation\n"
        "Vbe b 0 DC 0.8\n"
        "Vc c 0 DC 0.3\n"
        "Q1 c b 0 qn\n"
        ".model qn NPN(Is=1e-16 Bf=100 Br=1)\n"
        ".op\n",
        nullptr, settings);
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    const double i_vc = t.signals[col_of(t, "i(Vc)")].data[0];
    const double i_vbe = t.signals[col_of(t, "i(Vbe)")].data[0];
    const double i_mag = -i_vc;  // |Ic|
    const double ib_mag = -i_vbe;
    // Forward-active Ic at Vbe=0.8V (Vbc<<0): Is*exp(0.8/Vt) ≈ 2.9 mA.
    const double fa_ic = 1e-16 * std::exp(0.8 / kVt27);
    // In saturation Ic = Is*(exp(Vbe/Vt) - exp(Vbc/Vt)) < fa_ic.
    CHECK(i_mag < fa_ic);
    CHECK(i_mag > 0.0);  // still sourcing current (collector sinks)
    // The saturation Ic matches the transport formula.
    const double ref_ic = 1e-16 * (std::exp(0.8 / kVt27) - std::exp(0.5 / kVt27));
    CHECK(i_mag == doctest::Approx(ref_ic).epsilon(1e-4));
    // Ib is larger than Ic/Bf because the base drives both junctions
    // (Ib = Is/Bf*(exp(Vbe/Vt)-1) + Is/Br*(exp(Vbc/Vt)-1)).
    CHECK(ib_mag > i_mag / 100.0);
}

TEST_CASE("M4a BJT: .dc sweep of Vbe tracks exp(Vbe/Vt)") {
    // Sweep Vbe from 0.5 to 0.7V; at each point Ic ≈ Is*exp(Vbe/Vt)
    // (forward-active, since Vc=5V keeps Vbc << 0). The exponential I-V
    // is the BJT's signature - 60mV/decade of current.
    RunSettings settings;
    settings.reltol = 1e-9;
    settings.gmin = 0.0;
    const auto run = run_text(
        "bjt dc sweep\n"
        "Vbe b 0 DC 0\n"
        "Vc c 0 DC 5\n"
        "Q1 c b 0 qn\n"
        ".model qn NPN(Is=1e-16 Bf=100)\n"
        ".dc Vbe 0.5 0.7 0.05\n",
        nullptr, settings);
    REQUIRE(run);
    REQUIRE(run->results.tables.size() == 1);
    const Table& t = run->results.tables[0];
    const std::size_t col_vc = col_of(t, "i(Vc)");
    const std::size_t col_vbe = col_of(t, "v(b)");
    REQUIRE(t.signals[col_vc].data.size() == 5);
    for (std::size_t k = 0; k < t.signals[col_vc].data.size(); ++k) {
        const double vbe = t.signals[col_vbe].data[k];
        const double i_mag = -t.signals[col_vc].data[k];
        // Ebers-Moll forward-active: Ic ≈ Is*exp(Vbe/Vt) (the -1 and Ibc
        // terms are negligible at these biases).
        const double ref_ic = 1e-16 * std::exp(vbe / kVt27);
        if (ref_ic > 1e-9) {  // skip points where abstol dominates
            CHECK(i_mag == doctest::Approx(ref_ic).epsilon(1e-4));
        }
    }
}

TEST_CASE("M4a BJT: the ladder converges a stiff cold start") {
    // Vbe=0.8V directly across the BE junction - the first Newton iterate
    // from cold start (x=0) would put Vbe≈0.8V, and exp(0.8/0.026)=exp(31)
    // ≈ 3e13. pnjlim clamps the per-iteration jump to 2*Vt, walking Vbe_lim
    // up from 0 to ~0.8V in ~15 steps. The ladder's plain Newton stage
    // handles this (gmin/source/PTC never engage).
    RunSettings settings;
    settings.reltol = 1e-6;
    const auto run = run_text(
        "bjt stiff\n"
        "Vbe b 0 DC 0.8\n"
        "Vc c 0 DC 5\n"
        "Q1 c b 0 qn\n"
        ".model qn NPN(Is=1e-16 Bf=100)\n"
        ".op\n",
        nullptr, settings);
    REQUIRE(run);
    REQUIRE(!run->reports.empty());
    CHECK(run->reports.back().stage == "newton");
}

TEST_CASE("M4a BJT: PNP - polarity flips, same magnitudes") {
    // A PNP with Veb=0.7V (emitter at 0.7, base at 0) and Vc=-5V (collector
    // below base → Vcb reverse-biased for PNP → forward-active). The PNP's
    // Ic should flow OUT of the collector (negative i(Vc) by the "into the
    // terminal" convention), with the same magnitude as the NPN at Vbe=0.7V.
    RunSettings settings;
    settings.reltol = 1e-9;
    settings.gmin = 0.0;
    const auto run = run_text(
        "pnp forward-active\n"
        "Veb e 0 DC 0.7\n"
        "Vc c 0 DC -5\n"
        "Q1 c 0 e qp\n"
        ".model qp PNP(Is=1e-16 Bf=100)\n"
        ".op\n",
        nullptr, settings);
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    const double i_vc = t.signals[col_of(t, "i(Vc)")].data[0];
    // PNP: collector sources current → i(Vc) is POSITIVE (current flows
    // out of collector, so the Vc source sinks it → positive i(Vc)).
    // Magnitude matches the NPN at the same |Vbe|.
    const double ref_ic_mag = 1e-16 * std::exp(0.7 / kVt27);
    CHECK(i_vc == doctest::Approx(ref_ic_mag).epsilon(1e-4));
}

TEST_CASE("M4b diode Cjo: .ac low-pass cutoff matches RC formula") {
    // A reverse-biased diode (Vd=0) with Cjo acts as a capacitor Cjo.
    // R1=1k, Cjo=10pF → fc = 1/(2π*R*C) ≈ 15.9 MHz. Below fc, |v(out)| ≈ 1;
    // at fc, |v(out)| = 1/√2 ≈ 0.707; above fc, rolls off at -20 dB/dec.
    RunSettings settings;
    settings.reltol = 1e-9;
    const auto run = run_text(
        "diode cjo ac\n"
        "V1 in 0 DC 0 AC 1\n"
        "R1 in out 1k\n"
        "D1 out 0 dx\n"
        ".model dx D(Is=1e-14 N=1 Cjo=10p M=0.5 Vj=0.7)\n"
        ".ac dec 10 1Meg 1G\n",
        nullptr, settings);
    REQUIRE(run);
    REQUIRE(run->results.tables.size() == 1);
    const Table& t = run->results.tables[0];
    const std::size_t col_re = col_of(t, "re(v(out))");
    const std::size_t col_im = col_of(t, "im(v(out))");
    REQUIRE(t.signals[col_re].data.size() > 10);
    // At 1 MHz (well below fc=16 MHz): |v(out)| ≈ 1.
    double f_low = t.signals[0].data[0]; (void)f_low; // frequency column
    double mag_low = std::hypot(t.signals[col_re].data[0], t.signals[col_im].data[0]);
    CHECK(mag_low == doctest::Approx(1.0).epsilon(1e-3));
    // Find the point near fc=15.9 MHz and check |v(out)| ≈ 0.707.
    for (std::size_t k = 0; k < t.signals[0].data.size(); ++k) {
        const double f = t.signals[0].data[k];
        if (f > 15e6 && f < 17e6) {
            const double mag = std::hypot(t.signals[col_re].data[k], t.signals[col_im].data[k]);
            CHECK(mag == doctest::Approx(0.707).epsilon(0.05));  // 5% (frequency grid may not hit fc exactly)
            break;
        }
    }
    // At 1 GHz (well above fc): |v(out)| << 0.1.
    double mag_hi = std::hypot(t.signals[col_re].data.back(), t.signals[col_im].data.back());
    CHECK(mag_hi < 0.05);
}

TEST_CASE("M4b BJT Cje: .ac cutoff matches RC formula") {
    // A BJT with Cje=10pF: the BE junction cap forms a low-pass with the
    // source resistance. V1 drives through R1=1k into the base; Cje(B-E)
    // shunts the base to ground. fc = 1/(2π*R*C) ≈ 15.9 MHz.
    // Below fc: |v(b)| ≈ 1. Above fc: rolls off at -20 dB/dec.
    RunSettings settings;
    settings.reltol = 1e-9;
    const auto run = run_text(
        "bjt cje ac\n"
        "V1 src 0 DC 0 AC 1\n"
        "R1 src b 1k\n"
        "Vc c 0 DC 5\n"
        "Q1 c b 0 qn\n"
        ".model qn NPN(Is=1e-16 Bf=100 Cje=10p Mje=0.5 Vje=0.75)\n"
        ".ac dec 10 1Meg 1G\n",
        nullptr, settings);
    REQUIRE(run);
    REQUIRE(run->results.tables.size() == 1);
    const Table& t = run->results.tables[0];
    const std::size_t col_re = col_of(t, "re(v(b))");
    const std::size_t col_im = col_of(t, "im(v(b))");
    REQUIRE(t.signals[col_re].data.size() > 10);
    // At 1 MHz (well below fc=16 MHz): |v(b)| ≈ 1.
    const double mag_low = std::hypot(t.signals[col_re].data[0], t.signals[col_im].data[0]);
    CHECK(mag_low == doctest::Approx(1.0).epsilon(1e-3));
    // At 1 GHz (well above fc): |v(b)| << 0.1.
    const double mag_hi = std::hypot(t.signals[col_re].data.back(), t.signals[col_im].data.back());
    CHECK(mag_hi < 0.05);
}

TEST_CASE("M4b BJT Cje/Cjc: ngspice cross-validation of .op") {
    // The junction caps don't affect the DC operating point (caps are
    // open at DC). Verify the .op matches ngspice when Cje/Cjc are set.
    // This confirms the model parses correctly and the caps don't break
    // the DC solve. ngspice 39 gives (with Is=1e-16, Bf=100, Vbe=0.7,
    // Vc=5): i(Vbe)=-5.67e-7, i(Vc)=-5.67e-5.
    RunSettings settings;
    settings.reltol = 1e-9;
    const auto run = run_text(
        "bjt cje ngspice xval\n"
        "Vbe b 0 DC 0.7\n"
        "Vc c 0 DC 5\n"
        "Q1 c b 0 qn\n"
        ".model qn NPN(Is=1e-16 Bf=100 Cje=10p Cjc=5p)\n"
        ".op\n",
        nullptr, settings);
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    const double i_vc = t.signals[col_of(t, "i(Vc)")].data[0];
    const double i_vbe = t.signals[col_of(t, "i(Vbe)")].data[0];
    // ngspice values (from the M4a comparison): Ic ≈ 56.7 µA, Ib ≈ 0.567 µA.
    // The caps don't change the DC op.
    CHECK(-i_vc == doctest::Approx(5.67e-5).epsilon(1e-3));
    CHECK(-i_vbe == doctest::Approx(5.67e-7).epsilon(1e-3));
}

// --- §10.1 M4c: the MOSFET (level 1 Shichman-Hodges) ------------------------------

namespace {

// The level-1 model equation (the §10.1 reference, written out exactly as
// DESIGN.md pins it): cutoff below threshold, triode below Vdsat,
// saturation above, with channel-length modulation and the body effect.
// gmin is excluded here (the tests run at gmin=0 so the closed form is
// exact).
double mos_level1_id(double vgs, double vds, double vbs, double vto, double kp,
                     double lambda, double gamma, double phi, double l, double w) {
    double vth = vto;
    if (gamma != 0.0) {
        vth += gamma * (std::sqrt(2.0 * phi - vbs) - std::sqrt(2.0 * phi));
    }
    const double vov = vgs - vth;
    if (vov <= 0.0) {
        return 0.0;
    }
    const double beta = kp * w / l;
    if (vds < vov) {
        return beta * (vov - vds / 2.0) * vds * (1.0 + lambda * vds);
    }
    return 0.5 * beta * vov * vov * (1.0 + lambda * vds);
}

}  // namespace

TEST_CASE("M4c MOSFET: .op Id in cutoff/triode/saturation vs the model equations") {
    // §10.1 row: MOS level 1, Id in triode/saturation vs model equations at
    // 1e-9 rel. Four-terminal bias through V sources (the terminal voltages
    // are exact, so Id is the model equation verbatim). Vto=1, Kp=20u,
    // Lambda=0.01, L=W=1u unless stated; gmin=0 kills the channel floor so
    // the closed form is exact to the solver's 1e-9.
    RunSettings settings;
    settings.reltol = 1e-9;
    settings.gmin = 0.0;
    const auto run = run_text(
        "mos level1 op\n"
        "Vd d 0 DC 2\n"
        "Vg g 0 DC 2\n"
        "Vs s 0 DC 0\n"
        "Vb b 0 DC 0\n"
        "M1 d g s b mn L=1u W=1u\n"
        ".model mn NMOS(Vto=1 Kp=20u Lambda=0.01)\n"
        ".op\n",
        nullptr, settings);
    REQUIRE(run);
    REQUIRE(run->results.tables.size() == 1);
    const Table& t = run->results.tables[0];
    const double i_vd = t.signals[col_of(t, "i(Vd)")].data[0];
    const double i_vg = t.signals[col_of(t, "i(Vg)")].data[0];
    // Vgs=2, Vds=2: overdrive 1 V, Vds >= Vdsat -> saturation.
    // Id = 0.5*Kp*W/L*(Vgs-Vth)^2*(1+lambda*Vds) = 0.5*20u*1*(1.02).
    const double ref_sat =
        mos_level1_id(2.0, 2.0, 0.0, 1.0, 20e-6, 0.01, 0.0, 0.6, 1e-6, 1e-6);
    CHECK(ref_sat == doctest::Approx(10.2e-6).epsilon(1e-12));  // the reference itself
    CHECK(-i_vd == doctest::Approx(ref_sat).epsilon(1e-9));
    // The gate is capacitive: no DC gate current.
    CHECK(std::abs(i_vg) < 1e-20);
}

TEST_CASE("M4c MOSFET: triode region and the L/W geometry factor") {
    // Vds=0.5 < Vdsat=1 -> triode, with L=2u W=1u (beta halves). The M4c
    // ngspice cross-check used W=L; this row pins the geometry factor the
    // analytic gate never covered.
    RunSettings settings;
    settings.reltol = 1e-9;
    settings.gmin = 0.0;
    const auto run = run_text(
        "mos triode\n"
        "Vd d 0 DC 0.5\n"
        "Vg g 0 DC 2\n"
        "Vs s 0 DC 0\n"
        "Vb b 0 DC 0\n"
        "M1 d g s b mn L=2u W=1u\n"
        ".model mn NMOS(Vto=1 Kp=20u Lambda=0.01)\n"
        ".op\n",
        nullptr, settings);
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    const double i_vd = t.signals[col_of(t, "i(Vd)")].data[0];
    const double ref_triode =
        mos_level1_id(2.0, 0.5, 0.0, 1.0, 20e-6, 0.01, 0.0, 0.6, 2e-6, 1e-6);
    // Id = 10u*(1-0.25)*0.5*(1.005) = 3.76875 uA.
    CHECK(ref_triode == doctest::Approx(3.76875e-6).epsilon(1e-12));
    CHECK(-i_vd == doctest::Approx(ref_triode).epsilon(1e-9));
}

TEST_CASE("M4c MOSFET: cutoff below threshold is exactly off") {
    RunSettings settings;
    settings.reltol = 1e-9;
    settings.gmin = 0.0;
    const auto run = run_text(
        "mos cutoff\n"
        "Vd d 0 DC 2\n"
        "Vg g 0 DC 0.5\n"
        "Vs s 0 DC 0\n"
        "Vb b 0 DC 0\n"
        "M1 d g s b mn\n"
        ".model mn NMOS(Vto=1 Kp=20u)\n"
        ".op\n",
        nullptr, settings);
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    const double i_vd = t.signals[col_of(t, "i(Vd)")].data[0];
    CHECK(std::abs(i_vd) < 1e-20);  // no gmin floor: the channel is exactly off
}

TEST_CASE("M4c MOSFET: body effect shifts Vth per the model") {
    // Vbs=-1 with Gamma=0.5, Phi=0.6: Vth = 1 + 0.5*(sqrt(2.2)-sqrt(1.2)).
    RunSettings settings;
    settings.reltol = 1e-9;
    settings.gmin = 0.0;
    const auto run = run_text(
        "mos body effect\n"
        "Vd d 0 DC 3\n"
        "Vg g 0 DC 3\n"
        "Vs s 0 DC 0\n"
        "Vb b 0 DC -1\n"
        "M1 d g s b mn L=1u W=1u\n"
        ".model mn NMOS(Vto=1 Kp=20u Lambda=0.01 Gamma=0.5 Phi=0.6)\n"
        ".op\n",
        nullptr, settings);
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    const double i_vd = t.signals[col_of(t, "i(Vd)")].data[0];
    const double ref_body =
        mos_level1_id(3.0, 3.0, -1.0, 1.0, 20e-6, 0.01, 0.5, 0.6, 1e-6, 1e-6);
    CHECK(ref_body == doctest::Approx(3.3598672045603675e-5).epsilon(1e-9));
    CHECK(-i_vd == doctest::Approx(ref_body).epsilon(1e-9));
}

TEST_CASE("M4c MOSFET: .dc Vgs sweep crosses cutoff into saturation") {
    // A .dc sweep over Vgs at fixed Vds=2 (>= Vdsat everywhere above
    // threshold): every point is either cutoff (Vgs <= Vth) or saturation.
    // This is the sweep form of the analytic row - the region boundary is
    // crossed mid-sweep and each point still matches the equation.
    RunSettings settings;
    settings.reltol = 1e-9;
    settings.gmin = 0.0;
    const auto run = run_text(
        "mos dc sweep\n"
        "Vd d 0 DC 2\n"
        "Vg g 0 DC 0\n"
        "Vs s 0 DC 0\n"
        "Vb b 0 DC 0\n"
        "M1 d g s b mn L=1u W=1u\n"
        ".model mn NMOS(Vto=1 Kp=20u Lambda=0.01)\n"
        ".dc Vg 0 3 0.25\n",
        nullptr, settings);
    REQUIRE(run);
    REQUIRE(run->results.tables.size() == 1);
    const Table& t = run->results.tables[0];
    const std::size_t col_i = col_of(t, "i(Vd)");
    const std::size_t col_vg = col_of(t, "v(g)");
    REQUIRE(t.signals[col_i].data.size() == 13);
    for (std::size_t k = 0; k < 13; ++k) {
        const double vgs = t.signals[col_vg].data[k];
        const double ref = mos_level1_id(vgs, 2.0, 0.0, 1.0, 20e-6, 0.01, 0.0, 0.6,
                                         1e-6, 1e-6);
        const double i_mag = -t.signals[col_i].data[k];
        if (ref > 0.0) {
            CHECK(i_mag == doctest::Approx(ref).epsilon(1e-9));
        } else {
            CHECK(std::abs(i_mag) < 1e-20);
        }
    }
}

TEST_CASE("M4c MOSFET: PMOS - polarity flips, same magnitudes") {
    // Same biases negated on a PMOS: |Id| matches the NMOS saturation
    // equation. The drain sits at -2 V, so the drain source SINKS current
    // and i(Vd) reads POSITIVE (the ngspice sign convention: a delivering
    // source reads negative, an absorbing one positive).
    RunSettings settings;
    settings.reltol = 1e-9;
    settings.gmin = 0.0;
    const auto run = run_text(
        "mos pmos\n"
        "Vd d 0 DC -2\n"
        "Vg g 0 DC -2\n"
        "Vs s 0 DC 0\n"
        "Vb b 0 DC 0\n"
        "M1 d g s b mp L=1u W=1u\n"
        ".model mp PMOS(Vto=1 Kp=20u Lambda=0.01)\n"
        ".op\n",
        nullptr, settings);
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    const double i_vd = t.signals[col_of(t, "i(Vd)")].data[0];
    const double ref_sat =
        mos_level1_id(2.0, 2.0, 0.0, 1.0, 20e-6, 0.01, 0.0, 0.6, 1e-6, 1e-6);
    CHECK(i_vd == doctest::Approx(ref_sat).epsilon(1e-9));
}

// --- §10.1/§10.4 M4d: the behavioral B source (dual-number AD) --------------------

namespace {

// Runs one .ac point and returns re(v(out)) for `expr` biased at `dc`
// through R=1k: the small-signal transfer IS the AD gradient at the OP
// (§8.5: "the small-signal element is the gradient at the OP"), so this
// measures the dual-number derivative exactly - no finite-difference
// truncation anywhere.
double b_ac_gradient(const std::string& expr, double dc) {
    RunSettings settings;
    settings.reltol = 1e-9;
    const auto run = run_text(
        "b ac grad\n"
        "V1 in 0 DC " + std::to_string(dc) + " AC 1\n"
        "B1 out 0 I=1m*(" + expr + ")\n"
        "R1 out 0 1k\n"
        ".ac lin 1 1k 1k\n",
        nullptr, settings);
    REQUIRE(run);
    REQUIRE(run->results.tables.size() == 1);
    const Table& t = run->results.tables[0];
    // re(v(out)) = -R * d/dv(in) [1m*(expr)] at v(in)=dc.
    return t.signals[col_of(t, "re(v(out))")].data[0];
}

}  // namespace

TEST_CASE("M4d B: §10.1 row - .op and .dc against the closed form") {
    // A behavioral current source I=1m*(2*v(in)+0.5*v(in)^2) over R=1k:
    // v(out) = -(2*v + 0.5*v^2), a quadratic - closed-form at 1e-9 and
    // exactly finite-difference-differentiable (a quadratic's central
    // difference has zero truncation error, which the derivative row uses).
    RunSettings settings;
    settings.reltol = 1e-9;
    settings.gmin = 0.0;
    const auto run = run_text(
        "b op dc\n"
        "V1 in 0 DC 1\n"
        "B1 out 0 I=1m*(2*v(in)+0.5*v(in)**2)\n"
        "R1 out 0 1k\n"
        ".op\n"
        ".dc V1 0 2 0.25\n",
        nullptr, settings);
    REQUIRE(run);
    REQUIRE(run->results.tables.size() == 2);
    const Table& op = run->results.tables[0];
    CHECK(op.signals[col_of(op, "v(out)")].data[0] == approx(-2.5));  // v(in)=1
    const Table& dc = run->results.tables[1];
    const std::size_t col_vo = col_of(dc, "v(out)");
    REQUIRE(dc.signals[col_vo].data.size() == 9);
    for (std::size_t k = 0; k < 9; ++k) {
        const double v = 0.25 * static_cast<double>(k);
        CHECK(dc.signals[col_vo].data[k] == approx(-(2.0 * v + 0.5 * v * v)));
    }
}

TEST_CASE("M4d B: the derivative row - central finite difference at 1e-6") {
    // §10.1: "behavioral B | .op, and derivative | closed form; central
    // finite difference | 1e-9; 1e-6". The quadratic's central difference
    // is exact, so the FD-vs-AD comparison isolates the AD from FD noise.
    RunSettings settings;
    settings.reltol = 1e-9;
    settings.gmin = 0.0;
    const auto run = run_text(
        "b deriv fd\n"
        "V1 in 0 DC 0\n"
        "B1 out 0 I=1m*(2*v(in)+0.5*v(in)**2)\n"
        "R1 out 0 1k\n"
        ".dc V1 0.25 1.75 0.25\n",
        nullptr, settings);
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    const std::size_t col_vo = col_of(t, "v(out)");
    const std::size_t col_vi = col_of(t, "v(in)");
    for (std::size_t k = 1; k + 1 < t.signals[col_vo].data.size(); ++k) {
        const double dv = t.signals[col_vi].data[k + 1] - t.signals[col_vi].data[k - 1];
        const double fd = (t.signals[col_vo].data[k + 1] - t.signals[col_vo].data[k - 1]) / dv;
        // d(v_out)/d(v_in) = -(2 + v).
        CHECK(fd == doctest::Approx(-(2.0 + t.signals[col_vi].data[k])).epsilon(1e-6));
    }
}

TEST_CASE("M4d B: the AD gradient at the OP via .ac (§10.4 battery)") {
    // One row per §8.5 function family: the .ac transfer is the frozen
    // gradient, compared against the hand derivative at 1e-9. This is the
    // "every AD derivative is checked against finite differences" gate in
    // its exact form - stronger than FD (no truncation), and the FD row
    // above cross-checks the same machinery end to end.
    const double r = 1000.0;
    struct Row {
        const char* expr;
        double dc;
        double derv;  // d/dv of the expr at dc
    };
    const Row rows[] = {
        {"sqrt(v(in))", 1.0, 0.5},
        {"sqrt(v(in))", 4.0, 0.25},
        {"exp(v(in)/0.5)", 0.3, 2.0 * std::exp(0.6)},
        {"ln(1+v(in))", 1.0, 0.5},
        {"log10(1+v(in))", 1.0, 1.0 / (2.0 * std::log(10.0))},
        {"sin(v(in))", 0.7, std::cos(0.7)},
        {"cos(v(in))", 0.7, -std::sin(0.7)},
        {"tan(v(in))", 0.4, 1.0 / (std::cos(0.4) * std::cos(0.4))},
        {"atan(v(in))", 1.5, 1.0 / (1.0 + 1.5 * 1.5)},
        {"sinh(v(in))", 0.3, std::cosh(0.3)},
        {"tanh(v(in))", 0.5, 1.0 - std::tanh(0.5) * std::tanh(0.5)},
        {"abs(v(in))", 1.5, 1.0},
        {"abs(v(in))", -1.5, -1.0},
        {"uramp(v(in)-1)", 1.5, 1.0},
        {"uramp(v(in)-1)", 0.5, 0.0},
        {"pow(v(in),3)", 1.2, 3.0 * 1.2 * 1.2},
        {"v(in)**2.5", 1.5, 2.5 * std::pow(1.5, 1.5)},
        {"min(v(in),1)", 1.5, 0.0},
        {"max(v(in),1)", 1.5, 1.0},
        {"if(v(in)>1, v(in)**2)", 1.5, 3.0},
        {"if(v(in)>1, v(in)**2)", 0.5, 0.0},
        {"(v(in)>1 ? v(in) : 0.5*v(in))", 1.5, 1.0},
        {"(v(in)>1 ? v(in) : 0.5*v(in))", 0.5, 0.5},
    };
    for (const Row& row : rows) {
        const double got = b_ac_gradient(row.expr, row.dc);
        // transfer = -R * 1m * d(expr)/dv.
        CHECK_MESSAGE(got == doctest::Approx(-r * 1e-3 * row.derv).epsilon(1e-9),
                      ("expr: " + std::string(row.expr) + " at v=" +
                       std::to_string(row.dc))
                          .c_str());
    }
}

TEST_CASE("M4d B: exponential source converges to the Lambert-W closed form") {
    // The B source as a diode: I = Is*(exp(v(a)/Vt)-1) with the series R.
    // No limiter exists for behavioral sources (§8.5 defines none), so
    // this row pins the §5.1 residual-gate convergence: the two-gate
    // check walks the exponential to the fixed point from a cold start.
    RunSettings settings;
    settings.reltol = 1e-9;
    settings.gmin = 0.0;
    const auto run = run_text(
        "b lambert\n"
        "V1 src 0 DC 2\n"
        "R1 src a 1k\n"
        "B1 a 0 I=10f*(exp(v(a)/25.85m)-1)\n"
        ".op\n",
        nullptr, settings);
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    const double v_a = t.signals[col_of(t, "v(a)")].data[0];
    const double i_mag = -t.signals[col_of(t, "i(V1)")].data[0];
    // The same closed form as the diode row, with the deck's exact Vt.
    const double vt = 25.85e-3;
    const double is_val = 10e-15;
    const double r = 1000.0;
    const double z = lambert_w0((is_val * r / vt) * std::exp((2.0 + is_val * r) / vt));
    const double ref_i = (vt / r) * z - is_val;
    const double ref_vd = 2.0 - ref_i * r;
    CHECK(v_a == doctest::Approx(ref_vd).epsilon(1e-9));
    CHECK(i_mag == doctest::Approx(ref_i).epsilon(1e-9));
}

TEST_CASE("M4d B: V-type source - .op, .dc and the .ac gradient") {
    RunSettings settings;
    settings.reltol = 1e-9;
    settings.gmin = 0.0;
    const auto run = run_text(
        "b vtype\n"
        "V1 in 0 DC 1\n"
        "B1 out 0 V=2*v(in)\n"
        "R1 out 0 1k\n"
        ".op\n"
        ".dc V1 0 2 0.5\n",
        nullptr, settings);
    REQUIRE(run);
    REQUIRE(run->results.tables.size() == 2);
    CHECK(run->results.tables[0].signals[col_of(run->results.tables[0], "v(out)")].data[0]
          == approx(2.0));
    const Table& dc = run->results.tables[1];
    const std::size_t col_vo = col_of(dc, "v(out)");
    for (std::size_t k = 0; k < dc.signals[col_vo].data.size(); ++k) {
        CHECK(dc.signals[col_vo].data[k] == approx(2.0 * 0.5 * static_cast<double>(k)));
    }
    // The branch current is an unknown with the ngspice sign convention.
    CHECK(run->results.tables[0].signals[col_of(run->results.tables[0], "i(B1)")].data[0]
          == approx(-2.0 / 1000.0));

    // .ac: the small-signal of V=2*v(in) is a gain of 2 - exactly the E
    // source with gain 2, pinned against the same closed form.
    const auto ac = run_text(
        "b vtype ac\n"
        "V1 in 0 DC 0 AC 1\n"
        "B1 out 0 V=2*v(in)\n"
        "R1 out 0 1k\n"
        ".ac lin 1 1k 1k\n",
        nullptr, settings);
    REQUIRE(ac);
    CHECK(ac->results.tables[0].signals[col_of(ac->results.tables[0], "re(v(out))")]
              .data[0] == approx(2.0));
    CHECK(ac->results.tables[0].signals[col_of(ac->results.tables[0], "im(v(out))")]
              .data[0] == approx(0.0));
}

TEST_CASE("M4d B: domain errors name the source and the iterate (§8.5/§9.1)") {
    // ln(1+v(in)) with the source at -2 V: the cold iterate (v=0) is in
    // domain, the first solve leaves it - the error fires on the second
    // load with the offending operand, never as a NaN, and never as the
    // ladder's generic "no convergence after ...".
    const auto run = run_text(
        "b domain\n"
        "V1 in 0 DC -2\n"
        "B1 out 0 I=ln(1+v(in))\n"
        "R1 out 0 1k\n"
        ".op\n");
    const std::string msg = error_of(run);
    CHECK(msg.find("behavioral source B1") != std::string::npos);
    CHECK(msg.find("'ln'") != std::string::npos);
    CHECK(msg.find("operand -1") != std::string::npos);
    CHECK(msg.find("no convergence after") == std::string::npos);

    // The .tran twin names the step time (in-domain at the t=0 OP,
    // out of it once t passes 2u).
    const auto tran = run_text(
        "b domain tran\n"
        "B1 out 0 I=ln(2u-time)\n"
        "R1 out 0 1k\n"
        ".tran 0.5u 3u\n");
    const std::string tmsg = error_of(tran);
    CHECK(tmsg.find("behavioral source B1") != std::string::npos);
    CHECK(tmsg.find("at t = ") != std::string::npos);
}

TEST_CASE("M4d B: i(Vname) and v(a,b) references against closed forms") {
    // i(V1) = -1mA (V1 delivers into R1); the B multiplies by 2 and the
    // current then charges out through R2.
    const auto run = run_text(
        "b iref\n"
        "V1 a 0 DC 1\n"
        "R1 a 0 1k\n"
        "B1 out 0 I=2*i(V1)\n"
        "R2 out 0 500\n"
        ".op\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    CHECK(t.signals[col_of(t, "v(out)")].data[0] == approx(1.0));

    // v(p,n) is the differential: 1m*(3-1) drains out through 1k.
    const auto run2 = run_text(
        "b vdiff\n"
        "V1 p 0 DC 3\n"
        "V2 n 0 DC 1\n"
        "B1 out 0 I=1m*v(p,n)\n"
        "R1 out 0 1k\n"
        ".op\n");
    REQUIRE(run2);
    CHECK(run2->results.tables[0].signals[col_of(run2->results.tables[0], "v(out)")].data[0]
          == approx(-2.0));

    // An unknown i() source is a Semantic error naming it (the message
    // lowercases the name, like every name match in the deck layer).
    const auto bad = run_text(
        "b iref unknown\n"
        "V1 a 0 DC 1\n"
        "R1 a 0 1k\n"
        "B1 out 0 I=2*i(Vnope)\n"
        "R2 out 0 500\n"
        ".op\n");
    const std::string msg = error_of(bad);
    CHECK(msg.find("vnope") != std::string::npos);
    CHECK(msg.find("voltage source") != std::string::npos);
}

TEST_CASE("M4d B: the time variable in .tran (ramp exactness)") {
    // v(out) = -R*1m*t is algebraic in the state at every sample point -
    // exact regardless of the integrator, so the time binding is pinned
    // to 1e-9 at several samples.
    const auto run = run_text(
        "b time ramp\n"
        "B1 out 0 I=1m*time\n"
        "R1 out 0 1k\n"
        ".tran 0.1m 2m\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    const std::size_t col_vo = col_of(t, "v(out)");
    REQUIRE(t.signals[col_vo].data.size() > 10);
    for (std::size_t k = 0; k < t.signals[col_vo].data.size(); ++k) {
        const double time_k = t.rows()[k][0];  // the time axis
        CHECK(t.signals[col_vo].data[k] ==
              doctest::Approx(-1000.0 * 1e-3 * time_k).epsilon(1e-9));
    }
}

TEST_CASE("M4d B: .param binding and .step rebinding reach the expression") {
    // A plain name inside the expression is a parameter slot: .step
    // rebinds it per point like any device value.
    const auto run = run_text(
        "b step gain\n"
        ".param gain=1\n"
        "V1 in 0 DC 1\n"
        "B1 out 0 I=gain*1m*v(in)\n"
        "R1 out 0 1k\n"
        ".dc V1 1 1 1\n"
        ".step param gain list 1 2 4\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    const std::size_t col_vo = col_of(t, "v(out)");
    REQUIRE(t.signals[col_vo].data.size() == 3);
    CHECK(t.signals[col_vo].data[0] == approx(-1.0));
    CHECK(t.signals[col_vo].data[1] == approx(-2.0));
    CHECK(t.signals[col_vo].data[2] == approx(-4.0));
}

TEST_CASE("M4d B: rejections are loud and line-numbered") {
    // No V=/I= spec.
    CHECK(error_of(run_text("b none\nB1 a b 5\nR1 a 0 1k\nR2 b 0 1k\n.op\n"))
              .find("V= or I=") != std::string::npos);
    // A second spec token is a parse error in the expression, not silence.
    CHECK(!run_text("b both\nB1 a b V=1 I=2\nR1 a 0 1k\nR2 b 0 1k\n.op\n"));
    // Monte-Carlo draws are .param-only.
    CHECK(error_of(run_text("b gauss\nB1 a b I=gauss(1m)\nR1 a 0 1k\n.op\n"))
              .find(".param-only") != std::string::npos);
    // @dev[param] is a save-list form.
    CHECK(error_of(run_text("b at\nV1 a 0 1\nB1 b 0 I=@V1[dc]\nR1 b 0 1k\n.op\n"))
              .find(".save/.print") != std::string::npos);
    // v() arity is checked at parse.
    CHECK(error_of(run_text("b varity\nV1 a 0 1\nB1 b 0 I=v(a,b,c)\nR1 b 0 1k\n.op\n"))
              .find("one node or two") != std::string::npos);
}

TEST_CASE("M4d B: the netlist round-trip writes and re-solves equivalently") {
    // A B without .param references round-trips; one WITH a plain name
    // fails the writer naming the device (the flattened deck has no
    // .params - a loud gap, never a silently wrong card).
    const std::string deck =
        "b roundtrip\n"
        "V1 in 0 DC 1\n"
        "B1 out 0 I=3m*v(in)\n"
        "R1 out 0 1k\n"
        ".op\n";
    const ws::core::Result<ws::spice::Deck> parsed =
        read_deck("t.cir", deck, ws::spice::ParseOptions{});
    REQUIRE(parsed);
    std::vector<Warning> w;
    const ws::core::Result<ws::spice::DeckAst> ast = build_deck_ast(*parsed, w);
    REQUIRE(ast);
    const ws::core::Result<ws::spice::Circuit> circ = elaborate_deck(*ast, 27.0, w);
    REQUIRE(circ);
    const ws::core::Result<ws::spice::RunState> rs = ws::spice::RunState::bind(*circ);
    REQUIRE(rs);
    const ws::core::Result<std::string> net = write_netlist(*circ, *rs);
    REQUIRE(net);
    CHECK(net->find("B1 out 0 I=3m*v(in)") != std::string::npos);
    // Re-running the written deck (with an analysis card inserted ahead of
    // its .end - the writer emits devices/models only) gives the same
    // answer.
    const std::size_t end_pos = net->rfind(".end");
    REQUIRE(end_pos != std::string::npos);
    const auto again = run_text(net->substr(0, end_pos) + ".op\n.end\n");
    REQUIRE(again);
    REQUIRE(again->results.tables.size() == 1);
    CHECK(again->results.tables[0].signals[col_of(again->results.tables[0], "v(out)")].data[0]
          == approx(-3.0));

    const std::string param_deck =
        "b paramgap\n"
        ".param gain=3\n"
        "V1 in 0 DC 1\n"
        "B1 out 0 I=gain*v(in)\n"
        "R1 out 0 1k\n"
        ".op\n";
    const ws::core::Result<ws::spice::Deck> p2 =
        read_deck("t.cir", param_deck, ws::spice::ParseOptions{});
    REQUIRE(p2);
    const ws::core::Result<ws::spice::DeckAst> a2 = build_deck_ast(*p2, w);
    REQUIRE(a2);
    const ws::core::Result<ws::spice::Circuit> c2 = elaborate_deck(*a2, 27.0, w);
    REQUIRE(c2);
    const ws::core::Result<ws::spice::RunState> r2 = ws::spice::RunState::bind(*c2);
    REQUIRE(r2);
    const ws::core::Result<std::string> n2 = write_netlist(*c2, *r2);
    REQUIRE(!n2);
    CHECK(n2.error().message.find("B1") != std::string::npos);
}

// --- M4e: the lossless T-line (§6.2 method of characteristics) ------------------
//
// All rows are closed-form: the matched line carries legs intact and
// delayed exactly (linear-interp pickup is exact on piecewise-linear
// legs), the mismatched line's reflection ladder is geometric, and the
// wire-pair DC is the t->infinity limit the delay hides.

namespace {

// The (time, value) of column `name` in the row whose sweep value is
// nearest `at`. The closed form is then evaluated at the ROW's time, so
// the check holds on whatever grid the march chose (the march lands
// exactly on source breakpoints and delayed vertices, so the nearest
// row IS the event of interest).
struct RowPoint {
    double t;
    double v;
};

RowPoint point_at(const Table& t, double at, const std::string& name) {
    const std::size_t col = col_of(t, name);
    std::size_t best = 0;
    for (std::size_t i = 0; i < t.signals[col].data.size(); ++i) {
        if (std::abs(t.sweep_axes[0][i] - at) <
            std::abs(t.sweep_axes[0][best] - at)) {
            best = i;
        }
    }
    return {t.sweep_axes[0][best], t.signals[col].data[best]};
}

}  // namespace

TEST_CASE("M4e T: .op is the wire pair - divider through the line") {
    const auto run = run_text(
        "t op divider\n"
        "V1 in 0 DC 5\n"
        "R1 in a 1k\n"
        "T1 a 0 b 0 Z0=75 Td=5n\n"
        "R2 b 0 2k\n"
        ".op\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    REQUIRE(t.rows().size() == 1);
    CHECK(t.rows()[0][col_of(t, "v(a)")] == approx(5.0 * 2.0 / 3.0));
    CHECK(t.rows()[0][col_of(t, "v(b)")] == approx(5.0 * 2.0 / 3.0));
    // i(T1) is the port-1 branch current: the wire current itself.
    CHECK(t.rows()[0][col_of(t, "i(T1)")] == approx(5.0 / 3000.0));
    CHECK(t.rows()[0][col_of(t, "i(V1)")] == approx(-5.0 / 3000.0));
}

TEST_CASE("M4e T: .dc sweep through the line tracks the wire at every point") {
    const auto run = run_text(
        "t dc through\n"
        "V1 in 0 DC 5\n"
        "R1 in a 1k\n"
        "T1 a 0 b 0 Z0=50 Td=1n\n"
        "R2 b 0 2k\n"
        ".dc V1 0 5 1\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    REQUIRE(t.signals[col_of(t, "v(b)")].data.size() == 6);
    for (std::size_t k = 0; k < 6; ++k) {
        const double v = static_cast<double>(k);
        CHECK(t.signals[col_of(t, "v(a)")].data[k] == approx(v * 2.0 / 3.0));
        CHECK(t.signals[col_of(t, "v(b)")].data[k] == approx(v * 2.0 / 3.0));
    }
}

TEST_CASE("M4e T: matched line carries a ramp delayed and intact (§10.1)") {
    // Rs = RL = Z0: no reflection ever returns, so v(b)(t) = v(a)(t-Td)
    // exactly and v(a) is the Z0 divider of the PWL source. The pickup
    // lands on stored vertices (delay crossings are breakpoints), so the
    // delayed legs reproduce bit-close, not just within LTE tolerance.
    const auto run = run_text(
        "t matched ramp\n"
        "V1 in 0 PWL(0 0 1n 1)\n"
        "Rs in a 50\n"
        "T1 a 0 b 0 Z0=50 Td=2n\n"
        "Rl b 0 50\n"
        ".tran 0.2n 8n\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    // v(a) is the divider all along the ramp; v(b) is 0 before Td.
    for (const double at : {0.4e-9, 0.8e-9, 1.0e-9, 1.4e-9}) {
        const auto [tt, va] = point_at(t, at, "v(a)");
        CHECK(va == approx(0.5 * std::clamp(tt / 1e-9, 0.0, 1.0)));
        CHECK(point_at(t, at, "v(b)").v == approx(0.0));
    }
    // After Td: v(b) = v(a)(t - Td) - the same legs, two nanoseconds
    // later, down to the last double digit.
    for (const double at : {2.4e-9, 2.8e-9, 3.0e-9, 3.4e-9, 5e-9, 7e-9}) {
        const auto [tt, vb] = point_at(t, at, "v(b)");
        CHECK(vb == approx(0.5 * std::clamp((tt - 2e-9) / 1e-9, 0.0, 1.0)));
    }
    // Settled: the line's DC limit is the wire - but matched, so both
    // ends sit at V/2 and the port current is v(a)/Z0 forever.
    CHECK(point_at(t, 7e-9, "v(a)").v == approx(0.5));
    CHECK(point_at(t, 7e-9, "i(T1)").v == approx(0.5 / 50.0));
}

TEST_CASE("M4e T: open-end reflection ladder matches the closed form") {
    // Port 2 left genuinely open (the line's constraint edges are its DC
    // path, §6.6 - the wire at DC). The source side is matched (Rs = Z0),
    // so every reflection the open end returns is absorbed: W1 = V(s)
    // always, and the ladder closes exactly -
    //   v(b) = V(t-Td),   v(a) = (V(t) + V(t-2Td))/2,   final v = V.
    const auto run = run_text(
        "t open reflections\n"
        "V1 in 0 PWL(0 0 1n 1)\n"
        "Rs in a 50\n"
        "T1 a 0 b 0 Z0=50 Td=2n\n"
        ".tran 0.5n 9n\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    // Before the wave arrives: v(b) = 0, v(a) = V/2 (the Z0 divider).
    CHECK(point_at(t, 1.0e-9, "v(a)").v == approx(0.5));
    CHECK(point_at(t, 1.0e-9, "v(b)").v == approx(0.0));
    // The arrival doubles at the open end: v(b) = V(t - Td) exactly.
    for (const double at : {2.4e-9, 2.8e-9, 3.2e-9}) {
        const auto [tt, vb] = point_at(t, at, "v(b)");
        CHECK(vb == approx(std::clamp((tt - 2e-9) / 1e-9, 0.0, 1.0)));
    }
    // The reflection returns: v(a) = (V(t) + V(t - 2Td))/2 exactly.
    for (const double at : {4.4e-9, 4.8e-9, 5.2e-9, 6.0e-9}) {
        const auto [tt, va] = point_at(t, at, "v(a)");
        CHECK(va == approx((1.0 + std::clamp((tt - 4e-9) / 1e-9, 0.0, 1.0)) / 2.0));
    }
    // Steady state: the open line charges to V with zero current - the
    // DC wire answer with no load to leak through.
    CHECK(point_at(t, 8.5e-9, "v(a)").v == approx(1.0));
    CHECK(point_at(t, 8.5e-9, "v(b)").v == approx(1.0));
    CHECK(point_at(t, 8.5e-9, "i(T1)").v == approx(0.0));
}

TEST_CASE("M4e T: .ac - quarter-wave open, half-wave open, matched delay") {
    // Open far end: the input is a short at odd quarter-waves (v(a) = 0,
    // v(b) full) and an open at half-waves (i(T1) = 0, v(b) = -v(a)).
    const auto run = run_text(
        "t quarter wave\n"
        "V1 in 0 AC 1\n"
        "Rs in a 50\n"
        "T1 a 0 b 0 Z0=50 Td=1u\n"
        ".ac lin 3 250k 750k\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    REQUIRE(t.signals[col_of(t, "re(v(a))")].data.size() == 3);
    const auto re_im = [&](const std::string& n, std::size_t k) {
        return std::complex<double>(t.signals[col_of(t, "re(" + n + ")")].data[k],
                                    t.signals[col_of(t, "im(" + n + ")")].data[k]);
    };
    for (std::size_t k = 0; k < 3; ++k) {
        const double f = t.sweep_axes[0][k];
        const double w_td = 2.0 * 3.14159265358979323846 * f * 1e-6;
        // omega*Td in units of pi/2: odd = quarter-wave (input short),
        // even = half-wave (input open).
        const long q = std::lround(w_td / (3.14159265358979323846 / 2.0));
        CHECK((q == 1 || q == 2 || q == 3));
        if (q % 2 == 1) {
            // Odd quarter-wave: input short, full magnitude at the open.
            CHECK(std::abs(re_im("v(a)", k)) < 1e-3);
            CHECK(std::abs(re_im("v(b)", k)) == approx(1.0).epsilon(1e-6));
        } else {
            // Half-wave: input open (no current drawn from the source
            // chain beyond the RL leakage), v(b) = -v(a).
            CHECK(std::abs(re_im("i(T1)", k)) < 1e-6);
            CHECK(std::abs(re_im("v(b)", k) + re_im("v(a)", k)) < 1e-6);
        }
    }

    // Matched line: v(b)/v(a) = e^{-j*omega*Td}. At omega*Td = pi/4 the
    // ratio is (1 - j)/sqrt(2) to integration precision.
    const auto run2 = run_text(
        "t matched phase\n"
        "V1 in 0 AC 1\n"
        "Rs in a 50\n"
        "T1 a 0 b 0 Z0=50 Td=1u\n"
        "Rl b 0 50\n"
        ".ac lin 1 125k 125k\n");
    REQUIRE(run2);
    const Table& t2 = run2->results.tables[0];
    const std::complex<double> va(t2.signals[col_of(t2, "re(v(a))")].data[0],
                                  t2.signals[col_of(t2, "im(v(a))")].data[0]);
    const std::complex<double> vb(t2.signals[col_of(t2, "re(v(b))")].data[0],
                                  t2.signals[col_of(t2, "im(v(b))")].data[0]);
    const std::complex<double> ratio = vb / va;
    CHECK(std::abs(ratio) == approx(1.0).epsilon(1e-6));
    CHECK(ratio.real() == approx(std::sqrt(0.5)).epsilon(1e-6));
    CHECK(ratio.imag() == approx(-std::sqrt(0.5)).epsilon(1e-6));
}

TEST_CASE("M4e T: rejections are loud and line-numbered") {
    // No Z0 (ngspice requires it).
    CHECK(error_of(run_text("t none\nV1 a 0 1\nT1 a 0 b 0 Td=1n\nR1 b 0 1k\n.op\n"))
              .find("Z0") != std::string::npos);
    // Z0 must be positive.
    CHECK(error_of(run_text("t badz0\nV1 a 0 1\nT1 a 0 b 0 Z0=0 Td=1n\nR1 b 0 1k\n.op\n"))
              .find("Z0 > 0") != std::string::npos);
    // Zero delay is a wire pair, not a line.
    CHECK(error_of(run_text("t zerotd\nV1 a 0 1\nT1 a 0 b 0 Z0=50 Td=0\nR1 b 0 1k\n.op\n"))
              .find("TD > 0") != std::string::npos);
    // No delay given at all.
    CHECK(error_of(run_text("t notd\nV1 a 0 1\nT1 a 0 b 0 Z0=50\nR1 b 0 1k\n.op\n"))
              .find("TD=") != std::string::npos);
    // TD= and F= are mutually exclusive.
    CHECK(error_of(run_text("t both\nV1 a 0 1\nT1 a 0 b 0 Z0=50 Td=1n F=1meg\nR1 b 0 1k\n.op\n"))
              .find("not both") != std::string::npos);
    // F must be positive.
    CHECK(error_of(run_text("t badf\nV1 a 0 1\nT1 a 0 b 0 Z0=50 F=0 NL=1\nR1 b 0 1k\n.op\n"))
              .find("F > 0") != std::string::npos);
    // Arity: four nodes.
    CHECK(error_of(run_text("t arity\nV1 a 0 1\nT1 a b Z0=50\nR1 b 0 1k\n.op\n"))
              .find("4 nodes") != std::string::npos);
    // Unknown key.
    CHECK(error_of(run_text("t badkey\nV1 a 0 1\nT1 a 0 b 0 Z0=50 Td=1n foo=1\nR1 b 0 1k\n.op\n"))
              .find("foo=") != std::string::npos);
    // IC takes at most four values.
    CHECK(error_of(run_text("t badic\nV1 a 0 1\nT1 a 0 b 0 Z0=50 Td=1n IC=1 2 3 4 5\nR1 b 0 1k\n.op\n"))
              .find("one to four") != std::string::npos);
    // Bridging the same node pair at both ports is degenerate at DC.
    CHECK(error_of(run_text("t bridge\nV1 a 0 1\nT1 a 0 a 0 Z0=50 Td=1n\nR1 a 0 1k\n.op\n"))
              .find("same node pair") != std::string::npos);
    // Two lines in parallel over one node pair: a constraint loop.
    CHECK(error_of(run_text("t two\nV1 a 0 1\nT1 a 0 b 0 Z0=50 Td=1n\nT2 a 0 b 0 Z0=75 Td=2n\nR1 b 0 1k\n.op\n"))
              .find("voltage-source loop") != std::string::npos);
    // A V source bridging a line pair: same finding, named.
    CHECK(error_of(run_text("t vloop\nV1 a 0 1\nT1 a 0 b 0 Z0=50 Td=1n\nV2 a b 0\nR1 b 0 1k\n.op\n"))
              .find("voltage-source loop") != std::string::npos);
}

TEST_CASE("M4e T: F=/NL= is the TD= spelling (TD = NL/F, NL defaults 0.25)") {
    const auto run = run_text(
        "t fnl\n"
        "V1 in 0 DC 5\n"
        "R1 in a 1k\n"
        "T1 a 0 b 0 Z0=75 F=1meg NL=0.25\n"
        "R2 b 0 2k\n"
        ".op\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    CHECK(t.rows()[0][col_of(t, "v(b)")] == approx(5.0 * 2.0 / 3.0));
    // And the delay really is NL/F = 1/5meg = 200n: the ramp comes out
    // 200n late (the run must outlast the delay).
    const auto run2 = run_text(
        "t fnl delay\n"
        "V1 in 0 PWL(0 0 1n 1)\n"
        "Rs in a 50\n"
        "T1 a 0 b 0 Z0=50 F=5meg NL=1\n"
        "Rl b 0 50\n"
        ".tran 50n 250n\n");
    REQUIRE(run2);
    const Table& t2 = run2->results.tables[0];
    CHECK(point_at(t2, 190e-9, "v(b)").v == approx(0.0));
    CHECK(point_at(t2, 210e-9, "v(b)").v == approx(0.5));
}

TEST_CASE("M4e T: UIC + IC= seeds the waves, delay still exact") {
    // IC=0 0 0 0 matches the OP here, so the UIC run must reproduce the
    // plain run's delayed ramp - the seed path is exercised without any
    // order-accuracy claim (a cold UIC start is deliberately
    // inconsistent, §6.5).
    const std::string body =
        "V1 in 0 PWL(0 0 1n 1)\n"
        "Rs in a 50\n"
        "T1 a 0 b 0 Z0=50 Td=2n IC=0 0 0 0\n"
        "Rl b 0 50\n"
        ".tran 0.5n 5n";
    const auto plain = run_text("t uic plain\n" + body);
    const auto uic = run_text("t uic\n" + body + " UIC\n");
    REQUIRE(plain);
    REQUIRE(uic);
    const Table& tp = plain->results.tables[0];
    const Table& tu = uic->results.tables[0];
    CHECK(point_at(tu, 1.5e-9, "v(b)").v == approx(0.0));
    CHECK(point_at(tu, 3.0e-9, "v(b)").v == approx(0.5));
    // Same grid, same values: IC=0 on an all-zero OP is a no-op seed.
    REQUIRE(tu.signals[col_of(tu, "v(b)")].data.size() ==
            tp.signals[col_of(tp, "v(b)")].data.size());
    for (std::size_t k = 0; k < tu.signals[col_of(tu, "v(b)")].data.size(); ++k) {
        CHECK(tu.signals[col_of(tu, "v(b)")].data[k] ==
              doctest::Approx(tp.signals[col_of(tp, "v(b)")].data[k]).epsilon(1e-12));
    }
}

TEST_CASE("M4e T: the history ring overflows loudly (§8.5 channel)") {
    // 1 ps resolved steps inside a 2.05 ns delay window: more than the
    // 2048-sample ring can hold. The Timestep error names the device.
    RunSettings settings;
    settings.fixed_dt = 1e-12;
    const auto run = run_text(
        "t overflow\n"
        "V1 in 0 1\n"
        "Rs in a 50\n"
        "T1 a 0 b 0 Z0=50 Td=2.05n\n"
        "Rl b 0 50\n"
        ".tran 1p 4n\n", nullptr, settings);
    REQUIRE(!run);
    CHECK(run.error().message.find("T1") != std::string::npos);
    CHECK(run.error().message.find("delay window") != std::string::npos);
}

TEST_CASE("M4e T: the netlist round-trip writes and re-solves equivalently") {
    const std::string deck =
        "t roundtrip\n"
        "V1 in 0 DC 5\n"
        "R1 in a 1k\n"
        "T1 a 0 b 0 Z0=75 Td=5n\n"
        "R2 b 0 2k\n"
        ".op\n";
    const ws::core::Result<ws::spice::Deck> parsed =
        read_deck("t.cir", deck, ws::spice::ParseOptions{});
    REQUIRE(parsed);
    std::vector<Warning> w;
    const ws::core::Result<ws::spice::DeckAst> ast = build_deck_ast(*parsed, w);
    REQUIRE(ast);
    const ws::core::Result<ws::spice::Circuit> circ = elaborate_deck(*ast, 27.0, w);
    REQUIRE(circ);
    const ws::core::Result<ws::spice::RunState> rs = ws::spice::RunState::bind(*circ);
    REQUIRE(rs);
    const ws::core::Result<std::string> net = write_netlist(*circ, *rs);
    REQUIRE(net);
    // The writer spells numbers through to_chars (shortest round-trip).
    CHECK(net->find("T1 a 0 b 0 Z0=75 Td=5e-09") != std::string::npos);
    const std::size_t end_pos = net->rfind(".end");
    REQUIRE(end_pos != std::string::npos);
    const auto again = run_text(net->substr(0, end_pos) + ".op\n.end\n");
    REQUIRE(again);
    CHECK(again->results.tables[0]
              .signals[col_of(again->results.tables[0], "v(b)")]
              .data[0] == approx(5.0 * 2.0 / 3.0));

    // The IC= spelling round-trips too (comma form).
    const std::string ic_deck =
        "t ic roundtrip\n"
        "V1 in 0 PWL(0 0 1n 1)\n"
        "Rs in a 50\n"
        "T1 a 0 b 0 Z0=50 Td=2n IC=0,0,0,0\n"
        "Rl b 0 50\n"
        ".tran 0.5n 4n UIC\n";
    const ws::core::Result<ws::spice::Deck> p2 =
        read_deck("t.cir", ic_deck, ws::spice::ParseOptions{});
    REQUIRE(p2);
    const ws::core::Result<ws::spice::DeckAst> a2 = build_deck_ast(*p2, w);
    REQUIRE(a2);
    const ws::core::Result<ws::spice::Circuit> c2 = elaborate_deck(*a2, 27.0, w);
    REQUIRE(c2);
    const ws::core::Result<ws::spice::RunState> r2 = ws::spice::RunState::bind(*c2);
    REQUIRE(r2);
    const ws::core::Result<std::string> n2 = write_netlist(*c2, *r2);
    REQUIRE(n2);
    CHECK(n2->find("T1 a 0 b 0 Z0=50 Td=2e-09 IC=0,0,0,0") != std::string::npos);
    // The written deck re-runs to the same delayed answer.
    const std::size_t end2 = n2->rfind(".end");
    const auto again2 =
        run_text(n2->substr(0, end2) + ".tran 0.5n 4n UIC\n.end\n");
    REQUIRE(again2);
    const Table& t2 = again2->results.tables[0];
    CHECK(point_at(t2, 1.5e-9, "v(b)").v == approx(0.0));
    CHECK(point_at(t2, 3.0e-9, "v(b)").v == approx(0.5));
}

TEST_CASE("M4e T: .step rebinding reaches Z0 and TD (the §3.5a promise)") {
    // Z0 as a bare .param name: the .step point rebinds it without any
    // re-elaboration - the value change reaches the stamping coefficients
    // through the parameter arena alone. Family table: axes are
    // (step, z, time), one row per accepted time point per z point.
    const auto run = run_text(
        "t step z0\n"
        ".param z=50\n"
        "V1 in 0 PWL(0 0 1n 1)\n"
        "Rs in a 50\n"
        "T1 a 0 b 0 Z0=z Td=1n\n"
        "Rl b 0 50\n"
        ".tran 0.5n 3n\n"
        ".step param z list 50 100\n");
    REQUIRE(run);
    REQUIRE(run->results.tables.size() == 1);
    const Table& t = run->results.tables[0];
    REQUIRE(t.sweep_names == std::vector<std::string>{"step", "z", "time"});
    // Per z point: the nearest row to a time, inside that point's rows.
    auto point = [&](double z, double at, const char* name) {
        const std::size_t col = col_of(t, name);
        std::size_t best = static_cast<std::size_t>(-1);
        for (std::size_t i = 0; i < t.signals[col].data.size(); ++i) {
            if (t.sweep_axes[1][i] != z) {
                continue;
            }
            if (best == static_cast<std::size_t>(-1) ||
                std::abs(t.sweep_axes[2][i] - at) <
                    std::abs(t.sweep_axes[2][best] - at)) {
                best = i;
            }
        }
        REQUIRE(best != static_cast<std::size_t>(-1));
        return std::pair{t.sweep_axes[2][best], t.signals[col].data[best]};
    };
    // Before any reflection returns (t < Td) the line's input IS Z0:
    // v(a) = V * z/(Rs+z) - the rebind is visible at the input directly.
    for (const double z : {50.0, 100.0}) {
        const auto [tt, va] = point(z, 1.0e-9, "v(a)");
        CHECK(va == approx(std::clamp(tt / 1e-9, 0.0, 1.0) * z / (50.0 + z)));
        CHECK(point(z, 1.0e-9, "v(b)").second == approx(0.0));
    }
    // z=50 (matched): the output is the delayed divider.
    {
        const auto [tt, vb] = point(50.0, 2.5e-9, "v(b)");
        CHECK(vb == approx(0.5 * std::clamp((tt - 1e-9) / 1e-9, 0.0, 1.0)));
    }
    // z=100 (mismatched): the launched wave W1 = 2Vz/(Rs+z), the arrival
    // re-divides at the load, v(b) = W1 * RL/(RL+z).
    {
        const auto [tt, vb] = point(100.0, 2.5e-9, "v(b)");
        CHECK(vb == approx(2.0 * 100.0 / 150.0 * 50.0 / 150.0 *
                           std::clamp((tt - 1e-9) / 1e-9, 0.0, 1.0)));
    }
}


// --- BJT Early effect (Vaf/Var: the deferred-device-physics follow-up) ----------
//
// SPICE3's base charge without roll-off: the transport current is divided
// by qb = 1/oef, oef = 1 - Vbc/Vaf - Vbe/Var. Vaf is the finite output
// conductance (go = Ic/Vaf, ro = Vaf/Ic); Var is its reverse-active
// mirror. All rows are the exact closed form - the .dc ramp IS the
// derivative, so dIc/dVbc is pinned digit-for-digit.

namespace {

// Vt at the model's default Tnom = 27 C (matches devices_m4.cpp).
constexpr double kBjtVt = (27.0 + 273.15) * 8.617333262145e-5;

// The ws::spice BJT collector current (forward/reverse transport times
// the Early factor): Ic = Is*(e^{Vbe/(Nf*Vt)} - e^{Vbc/(Nr*Vt)})*oef.
double bjt_ic_early(double vbe, double vbc, double is_val, double nf, double nr,
                    double vaf, double var_val) {
    const double t = is_val * (std::exp(vbe / (nf * kBjtVt)) -
                               std::exp(vbc / (nr * kBjtVt)));
    const double oef = 1.0 - (vaf > 0.0 ? vbc / vaf : 0.0) -
                       (var_val > 0.0 ? vbe / var_val : 0.0);
    return t * oef;
}

}  // namespace

TEST_CASE("BJT Vaf/Var: the output characteristic is the Early ramp (exact)") {
    const auto run = run_text(
        "early output characteristic\n"
        "Vbe b 0 0.7\n"
        "Vce c 0 1\n"
        "Q1 c b 0 qn\n"
        ".model qn NPN(Is=1e-14 Bf=100 Vaf=50)\n"
        ".dc Vce 1 5 1\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    REQUIRE(t.signals[col_of(t, "i(Vce)")].data.size() == 5);
    for (std::size_t k = 0; k < 5; ++k) {
        const double vce = t.sweep_axes[0][k];
        // i(Vce) is negative when delivering: the collector source sinks
        // Ic. The Early ramp: Ic(Vce) = Ic0*(1 + (Vce - Vbe)/Vaf) plus
        // the (utterly negligible but exact) reverse exponent.
        CHECK(t.signals[col_of(t, "i(Vce)")].data[k] ==
              approx(-bjt_ic_early(0.7, 0.7 - vce, 1e-14, 1.0, 1.0, 50.0, 0.0)));
    }
    // The ramp's slope is the output conductance: dIc/dVce = Ic0/Vaf.
    const double slope = (t.signals[col_of(t, "i(Vce)")].data[4] -
                          t.signals[col_of(t, "i(Vce)")].data[0]) / 4.0;
    const double ic0 = bjt_ic_early(0.7, 0.7 - 3.0, 1e-14, 1.0, 1.0, 0.0, 0.0);
    CHECK(slope == approx(-ic0 / 50.0));
}

TEST_CASE("BJT Vaf/Var: Var scales the forward current; saturation combines both") {
    // Reverse Early: the forward Ic picks up the (1 - Vbe/Var) factor.
    // Fixed-bias sources keep Vbe/Vbc identical with and without Var, so
    // the ratio of two .op runs isolates the factor exactly.
    const std::string body =
        "Vbe b 0 0.65\n"
        "Vce c 0 5\n"
        "Q1 c b 0 qn\n"
        ".model qn NPN(Is=1e-14 Bf=100 {early})\n"
        ".op\n";
    const auto plain = run_text("early none\n" + std::string(body).replace(body.find("{early}"), 7, ""));
    const auto with_var = run_text("early var\n" + body.substr(0, body.find("{early}")) + "Var=20)\n.op\n");
    REQUIRE(plain);
    REQUIRE(with_var);
    const double ic_plain =
        -plain->results.tables[0].signals[col_of(plain->results.tables[0], "i(Vce)")].data[0];
    const double ic_var =
        -with_var->results.tables[0]
             .signals[col_of(with_var->results.tables[0], "i(Vce)")]
             .data[0];
    CHECK(ic_plain == approx(bjt_ic_early(0.65, 0.65 - 5.0, 1e-14, 1.0, 1.0, 0.0, 0.0)));
    CHECK(ic_var == approx(bjt_ic_early(0.65, 0.65 - 5.0, 1e-14, 1.0, 1.0, 0.0, 20.0)));
    CHECK(ic_var / ic_plain == approx(1.0 - 0.65 / 20.0));

    // Saturation: both junctions forward, both Early terms live at once -
    // the closed form covers it exactly.
    const auto sat = run_text(
        "early saturation\n"
        "Vbe b 0 0.8\n"
        "Vce c 0 0.3\n"
        "Q1 c b 0 qs\n"
        ".model qs NPN(Is=1e-14 Bf=100 Vaf=50 Var=20)\n"
        ".op\n");
    REQUIRE(sat);
    const double ic_sat =
        -sat->results.tables[0].signals[col_of(sat->results.tables[0], "i(Vce)")].data[0];
    CHECK(ic_sat == approx(bjt_ic_early(0.8, 0.8 - 0.3, 1e-14, 1.0, 1.0, 50.0, 20.0)));
}

TEST_CASE("BJT Vaf/Var: .ac exposes go = Ic/Vaf at the frozen OP") {
    // Base AC-grounded (a V source without an AC spec is a small-signal
    // short), collector AC-driven: the collector admittance is
    // dIc/dVc = gm_r*oef + Ic_transport/Vaf - the Early output
    // conductance plus the (here negligible) reverse junction term.
    const auto run = run_text(
        "early go\n"
        "Vbe b 0 DC 0.7\n"
        "Vce c 0 DC 5 AC 1\n"
        "Q1 c b 0 qn\n"
        ".model qn NPN(Is=1e-14 Bf=100 Vaf=50)\n"
        ".ac lin 1 1k 1k\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    const double re_i =
        t.signals[col_of(t, "re(i(Vce))")].data[0];
    const double vbe = 0.7;
    const double vbc = vbe - 5.0;
    const double t_tran = 1e-14 * (std::exp(vbe / kBjtVt) - std::exp(vbc / kBjtVt));
    const double gm_r = 1e-14 / (1.0 * kBjtVt) * std::exp(vbc / (1.0 * kBjtVt));
    const double oef = 1.0 - vbc / 50.0;
    const double go = gm_r * oef + t_tran / 50.0;
    CHECK(re_i == approx(-go));
    CHECK(t.signals[col_of(t, "im(i(Vce))")].data[0] == approx(0.0));
}

TEST_CASE("BJT Vaf/Var: Vaf=0 disables the term; negative values reject") {
    // Vaf=0 is the documented off switch: the M4a answer comes back
    // bit-for-bit (the stamp is byte-identical with both terms off).
    const auto run = run_text(
        "early off\n"
        "Vbe b 0 0.7\n"
        "Vce c 0 5\n"
        "Q1 c b 0 qn\n"
        ".model qn NPN(Is=1e-14 Bf=100 Vaf=0)\n"
        ".op\n");
    REQUIRE(run);
    CHECK(-run->results.tables[0]
               .signals[col_of(run->results.tables[0], "i(Vce)")]
               .data[0] ==
          approx(bjt_ic_early(0.7, 0.7 - 5.0, 1e-14, 1.0, 1.0, 0.0, 0.0)));

    // Negative Early voltages flip the output conductance's sign -
    // rejected loudly, not silently simulated.
    CHECK(error_of(run_text("early bad\n"
                            "Vbe b 0 0.7\n"
                            "Vce c 0 5\n"
                            "Q1 c b 0 qn\n"
                            ".model qn NPN(Is=1e-14 Vaf=-50)\n"
                            ".op\n"))
              .find("Vaf >= 0") != std::string::npos);
    CHECK(error_of(run_text("early badr\n"
                            "Vbe b 0 0.7\n"
                            "Vce c 0 5\n"
                            "Q1 c b 0 qn\n"
                            ".model qn NPN(Is=1e-14 Var=-20)\n"
                            ".op\n"))
              .find("Var >= 0") != std::string::npos);
}

// --- diode Rs (series resistance: the deferred-device-physics pull-in) -----------
//
// Rs > 0 moves the junction to an internal 'name#mid' node (SPICE3's
// topology, the L Rser precedent): anode --[Rs/area]-- mid --[Shockley +
// Cjo companion]-- cathode. The .op of a series-R diode is the SAME
// Lambert-W closed form as the M3 row, with R + Rs as the loop
// resistance - the internal and external resistors are in series.

namespace {

// Halley-iterated Lambert-W solution of V = I*(Rext+Rs) + n*Vt*ln(I/Is + 1)
// (the same reference as the M3 diode_series_r_op row).
double diode_rs_op_current(double v, double r_total, double is_val, double n) {
    constexpr double vt = (27.0 + 273.15) * 8.617333262145e-5;
    const double b = n * vt;
    const double a = is_val * r_total;
    const double z_arg = (a / b) * std::exp((v + a) / b);
    double w = std::log(z_arg) - std::log(std::log(z_arg));
    for (int i = 0; i < 80; ++i) {
        const double e = std::exp(w);
        const double f = w * e - z_arg;
        const double df = e * (w + 1.0);
        const double d2f = e * (w + 2.0);
        const double den = 2.0 * df * df - f * d2f;
        if (den == 0.0) {
            break;
        }
        const double step = 2.0 * f * df / den;
        w -= step;
        if (std::abs(step) < 1e-15 * std::max(1.0, std::abs(w))) {
            break;
        }
    }
    return (b / r_total) * w - is_val;
}

}  // namespace

TEST_CASE("diode Rs: the .op is the Lambert-W form with R+Rs in the loop") {
    const auto run = run_text(
        "rs op\n"
        "V1 in 0 5\n"
        "R1 in out 1k\n"
        "D1 out 0 dx\n"
        ".model dx D(Is=1e-14 N=1 Rs=200)\n"
        ".op\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    REQUIRE(t.rows().size() == 1);
    const double ref_i = diode_rs_op_current(5.0, 1200.0, 1e-14, 1.0);
    CHECK(t.rows()[0][col_of(t, "i(V1)")] == doctest::Approx(-ref_i).epsilon(1e-7));
    CHECK(t.rows()[0][col_of(t, "v(out)")] ==
          doctest::Approx(5.0 - ref_i * 1000.0).epsilon(1e-7));
    // The internal node carries the TRUE junction voltage: Vd = V - I*(R+Rs)
    // (a deliverable: v(D1#mid) is what a probing engineer would call Vd).
    CHECK(t.rows()[0][col_of(t, "v(D1#mid)")] ==
          doctest::Approx(5.0 - ref_i * 1200.0).epsilon(1e-7));
    // And with Rs = 0 the #mid node does not exist at all (the M3 save
    // list is untouched).
    const auto plain = run_text(
        "rs none\n"
        "V1 in 0 5\n"
        "R1 in out 1k\n"
        "D1 out 0 dx\n"
        ".model dx D(Is=1e-14 N=1)\n"
        ".op\n");
    REQUIRE(plain);
    for (const std::string& name :
         signal_names(plain->results.tables[0])) {
        CHECK(name.find("#mid") == std::string::npos);
    }
    CHECK(plain->results.tables[0].rows()[0][col_of(plain->results.tables[0], "i(V1)")] ==
          doctest::Approx(-diode_rs_op_current(5.0, 1000.0, 1e-14, 1.0)).epsilon(1e-7));
}

TEST_CASE("diode Rs: .dc tracks the Lambert-W point; area scales Rs down") {
    const auto run = run_text(
        "rs dc\n"
        "V1 in 0 3\n"
        "R1 in out 1k\n"
        "D1 out 0 dx 2\n"
        ".model dx D(Is=1e-14 N=1 Rs=400)\n"
        ".dc V1 1 3 1\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    REQUIRE(t.signals[col_of(t, "i(V1)")].data.size() == 3);
    for (std::size_t k = 0; k < 3; ++k) {
        const double v = t.sweep_axes[0][k];
        // Area 2: Is*2 conductance but Rs/2 = 200 ohms - the effective
        // loop resistance is Rext + Rs/area, and Is scales up.
        const double ref = diode_rs_op_current(v, 1000.0 + 200.0, 2e-14, 1.0);
        CHECK(t.signals[col_of(t, "i(V1)")].data[k] ==
              doctest::Approx(-ref).epsilon(1e-7));
    }
}

TEST_CASE("diode Rs: .tran settles to the same OP through the Rs*Cjo pole") {
    // The Rs x Cjo pair is a real RC now: the ramp charges the junction
    // through it and the transient settles at the Lambert-W OP (the Cjo
    // companion rides the mid node). The settle carries the classic trap
    // ring on this stiff pole (dt >> tau): the alternating component sits
    // inside the trtol-scaled LTE and never trips the 6.1 BE-damping
    // (which fires on REJECTED steps) - ngspice rings the same way - so
    // the gate takes the mean of the tail, where the alternating part
    // cancels to 1e-9 and the OP is left.
    const auto run = run_text(
        "rs tran\n"
        "V1 in 0 PWL(0 0 20u 5)\n"
        "R1 in out 1k\n"
        "D1 out 0 dx\n"
        ".model dx D(Is=1e-14 N=1 Rs=200 Cjo=10p)\n"
        ".tran 1u 60u\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    const double ref_i = diode_rs_op_current(5.0, 1200.0, 1e-14, 1.0);
    const std::size_t col_out = col_of(t, "v(out)");
    const std::size_t col_mid = col_of(t, "v(D1#mid)");
    REQUIRE(t.signals[col_out].data.size() > 8);
    double mean_out = 0.0;
    double mean_mid = 0.0;
    std::size_t n = 0;
    for (std::size_t k = t.signals[col_out].data.size() - 6;
         k < t.signals[col_out].data.size(); ++k, ++n) {
        mean_out += t.signals[col_out].data[k];
        mean_mid += t.signals[col_mid].data[k];
    }
    mean_out /= static_cast<double>(n);
    mean_mid /= static_cast<double>(n);
    CHECK(mean_out == doctest::Approx(5.0 - ref_i * 1000.0).epsilon(1e-6));
    CHECK(mean_mid == doctest::Approx(5.0 - ref_i * 1200.0).epsilon(1e-6));
}

TEST_CASE("diode Rs: .ac sees Rs + 1/(gd + j*w*Cj) at the frozen OP") {
    // Reverse-biased diode, AC-driven through the anode: the small-signal
    // impedance is the series combination of Rs and the junction (gd in
    // parallel with Cj(Vd_op)). With a deep reverse bias gd is ~0 and
    // Cj(-5V) = Cjo/sqrt(1 + 5/Vj) - the real part isolates Rs.
    const auto run = run_text(
        "rs ac\n"
        "V1 a 0 DC -5 AC 1\n"
        "D1 a 0 dx\n"
        ".model dx D(Is=1e-14 N=1 Rs=200 Cjo=10p Vj=1 M=0.5)\n"
        ".ac lin 1 1meg 1meg\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    constexpr double vt = (27.0 + 273.15) * 8.617333262145e-5;
    const double cj = 10e-12 / std::sqrt(1.0 + 5.0 / 1.0);
    const double gd = 1e-14 / vt * std::exp(-5.0 / vt);  // ~0 at -5 V
    const double w = 2.0 * 3.14159265358979323846 * 1e6;
    // Y_junction = gd + j*w*Cj; Z_total = Rs + 1/Y_junction.
    const std::complex<double> yj(gd, w * cj);
    const std::complex<double> z_total = 200.0 + 1.0 / yj;
    const std::complex<double> y_total = 1.0 / z_total;
    // i(V1) is negative when delivering into the diode.
    CHECK(t.signals[col_of(t, "re(i(V1))")].data[0] ==
          doctest::Approx(-y_total.real()).epsilon(1e-6));
    CHECK(t.signals[col_of(t, "im(i(V1))")].data[0] ==
          doctest::Approx(-y_total.imag()).epsilon(1e-6));
}

TEST_CASE("diode Rs: rejections and the netlist round-trip") {
    CHECK(error_of(run_text("rs bad\nV1 a 0 1\nD1 a 0 dx\n.model dx D(Is=1e-14 Rs=-1)\nR1 a 0 1k\n.op\n"))
              .find("Rs >= 0") != std::string::npos);
    // The writer re-emits the model card (Rs rides along in the raw
    // tokens); the written deck re-solves to the same OP.
    const std::string deck =
        "rs roundtrip\n"
        "V1 in 0 5\n"
        "R1 in out 1k\n"
        "D1 out 0 dx\n"
        ".model dx D(Is=1e-14 N=1 Rs=200)\n"
        ".op\n";
    const ws::core::Result<ws::spice::Deck> parsed =
        read_deck("t.cir", deck, ws::spice::ParseOptions{});
    REQUIRE(parsed);
    std::vector<Warning> w;
    const ws::core::Result<ws::spice::DeckAst> ast = build_deck_ast(*parsed, w);
    REQUIRE(ast);
    const ws::core::Result<ws::spice::Circuit> circ = elaborate_deck(*ast, 27.0, w);
    REQUIRE(circ);
    const ws::core::Result<ws::spice::RunState> rs = ws::spice::RunState::bind(*circ);
    REQUIRE(rs);
    const ws::core::Result<std::string> net = write_netlist(*circ, *rs);
    REQUIRE(net);
    CHECK(net->find("Rs=200") != std::string::npos);
    const std::size_t end_pos = net->rfind(".end");
    REQUIRE(end_pos != std::string::npos);
    const auto again = run_text(net->substr(0, end_pos) + ".op\n.end\n");
    REQUIRE(again);
    const double ref_i = diode_rs_op_current(5.0, 1200.0, 1e-14, 1.0);
    CHECK(again->results.tables[0]
              .signals[col_of(again->results.tables[0], "i(V1)")]
              .data[0] == doctest::Approx(-ref_i).epsilon(1e-7));
}

// --- §7.7 temperature (M5) -------------------------------------------------------

TEST_CASE("temp: diode .dc temp sweeps Is(T)/Vt(T) - the SPICE3 form") {
    const auto run = run_text(
        "dtemp\n"
        "V1 a 0 0.65\n"
        "D1 a 0 dx\n"
        ".model dx D(Is=1e-14 N=1 Xti=3 Eg=1.11)\n"
        ".dc temp 27 127 25\n"
        ".print dc v(a) i(D1)\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    REQUIRE(t.row_count() == 5);
    const double k_over_q = 8.617333262145e-5;  // the repo's CODATA set
    for (std::size_t k = 0; k < 5; ++k) {
        const double c = 27.0 + 25.0 * k;
        const double tk = c + 273.15;
        const double tnom = 300.15;
        const double vt = tk * k_over_q;
        // SPICE3 diotemp.c (tlev=0): the emission-coefficient-scaled form.
        const double is_t = 1e-14 *
                            std::exp((tk / tnom - 1.0) * 1.11 / vt +
                                     (3.0 / 1.0) * std::log(tk / tnom));
        const double ref = is_t * (std::exp(0.65 / vt) - 1.0);
        CAPTURE(c);
        CHECK(t.sweep_axes[0][k] == doctest::Approx(c));
        CHECK(t.signal("i(D1)")[k] == doctest::Approx(ref).epsilon(1e-12));
    }
}

TEST_CASE("temp: diode at Tnom is bit-identical; per-instance temp overrides") {
    // At T == Tnom the temp factor is exactly 0 - the M3 answer stands.
    const auto base = run_text("d0\nV1 a 0 0.7\nD1 a 0 dx\n.model dx D(Is=1e-14)\n.op\n");
    const auto hot = run_text(
        "d1\nV1 a 0 0.7\nD1 a 0 dx\n.model dx D(Is=1e-14)\n.options temp=27\n.op\n");
    REQUIRE(base);
    REQUIRE(hot);
    const double i0 = base->results.tables[0].rows()[0][col_of(base->results.tables[0], "i(D1)")];
    const double i1 = hot->results.tables[0].rows()[0][col_of(hot->results.tables[0], "i(D1)")];
    CHECK(i0 == i1);  // exactly

    // One diode at temp=77 next to one at the circuit temperature: the
    // instance override reaches the Shockley equation alone.
    const auto pair = run_text(
        "d2\nV1 a 0 0.7\nD1 a 0 dx\nD2 a 0 dx temp=77\n.model dx D(Is=1e-14)\n.op\n");
    REQUIRE(pair);
    const Table& t = pair->results.tables[0];
    const double k_over_q = 8.617333262145e-5;
    const double tk = 77.0 + 273.15, tnom = 300.15, vt = tk * k_over_q;
    const double is_t =
        1e-14 * std::exp((tk / tnom - 1.0) * 1.11 / vt + 3.0 * std::log(tk / tnom));
    const double vd = 0.7;
    CHECK(t.rows()[0][col_of(t, "i(D2)")] ==
          doctest::Approx(is_t * (std::exp(vd / vt) - 1.0)).epsilon(1e-12));
}

TEST_CASE("temp: R TC1/TC2 polynomial, the model form, and instance temp=") {
    // R(T) = R*(1 + TC1*dt + TC2*dt^2) - the exact polynomial.
    const auto run = run_text(
        "rtc\n"
        "V1 in 0 5\n"
        "R1 in out 2k tc1=0.005 tc2=2e-5\n"
        "R2 out 0 1k\n"
        ".options temp=100\n"
        ".op\n");
    REQUIRE(run);
    const double dt = 100.0 - 27.0;
    const double r1 = 2000.0 * (1.0 + 0.005 * dt + 2e-5 * dt * dt);
    const Table& t = run->results.tables[0];
    CHECK(t.rows()[0][col_of(t, "v(out)")] ==
          doctest::Approx(5.0 * 1000.0 / (1000.0 + r1)).epsilon(1e-12));

    // The model form: same numbers through .model RM R(...).
    const auto model_run = run_text(
        "rtcm\n"
        "V1 in 0 5\n"
        "R1 in out RM 2k\n"
        "R2 out 0 1k\n"
        ".model RM R(tc1=0.005 tc2=2e-5)\n"
        ".options temp=100\n"
        ".op\n");
    REQUIRE(model_run);
    CHECK(model_run->results.tables[0].rows()[0][
              col_of(model_run->results.tables[0], "v(out)")] ==
          doctest::Approx(5.0 * 1000.0 / (1000.0 + r1)).epsilon(1e-12));

    // The instance tc1= overrides the model's; instance temp= overrides
    // the run temperature for that device alone.
    const auto mixed = run_text(
        "rtcx\n"
        "V1 in 0 5\n"
        "R1 in out RM 2k tc1=0\n"
        "R2 out 0 1k temp=127\n"
        ".model RM R(tc1=0.005 tc2=2e-5)\n"
        ".options temp=100\n"
        ".op\n");
    REQUIRE(mixed);
    // tc1=0 (instance) kills tc1 only; the model tc2 still applies.
    const double r1_eff = 2000.0 * (1.0 + 0.0 + 2e-5 * dt * dt);
    const double r2_eff = 1000.0;  // temp=127 with TC=0: unchanged
    CHECK(mixed->results.tables[0].rows()[0][
              col_of(mixed->results.tables[0], "v(out)")] ==
          doctest::Approx(5.0 * r2_eff / (r2_eff + r1_eff)).epsilon(1e-12));
}

TEST_CASE("temp: C TC shifts the RC pole; Tnom is per-model") {
    const auto run = run_text(
        "ctc\n"
        "V1 in 0 AC 1\n"
        "R1 in out 1k\n"
        "C1 out 0 1n tc1=0.01\n"
        ".options temp=127\n"
        ".ac lin 1 159154.943 159154.943\n");
    REQUIRE(run);
    // C(127) = 1n*(1 + 0.01*100) = 2n; fc = 1/(2pi*R*C) = 79.6 kHz; at
    // 159.15 kHz the response is 1/(1+j*2) - re=0.2, im=-0.4 (the
    // default re()/im() pair columns; the derived forms land with §7.4).
    const Table& t = run->results.tables[0];
    CHECK(t.signal("re(v(out))")[0] == doctest::Approx(0.2).epsilon(1e-9));
    CHECK(t.signal("im(v(out))")[0] == doctest::Approx(-0.4).epsilon(1e-9));
}

TEST_CASE("temp: BJT Is(T)/Vt(T) and the XTB beta exponent") {
    // Vbe-driven: Ic = Is(T)*(e^Vbe/Vt(T) - 1) (Early off), and
    // Ib = Ic/Bf(T) with Bf(T) = Bf*exp(XTB*ln(T/Tnom)).
    const auto run = run_text(
        "qtemp\n"
        "Q1 c b 0 np\n"
        "Vb b 0 0.7\n"
        "Vc c 0 5\n"
        ".model np NPN(Is=1e-15 Bf=100 Xti=3 Xtb=1.5)\n"
        ".options temp=77\n"
        ".op\n");
    REQUIRE(run);
    const double k_over_q = 8.617333262145e-5;
    const double tk = 77.0 + 273.15, tnom = 300.15, vt = tk * k_over_q;
    const double is_t =
        1e-15 * std::exp((tk / tnom - 1.0) * 1.11 / vt + 3.0 * std::log(tk / tnom));
    const double ic_ref = is_t * (std::exp(0.7 / vt) - 1.0);
    const double bf_t = 100.0 * std::exp(1.5 * std::log(tk / tnom));
    const Table& t = run->results.tables[0];
    CHECK(t.rows()[0][col_of(t, "i(Vc)")] ==
          doctest::Approx(-ic_ref).epsilon(1e-10));
    CHECK(t.rows()[0][col_of(t, "i(Vb)")] ==
          doctest::Approx(-ic_ref / bf_t).epsilon(1e-10));
}

TEST_CASE("temp: MOS Vto(T)/Kp(T) - the mos1temp pbfact chain") {
    const auto run = run_text(
        "mtemp\n"
        "M1 d g 0 0 nm L=2u W=10u\n"
        "Vg g 0 2\n"
        "Vd d 0 5\n"
        ".model nm NMOS(Vto=1 Kp=50u Lambda=0.02 Gamma=0.4 Phi=0.6)\n"
        ".options temp=100\n"
        ".op\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    CHECK(t.rows()[0][col_of(t, "i(Vd)")] ==
          doctest::Approx(-0.000124400975915).epsilon(1e-9));
    // Pinned against ngspice 44.2 (.op id = 1.24401e-4 at TEMP=100,
    // TNOM=27) - the reference row for the whole pbfact chain.
}

// --- .nodeset (§7.2/M5) ----------------------------------------------------------

namespace {
// The bistable B-source deck: an N-shaped device I(V) (a tunnel-diode-like
// cubic) behind a 300-ohm load line - three load-line crossings, two
// stable. The device current at V (the KCL residual identity the checks
// below reuse).
double bistable_device_i(double v) {
    return 1.3 * (v * v * v - 0.72 * v * v + 0.108 * v);
}
}  // namespace

TEST_CASE(".nodeset: steers the DC operating point's basin on a bistable") {
    const char* deck_head =
        "ns bistable\n"
        "V1 in 0 1.2\n"
        "R1 in out 300\n"
        "B1 out 0 I=1.3*(v(out)^3 - 0.72*v(out)^2 + 0.108*v(out))\n";
    // High hint -> the high-voltage stable point.
    const auto hi = run_text(std::string(deck_head) +
                             ".nodeset v(out)=0.8\n"
                             ".op\n");
    REQUIRE(hi);
    const double v_hi = hi->results.tables[0].rows()[0][
        col_of(hi->results.tables[0], "v(out)")];
    // Low hint -> the low-voltage stable point.
    const auto lo = run_text(std::string(deck_head) +
                             ".nodeset v(out)=0.02\n"
                             ".op\n");
    REQUIRE(lo);
    const double v_lo = lo->results.tables[0].rows()[0][
        col_of(lo->results.tables[0], "v(out)")];
    // No hint: the plain cold start picks the low basin.
    const auto plain = run_text(std::string(deck_head) + ".op\n");
    REQUIRE(plain);
    const double v_none = plain->results.tables[0].rows()[0][
        col_of(plain->results.tables[0], "v(out)")];
    CHECK(v_hi > 0.4);
    CHECK(v_lo < 0.1);
    CHECK(v_none == doctest::Approx(v_lo).epsilon(1e-12));
    // Both are TRUE solutions: the device current matches the load line
    // (KCL at `out`) to solver tolerance - a hint may pick a basin, never
    // bend the answer.
    for (const double v : {v_hi, v_lo, v_none}) {
        CHECK(bistable_device_i(v) ==
              doctest::Approx((1.2 - v) / 300.0).epsilon(1e-9));
    }
}

TEST_CASE(".nodeset: rejects ground and unknown nodes; .ac OP and .tran t=0") {
    CHECK(error_of(run_text(
              "ns bad\nV1 a 0 1\nR1 a 0 1k\n.nodeset v(0)=1\n.op\n"))
              .find("ground") != std::string::npos);
    CHECK(error_of(run_text(
              "ns bad\nV1 a 0 1\nR1 a 0 1k\n.nodeset v(nope)=1\n.op\n"))
              .find("unknown node") != std::string::npos);
    CHECK(error_of(run_text(
              "ns form\nV1 a 0 1\nR1 a 0 1k\n.nodeset a 5\n.op\n"))
              .find("v(node)=value") != std::string::npos);

    // The .ac OP honors the hint (same bistable, high basin).
    const auto ac = run_text(
        "ns ac\n"
        "V1 in 0 1.2 AC 1\n"
        "R1 in out 300\n"
        "B1 out 0 I=1.3*(v(out)^3 - 0.72*v(out)^2 + 0.108*v(out))\n"
        ".nodeset v(out)=0.8\n"
        ".ac lin 1 1 1\n");
    REQUIRE(ac);
    // The .ac table's re(v(out)) names the small-signal response; the OP
    // landed in the high basin iff the linearized conductance is positive
    // there (the device slope at the high point exceeds the load).
    CHECK(ac->reports[0].analysis == ".ac");

    // The .tran t=0 OP honors it too (the transient then marches from the
    // hinted basin; at these values the OP is a genuine rest point).
    const auto tr = run_text(
        "ns tran\n"
        "V1 in 0 1.2\n"
        "R1 in out 300\n"
        "B1 out 0 I=1.3*(v(out)^3 - 0.72*v(out)^2 + 0.108*v(out))\n"
        ".nodeset v(out)=0.8\n"
        ".tran 1u 5u\n");
    REQUIRE(tr);
    const Table& tt = tr->results.tables[0];
    // The t=0 row sits in the HIGH basin, and it is a rest point: the
    // transient holds it for the whole run (nothing drives it away).
    const std::span<const double> vout = tt.signal("v(out)");
    REQUIRE(!vout.empty());
    CHECK(vout.front() > 0.4);
    for (const double v : vout) {
        CHECK(v == doctest::Approx(vout.front()).epsilon(1e-6));
    }
}
