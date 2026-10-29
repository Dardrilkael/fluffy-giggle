// M1 devices (DESIGN.md §3/§11): R, V, I, E, F, G, H - all linear, all
// stateless. Each factory resolves its own terminals and parameters
// (arity belongs to the device, not the elaborator); F/H register control
// requests that the elaborator resolves to branch ids once every V source
// has claimed one.

#include "cards.hpp"

#include <cmath>
#include <memory>
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

// Resolves exactly `count` node tokens starting at rest[base].
[[nodiscard]] ws::core::Result<std::vector<NodeId>> take_nodes(const DeviceCard& card,
                                                               NodeResolver& nodes,
                                                               std::size_t count,
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

private:
    std::string name_;
    NodeId a_, b_;
    double g_;
};

ws::core::Result<std::unique_ptr<Device>> make_resistor(const DeviceCard& card,
                                                        NodeResolver& nodes,
                                                        const ParamTable& params,
                                                        std::vector<ControlRequest>&) {
    WS_TRY_ASSIGN(const std::vector<NodeId> n, take_nodes(card, nodes, 2, 0));
    if (card.rest.size() < 3) {
        return spice_fail(dev_diag(ErrorKind::Semantic, "spice.semantic.arity",
                                   "device " + card.name.text + " needs a value",
                                   card.loc));
    }
    // R name a b value | modelname [w= l= ...]
    double ohms = 0.0;
    const ws::core::Result<double> direct = resolve_value_token(card.rest[2], params);
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

// --- V / I ---------------------------------------------------------------------

class VoltageSource final : public Device {
public:
    VoltageSource(std::string display, NodeId p, NodeId n, BranchId branch, double volts)
        : name_(std::move(display)), p_(p), n_(n), branch_(branch), volts_(volts) {}

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
        cx.stamp_branch_voltage(branch_, p_, n_, volts_);
    }
    [[nodiscard]] bool set_sweep_value(double v) override {
        volts_ = v;
        return true;
    }

private:
    std::string name_;
    NodeId p_, n_;
    BranchId branch_;
    double volts_;
};

class CurrentSource final : public Device {
public:
    CurrentSource(std::string display, NodeId p, NodeId n, double amps)
        : name_(std::move(display)), p_(p), n_(n), amps_(amps) {}

    [[nodiscard]] const std::string& name() const override { return name_; }
    [[nodiscard]] char kind() const override { return 'I'; }
    [[nodiscard]] std::vector<NodeId> terminals() const override { return {p_, n_}; }
    void load_dc(DcLoadContext& cx) const override {
        // ngspice: positive current flows from N+ through the source to
        // N- - it LEAVES p_ and ENTERS n_ ("I1 0 out 1m" charges 'out'
        // positive, the familiar orientation).
        cx.add_current(n_, p_, amps_);
    }
    [[nodiscard]] bool set_sweep_value(double v) override {
        amps_ = v;
        return true;
    }

private:
    std::string name_;
    NodeId p_, n_;
    double amps_;
};

ws::core::Result<std::unique_ptr<Device>> make_vsource(const DeviceCard& card,
                                                       NodeResolver& nodes,
                                                       const ParamTable& params,
                                                       std::vector<ControlRequest>&) {
    WS_TRY_ASSIGN(const std::vector<NodeId> n, take_nodes(card, nodes, 2, 0));
    double volts = 0.0;
    if (card.rest.size() == 4 && to_lower(card.rest[2].text) == "dc") {
        WS_TRY_ASSIGN(volts, resolve_value_token(card.rest[3], params));
    } else if (card.rest.size() == 3) {
        WS_TRY_ASSIGN(volts, resolve_value_token(card.rest[2], params));
    } else {
        return spice_fail(dev_diag(
            ErrorKind::Semantic, "spice.semantic.v_form",
            "source " + card.name.text +
                " takes DC VALUE only in v1 (PULSE/SIN/EXP/PWL land with .tran, M2)",
            card.loc));
    }
    return std::make_unique<VoltageSource>(card.name.text, n[0], n[1], nodes.claim_branch(),
                                           volts);
}

ws::core::Result<std::unique_ptr<Device>> make_isource(const DeviceCard& card,
                                                       NodeResolver& nodes,
                                                       const ParamTable& params,
                                                       std::vector<ControlRequest>&) {
    WS_TRY_ASSIGN(const std::vector<NodeId> n, take_nodes(card, nodes, 2, 0));
    double amps = 0.0;
    if (card.rest.size() == 4 && to_lower(card.rest[2].text) == "dc") {
        WS_TRY_ASSIGN(amps, resolve_value_token(card.rest[3], params));
    } else if (card.rest.size() == 3) {
        WS_TRY_ASSIGN(amps, resolve_value_token(card.rest[2], params));
    } else {
        return spice_fail(dev_diag(
            ErrorKind::Semantic, "spice.semantic.i_form",
            "source " + card.name.text +
                " takes DC VALUE only in v1 (PULSE/SIN/EXP/PWL land with .tran, M2)",
            card.loc));
    }
    return std::make_unique<CurrentSource>(card.name.text, n[0], n[1], amps);
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

private:
    std::string name_;
    NodeId p_, n_, cp_, cn_;
    BranchId branch_;
    double gain_;
};

ws::core::Result<std::unique_ptr<Device>> make_vcvs(const DeviceCard& card,
                                                    NodeResolver& nodes,
                                                    const ParamTable& params,
                                                    std::vector<ControlRequest>&) {
    WS_TRY_ASSIGN(const std::vector<NodeId> n, take_nodes(card, nodes, 4, 0));
    if (card.rest.size() != 5) {
        return spice_fail(dev_diag(ErrorKind::Semantic, "spice.semantic.arity",
                                   "device " + card.name.text +
                                       " takes four nodes and a gain",
                                   card.loc));
    }
    double gain = 0.0;
    WS_TRY_ASSIGN(gain, resolve_value_token(card.rest[4], params));
    return std::make_unique<Vcvs>(card.name.text, n[0], n[1], n[2], n[3],
                                  nodes.claim_branch(), gain);
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

private:
    std::string name_;
    NodeId p_, n_, cp_, cn_;
    double gm_;
};

ws::core::Result<std::unique_ptr<Device>> make_vccs(const DeviceCard& card,
                                                    NodeResolver& nodes,
                                                    const ParamTable& params,
                                                    std::vector<ControlRequest>&) {
    WS_TRY_ASSIGN(const std::vector<NodeId> n, take_nodes(card, nodes, 4, 0));
    if (card.rest.size() != 5) {
        return spice_fail(dev_diag(ErrorKind::Semantic, "spice.semantic.arity",
                                   "device " + card.name.text +
                                       " takes four nodes and a transconductance",
                                   card.loc));
    }
    double gm = 0.0;
    WS_TRY_ASSIGN(gm, resolve_value_token(card.rest[4], params));
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

private:
    std::string name_;
    NodeId p_, n_;
    BranchId branch_;
    BranchId control_ = 0;
    double r_;
};

ws::core::Result<std::unique_ptr<Device>> make_cccs(const DeviceCard& card,
                                                    NodeResolver& nodes,
                                                    const ParamTable& params,
                                                    std::vector<ControlRequest>& controls) {
    WS_TRY_ASSIGN(const std::vector<NodeId> n, take_nodes(card, nodes, 2, 0));
    if (card.rest.size() != 4) {
        return spice_fail(dev_diag(ErrorKind::Semantic, "spice.semantic.arity",
                                   "device " + card.name.text +
                                       " takes two nodes, a controlling V source"
                                       " and a gain",
                                   card.loc));
    }
    double gain = 0.0;
    WS_TRY_ASSIGN(gain, resolve_value_token(card.rest[3], params));
    auto dev = std::make_unique<Cccs>(card.name.text, n[0], n[1], gain);
    controls.push_back(ControlRequest{dev.get(), card.rest[2]});
    return dev;
}

ws::core::Result<std::unique_ptr<Device>> make_ccvs(const DeviceCard& card,
                                                    NodeResolver& nodes,
                                                    const ParamTable& params,
                                                    std::vector<ControlRequest>& controls) {
    WS_TRY_ASSIGN(const std::vector<NodeId> n, take_nodes(card, nodes, 2, 0));
    if (card.rest.size() != 4) {
        return spice_fail(dev_diag(ErrorKind::Semantic, "spice.semantic.arity",
                                   "device " + card.name.text +
                                       " takes two nodes, a controlling V source"
                                       " and a transresistance",
                                   card.loc));
    }
    double r = 0.0;
    WS_TRY_ASSIGN(r, resolve_value_token(card.rest[3], params));
    auto dev = std::make_unique<Ccvs>(card.name.text, n[0], n[1], nodes.claim_branch(), r);
    controls.push_back(ControlRequest{dev.get(), card.rest[2]});
    return dev;
}

}  // namespace

// --- the factory table (§3.4: on the elaborator, never a global) ----------------

namespace cards {

const std::map<char, DeviceFactory>& builtin_device_factories() {
    static const std::map<char, DeviceFactory> table = {
        {'R', make_resistor}, {'V', make_vsource}, {'I', make_isource},
        {'E', make_vcvs},     {'G', make_vccs},    {'F', make_cccs},
        {'H', make_ccvs},
    };
    return table;
}

}  // namespace cards

}  // namespace ws::spice
