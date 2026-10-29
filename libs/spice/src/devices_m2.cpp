// M2 devices (DESIGN.md §6.2, §11): C and L with charge/flux-consistent
// trapezoidal companions (§6.1/§6.3 LTE state machine), pairwise K
// coupling, and S/W switches as Ron/Roff conductance models with
// hysteresis control. All dynamic state lives on the device (§3.5); the
// .tran driver drives begin_step -> load_tran -> lte_ok -> commit.
//
// The make_* factories have external linkage: the builtin table lives in
// devices.cpp and names them.

#include "cards.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <ws/spice/deck.hpp>
#include <ws/spice/device.hpp>

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

// Voltage across a two-terminal device from the solution vector.
[[nodiscard]] double v_across(const std::vector<double>& x, NodeId p, NodeId n) {
    const double vp = (p == kGround) ? 0.0 : x[p];
    const double vn = (n == kGround) ? 0.0 : x[n];
    return vp - vn;
}

// Third-order divided-difference LTE (§6.3): the trapezoidal local error
// is h^3/12 * |q'''| with q''' estimated as 6 * dd3 over the last four
// (t, q) points; tolq = trtol*(reltol*max(|q|,|q_prev|) + chgtol).
// Returns eps/tolq through `ratio` and whether the step passes.
bool dd3_lte(const std::vector<std::pair<double, double>>& hist, double t_new,
             double q_new, double dt, double q_scale, double reltol, double chgtol,
             double trtol, double* ratio) {
    if (hist.size() < 3) {
        if (ratio != nullptr) {
            *ratio = 0.0;
        }
        return true;  // not enough history (start / BE restart): accept
    }
    const double t0 = hist[hist.size() - 3].first;
    const double q0 = hist[hist.size() - 3].second;
    const double t1 = hist[hist.size() - 2].first;
    const double q1 = hist[hist.size() - 2].second;
    const double t2 = hist[hist.size() - 1].first;
    const double q2 = hist[hist.size() - 1].second;
    const double t3 = t_new;
    const double q3 = q_new;
    if (!(t1 > t0) || !(t2 > t1) || !(t3 > t2) || !(dt > 0.0)) {
        if (ratio != nullptr) {
            *ratio = 0.0;
        }
        return true;
    }
    const double d1a = (q1 - q0) / (t1 - t0);
    const double d1b = (q2 - q1) / (t2 - t1);
    const double d1c = (q3 - q2) / (t3 - t2);
    const double d2a = (d1b - d1a) / (t2 - t0);
    const double d2b = (d1c - d1b) / (t3 - t1);
    const double dd3 = (d2b - d2a) / (t3 - t0);
    const double eps = dt * dt * dt * std::abs(dd3) / 2.0;
    const double tolq =
        trtol * (reltol * std::max(std::abs(q3), std::abs(q_scale)) + chgtol);
    if (ratio != nullptr) {
        *ratio = (tolq > 0.0) ? (eps / tolq) : std::numeric_limits<double>::infinity();
    }
    return tolq > 0.0 && eps <= tolq;
}

// --- C -------------------------------------------------------------------------

class Capacitor final : public Device {
public:
    Capacitor(std::string display, NodeId p, NodeId n, double farads, bool has_ic,
              double ic)
        : name_(std::move(display)), p_(p), n_(n), c_(farads), has_ic_(has_ic), ic_(ic) {}

    [[nodiscard]] const std::string& name() const override { return name_; }
    [[nodiscard]] char kind() const override { return 'C'; }
    [[nodiscard]] std::vector<NodeId> terminals() const override { return {p_, n_}; }
    // Open at DC: no edges at all (a node whose only path is through caps
    // is the §6.6 "no DC path" finding).
    void load_dc(DcLoadContext&) const override {}
    void load_tran(DcLoadContext& cx, const TranStep&) const override {
        // Norton companion: geq in parallel with jeq; i = geq*v + jeq
        // flows p -> n through the device, so jeq is injected into n and
        // pulled out of p.
        cx.add_conductance(p_, n_, geq_);
        cx.add_current(n_, p_, jeq_);
    }
    void load_ac(AcLoadContext& cx) const override {
        cx.add_conductance(p_, n_, {0.0, cx.omega * c_});
    }

    [[nodiscard]] bool is_dynamic() const override { return true; }
    void begin_step(const TranStep& step) override {
        if (step.be) {
            geq_ = c_ / step.dt;
            jeq_ = -q_prev_ / step.dt;
        } else {
            geq_ = 2.0 * c_ / step.dt;
            jeq_ = -(geq_ * v_prev_ + i_prev_);
        }
    }
    void commit(const std::vector<double>& x, std::size_t node_count,
                const TranStep& step) override {
        (void)node_count;
        const double v = v_across(x, p_, n_);
        v_prev_ = v;
        q_prev_ = c_ * v;
        i_prev_ = geq_ * v + jeq_;
        if (step.be) {
            hist_.clear();  // BE restart invalidates the trap LTE history
        } else {
            hist_.emplace_back(step.time, q_prev_);
            if (hist_.size() > 3) {
                hist_.erase(hist_.begin());
            }
        }
    }
    [[nodiscard]] bool lte_ok(const TranStep& step, const std::vector<double>& x,
                              std::size_t node_count, double reltol, double chgtol,
                              double trtol, double* ratio) const override {
        (void)node_count;
        const double q = c_ * v_across(x, p_, n_);
        return dd3_lte(hist_, step.time, q, step.dt, q_prev_, reltol, chgtol, trtol,
                       ratio);
    }
    void seed_state(const std::vector<double>& x, std::size_t node_count) override {
        (void)node_count;
        v_prev_ = has_ic_ ? ic_ : v_across(x, p_, n_);
        q_prev_ = c_ * v_prev_;
        i_prev_ = 0.0;
        hist_.clear();
    }
    [[nodiscard]] bool has_ic() const override { return has_ic_; }
    [[nodiscard]] double ic_value() const override { return ic_; }

private:
    std::string name_;
    NodeId p_, n_;
    double c_;
    bool has_ic_;
    double ic_;

    // companion + state (§3.5)
    double geq_ = 0.0, jeq_ = 0.0;
    double v_prev_ = 0.0, i_prev_ = 0.0, q_prev_ = 0.0;
    std::vector<std::pair<double, double>> hist_;  // last 3 accepted (t, q)
};

// --- L (with optional Rser and pairwise K coupling) ------------------------------

class Inductor final : public Device {
public:
    Inductor(std::string display, NodeId p, NodeId n, NodeId mid, double henries,
             double rser, bool has_ic, double ic, BranchId branch)
        : name_(std::move(display)), p_(p), n_(n), mid_(mid), l_(henries), rser_(rser),
          has_ic_(has_ic), ic_(ic), branch_(branch) {}

    [[nodiscard]] const std::string& name() const override { return name_; }
    [[nodiscard]] char kind() const override { return 'L'; }
    [[nodiscard]] bool has_branch() const override { return true; }
    [[nodiscard]] BranchId branch() const override { return branch_; }
    [[nodiscard]] std::vector<NodeId> terminals() const override { return {p_, n_}; }
    // A short at DC: constraint edges (an L loop is a §6.6 loop finding;
    // the constraint also connects nodes for the DC-path check).
    [[nodiscard]] std::vector<std::pair<NodeId, NodeId>> constraint_edges()
        const override {
        return {{p_, n_}};
    }

    void load_dc(DcLoadContext& cx) const override {
        if (rser_ > 0.0) {
            cx.add_conductance(p_, mid_, 1.0 / rser_);
        }
        // Short: v(a) - v(b) = 0 across the inductor proper.
        cx.add_entry(cx.branch_row(branch_), a(), 1.0);
        cx.add_entry(cx.branch_row(branch_), b(), -1.0);
        cx.add_entry(a(), cx.branch_row(branch_), 1.0);
        cx.add_entry(b(), cx.branch_row(branch_), -1.0);
    }
    void load_tran(DcLoadContext& cx, const TranStep&) const override {
        if (rser_ > 0.0) {
            cx.add_conductance(p_, mid_, 1.0 / rser_);
        }
        const std::size_t row = cx.branch_row(branch_);
        // Branch equation: i - ka*v - kb*v_partner = rhs (§6.2 companion;
        // uncoupled: ka = k/L, kb = 0).
        cx.add_entry(row, row, 1.0);
        cx.add_entry(row, a(), -ka_);
        cx.add_entry(row, b(), ka_);
        if (partner_ != nullptr) {
            cx.add_entry(row, partner().a(), -kb_);
            cx.add_entry(row, partner().b(), kb_);
        }
        cx.add_rhs(row, rhs_);
        // KCL coupling (branch current leaves a, enters b).
        cx.add_entry(a(), row, 1.0);
        cx.add_entry(b(), row, -1.0);
    }
    void load_ac(AcLoadContext& cx) const override {
        if (rser_ > 0.0) {
            cx.add_conductance(p_, mid_, 1.0 / rser_);
        }
        const std::size_t row = cx.branch_row(branch_);
        // v(a) - v(b) - jwL*i - jwM*i_partner = 0
        cx.add_entry(row, a(), 1.0);
        cx.add_entry(row, b(), -1.0);
        cx.add_entry(row, row, -std::complex<double>(0.0, cx.omega * l_));
        if (partner_ != nullptr) {
            cx.add_entry(row, cx.branch_row(partner_->branch()),
                         -std::complex<double>(0.0, cx.omega * m_));
        }
        cx.add_entry(a(), row, 1.0);
        cx.add_entry(b(), row, -1.0);
    }

    [[nodiscard]] bool is_dynamic() const override { return true; }
    void begin_step(const TranStep& step) override {
        const double k = step.be ? step.dt : step.dt / 2.0;
        if (partner_ == nullptr) {
            ka_ = k / l_;
            kb_ = 0.0;
            rhs_ = step.be ? i_prev_ : (i_prev_ + ka_ * v_prev_);
            return;
        }
        // Minv = 1/(L1*L2 - M^2) * [L2, -M; -M, L1]
        const double l2 = partner().l_;
        const double d = l_ * l2 - m_ * m_;
        const double a = l2 / d;
        const double b = -m_ / d;
        ka_ = k * a;
        kb_ = k * b;
        rhs_ = step.be ? i_prev_
                       : (i_prev_ + k * (a * v_prev_ + b * partner().v_prev_));
    }
    void commit(const std::vector<double>& x, std::size_t node_count,
                const TranStep& step) override {
        const std::size_t row = node_count + branch_;
        i_prev_ = x[row];
        v_prev_ = v_across(x, a(), b());
        // Flux linkage phi = L*i + M*i_partner is what the companion
        // integrates (exactly, trap or BE); read it from the solution.
        phi_prev_ = l_ * i_prev_;
        if (partner_ != nullptr) {
            phi_prev_ += m_ * x[node_count + partner_->branch()];
        }
        if (step.be) {
            hist_.clear();
        } else {
            hist_.emplace_back(step.time, phi_prev_);
            if (hist_.size() > 3) {
                hist_.erase(hist_.begin());
            }
        }
    }
    [[nodiscard]] bool lte_ok(const TranStep& step, const std::vector<double>& x,
                              std::size_t node_count, double reltol, double chgtol,
                              double trtol, double* ratio) const override {
        const std::size_t row = node_count + branch_;
        double phi = l_ * x[row];
        if (partner_ != nullptr) {
            phi += m_ * x[node_count + partner_->branch()];
        }
        return dd3_lte(hist_, step.time, phi, step.dt, phi_prev_, reltol, chgtol,
                       trtol, ratio);
    }
    void seed_state(const std::vector<double>& x, std::size_t node_count) override {
        const std::size_t row = node_count + branch_;
        i_prev_ = has_ic_ ? ic_ : x[row];
        v_prev_ = v_across(x, a(), b());
        phi_prev_ = l_ * i_prev_;
        hist_.clear();
    }
    [[nodiscard]] bool has_ic() const override { return has_ic_; }
    [[nodiscard]] double ic_value() const override { return ic_; }

    // --- coupling protocol (resolved by the elaborator) ---------------------
    [[nodiscard]] bool couple_to(Device& partner, double k) override {
        if (partner_ != nullptr || partner.kind() != 'L' || &partner == this) {
            return false;
        }
        partner_ = &partner;
        m_ = k * std::sqrt(l_ * partner.inductance());
        return true;
    }
    [[nodiscard]] double inductance() const override { return l_; }
    [[nodiscard]] double dyn_i_prev() const override { return i_prev_; }
    [[nodiscard]] double dyn_v_prev() const override { return v_prev_; }

private:
    // The inductor proper sits between a() and b(): mid and n when Rser
    // is present, p and n otherwise.
    [[nodiscard]] NodeId a() const { return rser_ > 0.0 ? mid_ : p_; }
    [[nodiscard]] NodeId b() const { return n_; }

    // partner_ only ever holds an Inductor (couple_to checks kind() ==
    // 'L'); same-class private access makes its internals visible here.
    [[nodiscard]] const Inductor& partner() const {
        return *static_cast<const Inductor*>(partner_);
    }

    std::string name_;
    NodeId p_, n_, mid_;  // mid_ == kGround when no Rser
    double l_;
    double rser_;
    bool has_ic_;
    double ic_;
    BranchId branch_;

    Device* partner_ = nullptr;  // the other Inductor of a K pair
    double m_ = 0.0;             // mutual inductance (signed by k)

    // companion + state
    double ka_ = 0.0, kb_ = 0.0, rhs_ = 0.0;
    double v_prev_ = 0.0, i_prev_ = 0.0, phi_prev_ = 0.0;
    std::vector<std::pair<double, double>> hist_;  // last 3 accepted (t, phi)
};

// --- K (pairwise coupling; the device itself stamps nothing) ---------------------

class CouplingDevice final : public Device {
public:
    explicit CouplingDevice(std::string display) : name_(std::move(display)) {}
    [[nodiscard]] const std::string& name() const override { return name_; }
    [[nodiscard]] char kind() const override { return 'K'; }
    [[nodiscard]] std::vector<NodeId> terminals() const override { return {}; }
    void load_dc(DcLoadContext&) const override {}

private:
    std::string name_;
};

// --- S / W switches (§6.2: Ron/Roff conductance model, never a true open) --------

class SwitchDeviceBase : public Device {
public:
    SwitchDeviceBase(std::string display, NodeId p, NodeId n, bool initial_on)
        : name_(std::move(display)), p_(p), n_(n), on_(initial_on) {}

    [[nodiscard]] const std::string& name() const override { return name_; }
    [[nodiscard]] std::vector<NodeId> terminals() const override { return {p_, n_}; }
    [[nodiscard]] std::vector<std::pair<NodeId, NodeId>> conductance_edges()
        const override {
        return {{p_, n_}};  // both states conduct (Roff is finite)
    }
    void load_dc(DcLoadContext& cx) const override {
        cx.add_conductance(p_, n_, on_ ? gon_ : goff_);
    }
    void load_tran(DcLoadContext& cx, const TranStep&) const override {
        cx.add_conductance(p_, n_, on_ ? gon_ : goff_);
    }
    void load_ac(AcLoadContext& cx) const override {
        cx.add_conductance(p_, n_, on_ ? gon_ : goff_);
    }

    // .model SW (Ron= Roff= Vt= Vh= It= Ih=); S reads Vt/Vh, W reads It/Ih.
    ws::core::Status apply_model(const ModelDef& model) override {
        if (model.type != "sw") {
            return spice_fail(dev_diag(
                ErrorKind::Semantic, "spice.semantic.model_type",
                "switch " + name_ + " needs a SW model, got type '" + model.type + "'",
                model.loc));
        }
        double ron = 1.0, roff = 1e12, vt = 0.0, vh = 0.0, it = 0.0, ih = 0.0;
        const auto& toks = model.params;
        for (std::size_t i = 0; i < toks.size(); ++i) {
            std::string key;
            std::string value;
            // Paren glue: "(Ron=1" / "Roff=1meg)" / bare "(" / ")".
            std::string text = toks[i].text;
            while (!text.empty() && text.front() == '(') {
                text.erase(text.begin());
            }
            while (!text.empty() && text.back() == ')') {
                text.pop_back();
            }
            if (text.empty()) {
                continue;
            }
            const std::size_t eq = text.find('=');
            if (eq != std::string::npos && eq > 0) {
                key = to_lower(text.substr(0, eq));
                value = text.substr(eq + 1);
            } else {
                // "key value" across two tokens, or "key = value" across
                // three (only meaningful when a value follows).
                key = to_lower(text);
                std::size_t next = i + 1;
                if (next < toks.size() && toks[next].text == "=") {
                    ++next;
                }
                if (next >= toks.size()) {
                    return spice_fail(dev_diag(
                        ErrorKind::Semantic, "spice.semantic.model_param",
                        "model '" + model.name.text + "': '" + text + "' needs a value",
                        toks[i].loc));
                }
                value = toks[next].text;
                i = next;
            }
            // Strip paren glue from the value side too.
            while (!value.empty() && value.back() == ')') {
                value.pop_back();
            }
            double v = 0.0;
            if (const ws::core::Result<double> r = parse_number(value)) {
                v = *r;
            } else {
                return spice_fail(dev_diag(
                    ErrorKind::Semantic, "spice.semantic.model_param",
                    "model '" + model.name.text + "': bad value '" + value + "'",
                    toks[i].loc));
            }
            if (key == "ron") {
                ron = v;
            } else if (key == "roff") {
                roff = v;
            } else if (key == "vt") {
                vt = v;
            } else if (key == "vh") {
                vh = v;
            } else if (key == "it") {
                it = v;
            } else if (key == "ih") {
                ih = v;
            } else {
                return spice_fail(dev_diag(
                    ErrorKind::Semantic, "spice.semantic.model_param",
                    "model '" + model.name.text + "': unknown parameter '" + key +
                        "' (Ron, Roff, Vt, Vh, It, Ih)",
                    toks[i].loc));
            }
        }
        if (ron <= 0.0 || roff <= 0.0 || ron >= roff) {
            return spice_fail(dev_diag(
                ErrorKind::Semantic, "spice.semantic.sw_value",
                "model '" + model.name.text +
                    "' needs 0 < Ron < Roff (a closed switch must conduct more"
                    " than an open one)",
                model.loc));
        }
        ron_ = ron;
        roff_ = roff;
        vt_ = vt;
        vh_ = vh;
        it_ = it;
        ih_ = ih;
        gon_ = 1.0 / ron;
        goff_ = std::max(1.0 / roff, 1e-15);  // §6.2: tiny conductance floor
        return {};
    }

protected:
    // Hysteresis band: on above thr+band, off below thr-band, sticky
    // in between (the SW model's Vh/Ih).
    [[nodiscard]] static bool hysteresis_flip(bool on, double control, double thr,
                                              double band) {
        if (!on && control > thr + band) {
            return true;
        }
        if (on && control < thr - band) {
            return false;
        }
        return on;
    }

    std::string name_;
    NodeId p_, n_;
    bool on_;
    double ron_ = 1.0, roff_ = 1e12;
    double vt_ = 0.0, vh_ = 0.0, it_ = 0.0, ih_ = 0.0;
    double gon_ = 1.0, goff_ = 1e-12;
};

class Vswitch final : public SwitchDeviceBase {
public:
    Vswitch(std::string display, NodeId p, NodeId n, NodeId cp, NodeId cn, bool on)
        : SwitchDeviceBase(std::move(display), p, n, on), cp_(cp), cn_(cn) {}

    [[nodiscard]] char kind() const override { return 'S'; }
    [[nodiscard]] std::vector<NodeId> terminals() const override {
        return {p_, n_, cp_, cn_};
    }
    [[nodiscard]] bool update_state(const std::vector<double>& x,
                                    std::size_t node_count) override {
        (void)node_count;
        const double v = v_across(x, cp_, cn_);
        const bool next = hysteresis_flip(on_, v, vt_, vh_);
        const bool changed = next != on_;
        on_ = next;
        return changed;
    }

private:
    NodeId cp_, cn_;
};

class Iswitch final : public SwitchDeviceBase {
public:
    Iswitch(std::string display, NodeId p, NodeId n, BranchId control, bool on)
        : SwitchDeviceBase(std::move(display), p, n, on), control_(control) {}

    [[nodiscard]] char kind() const override { return 'W'; }
    void set_control_branch(BranchId br) override { control_ = br; }
    [[nodiscard]] bool update_state(const std::vector<double>& x,
                                    std::size_t node_count) override {
        const double i = x[node_count + control_];
        const bool next = hysteresis_flip(on_, i, it_, ih_);
        const bool changed = next != on_;
        on_ = next;
        return changed;
    }

private:
    BranchId control_;
};

}  // namespace

// --- the factories (external linkage; devices.cpp's table names them) ------------

ws::core::Result<std::unique_ptr<Device>> make_capacitor(const DeviceCard& card,
                                                         FactoryServices& s) {
    if (card.rest.size() < 3) {
        return spice_fail(dev_diag(ErrorKind::Semantic, "spice.semantic.arity",
                                   "capacitor " + card.name.text +
                                       " needs two nodes and a value",
                                   card.loc));
    }
    // C name p n value [ic=V] - models (TC, Cjo) land with M5.
    WS_TRY_ASSIGN(const double farads, resolve_value_token(card.rest[2], s.params));
    if (farads <= 0.0 || !std::isfinite(farads)) {
        return spice_fail(dev_diag(ErrorKind::Semantic, "spice.semantic.c_value",
                                   "capacitor " + card.name.text +
                                       " needs a positive finite value",
                                   card.loc));
    }
    bool has_ic = false;
    double ic = 0.0;
    for (std::size_t i = 3; i < card.rest.size(); ++i) {
        const std::string lower = to_lower(card.rest[i].text);
        if (lower.rfind("ic=", 0) == 0) {
            Token t;
            t.text = card.rest[i].text.substr(3);
            t.loc = card.rest[i].loc;
            WS_TRY_ASSIGN(ic, resolve_value_token(t, s.params));
            has_ic = true;
            continue;
        }
        return spice_fail(dev_diag(
            ErrorKind::Semantic, "spice.semantic.c_extra",
            "capacitor " + card.name.text +
                " takes a plain value and an optional ic= (models land with M5)",
            card.loc));
    }
    WS_TRY_ASSIGN(const std::vector<NodeId> n, take_nodes(card, s.nodes, 2, 0));
    return std::make_unique<Capacitor>(card.name.text, n[0], n[1], farads, has_ic, ic);
}

ws::core::Result<std::unique_ptr<Device>> make_inductor(const DeviceCard& card,
                                                        FactoryServices& s) {
    if (card.rest.size() < 3) {
        return spice_fail(dev_diag(ErrorKind::Semantic, "spice.semantic.arity",
                                   "inductor " + card.name.text +
                                       " needs two nodes and a value",
                                   card.loc));
    }
    WS_TRY_ASSIGN(const double henries, resolve_value_token(card.rest[2], s.params));
    if (henries <= 0.0 || !std::isfinite(henries)) {
        return spice_fail(dev_diag(ErrorKind::Semantic, "spice.semantic.l_value",
                                   "inductor " + card.name.text +
                                       " needs a positive finite value",
                                   card.loc));
    }
    double rser = 0.0;
    bool has_ic = false;
    double ic = 0.0;
    for (std::size_t i = 3; i < card.rest.size(); ++i) {
        const std::string lower = to_lower(card.rest[i].text);
        if (lower.rfind("rser=", 0) == 0) {
            Token t;
            t.text = card.rest[i].text.substr(5);
            t.loc = card.rest[i].loc;
            WS_TRY_ASSIGN(rser, resolve_value_token(t, s.params));
            continue;
        }
        if (lower.rfind("ic=", 0) == 0) {
            Token t;
            t.text = card.rest[i].text.substr(3);
            t.loc = card.rest[i].loc;
            WS_TRY_ASSIGN(ic, resolve_value_token(t, s.params));
            has_ic = true;
            continue;
        }
        return spice_fail(dev_diag(
            ErrorKind::Semantic, "spice.semantic.l_extra",
            "inductor " + card.name.text +
                " takes a plain value and optional ic= / Rser= (models land with M5)",
            card.loc));
    }
    if (rser < 0.0 || !std::isfinite(rser)) {
        return spice_fail(dev_diag(ErrorKind::Semantic, "spice.semantic.l_rser",
                                   "inductor " + card.name.text +
                                       " needs Rser >= 0",
                                   card.loc));
    }
    WS_TRY_ASSIGN(const std::vector<NodeId> n, take_nodes(card, s.nodes, 2, 0));
    NodeId mid = kGround;
    if (rser > 0.0) {
        Token t;
        t.text = card.name.text + "#mid";  // internal Rser/L junction node
        t.loc = card.loc;
        mid = s.nodes.node(t);
    }
    return std::make_unique<Inductor>(card.name.text, n[0], n[1], mid, henries, rser,
                                      has_ic, ic, s.nodes.claim_branch());
}

ws::core::Result<std::unique_ptr<Device>> make_coupling(const DeviceCard& card,
                                                        FactoryServices& s) {
    if (card.rest.size() != 3) {
        return spice_fail(dev_diag(
            ErrorKind::Semantic, "spice.semantic.k_arity",
            "coupling " + card.name.text + " takes two inductor names and k"
                                            " (nonlinear K models are backlog)",
            card.loc));
    }
    WS_TRY_ASSIGN(const double k, resolve_value_token(card.rest[2], s.params));
    if (!std::isfinite(k) || std::abs(k) >= 1.0) {
        return spice_fail(dev_diag(ErrorKind::Semantic, "spice.semantic.k_value",
                                   "coupling " + card.name.text +
                                       " needs |k| < 1 (k = 1 is singular)",
                                   card.loc));
    }
    auto dev = std::make_unique<CouplingDevice>(card.name.text);
    s.couplings.push_back(CouplingRequest{dev.get(), card.rest[0], card.rest[1], k});
    return dev;
}

ws::core::Result<std::unique_ptr<Device>> make_vswitch(const DeviceCard& card,
                                                       FactoryServices& s) {
    // S name p n pc nc model [ic=on|off]
    WS_TRY_ASSIGN(const std::vector<NodeId> n, take_nodes(card, s.nodes, 4, 0));
    if (card.rest.size() < 5) {
        return spice_fail(dev_diag(ErrorKind::Semantic, "spice.semantic.arity",
                                   "switch " + card.name.text +
                                       " takes four nodes and a model name",
                                   card.loc));
    }
    bool on = false;
    for (std::size_t i = 5; i < card.rest.size(); ++i) {
        const std::string lower = to_lower(card.rest[i].text);
        if (lower.rfind("ic=", 0) == 0) {
            const std::string v = to_lower(card.rest[i].text.substr(3));
            on = (v == "on" || v == "1");
            continue;
        }
        return spice_fail(dev_diag(ErrorKind::Semantic, "spice.semantic.s_extra",
                                   "switch " + card.name.text +
                                       " takes nodes, a model and an optional ic=",
                                   card.loc));
    }
    auto dev = std::make_unique<Vswitch>(card.name.text, n[0], n[1], n[2], n[3], on);
    s.models.push_back(ModelRequest{dev.get(), card.rest[4]});
    return dev;
}

ws::core::Result<std::unique_ptr<Device>> make_iswitch(const DeviceCard& card,
                                                       FactoryServices& s) {
    // W name p n Vname model [ic=on|off]
    WS_TRY_ASSIGN(const std::vector<NodeId> n, take_nodes(card, s.nodes, 2, 0));
    if (card.rest.size() < 4) {
        return spice_fail(dev_diag(ErrorKind::Semantic, "spice.semantic.arity",
                                   "switch " + card.name.text +
                                       " takes two nodes, a controlling V source"
                                       " and a model name",
                                   card.loc));
    }
    bool on = false;
    for (std::size_t i = 4; i < card.rest.size(); ++i) {
        const std::string lower = to_lower(card.rest[i].text);
        if (lower.rfind("ic=", 0) == 0) {
            const std::string v = to_lower(card.rest[i].text.substr(3));
            on = (v == "on" || v == "1");
            continue;
        }
        return spice_fail(dev_diag(ErrorKind::Semantic, "spice.semantic.w_extra",
                                   "switch " + card.name.text +
                                       " takes nodes, a control source, a model and"
                                       " an optional ic=",
                                   card.loc));
    }
    auto dev = std::make_unique<Iswitch>(card.name.text, n[0], n[1], 0, on);
    s.controls.push_back(ControlRequest{dev.get(), card.rest[2]});
    s.models.push_back(ModelRequest{dev.get(), card.rest[3]});
    return dev;
}

}  // namespace ws::spice
