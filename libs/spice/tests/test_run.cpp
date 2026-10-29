// The M1 end-to-end suite (DESIGN.md §10.1 rows for .op/.dc, §10.4
// parse-error goldens). Decks run through the public API exactly as the
// CLI does: read_deck -> build_deck_ast -> run_deck; closed-form answers
// at 1e-9 relative. No DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here -
// test_spice.cpp owns doctest's single implementation TU for this binary.

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
// warnings from every stage.
ws::core::Result<RunOutput> run_text(const std::string& deck,
                                     std::vector<Warning>* warnings = nullptr) {
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
    return run_deck(*ast, RunSettings{}, w);
}

// The rendered error message of a failed run.
std::string error_of(const ws::core::Result<RunOutput>& r) {
    REQUIRE(!r);
    return r.error().message;
}

// Column index of `name` in the table (sweep columns excluded).
std::size_t col_of(const Table& t, const std::string& name) {
    for (std::size_t i = 0; i < t.columns.size(); ++i) {
        if (t.columns[i] == name) {
            return i;
        }
    }
    REQUIRE_MESSAGE(false, ("missing column " + name).c_str());
    return 0;
}

doctest::Approx approx(double v) { return doctest::Approx(v).epsilon(1e-9); }

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
    CHECK(t.columns == std::vector<std::string>{"v(in)", "v(mid)", "i(V1)"});
    REQUIRE(t.rows.size() == 1);
    CHECK(t.rows[0][col_of(t, "v(in)")] == approx(5.0));
    CHECK(t.rows[0][col_of(t, "v(mid)")] == approx(5.0 * 2.0 / 3.0));
    CHECK(t.rows[0][col_of(t, "i(V1)")] == approx(-5.0 / 3000.0));

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
    CHECK(t.rows[0][col_of(t, "v(a)")] == approx(1.0));
    CHECK(t.rows[0][col_of(t, "v(b)")] == approx(2.0 / 3.0));
    CHECK(t.rows[0][col_of(t, "i(V1)")] == approx(i_v1));
    // E: v(c) = 3 * v(b).
    CHECK(t.rows[0][col_of(t, "v(c)")] == approx(2.0));
    // G: draws 0.5*v(a) out of d through 500 Ohm.
    CHECK(t.rows[0][col_of(t, "v(d)")] == approx(2.0 - 0.5 * 1.0 * 500.0));
    // H: v(e) = 100 * i(V1).
    CHECK(t.rows[0][col_of(t, "v(e)")] == approx(100.0 * i_v1));
    // F: v(f) = 2 * i(V1) * R4.
    CHECK(t.rows[0][col_of(t, "v(f)")] == approx(2.0 * i_v1 * 100.0));
    CHECK(t.rows[0][col_of(t, "i(E1)")] == approx(-0.5));
    CHECK(t.rows[0][col_of(t, "i(H1)")] == approx(-100.0 * i_v1 / 1000.0));
}

TEST_CASE(".op: I source orientation follows ngspice (positive: N+ to N-)") {
    // "I1 0 out 1m" charges 'out' positive through R.
    const auto run = run_text("i orientation\nI1 0 out 1m\nR1 out 0 1k\n.op\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    CHECK(t.rows[0][col_of(t, "v(out)")] == approx(1.0));
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
    CHECK(t.columns == std::vector<std::string>{
                           "v(in)", "v(X1:nm)", "v(out)", "i(V1)"});
    // Closed form: v(nm) = 4 * (Rb||R2) / (Rt + Rb||R2) with Rt=3k (the
    // override), Rb=1k, R2=5k.
    CHECK(t.rows[0][col_of(t, "v(in)")] == approx(4.0));
    CHECK(t.rows[0][col_of(t, "v(X1:nm)")] == approx(8.0 / 3.0));
    CHECK(t.rows[0][col_of(t, "v(out)")] == approx(20.0 / 9.0));
    CHECK(t.rows[0][col_of(t, "i(V1)")] == approx(-4.0 / 9000.0));
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
    CHECK(t.rows[0][col_of(t, "v(X1:mid)")] == approx(1.0));
    CHECK(t.rows[0][col_of(t, "i(V1)")] == approx(-1e-3));
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
    REQUIRE(t.rows.size() == 4);
    for (std::size_t k = 0; k < t.rows.size(); ++k) {
        CHECK(t.rows[k][0] == approx(0.3 * static_cast<double>(k)));
        CHECK(t.rows[k][col_of(t, "v(mid)") + 1] ==
              approx(0.3 * static_cast<double>(k) * 2.0 / 3.0));
    }
}

TEST_CASE(".dc: negative direction and single-point sweeps") {
    const auto back = run_text(
        "down\nV1 in 0 5\nR1 in 0 1k\n.dc V1 5 0 -2.5\n");
    REQUIRE(back);
    REQUIRE(back->results.tables[0].rows.size() == 3);
    CHECK(back->results.tables[0].rows[2][0] == approx(0.0));

    const auto one = run_text("single\nV1 in 0 5\nR1 in 0 1k\n.dc V1 3 3 1\n");
    REQUIRE(one);
    REQUIRE(one->results.tables[0].rows.size() == 1);
    CHECK(one->results.tables[0].rows[0][0] == approx(3.0));
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
    REQUIRE(t.rows.size() == 6);
    const std::size_t last = 5;
    CHECK(t.rows[last][0] == approx(4.0));
    CHECK(t.rows[last][1] == approx(1e-3));
    CHECK(t.rows[last][col_of(t, "v(mid)") + 2] == approx(2.0));
}

TEST_CASE(".dc: temp sweep is accepted (M1 devices carry no temp dependence)") {
    const auto run = run_text(
        "temp\nV1 in 0 5\nR1 in 0 1k\n.dc temp 0 100 50\n");
    REQUIRE(run);
    const Table& t = run->results.tables[0];
    REQUIRE(t.rows.size() == 3);
    for (const auto& row : t.rows) {
        CHECK(row[col_of(t, "v(in)") + 1] == approx(5.0));
    }
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
    REQUIRE(t.rows.size() == 5);  // 1, 1.5, 2, 2.5, 3
    CHECK(t.rows[0][0] == approx(1.0));
    CHECK(t.rows[4][0] == approx(3.0));
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
    CHECK(t.columns == std::vector<std::string>{"v(in)", "v(mid)", "i(V1)"});
    CHECK(t.rows[0][col_of(t, "v(in)")] == approx(5.0));
    CHECK(t.rows[0][col_of(t, "v(mid)")] == approx(5.0 * 2.0 / 3.0));
    CHECK(t.rows[0][col_of(t, "i(V1)")] == approx(-5.0 / 3000.0));
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
          "t.cir:4:10: warning: ignored .options key 'foo=1'");
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
    CHECK(t.columns == std::vector<std::string>{"v(in)", "v(mid)"});

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
        {"caps\nV1 a 0 5\nC1 a 0 1u\n",
         "t.cir:3:1: error: ", "device 'C'"},
        {"arity\nV1 a 0 5\nR1 a 0\n",
         "t.cir:3:1: error: ", "needs a value"},
        {"pulse\nV1 a 0 PULSE(0 1 1)\nR1 a 0 1k\n",
         "t.cir:2:1: error: ", "DC VALUE only"},
        {"sub\nX1 a b nosuch\nV1 a 0 5\nR1 a 0 1k\n.op\n",
         "t.cir:2:8: error: ", "unknown subcircuit"},
        {"dc arity\nV1 a 0 5\nR1 a 0 1k\n.dc V1 0 1\n",
         "t.cir:4:1: error: ", "'.dc' takes"},
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
    CHECK(t.rows[0][col_of(t, "v(a)")] == approx(3.0));
    CHECK(t.rows[0][col_of(t, "v(b)")] == approx(2.0));
    CHECK(t.rows[0][col_of(t, "i(V1)")] == approx(-2.5e-3));
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
