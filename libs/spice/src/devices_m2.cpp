// M2 devices (DESIGN.md §6.2, §11): C and L with charge/flux-consistent
// trapezoidal companions (§6.1/§6.3 LTE state machine), pairwise K
// coupling, and S/W switches as Ron/Roff conductance models with
// hysteresis control. Per the §3.5 revision, dynamic state lives in the
// run's arena (fixed-size double slices indexed through the Circuit's
// state_span), and card values are parameter sources rebindable per run.
// The .tran driver drives begin_step -> load_tran -> lte_ok -> commit,
// always through state slices.
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
// The history rides the device's state slice: 3 (t, q) pairs, oldest
// first, tracked by the device's commit().
struct LteHist {
    double t[3] = {0, 0, 0};
    double q[3] = {0, 0, 0};
    std::size_t n = 0;  // valid pairs, <= 3
};

bool dd3_lte(const LteHist& hist, double t_new, double q_new, double dt, double q_scale,
             double reltol, double chgtol, double trtol, double* ratio) {
    if (hist.n < 3) {
        if (ratio != nullptr) {
            *ratio = 0.0;
        }
        return true;  // not enough history (start / BE restart): accept
    }
    const double t0 = hist.t[0];
    const double q0 = hist.q[0];
    const double t1 = hist.t[1];
    const double q1 = hist.q[1];
    const double t2 = hist.t[2];
    const double q2 = hist.q[2];
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

// State layout (state_slots == 12):
//   [0] v_prev  [1] q_prev  [2] i_prev   companion inputs, seeded at t=0
//   [3] geq     [4] jeq                 companion coefficients per attempt
//   [5]         LTE history length (0..3)
//   [6..11]     LTE history (t, q) x3, oldest first
// The capacitance itself is a run parameter; the driver folds it into
// the slice (kCParam) once per step attempt through fold_state(), so the
// hooks never need a context beyond their own state.
class Capacitor final : public Device {
public:
    static constexpr std::size_t kV = 0, kQ = 1, kI = 2, kGeq = 3, kJeq = 4;
    static constexpr std::size_t kHistN = 5;   // history length
    static constexpr std::size_t kHist = 6;    // first (t, q) pair
    static constexpr std::size_t kCParam = 12; // folded capacitance mirror

    Capacitor(std::string display, NodeId p, NodeId n, ExprProgram farads, bool has_ic,
              double ic)
        : name_(std::move(display)), p_(p), n_(n), c_(std::move(farads)), has_ic_(has_ic),
          ic_(ic) {}

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
        cx.add_conductance(p_, n_, cx.st(kGeq));
        cx.add_current(n_, p_, cx.st(kJeq));
    }
    void load_ac(AcLoadContext& cx) const override {
        cx.add_conductance(p_, n_, {0.0, cx.omega * cx.par(0)});
    }

    [[nodiscard]] std::vector<ParamSpec> param_specs() const override {
        return {ParamSpec{"value", c_}};
    }
    [[nodiscard]] ws::core::Status check_params(const double* p) const override {
        if (!(p[0] > 0.0) || !std::isfinite(p[0])) {
            return spice_fail(dev_diag(
                ErrorKind::Semantic, "spice.semantic.c_value",
                "capacitor " + name_ + " needs a positive finite value", loc));
        }
        return {};
    }

    [[nodiscard]] bool is_dynamic() const override { return true; }
    [[nodiscard]] std::size_t state_slots() const override { return 13; }
    void fold_state(double* st, const RunState& rs, const Circuit& c) const override;
    [[nodiscard]] ws::core::Status link_dynamic(const Circuit& c,
                                                std::size_t self) override;

    void begin_step(const TranStep& step, double* st) override {
        const double c = st[kCParam];
        if (step.be) {
            st[kGeq] = c / step.dt;
            st[kJeq] = -st[kQ] / step.dt;
        } else {
            st[kGeq] = 2.0 * c / step.dt;
            st[kJeq] = -(st[kGeq] * st[kV] + st[kI]);
        }
    }
    void commit(const std::vector<double>& x, std::size_t node_count,
                const TranStep& step, double* st) override {
        (void)node_count;
        const double c = st[kCParam];
        const double v = v_across(x, p_, n_);
        st[kV] = v;
        st[kQ] = c * v;
        st[kI] = st[kGeq] * v + st[kJeq];
        if (step.be) {
            clear_hist(st);  // BE restart invalidates the trap LTE history
        } else {
            push_hist(st, step.time, st[kQ]);
        }
    }
    [[nodiscard]] bool lte_ok(const TranStep& step, const std::vector<double>& x,
                              std::size_t node_count, double reltol, double chgtol,
                              double trtol, const double* st, double* ratio) const override {
        (void)node_count;
        const double q = st[kCParam] * v_across(x, p_, n_);
        return dd3_lte(hist_of(st), step.time, q, step.dt, st[kQ], reltol, chgtol,
                       trtol, ratio);
    }
    void seed_state(const std::vector<double>& x, std::size_t node_count,
                    double* st) override {
        (void)node_count;
        st[kV] = has_ic_ ? ic_ : v_across(x, p_, n_);
        st[kQ] = st[kCParam] * st[kV];
        st[kI] = 0.0;
        clear_hist(st);
    }
    [[nodiscard]] bool has_ic() const override { return has_ic_; }
    [[nodiscard]] double ic_value() const override { return ic_; }

    [[nodiscard]] NodeId p() const { return p_; }
    [[nodiscard]] NodeId n() const { return n_; }

private:
    [[nodiscard]] static LteHist hist_of(const double* st) {
        LteHist h;
        h.n = static_cast<std::size_t>(st[kHistN]);
        for (std::size_t i = 0; i < 3 && i < h.n; ++i) {
            h.t[i] = st[kHist + 2 * i];
            h.q[i] = st[kHist + 2 * i + 1];
        }
        return h;
    }
    static void clear_hist(double* st) { st[kHistN] = 0.0; }
    static void push_hist(double* st, double t, double q) {
        const std::size_t n = static_cast<std::size_t>(st[kHistN]);
        if (n < 3) {
            st[kHist + 2 * n] = t;
            st[kHist + 2 * n + 1] = q;
            st[kHistN] = static_cast<double>(n + 1);
            return;
        }
        for (std::size_t i = 0; i < 2; ++i) {
            st[kHist + 2 * i] = st[kHist + 2 * (i + 1)];
            st[kHist + 2 * i + 1] = st[kHist + 2 * (i + 1) + 1];
        }
        st[kHist + 4] = t;
        st[kHist + 5] = q;
    }

    std::string name_;
    NodeId p_, n_;
    ExprProgram c_;
    bool has_ic_;
    double ic_;
    std::size_t self_index_ = 0;  // filled by link_dynamic
};

// --- L (with optional Rser and pairwise K coupling) ------------------------------

// State layout (state_slots == 16):
//   [0] i_prev  [1] v_prev  [2] phi_prev  companion inputs
//   [3] ka  [4] kb  [5] rhs               companion coefficients
//   [6]         LTE history length (0..3)
//   [7..12]     LTE history (t, phi) x3
//   [13]        [folded L | folded partner L] as two packed halves
// The L values of BOTH inductors of a K pair fold in per step attempt
// (fold_state): the mutual M = k*sqrt(L1*L2) recomputes from the CURRENT
// values, so a .step/Monte-Carlo rebind of either inductor stays exact.
class Inductor final : public Device {
public:
    static constexpr std::size_t kI = 0, kV = 1, kPhi = 2, kKa = 3, kKb = 4, kRhs = 5;
    static constexpr std::size_t kHistN = 6;
    static constexpr std::size_t kHist = 7;
    static constexpr std::size_t kLFold = 13;         // own L mirror
    static constexpr std::size_t kLPartnerFold = 14;  // partner L mirror
    static constexpr std::size_t kPartnerV = 15;      // partner v_prev at last accept

    Inductor(std::string display, NodeId p, NodeId n, NodeId mid, ExprProgram henries,
             double rser, bool has_ic, double ic, BranchId branch)
        : name_(std::move(display)), p_(p), n_(n), mid_(mid), l_(std::move(henries)),
          rser_(rser), has_ic_(has_ic), ic_(ic), branch_(branch) {}

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

    [[nodiscard]] std::vector<ParamSpec> param_specs() const override {
        return {ParamSpec{"value", l_}};
    }
    [[nodiscard]] ws::core::Status check_params(const double* p) const override {
        if (!(p[0] > 0.0) || !std::isfinite(p[0])) {
            return spice_fail(dev_diag(
                ErrorKind::Semantic, "spice.semantic.l_value",
                "inductor " + name_ + " needs a positive finite value", loc));
        }
        return {};
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
        cx.add_entry(row, a(), -cx.st(kKa));
        cx.add_entry(row, b(), cx.st(kKa));
        if (partner_ != nullptr) {
            cx.add_entry(row, partner().a(), -cx.st(kKb));
            cx.add_entry(row, partner().b(), cx.st(kKb));
        }
        cx.add_rhs(row, cx.st(kRhs));
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
        cx.add_entry(row, row, -std::complex<double>(0.0, cx.omega * cx.par(0)));
        if (partner_ != nullptr) {
            cx.add_entry(row, cx.branch_row(partner_->branch()),
                         -std::complex<double>(0.0, cx.omega * m_));
        }
        cx.add_entry(a(), row, 1.0);
        cx.add_entry(b(), row, -1.0);
    }

    [[nodiscard]] bool is_dynamic() const override { return true; }
    [[nodiscard]] std::size_t state_slots() const override { return 16; }
    void fold_state(double* st, const RunState& rs, const Circuit& c) const override;
    [[nodiscard]] ws::core::Status link_dynamic(const Circuit& c,
                                                std::size_t self) override;

    void begin_step(const TranStep& step, double* st) override {
        const double l = st[kLFold];
        const double k = step.be ? step.dt : step.dt / 2.0;
        if (partner_ == nullptr) {
            st[kKa] = k / l;
            st[kKb] = 0.0;
            st[kRhs] = step.be ? st[kI] : (st[kI] + st[kKa] * st[kV]);
            return;
        }
        // Minv = 1/(L1*L2 - M^2) * [L2, -M; -M, L1], M from the CURRENT
        // folded values (a rebind of either L updates the coupling).
        const double lp = st[kLPartnerFold];
        const double m = k_ * std::sqrt(l * lp);
        m_ = m;  // AC-side mirror (mutable; .ac folds before sweeping)
        const double d = l * lp - m * m;
        const double aa = lp / d;
        const double bb = -m / d;
        st[kKa] = k * aa;
        st[kKb] = k * bb;
        st[kRhs] = step.be ? st[kI]
                           : (st[kI] + k * (aa * st[kV] + bb * st[kPartnerV]));
    }
    void commit(const std::vector<double>& x, std::size_t node_count,
                const TranStep& step, double* st) override {
        const std::size_t row = node_count + branch_;
        st[kI] = x[row];
        st[kV] = v_across(x, a(), b());
        if (partner_ != nullptr) {
            st[kPartnerV] = v_across(x, partner().a(), partner().b());
        }
        // Flux linkage phi = L*i + M*i_partner is what the companion
        // integrates (exactly, trap or BE); read it from the solution.
        const double m = (partner_ != nullptr) ? k_ * std::sqrt(st[kLFold] * st[kLPartnerFold]) : 0.0;
        st[kPhi] = st[kLFold] * st[kI];
        if (partner_ != nullptr) {
            st[kPhi] += m * x[node_count + partner_->branch()];
        }
        if (step.be) {
            clear_hist(st);
        } else {
            push_hist(st, step.time, st[kPhi]);
        }
    }
    [[nodiscard]] bool lte_ok(const TranStep& step, const std::vector<double>& x,
                              std::size_t node_count, double reltol, double chgtol,
                              double trtol, const double* st, double* ratio) const override {
        const std::size_t row = node_count + branch_;
        double phi = st[kLFold] * x[row];
        if (partner_ != nullptr) {
            const double m = k_ * std::sqrt(st[kLFold] * st[kLPartnerFold]);
            phi += m * x[node_count + partner_->branch()];
        }
        return dd3_lte(hist_of(st), step.time, phi, step.dt, st[kPhi], reltol, chgtol,
                       trtol, ratio);
    }
    void seed_state(const std::vector<double>& x, std::size_t node_count,
                    double* st) override {
        const std::size_t row = node_count + branch_;
        st[kI] = has_ic_ ? ic_ : x[row];
        st[kV] = v_across(x, a(), b());
        st[kPhi] = st[kLFold] * st[kI];
        clear_hist(st);
    }
    [[nodiscard]] bool has_ic() const override { return has_ic_; }
    [[nodiscard]] double ic_value() const override { return ic_; }

    // --- coupling protocol (resolved by the elaborator) ---------------------
    [[nodiscard]] bool couple_to(Device& partner, double k) override {
        if (partner_ != nullptr || partner.kind() != 'L' || &partner == this) {
            return false;
        }
        partner_ = &partner;
        k_ = k;
        return true;
    }
    [[nodiscard]] double inductance() const override { return bound_l_; }

    [[nodiscard]] double bound_l() const { return bound_l_; }
    void set_bound_l(double v) { bound_l_ = v; }
    [[nodiscard]] NodeId a_node() const { return a(); }
    [[nodiscard]] NodeId b_node() const { return b(); }

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

    [[nodiscard]] static LteHist hist_of(const double* st) {
        LteHist h;
        h.n = static_cast<std::size_t>(st[kHistN]);
        for (std::size_t i = 0; i < 3 && i < h.n; ++i) {
            h.t[i] = st[kHist + 2 * i];
            h.q[i] = st[kHist + 2 * i + 1];
        }
        return h;
    }
    static void clear_hist(double* st) { st[kHistN] = 0.0; }
    static void push_hist(double* st, double t, double q) {
        const std::size_t n = static_cast<std::size_t>(st[kHistN]);
        if (n < 3) {
            st[kHist + 2 * n] = t;
            st[kHist + 2 * n + 1] = q;
            st[kHistN] = static_cast<double>(n + 1);
            return;
        }
        for (std::size_t i = 0; i < 2; ++i) {
            st[kHist + 2 * i] = st[kHist + 2 * (i + 1)];
            st[kHist + 2 * i + 1] = st[kHist + 2 * (i + 1) + 1];
        }
        st[kHist + 4] = t;
        st[kHist + 5] = q;
    }

    std::string name_;
    NodeId p_, n_, mid_;  // mid_ == kGround when no Rser
    ExprProgram l_;
    double bound_l_ = 0.0;  // elaboration-time value (validation + M at couple time)
    double rser_;
    bool has_ic_;
    double ic_;
    BranchId branch_;

    Device* partner_ = nullptr;       // the other Inductor of a K pair
    double k_ = 0.0;                  // coupling factor (signed)
    std::size_t self_index_ = 0;      // filled by link_dynamic
    std::size_t partner_index_ = static_cast<std::size_t>(-1);
    mutable double m_ = 0.0;          // mutual inductance mirror (AC path)
};

// --- fold/link implementations (state mirrors; §3.5b) ---------------------------

void Capacitor::fold_state(double* st, const RunState& rs, const Circuit& c) const {
    st[kCParam] = rs.params()[c.param_span[self_index_].begin];
}

ws::core::Status Capacitor::link_dynamic(const Circuit& c, std::size_t self) {
    self_index_ = self;
    (void)c;
    return {};
}

void Inductor::fold_state(double* st, const RunState& rs, const Circuit& c) const {
    st[kLFold] = rs.params()[c.param_span[self_index_].begin];
    if (partner_ != nullptr && partner_index_ != static_cast<std::size_t>(-1)) {
        st[kLPartnerFold] = rs.params()[c.param_span[partner_index_].begin];
        // The mutual M recomputes from the CURRENT values (the .ac path
        // reads this mirror; begin_step re-derives it the same way).
        m_ = k_ * std::sqrt(st[kLFold] * st[kLPartnerFold]);
    } else {
        st[kLPartnerFold] = 0.0;
    }
}

ws::core::Status Inductor::link_dynamic(const Circuit& c, std::size_t self) {
    self_index_ = self;
    if (partner_ != nullptr) {
        for (std::size_t i = 0; i < c.devices.size(); ++i) {
            if (c.devices[i].get() == partner_) {
                partner_index_ = i;
                return {};
            }
        }
        return spice_fail(dev_diag(
            ErrorKind::Usage, "spice.usage.link",
            "inductor " + name_ + " lost its coupling partner at finalize", loc));
    }
    return {};
}

// --- K (pairwise coupling; the device itself stamps nothing) ---------------------

class CouplingDevice final : public Device {
public:
    explicit CouplingDevice(std::string display) : name_(std::move(display)) {}
    [[nodiscard]] const std::string& name() const override { return name_; }
    [[nodiscard]] char kind() const override { return 'K'; }
    [[nodiscard]] std::vector<NodeId> terminals() const override { return {}; }
    void load_dc(DcLoadContext&) const override {}

    // The resolved pair, for the netlist writer and diagnostics.
    void set_pair(std::string a, std::string b, double k) {
        a_ = std::move(a);
        b_ = std::move(b);
        k_ = k;
    }
    [[nodiscard]] const std::string& pair_a() const { return a_; }
    [[nodiscard]] const std::string& pair_b() const { return b_; }
    [[nodiscard]] double factor() const { return k_; }

private:
    std::string name_;
    std::string a_, b_;
    double k_ = 0.0;
};

// --- S / W switches (§6.2: Ron/Roff conductance model, never a true open) --------

// State layout (state_slots == 1): [0] = on (nonzero when closed).
// Model values (Ron/Roff/Vt/Vh/It/Ih) are immutable construction data.
class SwitchDeviceBase : public Device {
public:
    SwitchDeviceBase(std::string display, NodeId p, NodeId n, bool initial_on)
        : name_(std::move(display)), p_(p), n_(n), initial_on_(initial_on) {}

    [[nodiscard]] const std::string& name() const override { return name_; }
    [[nodiscard]] std::vector<NodeId> terminals() const override { return {p_, n_}; }
    [[nodiscard]] std::vector<std::pair<NodeId, NodeId>> conductance_edges()
        const override {
        return {{p_, n_}};  // both states conduct (Roff is finite)
    }
    void load_dc(DcLoadContext& cx) const override {
        cx.add_conductance(p_, n_, cx.st(0) != 0.0 ? gon_ : goff_);
    }
    void load_tran(DcLoadContext& cx, const TranStep&) const override {
        cx.add_conductance(p_, n_, cx.st(0) != 0.0 ? gon_ : goff_);
    }
    void load_ac(AcLoadContext& cx) const override {
        cx.add_conductance(p_, n_, on_state_ ? gon_ : goff_);
    }

    [[nodiscard]] std::size_t state_slots() const override { return 1; }
    void init_state(double* st) const override { st[0] = initial_on_ ? 1.0 : 0.0; }

    // .model SW (Ron= Roff= Vt= Vh= It= Ih=); S reads Vt/Vh, W reads It/Ih.
    ws::core::Status apply_model(const ModelDef& model) override {
        if (model.type != "sw") {
            return spice_fail(dev_diag(
                ErrorKind::Semantic, "spice.semantic.model_type",
                "switch " + name_ + " needs a SW model, got type '" + model.type + "'",
                model.loc));
        }
        model_name_ = model.name.text;
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

    // The AC linearization freezes wherever the OP settled; the OP solve
    // reports the settled position through this mirror (kept current by
    // update_state, read by load_ac which has no state slice).
    void freeze_ac_state(bool on) const { on_state_ = on; }
    [[nodiscard]] bool model_valid() const { return model_applied_; }

    [[nodiscard]] double ron() const { return ron_; }
    [[nodiscard]] double roff() const { return roff_; }
    [[nodiscard]] const std::string& model_name() const { return model_name_; }

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
    bool initial_on_;
    double ron_ = 1.0, roff_ = 1e12;
    double vt_ = 0.0, vh_ = 0.0, it_ = 0.0, ih_ = 0.0;
    double gon_ = 1.0, goff_ = 1e-12;
    std::string model_name_;
    bool model_applied_ = false;
    mutable bool on_state_ = false;  // AC-side frozen mirror
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
                                    std::size_t node_count, double* st) override {
        (void)node_count;
        const double v = v_across(x, cp_, cn_);
        const bool on = st[0] != 0.0;
        const bool next = hysteresis_flip(on, v, vt_, vh_);
        const bool changed = next != on;
        st[0] = next ? 1.0 : 0.0;
        freeze_ac_state(next);
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
                                    std::size_t node_count, double* st) override {
        const double i = x[node_count + control_];
        const bool on = st[0] != 0.0;
        const bool next = hysteresis_flip(on, i, it_, ih_);
        const bool changed = next != on;
        st[0] = next ? 1.0 : 0.0;
        freeze_ac_state(next);
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
    WS_TRY_ASSIGN(ExprProgram farads, resolve_value_program(card.rest[2], s.params));
    {
        WS_TRY_ASSIGN(const double v, eval_param_for_validation(farads, s.params));
        if (!(v > 0.0) || !std::isfinite(v)) {
            return spice_fail(dev_diag(ErrorKind::Semantic, "spice.semantic.c_value",
                                       "capacitor " + card.name.text +
                                           " needs a positive finite value",
                                       card.loc));
        }
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
    auto dev = std::make_unique<Capacitor>(card.name.text, n[0], n[1], std::move(farads),
                                           has_ic, ic);
    dev->loc = card.loc;
    return dev;
}

ws::core::Result<std::unique_ptr<Device>> make_inductor(const DeviceCard& card,
                                                        FactoryServices& s) {
    if (card.rest.size() < 3) {
        return spice_fail(dev_diag(ErrorKind::Semantic, "spice.semantic.arity",
                                   "inductor " + card.name.text +
                                       " needs two nodes and a value",
                                   card.loc));
    }
    WS_TRY_ASSIGN(ExprProgram henries, resolve_value_program(card.rest[2], s.params));
    double bound_l = 0.0;
    {
        WS_TRY_ASSIGN(bound_l, eval_param_for_validation(henries, s.params));
        if (!(bound_l > 0.0) || !std::isfinite(bound_l)) {
            return spice_fail(dev_diag(ErrorKind::Semantic, "spice.semantic.l_value",
                                       "inductor " + card.name.text +
                                           " needs a positive finite value",
                                       card.loc));
        }
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
                                   "inductor " + card.name.text + " needs Rser >= 0",
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
    auto dev = std::make_unique<Inductor>(card.name.text, n[0], n[1], mid,
                                          std::move(henries), rser, has_ic, ic,
                                          s.nodes.claim_branch());
    dev->set_bound_l(bound_l);
    dev->loc = card.loc;
    return dev;
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
    dev->loc = card.loc;
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
    dev->loc = card.loc;
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
    dev->loc = card.loc;
    s.controls.push_back(ControlRequest{dev.get(), card.rest[2]});
    s.models.push_back(ModelRequest{dev.get(), card.rest[3]});
    return dev;
}

}  // namespace ws::spice
