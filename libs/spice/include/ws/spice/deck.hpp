#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <ws/core/result.hpp>
#include <ws/spice/error.hpp>
#include <ws/spice/expr.hpp>
#include <ws/spice/parse.hpp>

// Card AST (DESIGN.md §8.3-8.4): build_deck_ast turns records into typed
// cards - one function per card, everything unimplemented rejected with a
// line number, never a silent gap. Subckts keep their raw body records;
// elaboration re-validates them per instance (flattening). .param lives in
// ParamTable with lazy evaluation, scope chains and cycle naming.

namespace ws::spice {

// --- cards -------------------------------------------------------------------

// A device instance card. Elaboration (which owns arity per device kind)
// splits `rest` into values/gains/source names/params; parse_card only
// validates the letter and the name.
struct DeviceCard {
    Token name;               // "R1", "V2", "X3"... first char = kind letter
    std::vector<Token> rest;  // everything after the name, tokens as spelled
    SourceLoc loc;
};

struct ParamDef {
    Token name;      // "rload"
    ExprProgram expr;
};

// .model name type ( key value ... ) - values stay raw tokens; elaboration
// parses them (temperature re-evaluation, §7.7).
struct ModelDef {
    Token name;
    std::string type;             // lowercased: "r" today, diode/bjt/mos later
    std::vector<Token> params;    // flattened key value key value...
    SourceLoc loc;
};

struct OptionsCard {
    SourceLoc loc;
    std::vector<std::pair<Token, Token>> kv;  // key (lowercased key inside), value
};

struct OpCard {
    SourceLoc loc;
};

struct DcSweep {
    Token source;  // V/I source name, or "temp" (§7.3)
    ExprProgram start;
    ExprProgram stop;
    ExprProgram step;
};

struct DcCard {
    SourceLoc loc;
    DcSweep first;                  // first source is the INNERMOST (ngspice)
    std::optional<DcSweep> second;  // optional outer sweep
};

// .tran tstep tstop [tstart [tmax]] [UIC] (§6/§11: M2)
struct TranCard {
    SourceLoc loc;
    ExprProgram tstep;
    ExprProgram tstop;
    ExprProgram tstart;
    ExprProgram tmax;
    bool has_tstart = false;
    bool has_tmax = false;
    bool uic = false;
};

// .ac dec|oct|lin N fstart fstop (§7.4)
struct AcCard {
    enum class Sweep { Dec, Oct, Lin };
    SourceLoc loc;
    Sweep sweep = Sweep::Dec;
    ExprProgram n;
    ExprProgram fstart;
    ExprProgram fstop;
};

// .ic v(node)=value ... (§6.5). One entry per v(name)=expr token; the
// .tran driver resolves node names against the elaborated circuit.
struct IcCard {
    SourceLoc loc;
    std::vector<std::pair<Token, ExprProgram>> node_values;  // node token, volts
};

// .meas (§0.4): one measurement over an analysis's results. The MODELS
// are the §0.4 subset: TRIG/TARG, WHEN, AVG, RMS, MAX, MIN, PP, INTEG,
// FIND...WHEN, FIND...AT, DERIV - a thin parser onto the measure::
// library functions (measure.hpp); anything else rejects at parse.
struct MeasCard {
    enum class Mode {
        TrigTarg, When, Avg, Rms, Max, Min, Pp, Integ, FindWhen, FindAt, Deriv,
    };
    enum class Analysis { Tran, Dc, Ac, Op };
    SourceLoc loc;
    Analysis analysis = Analysis::Tran;
    Token result;                  // the measurement's name
    Mode mode = Mode::Avg;
    Token sig;                     // the measured signal ("v(out)")
    Token find_sig;                // FIND: the signal whose VALUE is reported
    Token trig_sig, targ_sig;      // TRIG/TARG signals
    Token when_sig;                // WHEN signal
    ExprProgram level;             // VAL= / WHEN level / DERIV AT (shared: the
                                   // first numeric operand per mode)
    ExprProgram targ_level;        // TARG VAL=
    ExprProgram at;                // FIND x AT=t
    ExprProgram from, to;          // FROM= TO= window
    bool has_from = false, has_to = false;
    bool has_at = false;
    int rise = 1, targ_rise = 1;   // 1-based crossing counts; 0 = fall
    bool trig_fall = false, targ_fall = false, when_fall = false;
};

// .save / .print / .probe (§0.5 rev): the deck's REQUESTED save-list as
// expressions - plain names (v(out), i(V1)) or post-hoc waveform math
// (v(out)-v(in), i(V1)*v(in), @R1[value]). Evaluated against the stored
// default columns after the run; without any save cards the default list
// stands (all node voltages + branch currents, §0.5).
struct SaveCard {
    SourceLoc loc;
    bool all_analyses = true;  // .save: every analysis; .print names one
    MeasCard::Analysis which = MeasCard::Analysis::Tran;
    std::vector<Token> exprs;  // one per listed quantity
};

// .step (§7.6): one stepped quantity. Forms:
//   .step param NAME start stop inc          (linear)
//   .step param NAME list v1 v2 ...          (explicit list)
//   .step dec|oct param NAME start stop N    (logarithmic)
//   .step temp start stop inc [list|dec...]  (temperature; re-elaborates)
//   .step SRC start stop inc                 (a V/I source's value)
// Multiple .step cards nest, first card outermost.
struct StepCard {
    enum class Kind { Lin, List, Dec, Oct };
    SourceLoc loc;
    Kind kind = Kind::Lin;
    bool is_temp = false;   // re-elaborate per point (§7.7)
    bool is_source = false;  // patch the named V/I source's dc slot
    Token name;              // param name, "temp", or a source name
    ExprProgram start;
    ExprProgram stop;
    ExprProgram inc;         // lin: step; dec/oct: N
    std::vector<ExprProgram> list;  // list form values
};

struct SubcktDef {
    Token name;                       // ".subckt foo a b"
    std::vector<Token> ports;         // port node names, as spelled
    std::vector<ParamDef> defaults;   // "params:" section of the card
    std::vector<Record> body;         // records until .ends (validated later, per use)
    SourceLoc loc;
};

// Analyses run in deck order (§7.1) - the variant keeps that order.
// Exactly one of the four is set (validated by run_deck; the parser can
// only build one per card).
struct AnalysisCard {
    SourceLoc loc;
    std::optional<OpCard> op;
    std::optional<DcCard> dc;
    std::optional<TranCard> tran;
    std::optional<AcCard> ac;
};

struct DeckAst {
    std::string title;
    std::vector<DeviceCard> devices;      // top-level instances (incl. X)
    std::vector<ParamDef> params;         // top-level .param, in order
    std::vector<ModelDef> models;         // top-level .model
    std::vector<OptionsCard> options;     // top-level .options, in order
    std::vector<AnalysisCard> analyses;   // deck order
    std::vector<IcCard> ics;              // top-level .ic cards, in order
    std::vector<StepCard> steps;          // top-level .step cards, in order
    std::vector<MeasCard> meas;           // top-level .meas cards, in order
    std::vector<SaveCard> saves;          // .save/.print/.probe, in order
    std::map<std::string, SubcktDef> subckts;  // key: lowercased name
};

// Turns records into the typed AST. Unknown cards/letters and every
// known-but-unimplemented construct are Parse/Semantic errors with
// file:line; ignored .options keys are warnings (§9.3), collected, never
// fatal.
[[nodiscard]] ws::core::Result<DeckAst> build_deck_ast(const Deck& deck,
                                                       std::vector<Warning>& warnings);

// --- parameter scopes (§8.4) ---------------------------------------------------

// A lazily-evaluated .param scope with a parent chain (subckt instance
// params -> subckt defaults -> global). Values memoize once resolved;
// cycles are reported naming the chain.
class ParamTable {
public:
    ParamTable() = default;
    explicit ParamTable(const ParamTable* parent) : parent_(parent) {}

    // Adds (or replaces) a definition.
    void define(ParamDef def);

    // Instantiates a scope that layers `overrides` (from an X card's
    // params: section) over this table's definitions.
    [[nodiscard]] ParamTable with_overrides(const std::vector<ParamDef>& overrides) const;

    // True when `lower` (already lowercased) is defined here or in a parent.
    [[nodiscard]] bool knows(const std::string& lower) const;

    // True when `lower` is defined in THIS table only - not a parent. The
    // subckt flattener inlines locals into constants and leaves global
    // references live, so a .step/Monte-Carlo override of a global param
    // still reaches inside subckt instances.
    [[nodiscard]] bool knows_local(const std::string& lower) const;

    // Binds an expression's variables against this scope; unbound
    // variables stay nullopt in the result (the expression evaluator
    // reports them with its own context). `rng` (when given) serves
    // gauss()/flat() draws inside REFERENCED definitions - the §8.5 rev
    // Monte-Carlo chain.
    [[nodiscard]] ws::core::Result<std::vector<std::optional<double>>> bind(
        const ExprProgram& prog, class Rand* rng = nullptr) const;

    // Resolves one name (lowercased) to a value, with cycle detection.
    [[nodiscard]] ws::core::Result<double> value(const std::string& lower,
                                                 class Rand* rng = nullptr) const;

    // Resolve cache (§8.4 rev): within one bind, a name resolves to one
    // value - two devices referencing the same Monte-Carlo parameter see
    // the SAME draw. Cleared on (re)definition of that name and by
    // RunState::bind; walks the parent chain.
    void clear_cache() const;

private:
    [[nodiscard]] ws::core::Result<double> resolve(
        const std::string& lower, std::vector<std::string>& active,
        Rand* rng = nullptr) const;
    // bind() with the cycle-detection chain threaded through, so a cycle
    // spanning several definitions (a=b+1, b=a) is caught no matter how
    // deep the reference nest goes.
    [[nodiscard]] ws::core::Result<std::vector<std::optional<double>>> bind_active(
        const ExprProgram& prog, std::vector<std::string>& active,
        Rand* rng = nullptr) const;

    std::map<std::string, ParamDef> defs_;
    mutable std::map<std::string, double> cache_;  // per-bind resolution
    const ParamTable* parent_ = nullptr;
};

}  // namespace ws::spice
