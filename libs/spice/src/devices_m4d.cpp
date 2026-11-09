// M4d (DESIGN.md §8.5/§11): the behavioral B source - a nonlinear V or I
// source whose value is an EXPRESSION over circuit quantities:
//
//   B name n+ n- V=expression     (a behavioral voltage source)
//   B name n+ n- I=expression     (a behavioral current source)
//
// The expression language is the shared §8.5 set (+ - * / **, comparisons,
// ?:, the function table) with the behavioral-only signal references
// v(node), v(a,b), i(Vname) plus the variable `time` (.tran; 0 at OP/DC).
// Plain names resolve against the .param scope through parameter slots.
// The expression parses once at elaboration; every Newton iteration then
// evaluates it in FIXED-SIZE dual-number arithmetic: one value pass plus
// one directional-derivative pass per signal reference - §8.5's "value
// plus k-wide gradient in preallocated scratch", the k Jacobian columns
// computed without a single allocation (stack arrays with caps that
// reject loudly at elaboration, never truncate silently).
//
// The stamp is the Norton linearization around the current iterate x0:
//   f(x) = f0 + sum_r (df/dx_r)*(x_r - x_r0)
// The sum's Jacobian part lands in the matrix (E/G-style gain entries at
// the referenced unknowns, recomputed per iteration), the remainder in
// the rhs. Stamp POSITIONS come from the parsed reference list only
// (§3.2): values never move them.
//
// Derivatives of non-smooth functions follow §8.5: abs/sgn take the
// active branch (0 at the kink), min/max the selected argument, if/?:
// the taken branch; sqrt/ln/log/pow outside their domain, division by
// zero and any non-finite result are a Convergence error naming the
// source and the iterate (§9.1) - reported through Device::load_error
// (the kernel's FatalErr path), never a NaN into the matrix.
//
// AC (§8.5): the small-signal element is the gradient at the OP - the
// reference values are frozen in the state slice by update_state at the
// converged OP, and load_ac re-runs the derivative passes there. A
// behavioral source is never a stimulus (no AC spec).
//
// State layout (R = the reference count, fixed at construction):
//   [0 .. R-1]   the ref values at the last update_state (the OP for .ac)
//   [R]          f_cur  - the value at the last load point (residual)
//   [R+1]        f_prev
// The B source is NOT dynamic (no companion state; load_tran = load_dc
// plus the time variable from the TranStep).

#include "cards.hpp"

#include "physconst.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <ws/spice/circuit.hpp>
#include <ws/spice/deck.hpp>
#include <ws/spice/device.hpp>
#include <ws/spice/state.hpp>

namespace ws::spice {

namespace {

Diagnostic dev_diag(ErrorKind kind, std::string code, std::string message, SourceLoc loc) {
    Diagnostic d;
    d.kind = kind;
    d.code = std::move(code);
    d.message = std::move(message);
    d.loc = std::move(loc);
    return d;
}

// --- fixed-size caps (loud at elaboration, never silent truncation) ------------

constexpr std::size_t kMaxVars = 64;    // distinct variables per expression
constexpr std::size_t kMaxRefs = 32;    // signal references per expression
constexpr std::size_t kMaxStack = 256;  // dual evaluation stack depth

// --- dual-number arithmetic (§8.5) ----------------------------------------------

struct Dual {
    double v = 0.0;
    double d = 0.0;
};

struct DualOutcome {
    bool ok = true;
    const char* fn = nullptr;  // the domain failure's function name
    double input = 0.0;        // the offending operand (the iterate value)
};

// One evaluation pass over the program: the value plus the derivative
// w.r.t. variable slot `seed` (-1 = none; forward mode - the seeded slot
// loads d=1). Domain failures mirror ExprProgram::evaluate exactly (the
// same conditions, the same function names) so the two evaluators can
// never disagree about validity. Validated programs cannot underflow or
// overflow the stack; the guards turn a hypothetical violation into a
// loud failure instead of UB.
DualOutcome dual_pass(const ExprProgram& prog, const double* var_values, int seed,
                      Dual& out) {
    DualOutcome res;
    auto fail = [&res](const char* fn, double input) {
        res.ok = false;
        res.fn = fn;
        res.input = input;
    };
    Dual stack[kMaxStack];
    std::size_t depth = 0;
    auto push = [&](Dual v) -> bool {
        if (depth >= kMaxStack) {
            return false;
        }
        stack[depth++] = v;
        return true;
    };
    auto pop = [&](Dual& v) -> bool {
        if (depth == 0) {
            return false;
        }
        v = stack[--depth];
        return true;
    };

    using Op = ExprProgram::Op;
    const auto& code = prog.code();
    for (std::size_t pc = 0; pc < code.size();) {
        const ExprProgram::Instr& in = code[pc];
        switch (in.op) {
            case Op::Const: {
                if (!push({prog.consts_[static_cast<std::size_t>(in.a)], 0.0})) {
                    fail("stack", 0.0);
                    return res;
                }
                ++pc;
                break;
            }
            case Op::Load: {
                if (!push({var_values[static_cast<std::size_t>(in.a)],
                           (in.a == seed) ? 1.0 : 0.0})) {
                    fail("stack", 0.0);
                    return res;
                }
                ++pc;
                break;
            }
            case Op::Neg: {
                Dual a;
                if (!pop(a) || !push({-a.v, -a.d})) {
                    fail("stack", 0.0);
                    return res;
                }
                ++pc;
                break;
            }
            case Op::Add: {
                Dual a, b;
                if (!pop(b) || !pop(a) || !push({a.v + b.v, a.d + b.d})) {
                    fail("stack", 0.0);
                    return res;
                }
                ++pc;
                break;
            }
            case Op::Sub: {
                Dual a, b;
                if (!pop(b) || !pop(a) || !push({a.v - b.v, a.d - b.d})) {
                    fail("stack", 0.0);
                    return res;
                }
                ++pc;
                break;
            }
            case Op::Mul: {
                Dual a, b;
                if (!pop(b) || !pop(a) ||
                    !push({a.v * b.v, a.d * b.v + a.v * b.d})) {
                    fail("stack", 0.0);
                    return res;
                }
                ++pc;
                break;
            }
            case Op::Div: {
                Dual a, b;
                if (!pop(b) || !pop(a)) {
                    fail("stack", 0.0);
                    return res;
                }
                if (b.v == 0.0) {
                    fail("division", a.v);
                    return res;
                }
                const double w = b.v * b.v;
                if (!push({a.v / b.v, (a.d * b.v - a.v * b.d) / w})) {
                    fail("stack", 0.0);
                    return res;
                }
                ++pc;
                break;
            }
            case Op::Pow:
            case Op::Pow2: {
                Dual a, b;
                if (!pop(b) || !pop(a)) {
                    fail("stack", 0.0);
                    return res;
                }
                double v = 0.0, d = 0.0;
                if (a.v > 0.0) {
                    v = std::pow(a.v, b.v);
                    d = v * (b.v * a.d / a.v + std::log(a.v) * b.d);
                } else if (a.v == 0.0) {
                    // 0^b: the value is 0 for b > 0; the slope is
                    // infinite for 0 < b < 1 (nudged like sqrt) and 0 for
                    // b >= 1. b <= 0 diverges - out of domain, like
                    // ExprProgram::evaluate's pow.
                    if (b.v <= 0.0) {
                        fail("pow", a.v);
                        return res;
                    }
                    v = 0.0;
                    d = (b.v >= 1.0) ? 0.0
                                     : b.v * std::pow(1e-12, b.v - 1.0) * a.d;
                } else if (b.v == std::floor(b.v) && std::abs(b.v) < 1e15) {
                    // Integer exponent: defined for negative bases. The
                    // b-partial does not exist over the reals; the chain
                    // rule uses the a-partial only.
                    v = std::pow(a.v, b.v);
                    d = b.v * std::pow(a.v, b.v - 1.0) * a.d;
                } else {
                    fail("pow", a.v);
                    return res;
                }
                if (!std::isfinite(v) || !std::isfinite(d)) {
                    fail("pow", a.v);
                    return res;
                }
                if (!push({v, d})) {
                    fail("stack", 0.0);
                    return res;
                }
                ++pc;
                break;
            }
            case Op::Not: {
                Dual a;
                if (!pop(a) || !push({(a.v == 0.0) ? 1.0 : 0.0, 0.0})) {
                    fail("stack", 0.0);
                    return res;
                }
                ++pc;
                break;
            }
            case Op::Lt:
            case Op::Le:
            case Op::Gt:
            case Op::Ge:
            case Op::Eq:
            case Op::Ne:
            case Op::And:
            case Op::Or: {
                // Predicates: 1/0 values, zero gradient (§8.5).
                Dual a, b;
                if (!pop(b) || !pop(a)) {
                    fail("stack", 0.0);
                    return res;
                }
                double v = 0.0;
                switch (in.op) {
                    case Op::Lt: v = a.v < b.v; break;
                    case Op::Le: v = a.v <= b.v; break;
                    case Op::Gt: v = a.v > b.v; break;
                    case Op::Ge: v = a.v >= b.v; break;
                    case Op::Eq: v = a.v == b.v; break;
                    case Op::Ne: v = a.v != b.v; break;
                    case Op::And: v = (a.v != 0.0 && b.v != 0.0); break;
                    default: v = (a.v != 0.0 || b.v != 0.0); break;
                }
                if (!push({v, 0.0})) {
                    fail("stack", 0.0);
                    return res;
                }
                ++pc;
                break;
            }
            case Op::Jump:
                pc = static_cast<std::size_t>(in.a);
                break;
            case Op::JumpIfFalse: {
                Dual c;
                if (!pop(c)) {
                    fail("stack", 0.0);
                    return res;
                }
                pc = (c.v == 0.0) ? static_cast<std::size_t>(in.b) : pc + 1;
                break;
            }
            case Op::Ternary:
                // The parser lowers ?: to jumps; never executed (same as
                // ExprProgram::evaluate).
                fail("ternary", 0.0);
                return res;
            case Op::Abs: {
                Dual a;
                if (!pop(a)) {
                    fail("stack", 0.0);
                    return res;
                }
                // §8.5: the active branch, 0 at the kink.
                const double s = static_cast<double>((a.v > 0.0) - (a.v < 0.0));
                if (!push({std::abs(a.v), s * a.d})) {
                    fail("stack", 0.0);
                    return res;
                }
                ++pc;
                break;
            }
            case Op::Sqrt: {
                Dual a;
                if (!pop(a)) {
                    fail("stack", 0.0);
                    return res;
                }
                if (a.v < 0.0) {
                    fail("sqrt", a.v);
                    return res;
                }
                // The value is exact everywhere in-domain; the SLOPE at
                // the a=0 kink is infinite. The kernel's cold start is
                // x=0, so every sqrt(v(...)) deck sits on the kink at
                // its first iterate - the slope is evaluated at an
                // argument nudged 1e-12 inside the domain instead (the
                // pnjlim spirit: clamp the linearization, never the
                // value; a genuine out-of-domain VALUE still errors).
                const double s = std::sqrt(a.v);
                const double slope = 1.0 / (2.0 * std::max(s, 1e-12));
                if (!push({s, slope * a.d})) {
                    fail("stack", 0.0);
                    return res;
                }
                ++pc;
                break;
            }
            case Op::Exp: {
                Dual a;
                if (!pop(a)) {
                    fail("stack", 0.0);
                    return res;
                }
                const double e = std::exp(a.v);
                if (!std::isfinite(e)) {
                    fail("exp", a.v);
                    return res;
                }
                if (!push({e, e * a.d})) {
                    fail("stack", 0.0);
                    return res;
                }
                ++pc;
                break;
            }
            case Op::Ln: {
                Dual a;
                if (!pop(a)) {
                    fail("stack", 0.0);
                    return res;
                }
                if (a.v <= 0.0) {
                    fail("ln", a.v);
                    return res;
                }
                if (!push({std::log(a.v), a.d / a.v})) {
                    fail("stack", 0.0);
                    return res;
                }
                ++pc;
                break;
            }
            case Op::Log:
            case Op::Log10: {
                Dual a;
                if (!pop(a)) {
                    fail("stack", 0.0);
                    return res;
                }
                if (a.v <= 0.0) {
                    fail("log", a.v);
                    return res;
                }
                constexpr double kLn10 = 2.30258509299404568402;
                if (!push({std::log10(a.v), a.d / (a.v * kLn10)})) {
                    fail("stack", 0.0);
                    return res;
                }
                ++pc;
                break;
            }
            case Op::Sin: {
                Dual a;
                if (!pop(a) || !push({std::sin(a.v), std::cos(a.v) * a.d})) {
                    fail("stack", 0.0);
                    return res;
                }
                ++pc;
                break;
            }
            case Op::Cos: {
                Dual a;
                if (!pop(a) || !push({std::cos(a.v), -std::sin(a.v) * a.d})) {
                    fail("stack", 0.0);
                    return res;
                }
                ++pc;
                break;
            }
            case Op::Tan: {
                Dual a;
                if (!pop(a)) {
                    fail("stack", 0.0);
                    return res;
                }
                const double t = std::tan(a.v);
                if (!push({t, (1.0 + t * t) * a.d})) {
                    fail("stack", 0.0);
                    return res;
                }
                ++pc;
                break;
            }
            case Op::Asin: {
                Dual a;
                if (!pop(a)) {
                    fail("stack", 0.0);
                    return res;
                }
                if (a.v < -1.0 || a.v > 1.0) {
                    fail("asin", a.v);
                    return res;
                }
                // The ±1 endpoints have infinite slope - nudged (as sqrt).
                const double w = std::max(std::sqrt(1.0 - a.v * a.v), 1e-12);
                if (!push({std::asin(a.v), a.d / w})) {
                    fail("stack", 0.0);
                    return res;
                }
                ++pc;
                break;
            }
            case Op::Acos: {
                Dual a;
                if (!pop(a)) {
                    fail("stack", 0.0);
                    return res;
                }
                if (a.v < -1.0 || a.v > 1.0) {
                    fail("acos", a.v);
                    return res;
                }
                const double w = std::max(std::sqrt(1.0 - a.v * a.v), 1e-12);
                if (!push({std::acos(a.v), -a.d / w})) {
                    fail("stack", 0.0);
                    return res;
                }
                ++pc;
                break;
            }
            case Op::Atan: {
                Dual a;
                if (!pop(a) || !push({std::atan(a.v), a.d / (1.0 + a.v * a.v)})) {
                    fail("stack", 0.0);
                    return res;
                }
                ++pc;
                break;
            }
            case Op::Sinh: {
                Dual a;
                if (!pop(a) || !push({std::sinh(a.v), std::cosh(a.v) * a.d})) {
                    fail("stack", 0.0);
                    return res;
                }
                ++pc;
                break;
            }
            case Op::Cosh: {
                Dual a;
                if (!pop(a) || !push({std::cosh(a.v), std::sinh(a.v) * a.d})) {
                    fail("stack", 0.0);
                    return res;
                }
                ++pc;
                break;
            }
            case Op::Tanh: {
                Dual a;
                if (!pop(a)) {
                    fail("stack", 0.0);
                    return res;
                }
                const double t = std::tanh(a.v);
                if (!push({t, (1.0 - t * t) * a.d})) {
                    fail("stack", 0.0);
                    return res;
                }
                ++pc;
                break;
            }
            case Op::Sgn: {
                Dual a;
                if (!pop(a) ||
                    !push({static_cast<double>((a.v > 0.0) - (a.v < 0.0)), 0.0})) {
                    fail("stack", 0.0);
                    return res;
                }
                ++pc;
                break;
            }
            case Op::Floor: {
                Dual a;
                if (!pop(a) || !push({std::floor(a.v), 0.0})) {
                    fail("stack", 0.0);
                    return res;
                }
                ++pc;
                break;
            }
            case Op::Ceil: {
                Dual a;
                if (!pop(a) || !push({std::ceil(a.v), 0.0})) {
                    fail("stack", 0.0);
                    return res;
                }
                ++pc;
                break;
            }
            case Op::Int: {
                Dual a;
                if (!pop(a) || !push({std::trunc(a.v), 0.0})) {
                    fail("stack", 0.0);
                    return res;
                }
                ++pc;
                break;
            }
            case Op::Uramp: {
                Dual a;
                if (!pop(a) ||
                    !push({a.v > 0.0 ? a.v : 0.0, a.v > 0.0 ? a.d : 0.0})) {
                    fail("stack", 0.0);
                    return res;
                }
                ++pc;
                break;
            }
            case Op::U: {
                Dual a;
                if (!pop(a) || !push({a.v > 0.0 ? 1.0 : 0.0, 0.0})) {
                    fail("stack", 0.0);
                    return res;
                }
                ++pc;
                break;
            }
            case Op::Gauss:
            case Op::Flat:
                // Rejected at the factory; unreachable (and a draw has no
                // derivative).
                fail("gauss", 0.0);
                return res;
            case Op::Atan2: {
                Dual a, b;
                if (!pop(b) || !pop(a)) {
                    fail("stack", 0.0);
                    return res;
                }
                const double w = a.v * a.v + b.v * b.v;
                if (!push({std::atan2(a.v, b.v), (b.v * a.d - a.v * b.d) / w})) {
                    fail("stack", 0.0);
                    return res;
                }
                ++pc;
                break;
            }
            case Op::Min: {
                // §8.5: the selected argument's derivative.
                Dual a, b;
                if (!pop(b) || !pop(a) || !push(a.v <= b.v ? a : b)) {
                    fail("stack", 0.0);
                    return res;
                }
                ++pc;
                break;
            }
            case Op::Max: {
                Dual a, b;
                if (!pop(b) || !pop(a) || !push(a.v >= b.v ? a : b)) {
                    fail("stack", 0.0);
                    return res;
                }
                ++pc;
                break;
            }
            case Op::Limit: {
                // Mirrors ExprProgram::evaluate's 2-arg clamp spelling
                // (clamp(a, min, max) over the pair is a itself).
                Dual a, b;
                if (!pop(b) || !pop(a) || !push(a)) {
                    fail("stack", 0.0);
                    return res;
                }
                ++pc;
                break;
            }
            case Op::If: {
                Dual a, b;
                if (!pop(b) || !pop(a)) {
                    fail("stack", 0.0);
                    return res;
                }
                if (!push({a.v != 0.0 ? b.v : 0.0, a.v != 0.0 ? b.d : 0.0})) {
                    fail("stack", 0.0);
                    return res;
                }
                ++pc;
                break;
            }
        }
    }
    if (depth != 1) {
        res.ok = false;
        res.fn = "stack";
        return res;
    }
    out = stack[0];
    if (!std::isfinite(out.v) || !std::isfinite(out.d)) {
        fail("expression", out.v);
        return res;
    }
    return res;
}

// --- the B source -----------------------------------------------------------------

// One signal reference (v(node), v(a,b) or i(Vname)) as fixed construction
// data: which program variable slot it reads, which unknowns it touches.
struct SignalRef {
    int var_slot = -1;
    bool is_current = false;       // i(Vsrc); else v(...)
    NodeId pos = kGround;          // v(a) positive node
    NodeId neg = kGround;          // v(a,b) negative node
    std::size_t ctl_ordinal = 0;   // i(): index for set_control_branch
    BranchId branch = 0;           // i(): the controlling branch
};

class Bsource final : public Device {
public:
    Bsource(std::string display, NodeId p, NodeId n, bool is_voltage, ExprProgram prog,
            std::string expr_text, std::vector<SignalRef> refs,
            std::vector<std::string> plains, bool uses_time, BranchId branch)
        : name_(std::move(display)), p_(p), n_(n), is_voltage_(is_voltage),
          prog_(std::move(prog)), expr_text_(std::move(expr_text)), refs_(std::move(refs)),
          plains_(std::move(plains)), uses_time_(uses_time), branch_(branch) {}

    void set_control_branch(BranchId br, std::size_t which) override {
        for (SignalRef& r : refs_) {
            if (r.is_current && r.ctl_ordinal == which) {
                r.branch = br;
            }
        }
    }

    [[nodiscard]] const std::string& name() const override { return name_; }
    [[nodiscard]] char kind() const override { return 'B'; }
    [[nodiscard]] bool has_branch() const override { return is_voltage_; }
    [[nodiscard]] BranchId branch() const override { return branch_; }
    [[nodiscard]] std::vector<NodeId> terminals() const override { return {p_, n_}; }
    [[nodiscard]] std::vector<std::pair<NodeId, NodeId>> constraint_edges()
        const override {
        // A behavioral V is a voltage constraint like V/E (§6.6); an I
        // source never connects its terminals.
        if (is_voltage_) {
            return {{p_, n_}};
        }
        return {};
    }

    // --- parameter model (§3.5a): one slot per distinct plain variable ------
    [[nodiscard]] std::vector<ParamSpec> param_specs() const override {
        std::vector<ParamSpec> out;
        for (const std::string& v : plains_) {
            // A lone variable reference - the resolve_value_program form.
            ExprProgram prog;
            prog.vars_ = {v};
            prog.code_ = {ExprProgram::Instr{ExprProgram::Op::Load, 0, -1}};
            out.push_back(ParamSpec{v, std::move(prog)});
        }
        return out;
    }
    [[nodiscard]] ws::core::Status check_params(const double* par) const override {
        for (std::size_t i = 0; i < plains_.size(); ++i) {
            if (!std::isfinite(par[i])) {
                return spice_fail(dev_diag(
                    ErrorKind::Semantic, "spice.semantic.bsource_param",
                    "behavioral source " + name_ + ": parameter '" + plains_[i] +
                        "' is not a finite number",
                    loc));
            }
        }
        return {};
    }

    // --- state model (§3.5b): ref values + the residual pair ----------------
    [[nodiscard]] std::size_t state_slots() const override { return refs_.size() + 2; }

    // --- stamps --------------------------------------------------------------

    void load_dc(DcLoadContext& cx) const override { load_common(cx, 0.0); }

    void load_tran(DcLoadContext& cx, const TranStep& step) const override {
        load_common(cx, step.time);
    }

    void load_common(DcLoadContext& cx, double time_val) const {
        if (cx.pattern_mode()) {
            stamp_positions(cx);
            return;
        }
        double ref_vals[kMaxRefs];
        for (std::size_t r = 0; r < refs_.size(); ++r) {
            ref_vals[r] = ref_value(cx, refs_[r]);
        }
        const BEval e = evaluate(ref_vals, time_val, cx.params_);
        if (!e.ok) {
            // §8.5: never a NaN into the matrix. The zero fallback keeps
            // every recorded position well-posed; load_error (checked
            // right after this load) fails the pass with the source name.
            stamp_positions(cx);
            return;
        }
        stamp_linearized(cx, e.value, e.partials, ref_vals);
    }

    // The small-signal twin (§8.5): the gradient at the frozen OP. Every
    // position is touched with zero first, then the active path adds on
    // top - the pattern (the dry run) and the numeric pass can never
    // disagree about positions.
    void load_ac(AcLoadContext& cx) const override {
        if (is_voltage_) {
            cx.stamp_branch_voltage(branch_, p_, n_, {0.0, 0.0});
        }
        for (const SignalRef& r : refs_) {
            stamp_ref_entries_ac(cx, r, 0.0);
        }
        // Gradient at the OP (the state slice holds the OP ref values).
        double ref_vals[kMaxRefs];
        for (std::size_t r = 0; r < refs_.size(); ++r) {
            ref_vals[r] = cx.st(r);
        }
        const BEval e = evaluate(ref_vals, 0.0, cx.params_);
        if (!e.ok) {
            // The OP solved clean at these values; the AC re-evaluation
            // cannot fail in practice. Zero contribution, never a NaN.
            return;
        }
        for (std::size_t r = 0; r < refs_.size(); ++r) {
            stamp_ref_entries_ac(cx, refs_[r], e.partials[r]);
        }
    }

    // --- state hooks -----------------------------------------------------------

    // No limiting (§8.5 defines no B-source limiter; the ladder rescues
    // stiff decks). update_state freezes the ref values (for .ac) and
    // tracks f at the load point for the §5.1 residual gate. The step
    // time is constant within one Newton loop, so evaluating the residual
    // at time=0 scales product-form f by a constant factor - the
    // delta/ref ratio the gate compares is invariant.
    [[nodiscard]] bool update_state(const std::vector<double>& x,
                                    std::size_t node_count, double* st,
                                    const std::vector<double>* x_prev = nullptr,
                                    const double* params = nullptr,
                                    double /*temp_k*/ = 300.15) override {
        const std::vector<double>& xl =
            (x_prev != nullptr && !x_prev->empty()) ? *x_prev : x;
        double ref_vals[kMaxRefs];
        for (std::size_t r = 0; r < refs_.size(); ++r) {
            ref_vals[r] = ref_value_at(xl, node_count, refs_[r]);
            st[r] = ref_vals[r];
        }
        const BEval e = evaluate(ref_vals, 0.0, params);
        st[refs_.size() + 1] = st[refs_.size()];
        st[refs_.size()] = e.value;  // a failed eval keeps the last good f
        return false;
    }

    [[nodiscard]] NonlinResid nonlinear_residual(const double* st) const override {
        const std::size_t kF = refs_.size();
        const double cur = st[kF];
        const double prev = st[kF + 1];
        return {std::abs(cur - prev), std::max(std::abs(cur), std::abs(prev))};
    }

    // The §8.5 domain-error channel: re-evaluates the value at the SAME
    // iterate the load just used (only the failing source builds the
    // diagnostic; every other device's default is a constant {}).
    [[nodiscard]] ws::core::Status load_error(const std::vector<double>& x,
                                              const TranStep* step,
                                              const double* params,
                                              std::size_t node_count) const override {
        double ref_vals[kMaxRefs];
        for (std::size_t r = 0; r < refs_.size(); ++r) {
            ref_vals[r] = ref_value_at(x, node_count, refs_[r]);
        }
        const BEval e = evaluate(ref_vals, step ? step->time : 0.0, params);
        if (e.ok) {
            return {};
        }
        std::string when;
        if (step != nullptr) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), " at t = %.6g s", step->time);
            when = buf;
        }
        Diagnostic d = dev_diag(
            ErrorKind::Convergence, "spice.bsource.domain",
            "behavioral source " + name_ + ": '" + std::string(e.fn) +
                "' evaluated outside its domain" + when + " (operand " +
                fmt_operand(e.input) + "; the expression is '" + expr_text_ + "')",
            loc);
        d.names = {name_};
        d.hint = "sqrt/ln/log need non-negative/non-zero arguments; division by"
                 " zero and overflow are errors, not zero - clamp the argument"
                 " (limit/min/max) or restructure the expression";
        return spice_fail(std::move(d));
    }

    // Behavioral sources publish no breakpoints: `time`-dependent edges are
    // stepped over by LTE like any smooth source (ngspice semantics).
    [[nodiscard]] bool write_card(std::string& out, const CardWriter& w) const override {
        if (!plains_.empty()) {
            // The netlist writer flattens to a numeric deck; an expression
            // referencing .param names cannot be numericized without an
            // AST rewrite. Loud gap, never a silently wrong card.
            return false;
        }
        out += name_ + " " + w.node(p_) + " " + w.node(n_) + " " +
               (is_voltage_ ? "V=" : "I=") + expr_text_ + "\n";
        return true;
    }

private:
    struct BEval {
        double value = 0.0;
        double partials[kMaxRefs] = {};
        bool ok = true;
        const char* fn = nullptr;
        double input = 0.0;
    };

    // `params` may be null (contexts without a parameter slice); plain
    // variables then read as 0.
    [[nodiscard]] BEval evaluate(const double* ref_vals, double time_val,
                                 const double* params) const {
        BEval out;
        double vals[kMaxVars];
        const auto& vars = prog_.vars();
        for (std::size_t i = 0; i < vars.size(); ++i) {
            vals[i] = var_value(static_cast<int>(i), ref_vals, time_val, params);
        }
        Dual res;
        DualOutcome st = dual_pass(prog_, vals, -1, res);
        if (!st.ok) {
            out.ok = false;
            out.fn = st.fn;
            out.input = st.input;
            return out;
        }
        out.value = res.v;
        for (std::size_t r = 0; r < refs_.size(); ++r) {
            st = dual_pass(prog_, vals, refs_[r].var_slot, res);
            if (!st.ok) {
                out.ok = false;
                out.fn = st.fn;
                out.input = st.input;
                return out;
            }
            out.partials[r] = res.d;
        }
        return out;
    }

    [[nodiscard]] double var_value(int slot, const double* ref_vals, double time_val,
                                   const double* params) const {
        if (uses_time_ && prog_.vars()[static_cast<std::size_t>(slot)] == "time") {
            return time_val;
        }
        for (std::size_t r = 0; r < refs_.size(); ++r) {
            if (refs_[r].var_slot == slot) {
                return ref_vals[r];
            }
        }
        for (std::size_t i = 0; i < plains_.size(); ++i) {
            if (plains_[i] == prog_.vars()[static_cast<std::size_t>(slot)]) {
                return params != nullptr ? params[i] : 0.0;
            }
        }
        return 0.0;
    }

    [[nodiscard]] double ref_value(const DcLoadContext& cx, const SignalRef& r) const {
        if (r.is_current) {
            return cx.branch_i(r.branch);
        }
        return cx.node_v(r.pos) - cx.node_v(r.neg);
    }

    [[nodiscard]] static double ref_value_at(const std::vector<double>& x,
                                             std::size_t node_count,
                                             const SignalRef& r) {
        if (r.is_current) {
            return x[node_count + r.branch];
        }
        const double vp = (r.pos == kGround) ? 0.0 : x[r.pos];
        const double vn = (r.neg == kGround) ? 0.0 : x[r.neg];
        return vp - vn;
    }

    // Every matrix position a numeric pass could ever touch, zero-valued:
    // the pattern dry run and the domain-error fallback both land here.
    void stamp_positions(DcLoadContext& cx) const {
        if (is_voltage_) {
            cx.stamp_branch_voltage(branch_, p_, n_, 0.0);
        }
        for (const SignalRef& r : refs_) {
            stamp_ref_entries(cx, r, 0.0);
        }
    }

    // The Norton linearization (§8.5): the Jacobian part into the matrix,
    // f0 - sum(df/dx_r * x_r0) into the rhs.
    void stamp_linearized(DcLoadContext& cx, double value, const double* partials,
                          const double* ref_vals) const {
        double offset = value;
        for (std::size_t r = 0; r < refs_.size(); ++r) {
            offset -= partials[r] * ref_vals[r];
            stamp_ref_entries(cx, refs_[r], partials[r]);
        }
        if (is_voltage_) {
            // Branch row: v(p) - v(n) - sum(dr*x_r) = offset.
            cx.stamp_branch_voltage(branch_, p_, n_, offset);
        } else {
            // Current f (p -> n through the source), ngspice I convention:
            // injected into n_, pulled from p_.
            cx.add_current(n_, p_, offset);
        }
    }

    // The Jacobian contribution of one reference: df/dx_r = dr enters
    // the LHS as a LEAVING current at p_ (the source draws f from p_) and
    // an arriving current at n_ - the VCCS stamp's convention
    // (v(a,b) contributes +dr at a, -dr at b; i(Vsrc) +dr at the branch
    // row). A V-type B moves f's dependence into its branch row instead.
    // Ground refs touch nothing.
    void stamp_ref_entries(DcLoadContext& cx, const SignalRef& r, double dr) const {
        if (r.is_current) {
            const std::size_t col = cx.branch_row(r.branch);
            if (is_voltage_) {
                cx.add_entry(cx.branch_row(branch_), col, -dr);
            } else {
                cx.add_entry(p_, col, dr);
                cx.add_entry(n_, col, -dr);
            }
            return;
        }
        const auto touch = [&](NodeId node, double sign) {
            if (node == kGround) {
                return;
            }
            if (is_voltage_) {
                cx.add_entry(cx.branch_row(branch_), node, -sign * dr);
            } else {
                cx.add_entry(p_, node, sign * dr);
                cx.add_entry(n_, node, -sign * dr);
            }
        };
        touch(r.pos, +1.0);
        touch(r.neg, -1.0);
    }

    void stamp_ref_entries_ac(AcLoadContext& cx, const SignalRef& r, double dr) const {
        const std::complex<double> z{dr, 0.0};
        if (r.is_current) {
            const std::size_t col = cx.branch_row(r.branch);
            if (is_voltage_) {
                cx.add_entry(cx.branch_row(branch_), col, -z);
            } else {
                cx.add_entry(p_, col, z);
                cx.add_entry(n_, col, -z);
            }
            return;
        }
        const auto touch = [&](NodeId node, double sign) {
            if (node == kGround) {
                return;
            }
            if (is_voltage_) {
                cx.add_entry(cx.branch_row(branch_), node, -sign * z);
            } else {
                cx.add_entry(p_, node, sign * z);
                cx.add_entry(n_, node, -sign * z);
            }
        };
        touch(r.pos, +1.0);
        touch(r.neg, -1.0);
    }

    [[nodiscard]] static std::string fmt_operand(double v) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.6g", v);
        return buf;
    }

    std::string name_;
    NodeId p_, n_;
    bool is_voltage_;
    ExprProgram prog_;
    std::string expr_text_;
    std::vector<SignalRef> refs_;
    std::vector<std::string> plains_;
    bool uses_time_;
    BranchId branch_;
};

// Splits the inside of "v(a,b)" / "i(v1)" into its comma-separated args.
std::vector<std::string> ref_args(const std::string& var) {
    const std::size_t open = var.find('(');
    const std::size_t close = var.rfind(')');
    std::vector<std::string> out;
    if (open == std::string::npos || close == std::string::npos || close <= open) {
        return out;
    }
    std::size_t start = open + 1;
    for (std::size_t i = open + 1; i <= close; ++i) {
        if (i == close || var[i] == ',') {
            if (i > start) {
                out.push_back(var.substr(start, i - start));
            }
            start = i + 1;
        }
    }
    return out;
}

}  // namespace

ws::core::Result<std::unique_ptr<Device>> make_bsource(const DeviceCard& card,
                                                       FactoryServices& s) {
    WS_TRY_ASSIGN(const std::vector<NodeId> n, take_nodes(card, s.nodes, 2, 0));
    if (card.rest.size() < 3) {
        return spice_fail(dev_diag(
            ErrorKind::Semantic, "spice.semantic.bsource_form",
            "behavioral source " + card.name.text +
                " needs V= or I= and an expression (B name n+ n- V=3*v(a))",
            card.loc));
    }
    // The spec starts at the first token after the nodes: "V=expr" /
    // "I=expr" (glued) or the bare key followed by the expression tokens;
    // expressions may contain spaces and run to the end of the card.
    const std::string first = to_lower(card.rest[2].text);
    bool is_voltage = false;
    bool found = false;
    std::string expr_text;
    if (first.rfind("v=", 0) == 0 || first.rfind("i=", 0) == 0) {
        is_voltage = (first[0] == 'v');
        found = true;
        expr_text = card.rest[2].text.substr(2);
    } else if (first == "v" || first == "i") {
        is_voltage = (first[0] == 'v');
        found = true;
    }
    if (!found) {
        return spice_fail(dev_diag(
            ErrorKind::Semantic, "spice.semantic.bsource_form",
            "behavioral source " + card.name.text + ": expected V= or I=, got '" +
                card.rest[2].text + "'",
            card.loc));
    }
    for (std::size_t i = 3; i < card.rest.size(); ++i) {
        if (!expr_text.empty()) {
            expr_text += " ";
        }
        expr_text += card.rest[i].text;
    }
    while (!expr_text.empty() &&
           std::isspace(static_cast<unsigned char>(expr_text.front()))) {
        expr_text.erase(expr_text.begin());
    }
    while (!expr_text.empty() &&
           std::isspace(static_cast<unsigned char>(expr_text.back()))) {
        expr_text.pop_back();
    }
    if (expr_text.empty()) {
        return spice_fail(dev_diag(
            ErrorKind::Semantic, "spice.semantic.bsource_form",
            "behavioral source " + card.name.text + " has an empty expression",
            card.loc));
    }

    const ws::core::Result<ExprProgram> prog = parse_expression(expr_text);
    if (!prog) {
        Diagnostic d = dev_diag(ErrorKind::Parse, "spice.parse.expr",
                                prog.error().message, card.loc);
        return spice_fail(std::move(d));
    }

    // Monte-Carlo draws are .param-only (§8.5 rev): they bind once per run
    // with a recorded seed, not per Newton iteration.
    for (const auto& in : prog->code()) {
        if (in.op == ExprProgram::Op::Gauss || in.op == ExprProgram::Op::Flat) {
            return spice_fail(dev_diag(
                ErrorKind::Semantic, "spice.semantic.bsource_draw",
                "behavioral source " + card.name.text +
                    ": gauss()/flat() draws are .param-only (fold the draw"
                    " into a .param and reference the name)",
                card.loc));
        }
    }
    if (prog->vars().size() > kMaxVars) {
        return spice_fail(dev_diag(
            ErrorKind::Semantic, "spice.semantic.bsource_form",
            "behavioral source " + card.name.text + ": expression references " +
                std::to_string(prog->vars().size()) + " variables (the cap is " +
                std::to_string(kMaxVars) + ")",
            card.loc));
    }
    if (prog->code().size() > kMaxStack) {
        return spice_fail(dev_diag(
            ErrorKind::Semantic, "spice.semantic.bsource_form",
            "behavioral source " + card.name.text + ": expression needs a " +
                std::to_string(prog->code().size()) + "-deep evaluation stack (the cap is " +
                std::to_string(kMaxStack) + ")",
            card.loc));
    }

    // Classify the variable slots: signal refs (resolved now for v(), by
    // control request for i()), plain names (parameter slots), time. The
    // placeholder object exists so ControlRequest can name the final
    // pointer; the real construction below overwrites it in place.
    auto dev = std::make_unique<Bsource>(card.name.text, n[0], n[1], is_voltage, *prog,
                                         expr_text, std::vector<SignalRef>{},
                                         std::vector<std::string>{}, false, 0);
    std::vector<SignalRef> refs;
    std::vector<std::string> plains;
    bool uses_time = false;
    std::size_t current_ordinal = 0;
    for (std::size_t slot = 0; slot < prog->vars().size(); ++slot) {
        const std::string& var = prog->vars()[slot];
        if (var == "time") {
            uses_time = true;
            continue;
        }
        if (var.rfind("v(", 0) == 0) {
            const std::vector<std::string> args = ref_args(var);
            if (args.empty() || args.size() > 2) {
                return spice_fail(dev_diag(
                    ErrorKind::Semantic, "spice.semantic.bsource_ref",
                    "behavioral source " + card.name.text +
                        ": v() takes one node or two (v(a,b) is the differential)",
                    card.loc));
            }
            SignalRef r;
            r.var_slot = static_cast<int>(slot);
            r.pos = s.nodes.node(Token{args[0], card.loc});
            if (args.size() == 2) {
                r.neg = s.nodes.node(Token{args[1], card.loc});
            }
            refs.push_back(r);
            continue;
        }
        if (var.rfind("i(", 0) == 0) {
            const std::vector<std::string> args = ref_args(var);
            if (args.size() != 1) {
                return spice_fail(dev_diag(
                    ErrorKind::Semantic, "spice.semantic.bsource_ref",
                    "behavioral source " + card.name.text +
                        ": i() takes one voltage-source name",
                    card.loc));
            }
            SignalRef r;
            r.var_slot = static_cast<int>(slot);
            r.is_current = true;
            r.ctl_ordinal = current_ordinal++;
            refs.push_back(r);
            s.controls.push_back(
                ControlRequest{dev.get(), Token{args[0], card.loc}, r.ctl_ordinal});
            continue;
        }
        if (var.rfind("@", 0) == 0) {
            return spice_fail(dev_diag(
                ErrorKind::Semantic, "spice.semantic.bsource_ref",
                "behavioral source " + card.name.text + ": @dev[param] is a"
                " .save/.print form; behavioral expressions read v(), i()"
                " and .param names",
                card.loc));
        }
        // A plain name: bound per run against the .param scope (an
        // unbound name fails loudly at bind time).
        if (std::find(plains.begin(), plains.end(), var) == plains.end()) {
            plains.push_back(var);
        }
    }
    if (refs.size() > kMaxRefs) {
        return spice_fail(dev_diag(
            ErrorKind::Semantic, "spice.semantic.bsource_form",
            "behavioral source " + card.name.text + ": expression references " +
                std::to_string(refs.size()) + " signals (the cap is " +
                std::to_string(kMaxRefs) + ")",
            card.loc));
    }

    const BranchId branch = is_voltage ? s.nodes.claim_branch() : 0;
    *dev = Bsource(card.name.text, n[0], n[1], is_voltage, *prog, expr_text,
                   std::move(refs), std::move(plains), uses_time, branch);
    dev->loc = card.loc;
    return dev;
}

}  // namespace ws::spice
