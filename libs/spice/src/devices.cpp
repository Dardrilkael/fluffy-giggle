// M1 devices (DESIGN.md §3/§11): R, V, I, E, F, G, H - all linear, all
// stateless. Each factory resolves its own terminals and parameters
// (arity belongs to the device, not the elaborator); F/H register control
// requests that the elaborator resolves to branch ids once every V source
// has claimed one. M2's C/L/K/S/W live in devices_m2.cpp.
//
// 2026-10-29 (§3.5 rev): every card VALUE is captured as a parameter
// source (an ExprProgram - a constant for plain numbers, the parsed
// expression for {...}, a lone variable reference for bare .param names)
// instead of a resolved double. The double is still evaluated once at
// elaboration for validation; the program re-evaluates per run against
// the .param scope, which is what makes .step / Monte-Carlo / handle
// writes (Simulator::set) value-only changes - the stamp pattern cannot
// move.

#include "cards.hpp"
#include "waveform.hpp"

#include <cmath>
#include <complex>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <ws/spice/deck.hpp>
#include <ws/spice/device.hpp>
#include <ws/spice/state.hpp>

namespace ws::spice {

// resolve_value_token needs the full ParamTable (deck.hpp included above).
ws::core::Result<double> resolve_value_token(const Token& tok, const ParamTable& params) {
    // '{expression}' - the LTspice form (§8.3).
    if (tok.text.size() >= 2 && tok.text.front() == '{' && tok.text.back() == '}') {
        const ws::core::Result<ExprProgram> prog =
            parse_expression(tok.text.substr(1, tok.text.size() - 2));
        if (!prog) {
            Diagnostic d;
            d.kind = ErrorKind::Parse;
            d.code = "spice.parse.expr";
            d.message = prog.error().message;
            d.loc = tok.loc;
            return spice_fail(std::move(d));
        }
        WS_TRY_ASSIGN(const std::vector<std::optional<double>> bindings, params.bind(*prog));
        return prog->evaluate(bindings);
    }
    // Plain SPICE number.
    const ws::core::Result<double> num = parse_number(tok.text);
    if (num) {
        return num;
    }
    // Bare .param name (the ngspice form).
    if (params.knows(to_lower(tok.text))) {
        return params.value(to_lower(tok.text));
    }
    Diagnostic d;
    d.kind = ErrorKind::Parse;
    d.code = "spice.parse.value";
    d.message = "expected a number, {expression} or parameter name, got '" + tok.text + "'";
    d.loc = tok.loc;
    return spice_fail(std::move(d));
}

// The same token as a parameter SOURCE: numbers become constant programs,
// {expr} parses, bare names become lone variable references. This is the
// per-run rebindable form of every device value.
ws::core::Result<ExprProgram> resolve_value_program(const Token& tok,
                                                    const ParamTable& params) {
    auto bad = [&](std::string message) {
        Diagnostic d;
        d.kind = ErrorKind::Parse;
        d.code = "spice.parse.value";
        d.message = std::move(message);
        d.loc = tok.loc;
        return ws::core::unexpected(ws::spice::make_error(std::move(d)));
    };
    if (tok.text.size() >= 2 && tok.text.front() == '{' && tok.text.back() == '}') {
        const ws::core::Result<ExprProgram> prog =
            parse_expression(tok.text.substr(1, tok.text.size() - 2));
        if (!prog) {
            return bad(prog.error().message);
        }
        return prog;
    }
    if (const ws::core::Result<double> num = parse_number(tok.text)) {
        return ExprProgram::constant(*num);
    }
    const std::string lower = to_lower(tok.text);
    if (params.knows(lower)) {
        // A lone variable reference: Load <slot> with vars_ = {name}. The
        // program is assembled directly - parse_expression would need a
        // source string, and the variable is already validated to exist.
        ExprProgram p;
        p.vars_ = {lower};
        p.code_ = {ExprProgram::Instr{ExprProgram::Op::Load, 0, -1}};
        return p;
    }
    return bad("expected a number, {expression} or parameter name, got '" + tok.text +
               "'");
}

namespace {

Diagnostic dev_diag(ErrorKind kind, std::string code, std::string message, SourceLoc loc) {
    Diagnostic d;
    d.kind = kind;
    d.code = std::move(code);
    d.message = std::move(message);
    d.loc = std::move(loc);
    return d;
}

}  // namespace

// Evaluates a value program once, for elaboration-time validation only
// (a gauss() draw here is dummy-seeded; the real draws happen at rebind).
// External linkage: devices_m2.cpp's factories share it through cards.hpp.
ws::core::Result<double> eval_param_for_validation(const ExprProgram& prog,
                                                   const ParamTable& params) {
    // A dummy-seeded rng serves both the direct draw and any gauss() in
    // the referenced .param chain - validation only cares that things
    // RESOLVE; the real draws happen per run at bind time.
    Rand dummy{0};
    WS_TRY_ASSIGN(const std::vector<std::optional<double>> bindings,
                  params.bind(prog, &dummy));
    return prog.evaluate(bindings, &dummy);
}

// Resolves exactly `count` node tokens starting at rest[base] - the
// common arity pre-check every terminal-taking factory starts with.
// (Namespace scope: devices_m2.cpp shares it through cards.hpp.)
ws::core::Result<std::vector<NodeId>> take_nodes(const DeviceCard& card,
                                                  NodeResolver& nodes, std::size_t count,
                                                  std::size_t base) {
    if (card.rest.size() < base + count) {
        return spice_fail(dev_diag(
            ErrorKind::Semantic, "spice.semantic.arity",
            "device " + card.name.text + " expects " + std::to_string(count) +
                " nodes", card.loc));
    }
    std::vector<NodeId> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(nodes.node(card.rest[base + i]));
    }
    return out;
}

namespace {

// --- R -------------------------------------------------------------------------

class Resistor final : public Device {
public:
    Resistor(std::string display, NodeId a, NodeId b, ExprProgram ohms)
        : name_(std::move(display)), a_(a), b_(b), ohms_(std::move(ohms)) {}

    [[nodiscard]] const std::string& name() const override { return name_; }
    [[nodiscard]] char kind() const override { return 'R'; }
    [[nodiscard]] std::vector<NodeId> terminals() const override { return {a_, b_}; }
    [[nodiscard]] std::vector<std::pair<NodeId, NodeId>> conductance_edges()
        const override {
        return {{a_, b_}};
    }

    [[nodiscard]] std::vector<ParamSpec> param_specs() const override {
        return {ParamSpec{"value", ohms_}};
    }
    [[nodiscard]] ws::core::Status check_params(const double* p) const override {
        if (!(p[0] > 0.0) || !std::isfinite(p[0])) {
            return spice_fail(dev_diag(
                ErrorKind::Semantic, "spice.semantic.r_value",
                "resistor " + name_ + " needs a positive finite value", loc));
        }
        return {};
    }

    void load_dc(DcLoadContext& cx) const override {
        cx.add_conductance(a_, b_, 1.0 / cx.par(0));
    }
    void load_ac(AcLoadContext& cx) const override {
        cx.add_conductance(a_, b_, 1.0 / cx.par(0));
    }
    [[nodiscard]] bool write_card(std::string& out,
                                    const CardWriter& w) const override {
        out += name_ + " " + w.node(a_) + " " + w.node(b_) + " " +
               Device::CardWriter::num(w.params(*this)[0]) + "\n";
        return true;
    }

private:
    std::string name_;
    NodeId a_, b_;
    ExprProgram ohms_;
};

ws::core::Result<std::unique_ptr<Device>> make_resistor(const DeviceCard& card,
                                                        FactoryServices& s) {
    WS_TRY_ASSIGN(const std::vector<NodeId> n, take_nodes(card, s.nodes, 2, 0));
    if (card.rest.size() < 3) {
        return spice_fail(dev_diag(ErrorKind::Semantic, "spice.semantic.arity",
                                   "device " + card.name.text + " needs a value",
                                   card.loc));
    }
    // R name a b value | modelname [w= l= ...]
    WS_TRY_ASSIGN(ExprProgram ohms, resolve_value_program(card.rest[2], s.params));
    WS_TRY(eval_param_for_validation(ohms, s.params));
    if (card.rest.size() > 3) {
        return spice_fail(dev_diag(
            ErrorKind::Semantic, "spice.semantic.r_extra",
            "resistor " + card.name.text +
                " takes a plain value in v1 (models/geometry land with M5)",
            card.loc));
    }
    auto dev = std::make_unique<Resistor>(card.name.text, n[0], n[1], std::move(ohms));
    dev->loc = card.loc;
    return dev;
}

// --- V / I: DC value, optional AC spec, optional waveform (§6.2/§7.4) ---------

// The source card grammar after the two nodes:
//   [DC] value [AC mag [phase]] [PULSE(...)|SIN(...)|EXP(...)|PWL(...)]
// The waveform may span several tokens ("PULSE(" "0" "1" ")"); the first
// DC-position value may also be the waveform itself.
struct SourceSpec {
    ExprProgram dc;                 // the rebindable DC value (§3.5a)
    bool have_dc = false;
    std::optional<double> ac_mag;
    std::optional<double> ac_phase;  // degrees
    std::optional<Waveform> wave;
};

[[nodiscard]] ws::core::Status parse_source_spec(const DeviceCard& card,
                                                 const ParamTable& params,
                                                 SourceSpec& out) {
    std::size_t i = 2;  // after the two nodes
    while (i < card.rest.size()) {
        const std::string lower = to_lower(card.rest[i].text);
        if (lower == "dc") {
            if (i + 1 >= card.rest.size() || out.have_dc) {
                return spice_fail(dev_diag(
                    ErrorKind::Semantic, "spice.semantic.source_form",
                    "source " + card.name.text + ": 'DC' needs a value (once)",
                    card.loc));
            }
            WS_TRY_ASSIGN(out.dc, resolve_value_program(card.rest[i + 1], params));
            WS_TRY(eval_param_for_validation(out.dc, params));
            out.have_dc = true;
            i += 2;
            continue;
        }
        if (lower == "ac") {
            if (i + 1 >= card.rest.size() || out.ac_mag) {
                return spice_fail(dev_diag(
                    ErrorKind::Semantic, "spice.semantic.source_form",
                    "source " + card.name.text + ": 'AC' needs a magnitude (once)",
                    card.loc));
            }
            WS_TRY_ASSIGN(out.ac_mag, resolve_value_token(card.rest[i + 1], params));
            i += 2;
            // Optional phase in degrees: consumed only when it actually
            // resolves as a value (it must not eat a waveform token).
            if (i < card.rest.size()) {
                const ws::core::Result<double> ph =
                    resolve_value_token(card.rest[i], params);
                if (ph && !is_waveform_start(to_lower(card.rest[i].text))) {
                    out.ac_phase = *ph;
                    ++i;
                }
            }
            continue;
        }
        if (is_waveform_start(lower)) {
            if (out.wave) {
                return spice_fail(dev_diag(
                    ErrorKind::Semantic, "spice.semantic.source_form",
                    "source " + card.name.text + " carries two waveforms",
                    card.loc));
            }
            std::vector<Token> span(card.rest.begin() + static_cast<std::ptrdiff_t>(i),
                                    card.rest.end());
            WS_TRY_ASSIGN(out.wave, parse_waveform(span, params));
            // The span ran to the end of the card (the closing ')' ends it).
            i = card.rest.size();
            continue;
        }
        if (!out.have_dc) {
            WS_TRY_ASSIGN(out.dc, resolve_value_program(card.rest[i], params));
            WS_TRY(eval_param_for_validation(out.dc, params));
            out.have_dc = true;
            ++i;
            continue;
        }
        return spice_fail(dev_diag(
            ErrorKind::Semantic, "spice.semantic.source_form",
            "source " + card.name.text + ": unexpected '" + card.rest[i].text + "'",
            card.loc));
    }
    // ngspice: a waveform source without an explicit DC value uses the
    // waveform's value at t=0 for DC/OP solves (value(0) never depends on
    // the deferred tstep/tstop defaults). The DC PARAMETER stays a
    // constant zero here; load paths that need waveform(0) consult the
    // waveform first, exactly as before.
    if (!out.have_dc) {
        out.dc = ExprProgram::constant(out.wave ? out.wave->value(0.0) : 0.0);
        out.have_dc = true;
    }
    return {};
}

// The shared source body. Parameters: [0] = dc (scoped), [1] = force - a
// nonzero force pins the DC value OVER the waveform during .tran (what
// Simulator::set_source writes mid-run for closed-loop use); without it
// the waveform owns the transient, ngspice-style. The AC phasor and the
// waveform itself are immutable construction data.
class SourceCommon {
public:
    // slot 0 is the dc value; slot 1 the force flag
    static constexpr std::size_t kParDc = 0;
    static constexpr std::size_t kParForce = 1;

    [[nodiscard]] double tran_value(const DcLoadContext& cx, const TranStep& step) const {
        if (wave_ && cx.par(kParForce) == 0.0) {
            return wave_->value(step.time, step.tstep, step.tstop);
        }
        return cx.par(kParDc);
    }
    [[nodiscard]] double next_event(double t, double tstep, double tstop) const {
        return wave_ ? wave_->next_event(t, tstep, tstop)
                     : std::numeric_limits<double>::infinity();
    }
    [[nodiscard]] bool has_ac() const { return ac_mag_.has_value(); }
    [[nodiscard]] std::complex<double> ac_phasor() const {
        if (!ac_mag_) {
            return {0.0, 0.0};
        }
        const double phase_rad = (ac_phase_.value_or(0.0)) * 3.14159265358979323846 / 180.0;
        return std::polar(*ac_mag_, phase_rad);
    }

    [[nodiscard]] std::vector<ParamSpec> source_param_specs(const ExprProgram& dc) const {
        return {ParamSpec{"dc", dc}, ParamSpec{"force", ExprProgram::constant(0.0)}};
    }

    // The shared card tail: explicit DC value, then the waveform, then
    // the AC spec. The DC is always spelled: a waveform-only card derives
    // its DC from waveform(0) at parse time, so omitting it would not
    // round-trip a source whose deck DC differs.
    void write_source_tail(std::string& out, const Device::CardWriter& w,
                           const Device& self) const {
        const double* p = w.params(self);
        out += "DC " + Device::Device::CardWriter::num(p[0]);
        if (wave_) {
            out += std::string(" ") + wave_->name() + "(";
            if (wave_->kind == Waveform::Kind::Pwl) {
                for (std::size_t i = 0; i < wave_->pwl.size(); ++i) {
                    if (i != 0) out += " ";
                    out += Device::CardWriter::num(wave_->pwl[i].first) + " " +
                           Device::CardWriter::num(wave_->pwl[i].second);
                }
            } else {
                const int count = (wave_->kind == Waveform::Kind::Pulse) ? 7
                                  : (wave_->kind == Waveform::Kind::Sin)  ? 6
                                                                          : 6;
                for (int i = 0; i < count; ++i) {
                    if (i != 0) out += " ";
                    out += Device::CardWriter::num(wave_->p[i]);
                }
            }
            out += ")";
        }
        if (ac_mag_) {
            out += " AC " + Device::CardWriter::num(*ac_mag_);
            if (ac_phase_ && *ac_phase_ != 0.0) {
                out += " " + Device::CardWriter::num(*ac_phase_);
            }
        }
    }

    [[nodiscard]] ws::core::Status source_check_params(const double* p,
                                                       SourceLoc loc) const {
        if (!std::isfinite(p[0])) {
            return spice_fail(dev_diag(ErrorKind::Semantic, "spice.semantic.source_form",
                                       "source " + name_ + " needs a finite DC value",
                                       loc));
        }
        return {};
    }

protected:
    SourceCommon(std::string name, const SourceSpec& spec)
        : name_(std::move(name)), ac_mag_(spec.ac_mag), ac_phase_(spec.ac_phase),
          wave_(spec.wave) {}

    std::string name_;
    std::optional<double> ac_mag_;
    std::optional<double> ac_phase_;
    std::optional<Waveform> wave_;
};

class VoltageSource final : public Device, private SourceCommon {
public:
    VoltageSource(std::string display, NodeId p, NodeId n, BranchId branch,
                  const SourceSpec& spec, SourceLoc sl)
        : Device(), SourceCommon(std::move(display), spec), p_(p), n_(n),
          branch_(branch), dc_(spec.dc) {
        this->loc = sl;
    }

    [[nodiscard]] const std::string& name() const override { return name_; }
    [[nodiscard]] char kind() const override { return 'V'; }
    [[nodiscard]] bool has_branch() const override { return true; }
    [[nodiscard]] BranchId branch() const override { return branch_; }
    [[nodiscard]] std::vector<NodeId> terminals() const override { return {p_, n_}; }
    [[nodiscard]] std::vector<std::pair<NodeId, NodeId>> constraint_edges()
        const override {
        return {{p_, n_}};
    }

    [[nodiscard]] std::vector<ParamSpec> param_specs() const override {
        return source_param_specs(dc_);
    }
    [[nodiscard]] ws::core::Status check_params(const double* p) const override {
        return source_check_params(p, this->loc);
    }

    void load_dc(DcLoadContext& cx) const override {
        cx.stamp_branch_voltage(branch_, p_, n_, cx.par(kParDc));
    }
    void load_tran(DcLoadContext& cx, const TranStep& step) const override {
        cx.stamp_branch_voltage(branch_, p_, n_, tran_value(cx, step));
    }
    void load_ac(AcLoadContext& cx) const override {
        // A V source is a short to the small-signal system; the AC spec (or
        // zero) is the stimulus (§7.4).
        cx.stamp_branch_voltage(branch_, p_, n_, ac_phasor());
    }
    [[nodiscard]] double next_event(double t, double tstep,
                                     double tstop) const override {
        return SourceCommon::next_event(t, tstep, tstop);
    }
    [[nodiscard]] bool has_ac() const override { return SourceCommon::has_ac(); }
    [[nodiscard]] bool write_card(std::string& out,
                                    const CardWriter& w) const override {
        out += name_ + " " + w.node(p_) + " " + w.node(n_) + " ";
        write_source_tail(out, w, *this);
        out += "\n";
        return true;
    }

private:
    NodeId p_, n_;
    BranchId branch_;
    ExprProgram dc_;
};

class CurrentSource final : public Device, private SourceCommon {
public:
    CurrentSource(std::string display, NodeId p, NodeId n, const SourceSpec& spec,
                  SourceLoc sl)
        : Device(), SourceCommon(std::move(display), spec), p_(p), n_(n),
          dc_(spec.dc) {
        this->loc = sl;
    }

    [[nodiscard]] const std::string& name() const override { return name_; }
    [[nodiscard]] char kind() const override { return 'I'; }
    [[nodiscard]] std::vector<NodeId> terminals() const override { return {p_, n_}; }

    [[nodiscard]] std::vector<ParamSpec> param_specs() const override {
        return source_param_specs(dc_);
    }
    [[nodiscard]] ws::core::Status check_params(const double* p) const override {
        return source_check_params(p, this->loc);
    }

    void load_dc(DcLoadContext& cx) const override {
        // ngspice: positive current flows from N+ through the source to
        // N- - it LEAVES p_ and ENTERS n_ ("I1 0 out 1m" charges 'out'
        // positive, the familiar orientation).
        cx.add_current(n_, p_, cx.par(kParDc));
    }
    void load_tran(DcLoadContext& cx, const TranStep& step) const override {
        cx.add_current(n_, p_, tran_value(cx, step));
    }
    void load_ac(AcLoadContext& cx) const override {
        // No AC spec: open to the small-signal system (§7.4).
        if (has_ac()) {
            cx.add_current(n_, p_, ac_phasor());
        }
    }
    [[nodiscard]] double next_event(double t, double tstep,
                                     double tstop) const override {
        return SourceCommon::next_event(t, tstep, tstop);
    }
    [[nodiscard]] bool has_ac() const override { return SourceCommon::has_ac(); }
    [[nodiscard]] bool write_card(std::string& out,
                                    const CardWriter& w) const override {
        out += name_ + " " + w.node(p_) + " " + w.node(n_) + " ";
        write_source_tail(out, w, *this);
        out += "\n";
        return true;
    }

private:
    NodeId p_, n_;
    ExprProgram dc_;
};

ws::core::Result<std::unique_ptr<Device>> make_vsource(const DeviceCard& card,
                                                       FactoryServices& s) {
    WS_TRY_ASSIGN(const std::vector<NodeId> n, take_nodes(card, s.nodes, 2, 0));
    SourceSpec spec;
    WS_TRY(parse_source_spec(card, s.params, spec));
    return std::make_unique<VoltageSource>(card.name.text, n[0], n[1],
                                           s.nodes.claim_branch(), spec, card.loc);
}

ws::core::Result<std::unique_ptr<Device>> make_isource(const DeviceCard& card,
                                                       FactoryServices& s) {
    WS_TRY_ASSIGN(const std::vector<NodeId> n, take_nodes(card, s.nodes, 2, 0));
    SourceSpec spec;
    WS_TRY(parse_source_spec(card, s.params, spec));
    return std::make_unique<CurrentSource>(card.name.text, n[0], n[1], spec, card.loc);
}

// --- E (VCVS) --------------------------------------------------------------------

class Vcvs final : public Device {
public:
    Vcvs(std::string display, NodeId p, NodeId n, NodeId cp, NodeId cn, BranchId branch,
         ExprProgram gain, SourceLoc sl)
        : name_(std::move(display)), p_(p), n_(n), cp_(cp), cn_(cn), branch_(branch),
          gain_(std::move(gain)) {
        this->loc = sl;
    }

    [[nodiscard]] const std::string& name() const override { return name_; }
    [[nodiscard]] char kind() const override { return 'E'; }
    [[nodiscard]] bool has_branch() const override { return true; }
    [[nodiscard]] BranchId branch() const override { return branch_; }
    [[nodiscard]] std::vector<NodeId> terminals() const override {
        return {p_, n_, cp_, cn_};
    }
    [[nodiscard]] std::vector<std::pair<NodeId, NodeId>> constraint_edges()
        const override {
        return {{p_, n_}};  // output only; the control pair senses
    }

    [[nodiscard]] std::vector<ParamSpec> param_specs() const override {
        return {ParamSpec{"gain", gain_}};
    }
    [[nodiscard]] ws::core::Status check_params(const double* p) const override {
        if (!std::isfinite(p[0])) {
            return spice_fail(dev_diag(ErrorKind::Semantic, "spice.semantic.arity",
                                       "device " + name_ + " needs a finite gain", loc));
        }
        return {};
    }

    void load_dc(DcLoadContext& cx) const override {
        // v(p) - v(n) = gain * (v(cp) - v(cn)), branch current i flows p->n
        // through the source.
        const std::size_t row = cx.branch_row(branch_);
        cx.add_entry(row, p_, 1.0);
        cx.add_entry(row, n_, -1.0);
        cx.add_entry(row, cp_, -cx.par(0));
        cx.add_entry(row, cn_, cx.par(0));
        cx.add_entry(p_, row, 1.0);
        cx.add_entry(n_, row, -1.0);
    }
    void load_ac(AcLoadContext& cx) const override {
        const std::size_t row = cx.branch_row(branch_);
        cx.add_entry(row, p_, 1.0);
        cx.add_entry(row, n_, -1.0);
        cx.add_entry(row, cp_, -cx.par(0));
        cx.add_entry(row, cn_, cx.par(0));
        cx.add_entry(p_, row, 1.0);
        cx.add_entry(n_, row, -1.0);
    }
    [[nodiscard]] bool write_card(std::string& out,
                                    const CardWriter& w) const override {
        out += name_ + " " + w.node(p_) + " " + w.node(n_) + " " + w.node(cp_) + " " +
               w.node(cn_) + " " + Device::CardWriter::num(w.params(*this)[0]) + "\n";
        return true;
    }

private:
    std::string name_;
    NodeId p_, n_, cp_, cn_;
    BranchId branch_;
    ExprProgram gain_;
};

ws::core::Result<std::unique_ptr<Device>> make_vcvs(const DeviceCard& card,
                                                    FactoryServices& s) {
    WS_TRY_ASSIGN(const std::vector<NodeId> n, take_nodes(card, s.nodes, 4, 0));
    if (card.rest.size() != 5) {
        return spice_fail(dev_diag(ErrorKind::Semantic, "spice.semantic.arity",
                                   "device " + card.name.text +
                                       " takes four nodes and a gain",
                                   card.loc));
    }
    WS_TRY_ASSIGN(ExprProgram gain, resolve_value_program(card.rest[4], s.params));
    WS_TRY(eval_param_for_validation(gain, s.params));
    return std::make_unique<Vcvs>(card.name.text, n[0], n[1], n[2], n[3],
                                  s.nodes.claim_branch(), std::move(gain), card.loc);
}

// --- G (VCCS) --------------------------------------------------------------------

class Vccs final : public Device {
public:
    Vccs(std::string display, NodeId p, NodeId n, NodeId cp, NodeId cn, ExprProgram gm,
         SourceLoc sl)
        : name_(std::move(display)), p_(p), n_(n), cp_(cp), cn_(cn), gm_(std::move(gm)) {
        this->loc = sl;
    }

    [[nodiscard]] const std::string& name() const override { return name_; }
    [[nodiscard]] char kind() const override { return 'G'; }
    [[nodiscard]] std::vector<NodeId> terminals() const override {
        return {p_, n_, cp_, cn_};
    }

    [[nodiscard]] std::vector<ParamSpec> param_specs() const override {
        return {ParamSpec{"gm", gm_}};
    }
    [[nodiscard]] ws::core::Status check_params(const double* p) const override {
        if (!std::isfinite(p[0])) {
            return spice_fail(dev_diag(ErrorKind::Semantic, "spice.semantic.arity",
                                       "device " + name_ + " needs a finite gm", loc));
        }
        return {};
    }

    void load_dc(DcLoadContext& cx) const override {
        // i(p->n through the source) = gm * (v(cp) - v(cn))
        cx.add_entry(p_, cp_, cx.par(0));
        cx.add_entry(p_, cn_, -cx.par(0));
        cx.add_entry(n_, cp_, -cx.par(0));
        cx.add_entry(n_, cn_, cx.par(0));
    }
    void load_ac(AcLoadContext& cx) const override {
        cx.add_entry(p_, cp_, cx.par(0));
        cx.add_entry(p_, cn_, -cx.par(0));
        cx.add_entry(n_, cp_, -cx.par(0));
        cx.add_entry(n_, cn_, cx.par(0));
    }
    [[nodiscard]] bool write_card(std::string& out,
                                    const CardWriter& w) const override {
        out += name_ + " " + w.node(p_) + " " + w.node(n_) + " " + w.node(cp_) + " " +
               w.node(cn_) + " " + Device::CardWriter::num(w.params(*this)[0]) + "\n";
        return true;
    }

private:
    std::string name_;
    NodeId p_, n_, cp_, cn_;
    ExprProgram gm_;
};

ws::core::Result<std::unique_ptr<Device>> make_vccs(const DeviceCard& card,
                                                    FactoryServices& s) {
    WS_TRY_ASSIGN(const std::vector<NodeId> n, take_nodes(card, s.nodes, 4, 0));
    if (card.rest.size() != 5) {
        return spice_fail(dev_diag(ErrorKind::Semantic, "spice.semantic.arity",
                                   "device " + card.name.text +
                                       " takes four nodes and a transconductance",
                                   card.loc));
    }
    WS_TRY_ASSIGN(ExprProgram gm, resolve_value_program(card.rest[4], s.params));
    WS_TRY(eval_param_for_validation(gm, s.params));
    return std::make_unique<Vccs>(card.name.text, n[0], n[1], n[2], n[3], std::move(gm),
                                  card.loc);
}

// --- F (CCCS) / H (CCVS): controlled by a named V source's branch current ------

class Cccs final : public Device {
public:
    Cccs(std::string display, NodeId p, NodeId n, ExprProgram gain, SourceLoc sl)
        : name_(std::move(display)), p_(p), n_(n), gain_(std::move(gain)) {
        this->loc = sl;
    }

    void set_control_branch(BranchId br) override { control_ = br; }

    [[nodiscard]] const std::string& name() const override { return name_; }
    [[nodiscard]] char kind() const override { return 'F'; }
    [[nodiscard]] std::vector<NodeId> terminals() const override { return {p_, n_}; }

    [[nodiscard]] std::vector<ParamSpec> param_specs() const override {
        return {ParamSpec{"gain", gain_}};
    }
    [[nodiscard]] ws::core::Status check_params(const double* p) const override {
        if (!std::isfinite(p[0])) {
            return spice_fail(dev_diag(ErrorKind::Semantic, "spice.semantic.arity",
                                       "device " + name_ + " needs a finite gain", loc));
        }
        return {};
    }

    void load_dc(DcLoadContext& cx) const override {
        // i(p->n) = gain * i(control source)
        const std::size_t ctl = cx.branch_row(control_);
        cx.add_entry(p_, ctl, cx.par(0));
        cx.add_entry(n_, ctl, -cx.par(0));
    }
    void load_ac(AcLoadContext& cx) const override {
        const std::size_t ctl = cx.branch_row(control_);
        cx.add_entry(p_, ctl, cx.par(0));
        cx.add_entry(n_, ctl, -cx.par(0));
    }
    [[nodiscard]] bool write_card(std::string& out,
                                    const CardWriter& w) const override {
        out += name_ + " " + w.node(p_) + " " + w.node(n_) + " " +
               w.branch_owner(control_) + " " +
               Device::CardWriter::num(w.params(*this)[0]) + "\n";
        return true;
    }

private:
    std::string name_;
    NodeId p_, n_;
    BranchId control_ = 0;
    ExprProgram gain_;
};

class Ccvs final : public Device {
public:
    Ccvs(std::string display, NodeId p, NodeId n, BranchId branch, ExprProgram r,
         SourceLoc sl)
        : name_(std::move(display)), p_(p), n_(n), branch_(branch), r_(std::move(r)) {
        this->loc = sl;
    }

    void set_control_branch(BranchId br) override { control_ = br; }

    [[nodiscard]] const std::string& name() const override { return name_; }
    [[nodiscard]] char kind() const override { return 'H'; }
    [[nodiscard]] bool has_branch() const override { return true; }
    [[nodiscard]] BranchId branch() const override { return branch_; }
    [[nodiscard]] std::vector<NodeId> terminals() const override { return {p_, n_}; }
    [[nodiscard]] std::vector<std::pair<NodeId, NodeId>> constraint_edges()
        const override {
        return {{p_, n_}};
    }

    [[nodiscard]] std::vector<ParamSpec> param_specs() const override {
        return {ParamSpec{"r", r_}};
    }
    [[nodiscard]] ws::core::Status check_params(const double* p) const override {
        if (!std::isfinite(p[0])) {
            return spice_fail(dev_diag(ErrorKind::Semantic, "spice.semantic.arity",
                                       "device " + name_ + " needs a finite R", loc));
        }
        return {};
    }

    void load_dc(DcLoadContext& cx) const override {
        // v(p) - v(n) = r * i(control source)
        const std::size_t row = cx.branch_row(branch_);
        const std::size_t ctl = cx.branch_row(control_);
        cx.add_entry(row, p_, 1.0);
        cx.add_entry(row, n_, -1.0);
        cx.add_entry(row, ctl, -cx.par(0));
        cx.add_entry(p_, row, 1.0);
        cx.add_entry(n_, row, -1.0);
    }
    void load_ac(AcLoadContext& cx) const override {
        const std::size_t row = cx.branch_row(branch_);
        const std::size_t ctl = cx.branch_row(control_);
        cx.add_entry(row, p_, 1.0);
        cx.add_entry(row, n_, -1.0);
        cx.add_entry(row, ctl, -cx.par(0));
        cx.add_entry(p_, row, 1.0);
        cx.add_entry(n_, row, -1.0);
    }
    [[nodiscard]] bool write_card(std::string& out,
                                    const CardWriter& w) const override {
        out += name_ + " " + w.node(p_) + " " + w.node(n_) + " " +
               w.branch_owner(control_) + " " +
               Device::CardWriter::num(w.params(*this)[0]) + "\n";
        return true;
    }

private:
    std::string name_;
    NodeId p_, n_;
    BranchId branch_;
    BranchId control_ = 0;
    ExprProgram r_;
};

ws::core::Result<std::unique_ptr<Device>> make_cccs(const DeviceCard& card,
                                                    FactoryServices& s) {
    WS_TRY_ASSIGN(const std::vector<NodeId> n, take_nodes(card, s.nodes, 2, 0));
    if (card.rest.size() != 4) {
        return spice_fail(dev_diag(ErrorKind::Semantic, "spice.semantic.arity",
                                   "device " + card.name.text +
                                       " takes two nodes, a controlling V source"
                                       " and a gain",
                                   card.loc));
    }
    WS_TRY_ASSIGN(ExprProgram gain, resolve_value_program(card.rest[3], s.params));
    WS_TRY(eval_param_for_validation(gain, s.params));
    auto dev = std::make_unique<Cccs>(card.name.text, n[0], n[1], std::move(gain),
                                      card.loc);
    s.controls.push_back(ControlRequest{dev.get(), card.rest[2]});
    return dev;
}

ws::core::Result<std::unique_ptr<Device>> make_ccvs(const DeviceCard& card,
                                                    FactoryServices& s) {
    WS_TRY_ASSIGN(const std::vector<NodeId> n, take_nodes(card, s.nodes, 2, 0));
    if (card.rest.size() != 4) {
        return spice_fail(dev_diag(ErrorKind::Semantic, "spice.semantic.arity",
                                   "device " + card.name.text +
                                       " takes two nodes, a controlling V source"
                                       " and a transresistance",
                                   card.loc));
    }
    WS_TRY_ASSIGN(ExprProgram r, resolve_value_program(card.rest[3], s.params));
    WS_TRY(eval_param_for_validation(r, s.params));
    auto dev = std::make_unique<Ccvs>(card.name.text, n[0], n[1], s.nodes.claim_branch(),
                                      std::move(r), card.loc);
    s.controls.push_back(ControlRequest{dev.get(), card.rest[2]});
    return dev;
}

}  // namespace

// The default: only switches consume .model cards today (R models land
// with M5, and the R factory already rejects a model *value* form).
ws::core::Status Device::apply_model(const ModelDef& model) {
    return spice_fail(dev_diag(
        ErrorKind::Semantic, "spice.semantic.model_target",
        "device '" + name() + "' does not take .model '" + model.name.text + "'",
        model.loc));
}

// --- the factory table (§3.4: on the elaborator, never a global) ----------------

const std::map<char, DeviceFactory>& builtin_device_factories() {
    static const std::map<char, DeviceFactory> table = {
        {'R', make_resistor},  {'V', make_vsource},   {'I', make_isource},
        {'E', make_vcvs},      {'G', make_vccs},      {'F', make_cccs},
        {'H', make_ccvs},      {'C', make_capacitor}, {'L', make_inductor},
        {'K', make_coupling},  {'S', make_vswitch},   {'W', make_iswitch},
        {'D', make_diode},     {'Q', make_bjt},     {'M', make_mosfet},
    };
    return table;
}

}  // namespace ws::spice
