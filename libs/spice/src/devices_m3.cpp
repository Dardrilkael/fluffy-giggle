// M3 devices (DESIGN.md §3.6, §5, §11): the diode - the first nonlinear
// device. Shockley I = Is*(exp(Vd/(n*Vt)) - 1) + gmin*Vd, hand-differentiated
// (Gd = Is/(n*Vt)*exp(Vd/(n*Vt)) + gmin), linearized as a Norton equivalent
// (Gd in parallel with Ieq = I - Gd*Vd) stamped at the CURRENT iterate
// (§3.6, threaded through DcLoadContext::node_v). pnjlim (SPICE3 devlim,
// §5.3) clamps the per-iteration junction-voltage jump to 2*Vt scale past
// vcrit, preventing exp() overflow when the first Newton iterate wildly
// overshoots. The convergence ladder (gmin / source / PTC homotopies,
// §5.2) is already in the kernel; the diode is what makes the ladder
// load-bearing, exactly as §11 planned.
//
// State layout (state_slots == 3):
//   [0] Vd_prev  - the previous load's Vd (the vnew pnjlim compares to)
//   [1] I_cur    - I at the current load's Vd (for nonlinear_residual)
//   [2] I_prev   - I at the previous load's Vd
// All three zeroed at bind; advanced by update_state once per Newton
// iteration. The diode is NOT dynamic (no companion, no Cjo in M3) -
// is_dynamic() stays false; load_tran defaults to load_dc.
//
// The make_diode factory has external linkage: the builtin table in
// devices.cpp names it alongside the M1/M2 set.

#include "cards.hpp"

#include <algorithm>
#include <cmath>
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

// Voltage across a two-terminal device from the solution vector (ground → 0).
[[nodiscard]] double v_across(const std::vector<double>& x, NodeId p, NodeId n) {
    const double vp = (p == kGround) ? 0.0 : x[p];
    const double vn = (n == kGround) ? 0.0 : x[n];
    return vp - vn;
}

// Boltzmann's constant / electron charge (V/K) - the thermal voltage scale.
constexpr double kVtPerK = 8.617333262145e-5;

// §5.3 pnjlim (SPICE3 dev.c, simplified - the icheck first-time flag is
// folded into the always-directional form; the difference only matters on
// the first clamping iteration of a sequence, and there vold is typically
// 0 so both forms step up by 2*Vt anyway). Clamps the per-iteration ΔVd
// to 2*Vt when vnew crosses vcrit, preventing exp(Vd/nVt) overflow.
[[nodiscard]] double pnjlim(double vnew, double vold, double vt, double vcrit) {
    if (vnew > vcrit && std::abs(vnew - vold) > 2.0 * vt) {
        return vold + ((vnew > vold) ? 2.0 * vt : -2.0 * vt);
    }
    return vnew;
}

// Safe exp argument: clamp to ≤ 80 so exp() stays finite (exp(80) ≈ 5.5e34;
// Is*exp(80) ≈ 5e20 A for Is=1e-14 - absurd but finite, won't NaN the
// solve). pnjlim should keep us well below this in normal operation; the
// clamp is defense against ill-conditioned warm starts.
[[nodiscard]] double safe_exp_arg(double arg) {
    if (arg > 80.0) {
        return 80.0;
    }
    if (arg < -80.0) {
        return -80.0;
    }
    return arg;
}

// --- D (diode, §3.6/§5.3/§11 M3, §6.2 M4b: Cjo charge companion) ------------------

class Diode final : public Device {
public:
    // M3 state slots (always present):
    static constexpr std::size_t kVdPrev = 0;
    static constexpr std::size_t kICur = 1;
    static constexpr std::size_t kIPrev = 2;
    // M4b companion state slots (present when cjo_ > 0):
    static constexpr std::size_t kVComp = 3;   // V at last committed step
    static constexpr std::size_t kQComp = 4;   // Q at last committed step
    static constexpr std::size_t kIComp = 5;   // companion I at last step
    static constexpr std::size_t kGeq = 6;     // companion conductance (per attempt)
    static constexpr std::size_t kJeq = 7;     // companion current source
    static constexpr std::size_t kHistN = 8;   // LTE history length (0..3)
    static constexpr std::size_t kHist = 9;    // first (t, q) pair, 6 slots
    static constexpr std::size_t kCjoEff = 15; // folded Cjo*area

    Diode(std::string display, NodeId anode, NodeId cathode, ExprProgram area,
          bool has_ic, double ic)
        : name_(std::move(display)), anode_(anode), cathode_(cathode),
          area_(std::move(area)), has_ic_(has_ic), ic_(ic) {}

    [[nodiscard]] const std::string& name() const override { return name_; }
    [[nodiscard]] char kind() const override { return 'D'; }
    [[nodiscard]] std::vector<NodeId> terminals() const override {
        return {anode_, cathode_};
    }
    // The diode is a conductance path between its terminals (Gd is always
    // finite and nonzero - at Vd=0, Gd = Is/(n*Vt) + gmin, ~4e-13 S for
    // defaults; tiny but a real DC path, matching ngspice's graph check).
    [[nodiscard]] std::vector<std::pair<NodeId, NodeId>> conductance_edges()
        const override {
        return {{anode_, cathode_}};
    }

    // --- the stamp (§3.6: linearize at the current iterate) --------------
    void load_dc(DcLoadContext& cx) const override {
        const double area = cx.par(kAreaParam);
        const double is_eff = is_ * area;
        const double vd = cx.node_v(anode_) - cx.node_v(cathode_);
        const double vd_lim = pnjlim(vd, cx.st(kVdPrev), vt_, vcrit_);
        const double arg = safe_exp_arg(vd_lim / (n_ * vt_));
        const double ex = std::exp(arg);
        const double gm = cx.gmin();
        const double gd = is_eff / (n_ * vt_) * ex + gm;
        const double i = is_eff * (ex - 1.0) + gm * vd_lim;
        const double ieq = i - gd * vd_lim;  // Norton offset
        // Conductance gd between anode and cathode; current source ieq
        // flowing anode→cathode (the diode's forward-current direction).
        cx.add_conductance(anode_, cathode_, gd);
        cx.add_current(cathode_, anode_, ieq);
    }

    // M4b: when Cjo > 0, load_tran stamps the DC Shockley part PLUS the
    // charge companion (geq, jeq from begin_step). The companion uses
    // the same Norton form as the linear C: geq in parallel with jeq.
    void load_tran(DcLoadContext& cx, const TranStep&) const override {
        load_dc(cx);  // DC Shockley stamp
        if (!has_cap()) return;
        // Charge companion (trap/BE, same as the linear C in devices_m2.cpp).
        cx.add_conductance(anode_, cathode_, cx.st(kGeq));
        cx.add_current(cathode_, anode_, cx.st(kJeq));
    }

    // The small-signal conductance at the OP (frozen Vd from state[kVdPrev]).
    // M4b: adds j*omega*Cj(Vd) — the junction capacitance at the OP.
    void load_ac(AcLoadContext& cx) const override {
        const double area = cx.par(kAreaParam);
        const double is_eff = is_ * area;
        const double vd = cx.st(kVdPrev);
        const double arg = safe_exp_arg(vd / (n_ * vt_));
        const double ex = std::exp(arg);
        const double gd_op = is_eff / (n_ * vt_) * ex;
        cx.add_conductance(anode_, cathode_, {gd_op, 0.0});
        if (has_cap()) {
            const double cj = c_dep(vd, cjo_ * area);
            cx.add_conductance(anode_, cathode_, {0.0, cx.omega * cj});
        }
    }

    // --- parameter model (§3.5a) -----------------------------------------
    [[nodiscard]] std::vector<ParamSpec> param_specs() const override {
        return {ParamSpec{"area", area_}};
    }
    [[nodiscard]] ws::core::Status check_params(const double* p) const override {
        if (!(p[kAreaParam] > 0.0) || !std::isfinite(p[kAreaParam])) {
            return spice_fail(dev_diag(
                ErrorKind::Semantic, "spice.semantic.d_area",
                "diode " + name_ + " needs a positive finite area", loc));
        }
        return {};
    }

    // --- state model (§3.5b) ---------------------------------------------
    // M3: 3 slots (Vd_prev, I_cur, I_prev). M4b: 16 slots when Cjo > 0
    // (adds companion state + LTE history + folded Cjo*area).
    [[nodiscard]] std::size_t state_slots() const override {
        return has_cap() ? 16 : 3;
    }
    [[nodiscard]] bool is_dynamic() const override { return has_cap(); }

    // Folds Cjo*area into state[kCjoEff] once per step attempt (same
    // pattern as the Capacitor's kCParam fold). link_dynamic sets self_index_.
    void fold_state(double* st, const RunState& rs, const Circuit& c) const override {
        if (has_cap()) {
            st[kCjoEff] = cjo_ * rs.params()[c.param_span[self_index_].begin];
        }
    }
    [[nodiscard]] ws::core::Status link_dynamic(const Circuit& /*c*/,
                                                 std::size_t self) override {
        self_index_ = self;
        return {};
    }
    // init_state defaults to zero, which is correct: Vd_prev=0 (cold start),
    // I_cur=I_prev=0 (no previous load).

    // Advances the pnjlim reference and the residual trackers. §5.3: the
    // vold stored for the NEXT iteration's pnjlim must be the CLAMPED Vd
    // (Vd_lim), not the raw iterate - otherwise pnjlim compares the new
    // iterate to the previous overshoot and never clamps (the diode gets
    // stuck at Vd=100V from the first Newton step). Both load_dc and
    // update_state compute Vd_lim = pnjlim(Vd(x_prev), vold, ...) against
    // the same vold, so they agree; update_state stores Vd_lim as vold for
    // the next iteration. Returns false: the diode has no discrete state.
    [[nodiscard]] bool update_state(const std::vector<double>& x,
                                    std::size_t /*node_count*/, double* st,
                                    const std::vector<double>* x_prev = nullptr) override {
        const std::vector<double>& x_load =
            (x_prev != nullptr && !x_prev->empty()) ? *x_prev : x;
        const double vd_load = v_across(x_load, anode_, cathode_);
        const double vd_lim = pnjlim(vd_load, st[kVdPrev], vt_, vcrit_);
        // §5.3: return true (force another iteration) when pnjlim is
        // actively clamping — the linearization point differs from the
        // iterate, so convergence would be premature. Without this, a
        // stiff cold start (e.g. Vd=12V) falsely converges at Vd_lim=0.05
        // because I(Vd_lim) is below abstol and the residual check passes.
        const bool clamped = vd_lim != vd_load;
        // Track I at the clamped linearization point (the "current
        // predicted by the linearization", §5.1). Using is_ (area=1) is a
        // minor approximation when area≠1; the voltage-delta check
        // compensates, and the Lambert-W gate uses area=1 anyway.
        const double arg = safe_exp_arg(vd_lim / (n_ * vt_));
        const double i_lim = is_ * (std::exp(arg) - 1.0);
        st[kIPrev] = st[kICur];
        st[kICur] = i_lim;
        st[kVdPrev] = vd_lim;  // CLAMPED, for the NEXT iteration's pnjlim vold
        return clamped;
    }

    // §5.1: |ΔI| since the previous load, normalized by max(|I_cur|,|I_prev|).
    [[nodiscard]] NonlinResid nonlinear_residual(const double* st) const override {
        const double i_cur = st[kICur];
        const double i_prev = st[kIPrev];
        return {std::abs(i_cur - i_prev), std::max(std::abs(i_cur), std::abs(i_prev))};
    }

    // --- M4b dynamic hooks (charge companion, only when Cjo > 0) ----------
    // Same pattern as the linear C in devices_m2.cpp: begin_step computes
    // geq/jeq from the last committed step's V/Q/I; load_tran stamps them;
    // commit stores the new V/Q/I and pushes LTE history; lte_ok uses the
    // divided-difference LTE on the charge history.

    void begin_step(const TranStep& step, double* st) override {
        if (!has_cap()) return;
        const double cjo_eff = st[kCjoEff];
        // Capacitance at the last committed V (begin_step runs once per
        // step attempt, before the Newton loop - it can't see the current
        // iterate, so it uses V_comp from the last commit).
        const double c = c_dep(st[kVComp], cjo_eff);
        if (step.be) {
            st[kGeq] = c / step.dt;
            st[kJeq] = -st[kQComp] / step.dt;
        } else {
            st[kGeq] = 2.0 * c / step.dt;
            st[kJeq] = -(st[kGeq] * st[kVComp] + st[kIComp]);
        }
    }

    void commit(const std::vector<double>& x, std::size_t /*nc*/,
                const TranStep& step, double* st) override {
        if (!has_cap()) return;
        const double cjo_eff = st[kCjoEff];
        const double vd = v_across(x, anode_, cathode_);
        const double q = q_dep(vd, cjo_eff);
        st[kVComp] = vd;
        st[kQComp] = q;
        st[kIComp] = st[kGeq] * vd + st[kJeq];
        // LTE history (3 (t, q) pairs, oldest first).
        if (step.be) {
            st[kHistN] = 0.0;  // BE restart invalidates trap LTE history
        } else {
            const double n = st[kHistN];
            if (n < 3.0) {
                const std::size_t idx = static_cast<std::size_t>(n);
                st[kHist + idx * 2] = step.time;
                st[kHist + idx * 2 + 1] = q;
                st[kHistN] = n + 1.0;
            } else {
                // Shift: drop oldest, append newest.
                for (std::size_t i = 0; i < 2; ++i) {
                    st[kHist + i * 2] = st[kHist + (i + 1) * 2];
                    st[kHist + i * 2 + 1] = st[kHist + (i + 1) * 2 + 1];
                }
                st[kHist + 4] = step.time;
                st[kHist + 5] = q;
            }
        }
    }

    [[nodiscard]] bool lte_ok(const TranStep& step, const std::vector<double>& x,
                              std::size_t /*nc*/, double reltol, double chgtol,
                              double trtol, const double* st, double* ratio) const override {
        if (!has_cap()) {
            if (ratio) *ratio = 0.0;
            return true;
        }
        const double cjo_eff = st[kCjoEff];
        const double vd = v_across(x, anode_, cathode_);
        const double q_new = q_dep(vd, cjo_eff);
        // Reuse the dd3_lte logic from devices_m2.cpp (inline here since
        // it's in a different TU's anonymous namespace).
        const double n = st[kHistN];
        if (n < 3.0) {
            if (ratio) *ratio = 0.0;
            return true;  // not enough history
        }
        const double t0 = st[kHist], q0 = st[kHist + 1];
        const double t1 = st[kHist + 2], q1 = st[kHist + 3];
        const double t2 = st[kHist + 4], q2 = st[kHist + 5];
        const double t3 = step.time, q3 = q_new;
        if (!(t1 > t0) || !(t2 > t1) || !(t3 > t2) || !(step.dt > 0)) {
            if (ratio) *ratio = 0.0;
            return true;
        }
        const double d1a = (q1 - q0) / (t1 - t0);
        const double d1b = (q2 - q1) / (t2 - t1);
        const double d1c = (q3 - q2) / (t3 - t2);
        const double d2a = (d1b - d1a) / (t2 - t0);
        const double d2b = (d1c - d1b) / (t3 - t1);
        const double dd3 = (d2b - d2a) / (t3 - t0);
        const double eps = step.dt * step.dt * step.dt * std::abs(dd3) / 2.0;
        const double tolq = trtol * (reltol * std::max(std::abs(q3), std::abs(st[kQComp])) + chgtol);
        if (ratio) *ratio = (tolq > 0) ? (eps / tolq) : std::numeric_limits<double>::infinity();
        return tolq > 0 && eps <= tolq;
    }

    void seed_state(const std::vector<double>& x, std::size_t /*nc*/,
                    double* st) override {
        if (!has_cap()) return;
        const double vd = v_across(x, anode_, cathode_);
        st[kVComp] = vd;
        st[kQComp] = q_dep(vd, st[kCjoEff]);
        st[kIComp] = 0.0;
        st[kHistN] = 0.0;
    }

    // .model name D(Is=... N=... Rs=... Cjo=... ...). M3 implements Is, N,
    // Tnom (for Vt/vcrit); accepts Rs/Cjo/M/Vj/Bv/Ibv/Xti/Eg/Kf/Af/Fc
    // silently (warned once per model in `spice check`) - they land with
    // M4 (Cjo/M/Vj for the junction cap) or M5 (Tnom/Xti/Eg temp model,
    // Kf/Af noise, Bv/Ibv breakdown).
    ws::core::Status apply_model(const ModelDef& model) override {
        if (model.type != "d") {
            return spice_fail(dev_diag(
                ErrorKind::Semantic, "spice.semantic.model_type",
                "diode " + name_ + " needs a D model, got type '" + model.type + "'",
                model.loc));
        }
        model_name_ = model.name.text;
        // ngspice defaults: Is=1e-14 A, N=1, Tnom=27°C, Cjo=0 (no cap),
        // M=0.5, Vj=1V, Fc=0.5.
        double is_val = 1e-14;
        double n_val = 1.0;
        double tnom = 27.0;
        double cjo_val = 0.0, m_val = 0.5, vj_val = 1.0, fc_val = 0.5;
        const auto& toks = model.params;
        for (std::size_t i = 0; i < toks.size(); ++i) {
            std::string text = toks[i].text;
            // Paren glue (same as switches): "(Is=1e-14" / "N=1)".
            while (!text.empty() && text.front() == '(') {
                text.erase(text.begin());
            }
            while (!text.empty() && text.back() == ')') {
                text.pop_back();
            }
            if (text.empty()) {
                continue;
            }
            std::string key;
            std::string value;
            const std::size_t eq = text.find('=');
            if (eq != std::string::npos && eq > 0) {
                key = to_lower(text.substr(0, eq));
                value = text.substr(eq + 1);
            } else {
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
            if (key == "is") {
                is_val = v;
            } else if (key == "n") {
                n_val = v;
            } else if (key == "tnom") {
                tnom = v;
            } else if (key == "cjo") {
                cjo_val = v;
            } else if (key == "m") {
                m_val = v;
            } else if (key == "vj") {
                vj_val = v;
            } else if (key == "fc") {
                fc_val = v;
            } else if (key == "rs" || key == "bv" || key == "ibv" ||
                       key == "xti" || key == "eg" || key == "kf" ||
                       key == "af" || key == "tt") {
                // Accepted (so ngspice decks parse cleanly) but not
                // implemented in M4b. `spice check` warns per-model.
                // TT (diffusion charge) lands M4c; Rs/Bv/Ibv/Xti/Eg/Kf/Af land M5.
            } else {
                return spice_fail(dev_diag(
                    ErrorKind::Semantic, "spice.semantic.model_param",
                    "model '" + model.name.text + "': unknown parameter '" + key +
                        "' (Is, N, Tnom, Rs, Cjo, M, Vj, Bv, Ibv, Xti, Eg, Kf, Af, Fc)",
                    toks[i].loc));
            }
        }
        if (!(is_val > 0.0) || !std::isfinite(is_val)) {
            return spice_fail(dev_diag(
                ErrorKind::Semantic, "spice.semantic.d_is",
                "model '" + model.name.text + "' needs Is > 0", model.loc));
        }
        if (!(n_val > 0.0) || !std::isfinite(n_val)) {
            return spice_fail(dev_diag(
                ErrorKind::Semantic, "spice.semantic.d_n",
                "model '" + model.name.text + "' needs N > 0", model.loc));
        }
        is_ = is_val;
        n_ = n_val;
        // Vt at Tnom. M5 will re-evaluate per-run against TEMP; M3/M4b freezes
        // at Tnom (the Lambert-W gate uses the default 27°C).
        vt_ = (tnom + 273.15) * kVtPerK;
        // SPICE3 vcrit = N*Vt*ln(N*Vt / (sqrt(2)*Is)). With Is=1e-14, N=1,
        // Tnom=27: vcrit ≈ 0.728 V - the knee past which pnjlim engages.
        const double arg = n_ * vt_ / (std::sqrt(2.0) * is_val);
        vcrit_ = (arg > 1.0) ? n_ * vt_ * std::log(arg) : n_ * vt_;
        // M4b: junction capacitance params.
        cjo_ = cjo_val;
        m_ = m_val;
        vj_ = vj_val;
        fc_ = fc_val;
        return {};
    }

    [[nodiscard]] bool has_ic() const override { return has_ic_; }
    [[nodiscard]] double ic_value() const override { return ic_; }

    [[nodiscard]] bool write_card(std::string& out,
                                  const CardWriter& w) const override {
        out += name_ + " " + w.node(anode_) + " " + w.node(cathode_) + " " + model_name_;
        const double area = w.params(*this)[kAreaParam];
        // Omit area when it's exactly 1 (the ngspice default) - keeps the
        // netlist canonical.
        if (area != 1.0) {
            out += " " + Device::CardWriter::num(area);
        }
        if (has_ic_) {
            out += " ic=" + Device::CardWriter::num(ic_);
        }
        out += "\n";
        return true;
    }

private:
    static constexpr std::size_t kAreaParam = 0;

    // --- M4b: junction capacitance (Cjo) charge model -------------------
    // True when the diode has a junction cap (Cjo > 0) - gates is_dynamic,
    // state_slots, and all the companion hooks.
    [[nodiscard]] bool has_cap() const { return cjo_ > 0.0; }

    // Depletion charge Q_dep(Vd) and capacitance C_dep(Vd), using the
    // SPICE3/ngspice depletion model with the Fc linear extension past
    // Fc*Vj. cjo_eff = Cjo*area (folded into state[kCjoEff]).
    [[nodiscard]] double q_dep(double vd, double cjo_eff) const {
        if (cjo_eff <= 0.0) return 0.0;
        const double fc_vj = fc_ * vj_;
        if (vd < fc_vj) {
            double arg = 1.0 - vd / vj_;
            if (arg < 1e-12) arg = 1e-12;  // avoid singularity at Vd=Vj
            return cjo_eff * vj_ / (1.0 - m_) * (1.0 - std::pow(arg, 1.0 - m_));
        }
        // Linear extension past Fc*Vj (SPICE3 formula).
        const double f1 = vj_ * (1.0 - std::pow(1.0 - fc_, 1.0 - m_)) / (1.0 - m_);
        const double f2 = std::pow(1.0 - fc_, -m_);
        const double q_fcvj = cjo_eff * f1;
        const double c_fcvj = cjo_eff * f2;
        const double dv = vd - fc_vj;
        return q_fcvj + c_fcvj * dv * (1.0 + m_ * dv / (2.0 * vj_ * (1.0 - fc_)));
    }
    [[nodiscard]] double c_dep(double vd, double cjo_eff) const {
        if (cjo_eff <= 0.0) return 0.0;
        const double fc_vj = fc_ * vj_;
        if (vd < fc_vj) {
            double arg = 1.0 - vd / vj_;
            if (arg < 1e-12) arg = 1e-12;
            return cjo_eff / std::pow(arg, m_);
        }
        const double f2 = std::pow(1.0 - fc_, -m_);
        const double dv = vd - fc_vj;
        return cjo_eff * f2 * (1.0 + m_ * dv / (vj_ * (1.0 - fc_)));
    }

    std::string name_;
    NodeId anode_, cathode_;
    ExprProgram area_;
    bool has_ic_;
    double ic_;
    // Model fields (set by apply_model):
    double is_ = 1e-14;     // saturation current (per area=1)
    double n_ = 1.0;        // emission coefficient
    double vt_ = 0.02585;   // thermal voltage at Tnom (V)
    double vcrit_ = 0.7;    // pnjlim knee (V)
    // M4b: junction capacitance model params (Cjo > 0 enables the companion)
    double cjo_ = 0.0;      // zero-bias junction capacitance (F)
    double m_ = 0.5;        // grading coefficient
    double vj_ = 1.0;       // junction potential (V)
    double fc_ = 0.5;       // forward-bias depletion cap coefficient
    std::size_t self_index_ = 0;  // filled by link_dynamic (for fold_state)
    std::string model_name_;
};

}  // namespace

ws::core::Result<std::unique_ptr<Device>> make_diode(const DeviceCard& card,
                                                     FactoryServices& s) {
    // D name anode cathode model [area] [off] [ic=Vd] [temp=...]
    WS_TRY_ASSIGN(const std::vector<NodeId> n, take_nodes(card, s.nodes, 2, 0));
    if (card.rest.size() < 3) {
        return spice_fail(dev_diag(ErrorKind::Semantic, "spice.semantic.arity",
                                   "diode " + card.name.text +
                                       " takes two nodes and a model name",
                                   card.loc));
    }
    ExprProgram area_prog;
    bool has_area = false;
    bool has_ic = false;
    double ic_val = 0.0;
    for (std::size_t i = 3; i < card.rest.size(); ++i) {
        const std::string lower = to_lower(card.rest[i].text);
        if (lower.rfind("ic=", 0) == 0) {
            has_ic = true;
            if (const ws::core::Result<double> r =
                    parse_number(card.rest[i].text.substr(3))) {
                ic_val = *r;
            }
            continue;
        }
        if (lower == "off") {
            // ngspice: start the diode in cutoff (Vd=0 init). With a cold
            // start (x=0) this is already the default; accept and ignore.
            continue;
        }
        if (lower.rfind("temp=", 0) == 0) {
            // Per-instance temp override lands with M5 (temp-dependent
            // models). Accept and ignore in M3.
            continue;
        }
        if (!has_area) {
            // The first non-keyword token after the model name is `area`.
            WS_TRY_ASSIGN(area_prog, resolve_value_program(card.rest[i], s.params));
            WS_TRY(eval_param_for_validation(area_prog, s.params));
            has_area = true;
            continue;
        }
        return spice_fail(dev_diag(
            ErrorKind::Semantic, "spice.semantic.d_extra",
            "diode " + card.name.text +
                " takes nodes, a model, an optional area, ic=Vd, off, temp=",
            card.loc));
    }
    if (!has_area) {
        // Default area = 1.0.
        Token one;
        one.text = "1.0";
        WS_TRY_ASSIGN(area_prog, resolve_value_program(one, s.params));
    }
    auto dev = std::make_unique<Diode>(card.name.text, n[0], n[1], std::move(area_prog),
                                       has_ic, ic_val);
    dev->loc = card.loc;
    s.models.push_back(ModelRequest{dev.get(), card.rest[2]});
    return dev;
}

}  // namespace ws::spice
