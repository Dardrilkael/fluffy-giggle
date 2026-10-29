// apps/spice_example - the smallest possible ws::spice embedding.
//
// No argv, no file I/O: a RC-transient deck is hardcoded, parsed, run,
// and the v(out) column is sampled to stdout along with the .meas
// result. Three library calls (read_deck -> build_deck_ast -> run_deck)
// and one columnar accessor (Results::signal) is the whole shape.
//
// Build:   .\tools\build.ps1 -Target spice_example
// Run:     build\spice-debug\bin\spice_example.exe

#include <cstdio>
#include <string>
#include <vector>

#include <ws/spice/deck.hpp>
#include <ws/spice/error.hpp>
#include <ws/spice/parse.hpp>
#include <ws/spice/results.hpp>
#include <ws/spice/run.hpp>

namespace {

// A 1 kHz square-wave driving an RC low-pass (R=1k, C=1u). 5 ms of
// simulation, .meas finds the time v(out) first crosses 0.5 V.
// Line 1 is ALWAYS the title (SPICE rule), so the literal starts there
// (no leading newline) - otherwise line 2 would be parsed as a card.
constexpr const char* kDeck = R"SPICE(RC low-pass step response example

Vin in 0 PULSE(0 1 0 1u 1u 500u 1m)
R1  in out 1k
C1  out 0   1u

.tran 10u 5m 0 10u
.save v(out)
.meas tran t_cross WHEN v(out)=0.5 RISE=1
.end
)SPICE";

int fail(const ws::core::Error& e) {
    std::fprintf(stderr, "error: %s\n", e.message.c_str());
    return 1;
}

}  // namespace

int main() {
    using namespace ws::spice;

    // 1. Lex + .include expand -> records (no .include here, so the
    //    resolver is never called; an empty lambda is fine).
    ParseOptions parse_opts;
    parse_opts.include_resolver = [](const std::string&) {
        return ws::core::Result<std::string>(
            ws::core::unexpected(make_error(Diagnostic{
                ErrorKind::Io, "spice_example.no_includes", "no includes"})));
    };
    const ws::core::Result<Deck> deck =
        read_deck("example.cir", kDeck, parse_opts);
    if (!deck) return fail(deck.error());

    // 2. Records -> typed AST (devices, .meas, .tran, ...).
    std::vector<Warning> warnings;
    const ws::core::Result<DeckAst> ast = build_deck_ast(*deck, warnings);
    for (const Warning& w : warnings) {
        std::fprintf(stderr, "warning: %s\n", w.render().c_str());
    }
    if (!ast) return fail(ast.error());

    // 3. AST -> run. Defaults from RunSettings; .options cards would
    //    override them. The deck has one analysis (.tran), so the run
    //    produces one Table; the .meas is evaluated post-hoc.
    const ws::core::Result<RunOutput> run = run_deck(*ast, RunSettings{}, warnings);
    for (const Warning& w : warnings) {
        std::fprintf(stderr, "warning: %s\n", w.render().c_str());
    }
    if (!run) return fail(run.error());

    // 4. Pull the v(out) column out of the columnar Results - no copy,
    //    no stride. The Results::operator[] walks the tables in deck
    //    order and returns the first matching signal.
    const std::span<const double> v_out = run->results["v(out)"];
    if (v_out.empty()) {
        std::fprintf(stderr, "no 'v(out)' signal in run results\n");
        return 1;
    }

    // The shared sweep axis (time, for .tran) lives on the Table.
    const Table& tran = run->results.tables.front();
    const std::span<const double> time = tran.axis(0);

    std::printf("RC low-pass step response  (%zu samples)\n", v_out.size());
    std::printf("----------------------------------------\n");
    const std::size_t step = v_out.size() / 12;
    for (std::size_t i = 0; i < v_out.size(); i += (step ? step : 1)) {
        const double t = (i < time.size()) ? time[i] : 0.0;
        std::printf("  t=%7.4f ms   v(out)=%+.5f V\n", t * 1e3, v_out[i]);
    }
    std::printf("  ...\n  t=%7.4f ms   v(out)=%+.5f V  (last)\n",
                (time.empty() ? 0.0 : time.back()) * 1e3, v_out.back());

    // 5. The .meas result: a name + an optional value (a failed .meas
    //    does NOT fail the run - ngspice semantics).
    std::printf("\n.meas results:\n");
    for (const Measurement& m : run->measurements) {
        if (m.value.has_value()) {
            std::printf("  %s = %.6g\n", m.name.c_str(), *m.value);
        } else {
            std::printf("  %s: FAILED (%s)\n", m.name.c_str(), m.note.c_str());
        }
    }

    // 6. The per-analysis report (Newton iterations, LU factorizations,
    //    accepted/rejected steps - the instrumentation the kernel
    //    already collected).
    std::printf("\nAnalysis reports:\n");
    for (const AnalysisReport& r : run->reports) {
        std::printf("  %s: %zu newton iters, %zu LU factorizations, "
                    "%zu accepted / %zu rejected steps (stage=%s)\n",
                    r.analysis.c_str(), r.newton_iterations,
                    r.lu_factorizations, r.accepted_steps,
                    r.rejected_steps, r.stage.c_str());
    }
    return 0;
}
