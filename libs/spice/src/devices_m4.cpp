// M4 devices (DESIGN.md §3.6, §5, §11): the BJT - Ebers-Moll injection
// model, the first multi-junction nonlinear device. Two pn-junctions (BE
// and BC) modeled as Shockley diodes (reusing the M3 diode's pnjlim +
// Norton linearization), plus two current-controlled current sources
// (the forward αF and reverse αR transport) that couple them. The model
// is complete for all four operating regions (forward-active, saturation,
// cutoff, reverse-active) - Gummel-Poon refinements (Early effect, high-
// current β roll-off, junction caps) land in M4b/M4c.
//
// Ebers-Moll (NPN, "into the terminal" current convention):
//   Ibe = Is/αF * (exp(Vbe/(Nf*Vt)) - 1)    [BE diode, B→E]
//   Ibc = Is/αR * (exp(Vbc/(Nr*Vt)) - 1)    [BC diode, B→C]
//   Ic  = αF*Ibe - Ibc                       [into collector]
//   Ie  = -Ibe + αR*Ibc                      [into emitter]
//   Ib  = Ibe*(1-αF) + Ibc*(1-αR)            [into base]
//   αF = Bf/(Bf+1), αR = Br/(Br+1)
//
// PNP: same equations with Vbe/Vbc negated (Vbe_eff = Ve-Vb, Vbc_eff =
// Vc-Vb) and terminal currents negated. The conductance matrix entries
// are IDENTICAL for NPN and PNP at the same |Vbe|,|Vbc| (the small-
// signal behavior is polarity-independent); only the RHS (current source)
// signs flip with `sgn = pnp ? -1 : +1`.
//
// State layout (state_slots == 4):
//   [0] Vbe_prev  - BE junction's pnjlim vold (the clamped Vbe)
//   [1] Vbc_prev  - BC junction's pnjlim vold (the clamped Vbc)
//   [2] Ic_cur    - collector current at the current load (for residual)
//   [3] Ic_prev   - collector current at the previous load
// All zeroed at bind; advanced by update_state once per Newton iteration.
// The BJT is NOT dynamic in M4a (no junction caps - those land M4b);
// is_dynamic() stays false; load_tran defaults to load_dc.
//
// The make_bjt factory has external linkage: the builtin table in
// devices.cpp names it alongside the M1/M2/M3 set.

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

// Boltzmann/q in V/K (matches devices_m3.cpp's kVtPerK).
constexpr double kVtPerK = 8.617333262145e-5;

// §5.3 pnjlim (same simplified form as the diode - the icheck first-time
// flag folded into the always-directional step). Clamps |ΔV| to 2*Vt past
// vcrit, preventing exp() overflow on stiff cold starts.
[[nodiscard]] double pnjlim(double vnew, double vold, double vt, double vcrit) {
    if (vnew > vcrit && std::abs(vnew - vold) > 2.0 * vt) {
        return vold + ((vnew > vold) ? 2.0 * vt : -2.0 * vt);
    }
    return vnew;
}

[[nodiscard]] double safe_exp_arg(double arg) {
    if (arg > 80.0) {
        return 80.0;
    }
    if (arg < -80.0) {
        return -80.0;
    }
    return arg;
}

// --- Q (BJT, §3.6/§5.3/§11 M4a: Ebers-Moll) -------------------------------------

class Bjt final : public Device {
public:
    // M4a state slots (always present):
    static constexpr std::size_t kVbePrev = 0;
    static constexpr std::size_t kVbcPrev = 1;
    static constexpr std::size_t kICur = 2;
    static constexpr std::size_t kIPrev = 3;
    // M4b BE junction companion (when Cje > 0):
    static constexpr std::size_t kVbeComp = 4;
    static constexpr std::size_t kQbeComp = 5;
    static constexpr std::size_t kIbeComp = 6;
    static constexpr std::size_t kGeqBe = 7;
    static constexpr std::size_t kJeqBe = 8;
    // M4b BC junction companion (when Cjc > 0):
    static constexpr std::size_t kVbcComp = 9;
    static constexpr std::size_t kQbcComp = 10;
    static constexpr std::size_t kIbcComp = 11;
    static constexpr std::size_t kGeqBc = 12;
    static constexpr std::size_t kJeqBc = 13;
    // LTE history (3 (t,q) pairs per junction):
    static constexpr std::size_t kHistNBe = 14;
    static constexpr std::size_t kHistBe = 15;   // 6 slots
    static constexpr std::size_t kHistNBc = 21;
    static constexpr std::size_t kHistBc = 22;   // 6 slots
    // Folded params:
    static constexpr std::size_t kCjeEff = 28;
    static constexpr std::size_t kCjcEff = 29;

    Bjt(std::string display, NodeId c, NodeId b, NodeId e,
        ExprProgram area, bool has_ic, double ic_vbe, double ic_vbc)
        : name_(std::move(display)), c_(c), b_(b), e_(e),
          area_(std::move(area)), has_ic_(has_ic), ic_vbe_(ic_vbe), ic_vbc_(ic_vbc) {}

    [[nodiscard]] const std::string& name() const override { return name_; }
    [[nodiscard]] char kind() const override { return 'Q'; }
    [[nodiscard]] std::vector<NodeId> terminals() const override {
        return {c_, b_, e_};
    }
    // The BJT is a conductance path between all three terminals (both
    // junctions have finite Gd at any operating point - at Vbe=Vbc=0,
    // Gd_be ≈ Is/(αF*Nf*Vt), tiny but real). The graph check treats it
    // as connecting B to E and B to C, matching ngspice.
    [[nodiscard]] std::vector<std::pair<NodeId, NodeId>> conductance_edges()
        const override {
        return {{b_, e_}, {b_, c_}};
    }

    // --- the stamp (§3.6: linearize at the current iterate) --------------
    // Direct terminal-current stamp (transport model). The BJT pulls Ic,
    // Ib, Ie from the C, B, E nodes (current flows INTO the terminals).
    // We stamp the terminal currents' derivatives into G (current leaving
    // the node = current into the BJT) and the Norton offsets into rhs.
    // The G entries are polarity-independent (dIc/dVb is the same for NPN
    // and PNP at the same |Vbe|); only the RHS constant parts flip with
    // sgn. This avoids the sign pitfalls of the diode+CCCS decomposition
    // (where minority-carrier currents flow low→high, breaking add_conductance's
    // high→low assumption).
    void load_dc(DcLoadContext& cx) const override {
        const double area = cx.par(kAreaParam);
        const double is_eff = is_ * area;
        const double sgn = pnp_ ? -1.0 : 1.0;
        const double vbe_raw = sgn * (cx.node_v(b_) - cx.node_v(e_));
        const double vbc_raw = sgn * (cx.node_v(b_) - cx.node_v(c_));
        const double vbe = pnjlim(vbe_raw, cx.st(kVbePrev), nf_ * vt_, vcrit_be_);
        const double vbc = pnjlim(vbc_raw, cx.st(kVbcPrev), nr_ * vt_, vcrit_bc_);
        const double arg_be = safe_exp_arg(vbe / (nf_ * vt_));
        const double ex_be = std::exp(arg_be);
        const double arg_bc = safe_exp_arg(vbc / (nr_ * vt_));
        const double ex_bc = std::exp(arg_bc);
        const double gm = cx.gmin();
        // Transport terminal currents (into the terminals, sgn-scaled):
        //   Ic = Is*(ex_be - ex_bc)
        //   Ib = Is/Bf*(ex_be-1) + Is/Br*(ex_bc-1)  + gmin*(Vbe+Vbc)
        //   Ie = -(Ic + Ib)
        const double ic = sgn * is_eff * (ex_be - ex_bc);
        const double ib = sgn * (is_eff / bf_ * (ex_be - 1.0) +
                                  is_eff / br_ * (ex_bc - 1.0) +
                                  gm * (vbe + vbc));
        const double ie = -(ic + ib);
        // Derivatives w.r.t. the EFFECTIVE junction voltages:
        //   dIc/dVbe_eff = Is/(Nf*Vt)*ex_be = gm_f
        //   dIc/dVbc_eff = -Is/(Nr*Vt)*ex_bc = -gm_r
        //   dIb/dVbe_eff = Is/(Bf*Nf*Vt)*ex_be + gmin = gd_ib_f
        //   dIb/dVbc_eff = Is/(Br*Nr*Vt)*ex_bc + gmin = gd_ib_r
        const double gm_f = is_eff / (nf_ * vt_) * ex_be;
        const double gm_r = is_eff / (nr_ * vt_) * ex_bc;
        const double gd_ib_f = is_eff / (bf_ * nf_ * vt_) * ex_be + gm;
        const double gd_ib_r = is_eff / (br_ * nr_ * vt_) * ex_bc + gm;
        // Chain rule to node voltages. For NPN: Vbe_eff=Vb-Ve, Vbc_eff=Vb-Vc.
        // For PNP: Vbe_eff=Ve-Vb, Vbc_eff=Vc-Vb. The sgn factors cancel
        // (d(sgn*Ic)/dVb = sgn * dIc/dVbe_eff * dVbe_eff/dVb = sgn*gm_f*sgn = gm_f),
        // so dIc/dVb is the same for both polarities.
        const double dic_dvb = gm_f - gm_r;
        const double dic_dvc = gm_r;          // dIc/dVc = -(-gm_r) = gm_r
        const double dic_dve = -gm_f;         // dIc/dVe = gm_f*(-1) = -gm_f
        const double dib_dvb = gd_ib_f + gd_ib_r;
        const double dib_dvc = -gd_ib_r;
        const double dib_dve = -gd_ib_f;
        const double die_dvb = -(dic_dvb + dib_dvb);
        const double die_dvc = -(dic_dvc + dib_dvc);
        const double die_dve = -(dic_dve + dib_dve);
        // Norton offsets: the constant part of each terminal current after
        // subtracting the V-dependent (G matrix) part.
        const double ic_eq = ic - (dic_dvb * cx.node_v(b_) + dic_dvc * cx.node_v(c_) +
                                    dic_dve * cx.node_v(e_));
        const double ib_eq = ib - (dib_dvb * cx.node_v(b_) + dib_dvc * cx.node_v(c_) +
                                    dib_dve * cx.node_v(e_));
        const double ie_eq = ie - (die_dvb * cx.node_v(b_) + die_dvc * cx.node_v(c_) +
                                    die_dve * cx.node_v(e_));
        // G matrix: "current leaving node" = current into the BJT terminal.
        // The derivatives are d(terminal current)/dV, stamped directly.
        cx.add_entry(c_, b_, dic_dvb);
        cx.add_entry(c_, c_, dic_dvc);
        cx.add_entry(c_, e_, dic_dve);
        cx.add_entry(b_, b_, dib_dvb);
        cx.add_entry(b_, c_, dib_dvc);
        cx.add_entry(b_, e_, dib_dve);
        cx.add_entry(e_, b_, die_dvb);
        cx.add_entry(e_, c_, die_dvc);
        cx.add_entry(e_, e_, die_dve);
        // RHS: the BJT PULLS the terminal currents from the nodes (current
        // into the BJT = current leaving the node). rhs = current INJECTED
        // = -(current leaving) = -(terminal current). The V-dependent part
        // is in G; the constant part (Norton offset) goes here.
        cx.add_rhs(c_, -ic_eq);
        cx.add_rhs(b_, -ib_eq);
        cx.add_rhs(e_, -ie_eq);
    }

    // M4a: no junction caps (Cje/Cjc land M4b). load_tran = load_dc.
    // load_ac: small-signal conductances at the OP (frozen Vbe/Vbc from
    // state, set by update_state during the .ac OP solve).
    void load_ac(AcLoadContext& cx) const override {
        const double area = cx.par(kAreaParam);
        const double is_eff = is_ * area;
        const double vbe = cx.st(kVbePrev);
        const double vbc = cx.st(kVbcPrev);
        const double arg_be = safe_exp_arg(vbe / (nf_ * vt_));
        const double ex_be = std::exp(arg_be);
        const double arg_bc = safe_exp_arg(vbc / (nr_ * vt_));
        const double ex_bc = std::exp(arg_bc);
        const double gm_f = is_eff / (nf_ * vt_) * ex_be;
        const double gm_r = is_eff / (nr_ * vt_) * ex_bc;
        const double gd_ib_f = is_eff / (bf_ * nf_ * vt_) * ex_be;
        const double gd_ib_r = is_eff / (br_ * nr_ * vt_) * ex_bc;
        // Same G entries as load_dc (real only - no caps in M4a).
        const double dic_dvb = gm_f - gm_r, dic_dvc = gm_r, dic_dve = -gm_f;
        const double dib_dvb = gd_ib_f + gd_ib_r, dib_dvc = -gd_ib_r, dib_dve = -gd_ib_f;
        const double die_dvb = -(dic_dvb + dib_dvb);
        const double die_dvc = -(dic_dvc + dib_dvc);
        const double die_dve = -(dic_dve + dib_dve);
        cx.add_entry(c_, b_, {dic_dvb, 0.0});
        cx.add_entry(c_, c_, {dic_dvc, 0.0});
        cx.add_entry(c_, e_, {dic_dve, 0.0});
        cx.add_entry(b_, b_, {dib_dvb, 0.0});
        cx.add_entry(b_, c_, {dib_dvc, 0.0});
        cx.add_entry(b_, e_, {dib_dve, 0.0});
        cx.add_entry(e_, b_, {die_dvb, 0.0});
        cx.add_entry(e_, c_, {die_dvc, 0.0});
        cx.add_entry(e_, e_, {die_dve, 0.0});
        // M4b: junction caps Cje(Vbe_op) and Cjc(Vbc_op) as j*omega*C.
        if (has_caps()) {
            const double area = cx.par(kAreaParam);
            if (cje_ > 0.0) {
                const double cje_eff = cje_ * area;
                const double cj_be = c_dep_be(vbe, cje_eff);
                cx.add_conductance(b_, e_, {0.0, cx.omega * cj_be});
            }
            if (cjc_ > 0.0) {
                const double cjc_eff = cjc_ * area;
                const double cj_bc = c_dep_bc(vbc, cjc_eff);
                cx.add_conductance(b_, c_, {0.0, cx.omega * cj_bc});
            }
        }
    }

    // M4b: load_tran stamps the DC terminal currents PLUS the junction cap
    // companions (geq_be/jeq_be between B and E, geq_bc/jeq_bc between B and C).
    void load_tran(DcLoadContext& cx, const TranStep&) const override {
        load_dc(cx);  // DC terminal-current stamp (M4a)
        if (!has_caps()) return;
        if (cje_ > 0.0) {
            cx.add_conductance(b_, e_, cx.st(kGeqBe));
            cx.add_current(e_, b_, cx.st(kJeqBe));
        }
        if (cjc_ > 0.0) {
            cx.add_conductance(b_, c_, cx.st(kGeqBc));
            cx.add_current(c_, b_, cx.st(kJeqBc));
        }
    }

    // --- parameter model (§3.5a) -----------------------------------------
    [[nodiscard]] std::vector<ParamSpec> param_specs() const override {
        return {ParamSpec{"area", area_}};
    }
    [[nodiscard]] ws::core::Status check_params(const double* p) const override {
        if (!(p[kAreaParam] > 0.0) || !std::isfinite(p[kAreaParam])) {
            return spice_fail(dev_diag(
                ErrorKind::Semantic, "spice.semantic.q_area",
                "BJT " + name_ + " needs a positive finite area", loc));
        }
        return {};
    }

    // --- state model (§3.5b) ---------------------------------------------
    // M4a: 4 slots. M4b: 30 slots when Cje or Cjc > 0.
    [[nodiscard]] std::size_t state_slots() const override {
        return has_caps() ? 30 : 4;
    }
    [[nodiscard]] bool is_dynamic() const override { return has_caps(); }

    void fold_state(double* st, const RunState& rs, const Circuit& c) const override {
        if (has_caps()) {
            const double area = rs.params()[c.param_span[self_index_].begin];
            st[kCjeEff] = cje_ * area;
            st[kCjcEff] = cjc_ * area;
        }
    }
    [[nodiscard]] ws::core::Status link_dynamic(const Circuit& /*c*/,
                                                 std::size_t self) override {
        self_index_ = self;
        return {};
    }

    // --- M4b dynamic hooks (junction cap companions, only when Cje/Cjc > 0) ---
    void begin_step(const TranStep& step, double* st) override {
        if (!has_caps()) return;
        if (cje_ > 0.0) {
            const double c = c_dep_be(st[kVbeComp], st[kCjeEff]);
            if (step.be) { st[kGeqBe] = c / step.dt; st[kJeqBe] = -st[kQbeComp] / step.dt; }
            else { st[kGeqBe] = 2.0 * c / step.dt; st[kJeqBe] = -(st[kGeqBe] * st[kVbeComp] + st[kIbeComp]); }
        }
        if (cjc_ > 0.0) {
            const double c = c_dep_bc(st[kVbcComp], st[kCjcEff]);
            if (step.be) { st[kGeqBc] = c / step.dt; st[kJeqBc] = -st[kQbcComp] / step.dt; }
            else { st[kGeqBc] = 2.0 * c / step.dt; st[kJeqBc] = -(st[kGeqBc] * st[kVbcComp] + st[kIbcComp]); }
        }
    }

    void commit(const std::vector<double>& x, std::size_t /*nc*/,
                const TranStep& step, double* st) override {
        if (!has_caps()) return;
        const double vbe = v_across(x, b_, e_);
        const double vbc = v_across(x, b_, c_);
        if (cje_ > 0.0) {
            const double q = q_dep_be(vbe, st[kCjeEff]);
            st[kVbeComp] = vbe; st[kQbeComp] = q;
            st[kIbeComp] = st[kGeqBe] * vbe + st[kJeqBe];
            push_hist(st, kHistNBe, kHistBe, step.time, q, step.be);
        }
        if (cjc_ > 0.0) {
            const double q = q_dep_bc(vbc, st[kCjcEff]);
            st[kVbcComp] = vbc; st[kQbcComp] = q;
            st[kIbcComp] = st[kGeqBc] * vbc + st[kJeqBc];
            push_hist(st, kHistNBc, kHistBc, step.time, q, step.be);
        }
    }

    [[nodiscard]] bool lte_ok(const TranStep& step, const std::vector<double>& x,
                              std::size_t /*nc*/, double reltol, double chgtol,
                              double trtol, const double* st, double* ratio) const override {
        if (!has_caps()) { if (ratio) *ratio = 0.0; return true; }
        double worst = 0.0;
        if (cje_ > 0.0) {
            const double q = q_dep_be(v_across(x, b_, e_), st[kCjeEff]);
            double r = 0.0;
            dd3_lte_check(st + kHistNBe, st + kHistBe, step, q, st[kQbeComp],
                          reltol, chgtol, trtol, &r);
            worst = std::max(worst, r);
        }
        if (cjc_ > 0.0) {
            const double q = q_dep_bc(v_across(x, b_, c_), st[kCjcEff]);
            double r = 0.0;
            dd3_lte_check(st + kHistNBc, st + kHistBc, step, q, st[kQbcComp],
                          reltol, chgtol, trtol, &r);
            worst = std::max(worst, r);
        }
        if (ratio) *ratio = worst;
        return worst <= 1.0;
    }

    void seed_state(const std::vector<double>& x, std::size_t /*nc*/, double* st) override {
        if (!has_caps()) return;
        const double vbe = v_across(x, b_, e_);
        const double vbc = v_across(x, b_, c_);
        if (cje_ > 0.0) {
            st[kVbeComp] = vbe; st[kQbeComp] = q_dep_be(vbe, st[kCjeEff]);
            st[kIbeComp] = 0.0; st[kHistNBe] = 0.0;
        }
        if (cjc_ > 0.0) {
            st[kVbcComp] = vbc; st[kQbcComp] = q_dep_bc(vbc, st[kCjcEff]);
            st[kIbcComp] = 0.0; st[kHistNBc] = 0.0;
        }
    }

    // Advances both junctions' pnjlim vold and the Ic residual trackers.
    // §5.3: vold stored is the CLAMPED V (same lesson as the diode).
    [[nodiscard]] bool update_state(const std::vector<double>& x,
                                    std::size_t /*node_count*/, double* st,
                                    const std::vector<double>* x_prev = nullptr) override {
        const std::vector<double>& x_load =
            (x_prev != nullptr && !x_prev->empty()) ? *x_prev : x;
        const double sgn = pnp_ ? -1.0 : 1.0;
        const double vbe_raw = sgn * (v_across(x_load, b_, e_));
        const double vbc_raw = sgn * (v_across(x_load, b_, c_));
        const double vbe_lim = pnjlim(vbe_raw, st[kVbePrev], nf_ * vt_, vcrit_be_);
        const double vbc_lim = pnjlim(vbc_raw, st[kVbcPrev], nr_ * vt_, vcrit_bc_);
        // Ic at the clamped point (the "current predicted by the
        // linearization", §5.1). Uses is_ (area=1) - same minor
        // approximation as the diode; the voltage-delta check compensates.
        const double arg_be = safe_exp_arg(vbe_lim / (nf_ * vt_));
        const double i_be = is_ / af_ * (std::exp(arg_be) - 1.0);
        const double arg_bc = safe_exp_arg(vbc_lim / (nr_ * vt_));
        const double i_bc = is_ / ar_ * (std::exp(arg_bc) - 1.0);
        const double ic_lim = sgn * (af_ * i_be - i_bc);
        st[kIPrev] = st[kICur];
        st[kICur] = ic_lim;
        st[kVbePrev] = vbe_lim;
        st[kVbcPrev] = vbc_lim;
        return false;
    }

    // §5.1: |ΔIc| since the previous load, normalized by max(|Ic|).
    [[nodiscard]] NonlinResid nonlinear_residual(const double* st) const override {
        const double i_cur = st[kICur];
        const double i_prev = st[kIPrev];
        return {std::abs(i_cur - i_prev), std::max(std::abs(i_cur), std::abs(i_prev))};
    }

    // .model name NPN|PNP (Is= Bf= Br= Nf= Nr= Tnom= ...). M4a implements
    // Is, Bf, Br, Nf, Nr, Tnom; the Gummel-Poon params (Vaf, Var, Ikf,
    // Ikr, Cje, Cjc, Mje, Mjc, Vje, Vjc, Tf, Tr, Xti, Eg, Kf, Af, ...) are
    // accepted but not implemented - they land with M4b (junction caps) or
    // M5 (temp model, noise, Early effect).
    ws::core::Status apply_model(const ModelDef& model) override {
        if (model.type != "npn" && model.type != "pnp") {
            return spice_fail(dev_diag(
                ErrorKind::Semantic, "spice.semantic.model_type",
                "BJT " + name_ + " needs an NPN or PNP model, got type '" + model.type + "'",
                model.loc));
        }
        pnp_ = (model.type == "pnp");
        model_name_ = model.name.text;
        // ngspice defaults: Is=1e-16, Bf=100, Br=1, Nf=1, Nr=1, Tnom=27,
        // Cje=0, Cjc=0 (no caps), Mje=0.5, Mjc=0.5, Vje=0.75, Vjc=0.75, Fc=0.5.
        double is_val = 1e-16, bf = 100.0, br = 1.0, nf = 1.0, nr = 1.0, tnom = 27.0;
        double cje_val = 0.0, cjc_val = 0.0, mje_val = 0.5, mjc_val = 0.5;
        double vje_val = 0.75, vjc_val = 0.75, fc_val = 0.5;
        const auto& toks = model.params;
        for (std::size_t i = 0; i < toks.size(); ++i) {
            std::string text = toks[i].text;
            while (!text.empty() && text.front() == '(') text.erase(text.begin());
            while (!text.empty() && text.back() == ')') text.pop_back();
            if (text.empty()) continue;
            std::string key, value;
            const std::size_t eq = text.find('=');
            if (eq != std::string::npos && eq > 0) {
                key = to_lower(text.substr(0, eq));
                value = text.substr(eq + 1);
            } else {
                key = to_lower(text);
                std::size_t next = i + 1;
                if (next < toks.size() && toks[next].text == "=") ++next;
                if (next >= toks.size()) {
                    return spice_fail(dev_diag(
                        ErrorKind::Semantic, "spice.semantic.model_param",
                        "model '" + model.name.text + "': '" + text + "' needs a value",
                        toks[i].loc));
                }
                value = toks[next].text;
                i = next;
            }
            while (!value.empty() && value.back() == ')') value.pop_back();
            double v = 0.0;
            if (const ws::core::Result<double> r = parse_number(value)) {
                v = *r;
            } else {
                return spice_fail(dev_diag(
                    ErrorKind::Semantic, "spice.semantic.model_param",
                    "model '" + model.name.text + "': bad value '" + value + "'",
                    toks[i].loc));
            }
            if (key == "is") is_val = v;
            else if (key == "bf") bf = v;
            else if (key == "br") br = v;
            else if (key == "nf") nf = v;
            else if (key == "nr") nr = v;
            else if (key == "tnom") tnom = v;
            else if (key == "cje") cje_val = v;
            else if (key == "cjc") cjc_val = v;
            else if (key == "mje") mje_val = v;
            else if (key == "mjc") mjc_val = v;
            else if (key == "vje") vje_val = v;
            else if (key == "vjc") vjc_val = v;
            else if (key == "fc") fc_val = v;
            // Accepted but not implemented in M4b:
            else if (key == "vaf" || key == "var" || key == "ikf" || key == "ikr" ||
                     key == "tf" || key == "tr" ||
                     key == "xti" || key == "eg" || key == "kf" || key == "af" ||
                     key == "pe" || key == "pc" || key == "ise" ||
                     key == "isc" || key == "ne" || key == "nc" || key == "rb" ||
                     key == "rc" || key == "re") {
                // Gummel-Poon refinements - land M4c/M5 (Tf/Tr diffusion charge,
                // Early effect, temp model, noise, parasitic resistances).
            } else {
                return spice_fail(dev_diag(
                    ErrorKind::Semantic, "spice.semantic.model_param",
                    "model '" + model.name.text + "': unknown parameter '" + key +
                        "' (Is, Bf, Br, Nf, Nr, Tnom, + Gummel-Poon set)",
                    toks[i].loc));
            }
        }
        if (!(is_val > 0.0) || !std::isfinite(is_val)) {
            return spice_fail(dev_diag(
                ErrorKind::Semantic, "spice.semantic.q_is",
                "model '" + model.name.text + "' needs Is > 0", model.loc));
        }
        if (!(bf > 0.0) || !(br > 0.0) || !std::isfinite(bf) || !std::isfinite(br)) {
            return spice_fail(dev_diag(
                ErrorKind::Semantic, "spice.semantic.q_beta",
                "model '" + model.name.text + "' needs Bf > 0 and Br > 0", model.loc));
        }
        if (!(nf > 0.0) || !(nr > 0.0)) {
            return spice_fail(dev_diag(
                ErrorKind::Semantic, "spice.semantic.q_n",
                "model '" + model.name.text + "' needs Nf > 0 and Nr > 0", model.loc));
        }
        is_ = is_val;
        bf_ = bf; br_ = br;
        nf_ = nf; nr_ = nr;
        af_ = bf / (bf + 1.0);
        ar_ = br / (br + 1.0);
        vt_ = (tnom + 273.15) * kVtPerK;
        // vcrit per junction (SPICE3 formula, scaled by 1/α for the
        // diode's effective Is = Is/α).
        const double is_be = is_val / af_;
        const double is_bc = is_val / ar_;
        const double arg_be = nf_ * vt_ / (std::sqrt(2.0) * is_be);
        vcrit_be_ = (arg_be > 1.0) ? nf_ * vt_ * std::log(arg_be) : nf_ * vt_;
        const double arg_bc = nr_ * vt_ / (std::sqrt(2.0) * is_bc);
        vcrit_bc_ = (arg_bc > 1.0) ? nr_ * vt_ * std::log(arg_bc) : nr_ * vt_;
        // M4b: junction cap params.
        cje_ = cje_val; cjc_ = cjc_val;
        mje_ = mje_val; mjc_ = mjc_val;
        vje_ = vje_val; vjc_ = vjc_val;
        fc_ = fc_val;
        return {};
    }

    [[nodiscard]] bool has_ic() const override { return has_ic_; }
    // The BJT's ic= carries Vbe (and optionally Vbc); expose Vbe as the
    // primary ic value for the .ic/UIC machinery (M4a doesn't use it
    // loadingly - no dynamic state - but the card round-trips it).
    [[nodiscard]] double ic_value() const override { return ic_vbe_; }

    [[nodiscard]] bool write_card(std::string& out,
                                  const CardWriter& w) const override {
        out += name_ + " " + w.node(c_) + " " + w.node(b_) + " " + w.node(e_) +
               " " + model_name_;
        const double area = w.params(*this)[kAreaParam];
        if (area != 1.0) {
            out += " " + Device::CardWriter::num(area);
        }
        if (has_ic_) {
            out += " ic=" + Device::CardWriter::num(ic_vbe_);
            if (ic_vbc_ != 0.0) {
                out += "," + Device::CardWriter::num(ic_vbc_);
            }
        }
        out += "\n";
        return true;
    }

private:
    static constexpr std::size_t kAreaParam = 0;

    // Voltage across a two-terminal pair from x (ground → 0).
    [[nodiscard]] static double v_across(const std::vector<double>& x, NodeId p, NodeId n) {
        const double vp = (p == kGround) ? 0.0 : x[p];
        const double vn = (n == kGround) ? 0.0 : x[n];
        return vp - vn;
    }

    // --- M4b: junction capacitance --------------------------------------
    [[nodiscard]] bool has_caps() const { return cje_ > 0.0 || cjc_ > 0.0; }

    // Generic depletion charge/cap (same SPICE formula as the diode's Cjo,
    // parameterized by the junction's Cj/M/Vj/Fc).
    [[nodiscard]] static double q_dep_jn(double v, double cj, double m, double vj, double fc) {
        if (cj <= 0.0) return 0.0;
        const double fc_vj = fc * vj;
        if (v < fc_vj) {
            double arg = 1.0 - v / vj;
            if (arg < 1e-12) arg = 1e-12;
            return cj * vj / (1.0 - m) * (1.0 - std::pow(arg, 1.0 - m));
        }
        const double f1 = vj * (1.0 - std::pow(1.0 - fc, 1.0 - m)) / (1.0 - m);
        const double f2 = std::pow(1.0 - fc, -m);
        const double dv = v - fc_vj;
        return cj * f1 + cj * f2 * dv * (1.0 + m * dv / (2.0 * vj * (1.0 - fc)));
    }
    [[nodiscard]] static double c_dep_jn(double v, double cj, double m, double vj, double fc) {
        if (cj <= 0.0) return 0.0;
        const double fc_vj = fc * vj;
        if (v < fc_vj) {
            double arg = 1.0 - v / vj;
            if (arg < 1e-12) arg = 1e-12;
            return cj / std::pow(arg, m);
        }
        const double f2 = std::pow(1.0 - fc, -m);
        const double dv = v - fc_vj;
        return cj * f2 * (1.0 + m * dv / (vj * (1.0 - fc)));
    }
    // BE/BC junction wrappers (use the effective Cj from fold_state).
    [[nodiscard]] double q_dep_be(double vbe, double cje_eff) const {
        return q_dep_jn(vbe, cje_eff, mje_, vje_, fc_);
    }
    [[nodiscard]] double c_dep_be(double vbe, double cje_eff) const {
        return c_dep_jn(vbe, cje_eff, mje_, vje_, fc_);
    }
    [[nodiscard]] double q_dep_bc(double vbc, double cjc_eff) const {
        return q_dep_jn(vbc, cjc_eff, mjc_, vjc_, fc_);
    }
    [[nodiscard]] double c_dep_bc(double vbc, double cjc_eff) const {
        return c_dep_jn(vbc, cjc_eff, mjc_, vjc_, fc_);
    }

    // LTE history push (3 (t,q) pairs, oldest first). BE restart clears.
    static void push_hist(double* st, std::size_t n_slot, std::size_t hist_slot,
                          double t, double q, bool be) {
        if (be) { st[n_slot] = 0.0; return; }
        const double n = st[n_slot];
        if (n < 3.0) {
            const auto idx = static_cast<std::size_t>(n);
            st[hist_slot + idx * 2] = t;
            st[hist_slot + idx * 2 + 1] = q;
            st[n_slot] = n + 1.0;
        } else {
            for (std::size_t i = 0; i < 2; ++i) {
                st[hist_slot + i * 2] = st[hist_slot + (i + 1) * 2];
                st[hist_slot + i * 2 + 1] = st[hist_slot + (i + 1) * 2 + 1];
            }
            st[hist_slot + 4] = t;
            st[hist_slot + 5] = q;
        }
    }

    // Divided-difference LTE check (same formula as devices_m2.cpp's dd3_lte).
    static void dd3_lte_check(const double* n_ptr, const double* hist,
                              const TranStep& step, double q_new, double q_scale,
                              double reltol, double chgtol, double trtol, double* ratio) {
        const double n = *n_ptr;
        if (n < 3.0) {
            if (ratio) *ratio = 0.0;
            return;
        }
        const double t0 = hist[0], q0 = hist[1];
        const double t1 = hist[2], q1 = hist[3];
        const double t2 = hist[4], q2 = hist[5];
        const double t3 = step.time, q3 = q_new;
        if (!(t1 > t0) || !(t2 > t1) || !(t3 > t2) || !(step.dt > 0)) {
            if (ratio) *ratio = 0.0;
            return;
        }
        const double d1a = (q1 - q0) / (t1 - t0);
        const double d1b = (q2 - q1) / (t2 - t1);
        const double d1c = (q3 - q2) / (t3 - t2);
        const double d2a = (d1b - d1a) / (t2 - t0);
        const double d2b = (d1c - d1b) / (t3 - t1);
        const double dd3 = (d2b - d2a) / (t3 - t0);
        const double eps = step.dt * step.dt * step.dt * std::abs(dd3) / 2.0;
        const double tolq = trtol * (reltol * std::max(std::abs(q3), std::abs(q_scale)) + chgtol);
        if (ratio) *ratio = (tolq > 0) ? (eps / tolq) : std::numeric_limits<double>::infinity();
    }

    std::string name_;
    NodeId c_, b_, e_;
    ExprProgram area_;
    bool has_ic_;
    double ic_vbe_, ic_vbc_;
    // Model fields (set by apply_model):
    double is_ = 1e-16;
    double bf_ = 100.0, br_ = 1.0;
    double nf_ = 1.0, nr_ = 1.0;
    double af_ = 100.0 / 101.0;  // αF = Bf/(Bf+1)
    double ar_ = 1.0 / 2.0;      // αR = Br/(Br+1)
    double vt_ = 0.02585;
    double vcrit_be_ = 0.7;
    double vcrit_bc_ = 0.7;
    // M4b: junction cap params (Cje/Cjc > 0 enables the companions)
    double cje_ = 0.0, cjc_ = 0.0;
    double mje_ = 0.5, mjc_ = 0.5;
    double vje_ = 0.75, vjc_ = 0.75;
    double fc_ = 0.5;
    bool pnp_ = false;
    std::size_t self_index_ = 0;  // filled by link_dynamic
    std::string model_name_;
};

}  // namespace

ws::core::Result<std::unique_ptr<Device>> make_bjt(const DeviceCard& card,
                                                    FactoryServices& s) {
    // Q name C B E model [area] [off] [ic=Vbe,Vbc] [temp=...]
    // M4a: 3-terminal form only (substrate defaults to ground). The
    // 4-terminal form (Q name C B E S model ...) is rejected with a
    // clear error - substrate-junction modeling lands with M4b/M5.
    WS_TRY_ASSIGN(const std::vector<NodeId> n, take_nodes(card, s.nodes, 3, 0));
    constexpr std::size_t model_idx = 3;
    if (card.rest.size() <= model_idx) {
        return spice_fail(dev_diag(ErrorKind::Semantic, "spice.semantic.arity",
                                   "BJT " + card.name.text +
                                       " takes three nodes (C B E) and a model name",
                                   card.loc));
    }
    ExprProgram area_prog;
    bool has_area = false;
    bool has_ic = false;
    double ic_vbe = 0.0, ic_vbc = 0.0;
    for (std::size_t i = model_idx + 1; i < card.rest.size(); ++i) {
        const std::string lower = to_lower(card.rest[i].text);
        if (lower.rfind("ic=", 0) == 0) {
            has_ic = true;
            const std::string v = card.rest[i].text.substr(3);
            const std::size_t comma = v.find(',');
            if (const ws::core::Result<double> r = parse_number(v.substr(0, comma))) {
                ic_vbe = *r;
            }
            if (comma != std::string::npos) {
                if (const ws::core::Result<double> r = parse_number(v.substr(comma + 1))) {
                    ic_vbc = *r;
                }
            }
            continue;
        }
        if (lower == "off") continue;
        if (lower.rfind("temp=", 0) == 0) continue;  // per-instance temp: M5
        if (!has_area) {
            WS_TRY_ASSIGN(area_prog, resolve_value_program(card.rest[i], s.params));
            WS_TRY(eval_param_for_validation(area_prog, s.params));
            has_area = true;
            continue;
        }
        return spice_fail(dev_diag(
            ErrorKind::Semantic, "spice.semantic.q_extra",
            "BJT " + card.name.text +
                " takes three nodes, a model, an optional area, ic=, off, temp="
                " (the 4-terminal substrate form lands with M4b)",
            card.loc));
    }
    if (!has_area) {
        Token one;
        one.text = "1.0";
        WS_TRY_ASSIGN(area_prog, resolve_value_program(one, s.params));
    }
    auto dev = std::make_unique<Bjt>(card.name.text, n[0], n[1], n[2],
                                      std::move(area_prog), has_ic, ic_vbe, ic_vbc);
    dev->loc = card.loc;
    s.models.push_back(ModelRequest{dev.get(), card.rest[model_idx]});
    return dev;
}

}  // namespace ws::spice
