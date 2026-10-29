// The M1 end-to-end suite (DESIGN.md §10.1 rows for .op/.dc, §10.4
// parse-error goldens). Decks run through the public API exactly as the
// CLI does: read_deck -> build_deck_ast -> run_deck; closed-form answers
// at 1e-9 relative. No DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here -
// test_spice.cpp owns doctest's single implementation TU for this binary.

#include <cmath>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include <ws/spice/deck.hpp>
#include <ws/spice/error.hpp>
#include <ws/spice/parse.hpp>
#include <ws/spice/results.hpp>
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
        return ws::core::unexpected(parsed.error());
    }
    const ws::core::Result<ws::spice::DeckAst> ast = build_deck_ast(*parsed, w);
    if (!ast) {
        return ws::core::unexpected(ast.error());
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
        {"bjt\nV1 a 0 5\nQ1 a 0 0 qm\n",
         "t.cir:3:1: error: ", "device 'Q'"},
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
