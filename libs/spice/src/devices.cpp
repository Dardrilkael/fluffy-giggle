// M1 devices (DESIGN.md §3/§11): R, V, I, E, F, G, H - all linear, all
// stateless. Each factory resolves its own terminals and parameters
// (arity belongs to the device, not the elaborator); F/H register control
// requests that the elaborator resolves to branch ids once every V source
// has claimed one. M2 grows the waveform/AC source specs here; C/L/K/S/W
// live in devices_m2.cpp.

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
    Resistor(std::string display, NodeId a, NodeId b, double ohms)
        : name_(std::move(display)), a_(a), b_(b), g_(ohms != 0.0 ? 1.0 / ohms : 0.0) {}

    [[nodiscard]] const std::string& name() const override { return name_; }
    [[nodiscard]] char kind() const override { return 'R'; }
    [[nodiscard]] std::vector<NodeId> terminals() const override { return {a_, b_}; }
    [[nodiscard]] std::vector<std::pair<NodeId, NodeId>> conductance_edges()
        const override {
        return {{a_, b_}};
    }
    void load_dc(DcLoadContext& cx) const override { cx.add_conductance(a_, b_, g_); }
    void load_ac(AcLoadContext& cx) const override {
        cx.add_conductance(a_, b_, g_);
    }

private:
    std::string name_;
    NodeId a_, b_;
    double g_;
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
    double ohms = 0.0;
    const ws::core::Result<double> direct = resolve_value_token(card.rest[2], s.params);
    if (!direct) {
        // The inner error already carries its own file:line:col.
        return ws::core::unexpected(direct.error());
    }
    ohms = *direct;
    if (card.rest.size() > 3) {
        return spice_fail(dev_diag(
            ErrorKind::Semantic, "spice.semantic.r_extra",
            "resistor " + card.name.text +
                " takes a plain value in v1 (models/geometry land with M5)",
            card.loc));
    }
    if (ohms <= 0.0 || !std::isfinite(ohms)) {
        return spice_fail(dev_diag(ErrorKind::Semantic, "spice.semantic.r_value",
                                   "resistor " + card.name.text +
                                       " needs a positive finite value",
                                   card.loc));
    }
    return std::make_unique<Resistor>(card.name.text, n[0], n[1], ohms);
}

// --- V / I: DC value, optional AC spec, optional waveform (§6.2/§7.4) ---------

// The source card grammar after the two nodes:
//   [DC] value [AC mag [phase]] [PULSE(...)|SIN(...)|EXP(...)|PWL(...)]
// The waveform may span several tokens ("PULSE(" "0" "1" ")"); the first
// DC-position value may also be the waveform itself.
struct SourceSpec {
    double dc = 0.0;
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
            WS_TRY_ASSIGN(out.dc, resolve_value_token(card.rest[i + 1], params));
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
            WS_TRY_ASSIGN(out.dc, resolve_value_token(card.rest[i], params));
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
    // the deferred tstep/tstop defaults).
    if (!out.have_dc && out.wave) {
        out.dc = out.wave->value(0.0);
        out.have_dc = true;
    }
    return {};
}

// The shared source body: DC value + optional AC phasor + waveform.
class SourceCommon {
public:
    double value_at(double t) const { return wave_ ? wave_->value(t) : dc_; }
    [[nodiscard]] double dc_value() const { return dc_; }
    [[nodiscard]] bool set_sweep_value(double v) {
        dc_ = v;
        return true;
    }
    [[nodiscard]] double next_event(double t) const {
        return wave_ ? wave_->next_event(t)
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
    void finalize_waveform(double tstep, double tstop) {
        if (wave_) {
            wave_->finalize(tstep, tstop);
        }
    }

protected:
    SourceCommon(const SourceSpec& spec)
        : dc_(spec.dc), ac_mag_(spec.ac_mag), ac_phase_(spec.ac_phase),
          wave_(spec.wave) {}

    double dc_;
    std::optional<double> ac_mag_;
    std::optional<double> ac_phase_;
    std::optional<Waveform> wave_;
};

class VoltageSource final : public Device, private SourceCommon {
public:
    VoltageSource(std::string display, NodeId p, NodeId n, BranchId branch,
                  const SourceSpec& spec)
        : Device(), SourceCommon(spec), name_(std::move(display)), p_(p), n_(n),
          branch_(branch) {}

    [[nodiscard]] const std::string& name() const override { return name_; }
    [[nodiscard]] char kind() const override { return 'V'; }
    [[nodiscard]] bool has_branch() const override { return true; }
    [[nodiscard]] BranchId branch() const override { return branch_; }
    [[nodiscard]] std::vector<NodeId> terminals() const override { return {p_, n_}; }
    [[nodiscard]] std::vector<std::pair<NodeId, NodeId>> constraint_edges()
        const override {
        return {{p_, n_}};
    }
    void load_dc(DcLoadContext& cx) const override {
        cx.stamp_branch_voltage(branch_, p_, n_, dc_value());
    }
    void load_tran(DcLoadContext& cx, const TranStep& step) const override {
        cx.stamp_branch_voltage(branch_, p_, n_, value_at(step.time));
    }
    void load_ac(AcLoadContext& cx) const override {
        // A V source is a short to the small-signal system; the AC spec (or
        // zero) is the stimulus (§7.4).
        cx.stamp_branch_voltage(branch_, p_, n_, ac_phasor());
    }
    [[nodiscard]] bool set_sweep_value(double v) override {
        return SourceCommon::set_sweep_value(v);
    }
    [[nodiscard]] double next_event(double t) const override {
        return SourceCommon::next_event(t);
    }
    [[nodiscard]] bool has_ac() const override { return SourceCommon::has_ac(); }
    void finalize_waveform(double tstep, double tstop) override {
        SourceCommon::finalize_waveform(tstep, tstop);
    }

private:
    std::string name_;
    NodeId p_, n_;
    BranchId branch_;
};

class CurrentSource final : public Device, private SourceCommon {
public:
    CurrentSource(std::string display, NodeId p, NodeId n, const SourceSpec& spec)
        : Device(), SourceCommon(spec), name_(std::move(display)), p_(p), n_(n) {}

    [[nodiscard]] const std::string& name() const override { return name_; }
    [[nodiscard]] char kind() const override { return 'I'; }
    [[nodiscard]] std::vector<NodeId> terminals() const override { return {p_, n_}; }
    void load_dc(DcLoadContext& cx) const override {
        // ngspice: positive current flows from N+ through the source to
        // N- - it LEAVES p_ and ENTERS n_ ("I1 0 out 1m" charges 'out'
        // positive, the familiar orientation).
        cx.add_current(n_, p_, dc_value());
    }
    void load_tran(DcLoadContext& cx, const TranStep& step) const override {
        cx.add_current(n_, p_, value_at(step.time));
    }
    void load_ac(AcLoadContext& cx) const override {
        // No AC spec: open to the small-signal system (§7.4).
        if (has_ac()) {
            cx.add_current(n_, p_, ac_phasor());
        }
    }
    [[nodiscard]] bool set_sweep_value(double v) override {
        return SourceCommon::set_sweep_value(v);
    }
    [[nodiscard]] double next_event(double t) const override {
        return SourceCommon::next_event(t);
    }
    [[nodiscard]] bool has_ac() const override { return SourceCommon::has_ac(); }
    void finalize_waveform(double tstep, double tstop) override {
        SourceCommon::finalize_waveform(tstep, tstop);
    }

private:
    std::string name_;
    NodeId p_, n_;
};

ws::core::Result<std::unique_ptr<Device>> make_vsource(const DeviceCard& card,
                                                       FactoryServices& s) {
    WS_TRY_ASSIGN(const std::vector<NodeId> n, take_nodes(card, s.nodes, 2, 0));
    SourceSpec spec;
    WS_TRY(parse_source_spec(card, s.params, spec));
    return std::make_unique<VoltageSource>(card.name.text, n[0], n[1],
                                           s.nodes.claim_branch(), spec);
}

ws::core::Result<std::unique_ptr<Device>> make_isource(const DeviceCard& card,
                                                       FactoryServices& s) {
    WS_TRY_ASSIGN(const std::vector<NodeId> n, take_nodes(card, s.nodes, 2, 0));
    SourceSpec spec;
    WS_TRY(parse_source_spec(card, s.params, spec));
    return std::make_unique<CurrentSource>(card.name.text, n[0], n[1], spec);
}

// --- E (VCVS) --------------------------------------------------------------------

class Vcvs final : public Device {
public:
    Vcvs(std::string display, NodeId p, NodeId n, NodeId cp, NodeId cn, BranchId branch,
         double gain)
        : name_(std::move(display)), p_(p), n_(n), cp_(cp), cn_(cn), branch_(branch),
          gain_(gain) {}

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
    void load_dc(DcLoadContext& cx) const override {
        // v(p) - v(n) = gain * (v(cp) - v(cn)), branch current i flows p->n
        // through the source.
        const std::size_t row = cx.branch_row(branch_);
        cx.add_entry(row, p_, 1.0);
        cx.add_entry(row, n_, -1.0);
        cx.add_entry(row, cp_, -gain_);
        cx.add_entry(row, cn_, gain_);
        cx.add_entry(p_, row, 1.0);
        cx.add_entry(n_, row, -1.0);
    }
    void load_ac(AcLoadContext& cx) const override {
        const std::size_t row = cx.branch_row(branch_);
        cx.add_entry(row, p_, 1.0);
        cx.add_entry(row, n_, -1.0);
        cx.add_entry(row, cp_, -gain_);
        cx.add_entry(row, cn_, gain_);
        cx.add_entry(p_, row, 1.0);
        cx.add_entry(n_, row, -1.0);
    }

private:
    std::string name_;
    NodeId p_, n_, cp_, cn_;
    BranchId branch_;
    double gain_;
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
    double gain = 0.0;
    WS_TRY_ASSIGN(gain, resolve_value_token(card.rest[4], s.params));
    return std::make_unique<Vcvs>(card.name.text, n[0], n[1], n[2], n[3],
                                  s.nodes.claim_branch(), gain);
}

// --- G (VCCS) --------------------------------------------------------------------

class Vccs final : public Device {
public:
    Vccs(std::string display, NodeId p, NodeId n, NodeId cp, NodeId cn, double gm)
        : name_(std::move(display)), p_(p), n_(n), cp_(cp), cn_(cn), gm_(gm) {}

    [[nodiscard]] const std::string& name() const override { return name_; }
    [[nodiscard]] char kind() const override { return 'G'; }
    [[nodiscard]] std::vector<NodeId> terminals() const override {
        return {p_, n_, cp_, cn_};
    }
    void load_dc(DcLoadContext& cx) const override {
        // i(p->n through the source) = gm * (v(cp) - v(cn))
        cx.add_entry(p_, cp_, gm_);
        cx.add_entry(p_, cn_, -gm_);
        cx.add_entry(n_, cp_, -gm_);
        cx.add_entry(n_, cn_, gm_);
    }
    void load_ac(AcLoadContext& cx) const override {
        cx.add_entry(p_, cp_, gm_);
        cx.add_entry(p_, cn_, -gm_);
        cx.add_entry(n_, cp_, -gm_);
        cx.add_entry(n_, cn_, gm_);
    }

private:
    std::string name_;
    NodeId p_, n_, cp_, cn_;
    double gm_;
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
    double gm = 0.0;
    WS_TRY_ASSIGN(gm, resolve_value_token(card.rest[4], s.params));
    return std::make_unique<Vccs>(card.name.text, n[0], n[1], n[2], n[3], gm);
}

// --- F (CCCS) / H (CCVS): controlled by a named V source's branch current ------

class Cccs final : public Device {
public:
    Cccs(std::string display, NodeId p, NodeId n, double gain)
        : name_(std::move(display)), p_(p), n_(n), gain_(gain) {}

    void set_control(BranchId br) { control_ = br; }
    void set_control_branch(BranchId br) override { control_ = br; }

    [[nodiscard]] const std::string& name() const override { return name_; }
    [[nodiscard]] char kind() const override { return 'F'; }
    [[nodiscard]] std::vector<NodeId> terminals() const override { return {p_, n_}; }
    void load_dc(DcLoadContext& cx) const override {
        // i(p->n) = gain * i(control source)
        const std::size_t ctl = cx.branch_row(control_);
        cx.add_entry(p_, ctl, gain_);
        cx.add_entry(n_, ctl, -gain_);
    }
    void load_ac(AcLoadContext& cx) const override {
        const std::size_t ctl = cx.branch_row(control_);
        cx.add_entry(p_, ctl, gain_);
        cx.add_entry(n_, ctl, -gain_);
    }

private:
    std::string name_;
    NodeId p_, n_;
    BranchId control_ = 0;
    double gain_;
};

class Ccvs final : public Device {
public:
    Ccvs(std::string display, NodeId p, NodeId n, BranchId branch, double r)
        : name_(std::move(display)), p_(p), n_(n), branch_(branch), r_(r) {}

    void set_control(BranchId br) { control_ = br; }
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
    void load_dc(DcLoadContext& cx) const override {
        // v(p) - v(n) = r * i(control source)
        const std::size_t row = cx.branch_row(branch_);
        const std::size_t ctl = cx.branch_row(control_);
        cx.add_entry(row, p_, 1.0);
        cx.add_entry(row, n_, -1.0);
        cx.add_entry(row, ctl, -r_);
        cx.add_entry(p_, row, 1.0);
        cx.add_entry(n_, row, -1.0);
    }
    void load_ac(AcLoadContext& cx) const override {
        const std::size_t row = cx.branch_row(branch_);
        const std::size_t ctl = cx.branch_row(control_);
        cx.add_entry(row, p_, 1.0);
        cx.add_entry(row, n_, -1.0);
        cx.add_entry(row, ctl, -r_);
        cx.add_entry(p_, row, 1.0);
        cx.add_entry(n_, row, -1.0);
    }

private:
    std::string name_;
    NodeId p_, n_;
    BranchId branch_;
    BranchId control_ = 0;
    double r_;
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
    double gain = 0.0;
    WS_TRY_ASSIGN(gain, resolve_value_token(card.rest[3], s.params));
    auto dev = std::make_unique<Cccs>(card.name.text, n[0], n[1], gain);
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
    double r = 0.0;
    WS_TRY_ASSIGN(r, resolve_value_token(card.rest[3], s.params));
    auto dev = std::make_unique<Ccvs>(card.name.text, n[0], n[1],
                                      s.nodes.claim_branch(), r);
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
    };
    return table;
}

}  // namespace ws::spice
