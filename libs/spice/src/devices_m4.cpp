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

#include "physconst.hpp"

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
constexpr double kVtPerK = phys::kKoverQ;

// §7.7: the SPICE3 BJT saturation-current temperature factor (bjttemp.c,
// tlev=0): factlog = (T/Tnom − 1)·Eg/Vt_T + XTI·ln(T/Tnom) - note NO
// division by the emission coefficient (the diode's XTI/N form differs).
// Returns exactly 0.0 at T == Tnom.
[[nodiscard]] double bjt_is_temp_factor(double temp_k, double tnom_k, double eg,
                                        double xti) {
    const double ratlog = std::log(temp_k / tnom_k);
    const double ratio1 = temp_k / tnom_k - 1.0;
    if (ratlog == 0.0 && ratio1 == 0.0) {
        return 0.0;
    }
    return ratio1 * eg / (temp_k * kVtPerK) + xti * ratlog;
}

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
        ExprProgram area, bool has_ic, double ic_vbe, double ic_vbc,
        bool has_temp_inst, double temp_inst_c)
        : name_(std::move(display)), c_(c), b_(b), e_(e),
          area_(std::move(area)), has_ic_(has_ic), ic_vbe_(ic_vbe), ic_vbc_(ic_vbc),
          has_temp_inst_(has_temp_inst), temp_inst_c_(temp_inst_c) {}

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

    // §7.7 (M5): the effective temperature for this load - the instance
    // temp= override when given, else the run's (elaboration) temperature.
    [[nodiscard]] double load_temp_k(double ctx_temp_k) const {
        return has_temp_inst_ ? phys::c_to_k(temp_inst_c_) : ctx_temp_k;
    }

    // The temp-dependent model quantities at one (area, temperature):
    // Vt, the effective Is(T)·area, the Early-free betas Bf(T)/Br(T)
    // (SPICE3's XTB scaling) with their α forms, and both junctions'
    // pnjlim knees. At T == Tnom every field equals the frozen Tnom
    // values, so default decks keep the M4a/M4b answers bit-for-bit.
    struct TempOp {
        double vt = 0.0;
        double is_eff = 0.0;
        double bf = 100.0, br = 1.0;
        double af = 100.0 / 101.0, ar = 0.5;
        double vcrit_be = 0.7, vcrit_bc = 0.7;
        double is_be = 1e-18, is_bc = 1e-16;  // per-junction Is/α (noise)
    };
    [[nodiscard]] TempOp temp_op(double area, double temp_k) const {
        TempOp op;
        op.vt = temp_k * kVtPerK;
        const double tf = bjt_is_temp_factor(temp_k, tnom_k_, eg_, xti_);
        const double is_t = is_ * std::exp(tf);
        op.is_eff = is_t * area;
        // XTB: Bf(T) = Bf·exp(XTB·ln(T/Tnom)), same factor for Br
        // (bjttemp.c; XTB=0, the default, is exactly 1.0).
        const double ratlog = std::log(temp_k / tnom_k_);
        const double bfactor = std::exp(xtb_ * ratlog);
        op.bf = bf_ * bfactor;
        op.br = br_ * bfactor;
        op.af = op.bf / (op.bf + 1.0);
        op.ar = op.br / (op.br + 1.0);
        // Per-junction saturation currents and vcrit knees (SPICE3 form,
        // evaluated at the effective Is(T)).
        op.is_be = is_t / op.af;
        op.is_bc = is_t / op.ar;
        const double arg_be = nf_ * op.vt / (std::sqrt(2.0) * op.is_be);
        op.vcrit_be = (arg_be > 1.0) ? nf_ * op.vt * std::log(arg_be) : nf_ * op.vt;
        const double arg_bc = nr_ * op.vt / (std::sqrt(2.0) * op.is_bc);
        op.vcrit_bc = (arg_bc > 1.0) ? nr_ * op.vt * std::log(arg_bc) : nr_ * op.vt;
        return op;
    }

    void load_dc(DcLoadContext& cx) const override {
        const double area = cx.par(kAreaParam);
        const TempOp op = temp_op(area, load_temp_k(cx.temp_k()));
        const double is_eff = op.is_eff;
        const double sgn = pnp_ ? -1.0 : 1.0;
        const double vbe_raw = sgn * (cx.node_v(b_) - cx.node_v(e_));
        const double vbc_raw = sgn * (cx.node_v(b_) - cx.node_v(c_));
        const double vbe = pnjlim(vbe_raw, cx.st(kVbePrev), nf_ * op.vt, op.vcrit_be);
        const double vbc = pnjlim(vbc_raw, cx.st(kVbcPrev), nr_ * op.vt, op.vcrit_bc);
        const double arg_be = safe_exp_arg(vbe / (nf_ * op.vt));
        const double ex_be = std::exp(arg_be);
        const double arg_bc = safe_exp_arg(vbc / (nr_ * op.vt));
        const double ex_bc = std::exp(arg_bc);
        const double gm = cx.gmin();
        // Early effect (SPICE3's base charge, roll-off terms dropped):
        // the transport current is divided by qb = 1/oef with
        //   oef = 1 - Vbc/Vaf - Vbe/Var
        // (SPICE3's q1, evaluated at the clamped junction voltages). The
        // forward term is the finite output conductance go = Ic/Vaf; the
        // reverse term is its mirror in reverse-active. Vaf/Var = 0
        // (default) disables each term - oef == 1.0 and the stamp is
        // byte-identical to M4a. The base diode currents are NOT scaled
        // (SPICE3 likewise divides only the transport current).
        const double oef = 1.0 - vbc * inv_vaf_ - vbe * inv_var_;
        // Transport terminal currents (into the terminals, sgn-scaled):
        //   Ic = Is*(ex_be - ex_bc) * oef
        //   Ib = Is/Bf*(ex_be-1) + Is/Br*(ex_bc-1)  + gmin*(Vbe+Vbc)
        //   Ie = -(Ic + Ib)
        const double t_tran = is_eff * (ex_be - ex_bc);
        const double ic = sgn * t_tran * oef;
        const double ib = sgn * (is_eff / op.bf * (ex_be - 1.0) +
                                  is_eff / op.br * (ex_bc - 1.0) +
                                  gm * (vbe + vbc));
        const double ie = -(ic + ib);
        // Derivatives w.r.t. the EFFECTIVE junction voltages:
        //   dIc/dVbe_eff = (Is/(Nf*Vt)*ex_be - Is/(Nr*Vt)*ex_bc)*oef - T/Var
        //   dIc/dVbc_eff = -Is/(Nr*Vt)*ex_bc*oef - T/Vaf
        // (the Early terms: d(oef)/dVbe = -1/Var, d(oef)/dVbc = -1/Vaf;
        // T = the unscaled transport current). The pure junction
        // conductances:
        //   dIb/dVbe_eff = Is/(Bf*Nf*Vt)*ex_be + gmin = gd_ib_f
        //   dIb/dVbc_eff = Is/(Br*Nr*Vt)*ex_bc + gmin = gd_ib_r
        const double gm_f = is_eff / (nf_ * op.vt) * ex_be;
        const double gm_r = is_eff / (nr_ * op.vt) * ex_bc;
        const double gd_ib_f = is_eff / (op.bf * nf_ * op.vt) * ex_be + gm;
        const double gd_ib_r = is_eff / (op.br * nr_ * op.vt) * ex_bc + gm;
        // Chain rule to node voltages. For NPN: Vbe_eff=Vb-Ve, Vbc_eff=Vb-Vc.
        // For PNP: Vbe_eff=Ve-Vb, Vbc_eff=Vc-Vb. The sgn factors cancel
        // (d(sgn*Ic)/dVb = sgn * dIc/dVbe_eff * dVbe_eff/dVb = sgn*gm_f*sgn = gm_f),
        // so dIc/dVb is the same for both polarities - the Early terms
        // inherit the cancellation (they are functions of the effective
        // junction voltages only).
        const double dic_dvb = (gm_f - gm_r) * oef - t_tran * inv_var_;
        const double dic_dvc = gm_r * oef + t_tran * inv_vaf_;  // go side
        const double dic_dve = -gm_f * oef + t_tran * inv_var_;
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
        const TempOp op = temp_op(area, load_temp_k(cx.temp_k()));
        const double is_eff = op.is_eff;
        const double vbe = cx.st(kVbePrev);
        const double vbc = cx.st(kVbcPrev);
        const double arg_be = safe_exp_arg(vbe / (nf_ * op.vt));
        const double ex_be = std::exp(arg_be);
        const double arg_bc = safe_exp_arg(vbc / (nr_ * op.vt));
        const double ex_bc = std::exp(arg_bc);
        const double gm_f = is_eff / (nf_ * op.vt) * ex_be;
        const double gm_r = is_eff / (nr_ * op.vt) * ex_bc;
        const double gd_ib_f = is_eff / (op.bf * nf_ * op.vt) * ex_be;
        const double gd_ib_r = is_eff / (op.br * nr_ * op.vt) * ex_bc;
        // The Early terms at the frozen OP (go and its Vbe-side mirror).
        const double oef = 1.0 - vbc * inv_vaf_ - vbe * inv_var_;
        const double t_tran = is_eff * (ex_be - ex_bc);
        // Same G entries as load_dc (real only - no caps in M4a).
        const double dic_dvb = (gm_f - gm_r) * oef - t_tran * inv_var_;
        const double dic_dvc = gm_r * oef + t_tran * inv_vaf_;
        const double dic_dve = -gm_f * oef + t_tran * inv_var_;
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

    // §7.5 (M5, bjtnoise.c): _ic shot 2q·|Ic| between collector and
    // emitter, _ib shot 2q·|Ib| between base and emitter, _fn flicker
    // KF·|Ib|^AF/f (KP=0, the default, is silent). The rb/re/rc thermal
    // sources need the parasitic resistances this model does not carry.
    // The OP junction voltages ride state[kVbePrev]/[kVbcPrev]; the
    // currents are the sgn-effective terminal currents at the OP.
    void load_noise(NoiseContext& cx) const override {
        const double area = cx.par(kAreaParam);
        const TempOp op = temp_op(area, load_temp_k(cx.temp_k));
        const double sgn = pnp_ ? -1.0 : 1.0;
        const double vbe = cx.st(kVbePrev);
        const double vbc = cx.st(kVbcPrev);
        const double ex_be = std::exp(safe_exp_arg(vbe / (nf_ * op.vt)));
        const double ex_bc = std::exp(safe_exp_arg(vbc / (nr_ * op.vt)));
        const double oef = 1.0 - vbc * inv_vaf_ - vbe * inv_var_;
        const double ic = sgn * op.is_eff * (ex_be - ex_bc) * oef;
        const double ib = sgn * (op.is_eff / op.bf * (ex_be - 1.0) +
                                 op.is_eff / op.br * (ex_bc - 1.0));
        cx.add_source("ic", c_, e_, 2.0 * phys::kQ * std::abs(ic));
        cx.add_source("ib", b_, e_, 2.0 * phys::kQ * std::abs(ib));
        if (kf_ != 0.0 && cx.freq > 0.0 && ib != 0.0) {
            cx.add_source("fn", b_, e_,
                          kf_ * std::pow(std::abs(ib), flicker_exp_) / cx.freq);
        }
    }

    // Advances both junctions' pnjlim vold and the Ic residual trackers.
    // §5.3: vold stored is the CLAMPED V (same lesson as the diode).
    [[nodiscard]] bool update_state(const std::vector<double>& x,
                                    std::size_t /*node_count*/, double* st,
                                    const std::vector<double>* x_prev = nullptr,
                                    const double* params = nullptr,
                                    double temp_k = 300.15) override {
        const std::vector<double>& x_load =
            (x_prev != nullptr && !x_prev->empty()) ? *x_prev : x;
        double area = 1.0;
        if (params != nullptr) {
            area = params[kAreaParam];
        }
        const TempOp op = temp_op(area, load_temp_k(temp_k));
        const double sgn = pnp_ ? -1.0 : 1.0;
        const double vbe_raw = sgn * (v_across(x_load, b_, e_));
        const double vbc_raw = sgn * (v_across(x_load, b_, c_));
        const double vbe_lim = pnjlim(vbe_raw, st[kVbePrev], nf_ * op.vt, op.vcrit_be);
        const double vbc_lim = pnjlim(vbc_raw, st[kVbcPrev], nr_ * op.vt, op.vcrit_bc);
        // Ic at the clamped point (the "current predicted by the
        // linearization", §5.1).
        const double arg_be = safe_exp_arg(vbe_lim / (nf_ * op.vt));
        const double i_be = op.is_eff / op.af * (std::exp(arg_be) - 1.0);
        const double arg_bc = safe_exp_arg(vbc_lim / (nr_ * op.vt));
        const double i_bc = op.is_eff / op.ar * (std::exp(arg_bc) - 1.0);
        // Early factor at the clamped point (the linearization the load
        // just used; Vaf/Var = 0 leaves ic_lim untouched).
        const double oef = 1.0 - vbc_lim * inv_vaf_ - vbe_lim * inv_var_;
        const double ic_lim = sgn * (op.af * i_be - i_bc) * oef;
        st[kIPrev] = st[kICur];
        st[kICur] = ic_lim;
        st[kVbePrev] = vbe_lim;
        st[kVbcPrev] = vbc_lim;
        // §5.3: return true when either pnjlim is clamping — forces another
        // iteration so convergence isn't declared at a stale linearization
        // point (same fix as the diode; prevents false convergence when the
        // BJT current is below abstol at small Vbe_lim).
        return vbe_lim != vbe_raw || vbc_lim != vbc_raw;
    }

    // §5.1: |ΔIc| since the previous load, normalized by max(|Ic|).
    [[nodiscard]] NonlinResid nonlinear_residual(const double* st) const override {
        const double i_cur = st[kICur];
        const double i_prev = st[kIPrev];
        return {std::abs(i_cur - i_prev), std::max(std::abs(i_cur), std::abs(i_prev))};
    }

    // .model name NPN|PNP (Is= Bf= Br= Nf= Nr= Tnom= ...). M4a implements
    // Is, Bf, Br, Nf, Nr, Tnom; M4b adds Cje/Cjc/Mje/Mjc/Vje/Vjc/Fc; the
    // Early voltages Vaf/Var are implemented (the SPICE3 base-charge
    // form, no roll-off); §7.7 (M5) implements the temperature model
    // (Xti, Eg, Xtb - Is(T), the beta exponent); the remaining
    // Gummel-Poon params (Ikf, Ikr, Tf, Tr, Kf, Af, ...) are accepted
    // but not implemented - they land with the M5 device-physics follow-ups.
    ws::core::Status apply_model(const ModelDef& model,
                                 NodeResolver& /*nodes*/) override {
        if (model.type != "npn" && model.type != "pnp") {
            return spice_fail(dev_diag(
                ErrorKind::Semantic, "spice.semantic.model_type",
                "BJT " + name_ + " needs an NPN or PNP model, got type '" + model.type + "'",
                model.loc));
        }
        pnp_ = (model.type == "pnp");
        model_name_ = model.name.text;
        // ngspice defaults: Is=1e-16, Bf=100, Br=1, Nf=1, Nr=1, Tnom=27,
        // Cje=0, Cjc=0 (no caps), Mje=0.5, Mjc=0.5, Vje=0.75, Vjc=0.75,
        // Fc=0.5, Eg=1.11, Xti=3, Xtb=0 (bjtsetup.c).
        double is_val = 1e-16, bf = 100.0, br = 1.0, nf = 1.0, nr = 1.0, tnom = 27.0;
        double vaf_val = 0.0, var_val = 0.0;
        double cje_val = 0.0, cjc_val = 0.0, mje_val = 0.5, mjc_val = 0.5;
        double vje_val = 0.75, vjc_val = 0.75, fc_val = 0.5;
        double eg_val = 1.11, xti_val = 3.0, xtb_val = 0.0;
        double kf_val = 0.0, af_val = 1.0;
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
            else if (key == "vaf") vaf_val = v;
            else if (key == "var") var_val = v;
            else if (key == "xti") xti_val = v;
            else if (key == "eg") eg_val = v;
            else if (key == "xtb") xtb_val = v;
            else if (key == "kf") kf_val = v;
            else if (key == "af") af_val = v;
            else if (key == "cje") cje_val = v;
            else if (key == "cjc") cjc_val = v;
            else if (key == "mje") mje_val = v;
            else if (key == "mjc") mjc_val = v;
            else if (key == "vje") vje_val = v;
            else if (key == "vjc") vjc_val = v;
            else if (key == "fc") fc_val = v;
            // Accepted but not implemented (the M5 device-physics
            // follow-ups: diffusion charge, noise, parasitic resistances,
            // high-current roll-off):
            else if (key == "ikf" || key == "ikr" ||
                     key == "tf" || key == "tr" ||
                     key == "pe" || key == "pc" || key == "ise" ||
                     key == "isc" || key == "ne" || key == "nc" || key == "rb" ||
                     key == "rc" || key == "re") {
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
        // Early voltages: 0 (the default) disables the term; negative
        // values would flip the output-conductance sign - reject loudly.
        if (!(vaf_val >= 0.0) || !std::isfinite(vaf_val) ||
            !(var_val >= 0.0) || !std::isfinite(var_val)) {
            return spice_fail(dev_diag(
                ErrorKind::Semantic, "spice.semantic.q_early",
                "model '" + model.name.text + "' needs Vaf >= 0 and Var >= 0"
                " (0 disables the Early effect)",
                model.loc));
        }
        is_ = is_val;
        bf_ = bf; br_ = br;
        nf_ = nf; nr_ = nr;
        vaf_ = vaf_val; var_ = var_val;
        inv_vaf_ = (vaf_ > 0.0) ? 1.0 / vaf_ : 0.0;
        inv_var_ = (var_ > 0.0) ? 1.0 / var_ : 0.0;
        af_ = bf / (bf + 1.0);
        ar_ = br / (br + 1.0);
        tnom_k_ = phys::c_to_k(tnom);
        eg_ = eg_val;
        xti_ = xti_val;
        xtb_ = xtb_val;
        kf_ = kf_val;
        af_ = af_val;
        // vcrit per junction is a PER-LOAD quantity now (§7.7, M5): it
        // evaluates at the run temperature through temp_op(). At
        // T == Tnom the values equal the frozen-Tnom form bit-for-bit.
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
    // §7.7: per-instance temp= override (°C; ngspice's Q TEMP).
    bool has_temp_inst_ = false;
    double temp_inst_c_ = 27.0;
    // Model fields (set by apply_model):
    double is_ = 1e-16;
    double bf_ = 100.0, br_ = 1.0;
    double nf_ = 1.0, nr_ = 1.0;
    double af_ = 100.0 / 101.0;  // αF = Bf/(Bf+1)
    double ar_ = 1.0 / 2.0;      // αR = Br/(Br+1)
    double tnom_k_ = 300.15;     // model nominal temperature, K (§7.7)
    double eg_ = 1.11;           // energy gap, eV (§7.7 Is(T))
    double xti_ = 3.0;           // Is temperature exponent (§7.7)
    double xtb_ = 0.0;           // beta temperature exponent (§7.7)
    double kf_ = 0.0;            // flicker coefficient (§7.5; 0 = silent)
    double flicker_exp_ = 1.0;   // flicker current exponent AF (§7.5;
                                 // 'af_' is alpha-F here - the transport
                                 // model's forward alpha)
    // Early voltages (Vaf forward, Var reverse; 0 = off, the default).
    // The reciprocal is what the stamp multiplies by, precomputed so the
    // no-Early path costs one 1.0 multiply and two 0.0 adds - with both
    // off, oef == 1.0 exactly and the stamp is byte-identical to M4a.
    double vaf_ = 0.0, var_ = 0.0;
    double inv_vaf_ = 0.0, inv_var_ = 0.0;
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
    bool has_temp_inst = false;
    double temp_inst = 27.0;
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
        if (lower.rfind("temp=", 0) == 0) {
            // §7.7 (M5): per-instance temperature override (°C).
            if (const ws::core::Result<double> r = parse_number(card.rest[i].text.substr(5))) {
                has_temp_inst = true;
                temp_inst = *r;
            }
            continue;
        }
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
                                      std::move(area_prog), has_ic, ic_vbe, ic_vbc,
                                      has_temp_inst, temp_inst);
    dev->loc = card.loc;
    s.models.push_back(ModelRequest{dev.get(), card.rest[model_idx]});
    return dev;
}

// --- M (MOSFET, §3.6/§5.3/§11 M4c: Shichman-Hodges level 1) ----------------------

namespace {

class Mosfet final : public Device {
public:
    static constexpr std::size_t kVgsPrev = 0;
    static constexpr std::size_t kVdsPrev = 1;
    static constexpr std::size_t kICur = 2;
    static constexpr std::size_t kIPrev = 3;
    static constexpr std::size_t kVbsPrev = 4;  // §7.5: the OP body bias
    // Meyer-cap companion state (§6.2; present when has_caps()): the run
    // temperature (folded), then one (V, Q, I, geq, jeq) block per
    // terminal pair and a 3x (n, t, q) LTE history. The V/Q blocks carry
    // the MIRROR-frame pair voltages (vgs, vgd, vgb - SPICE3's qgs
    // precedent); the stamps flip jeq by sgn. vds = vgs - vgd and
    // vbs = vgs - vgb recover the bias for the cap evaluation.
    static constexpr std::size_t kTempK = 5;  // folded run temperature, K
    static constexpr std::size_t kGsV = 6, kGsQ = 7, kGsI = 8, kGsGeq = 9, kGsJeq = 10;
    static constexpr std::size_t kGdV = 11, kGdQ = 12, kGdI = 13, kGdGeq = 14,
                                 kGdJeq = 15;
    static constexpr std::size_t kGbV = 16, kGbQ = 17, kGbI = 18, kGbGeq = 19,
                                 kGbJeq = 20;
    static constexpr std::size_t kGsHistN = 21, kGsHist = 22;  // 6 slots
    static constexpr std::size_t kGdHistN = 28, kGdHist = 29;  // 6 slots
    static constexpr std::size_t kGbHistN = 35, kGbHist = 36;  // 6 slots

    Mosfet(std::string display, NodeId d, NodeId g, NodeId s, NodeId b,
           double l, double w, bool has_temp_inst, double temp_inst_c)
        : name_(std::move(display)), d_(d), g_(g), s_(s), b_(b), l_(l), w_(w),
          has_temp_inst_(has_temp_inst), temp_inst_c_(temp_inst_c) {}

    [[nodiscard]] const std::string& name() const override { return name_; }
    [[nodiscard]] char kind() const override { return 'M'; }
    [[nodiscard]] std::vector<NodeId> terminals() const override {
        return {d_, g_, s_, b_};
    }
    // The MOSFET's channel connects D-S (through the controlled current
    // source); the gate is capacitive only (no DC path). The body junction
    // (B-S, B-D) is reverse-biased (a diode) — for the graph check, treat
    // D-S as a conductance path (the channel is always slightly conducting
    // via gmin). The gate has no DC path (open).
    [[nodiscard]] std::vector<std::pair<NodeId, NodeId>> conductance_edges()
        const override {
        return {{d_, s_}};  // channel (weak but present via gmin)
    }

    // --- the stamp (direct terminal-current, like the BJT) --------------
    // Level 1 (Shichman-Hodges):
    //   Cutoff (Vgs < Vth): Id = 0
    //   Triode (Vgs > Vth, Vds < Vgs-Vth):
    //     Id = Kp*W/L*(Vgs-Vth - Vds/2)*Vds*(1+lambda*Vds)
    //   Saturation (Vgs > Vth, Vds >= Vgs-Vth):
    //     Id = 0.5*Kp*W/L*(Vgs-Vth)^2*(1+lambda*Vds)
    // Vth = Vto + gamma*(sqrt(2*phi - Vbs) - sqrt(2*phi))
    // PMOS: negate Vgs, Vds, Vbs; Id flows S→D (negate Id).

    // §7.7 (M5): the effective temperature for this load - the instance
    // temp= override when given, else the run's (elaboration) temperature.
    [[nodiscard]] double load_temp_k(double ctx_temp_k) const {
        return has_temp_inst_ ? phys::c_to_k(temp_inst_c_) : ctx_temp_k;
    }

    // The temp-dependent level-1 quantities (mos1temp.c, the tlev=0
    // machinery): Phi(T) through the SPICE3 pbfact form, Vto(T) through
    // the tVbi/tVto chain, and Kp(T) = Kp/(T/Tnom)^1.5 (surface
    // mobility). At T == Tnom every field equals the frozen Tnom value
    // (pbfact(Tnom) == pbfact1 exactly), so default decks keep the M4c
    // answers bit-for-bit.
    struct TempOp {
        double phi = 0.6;
        double vto = 0.0;
        double kp = 20e-6;
    };
    [[nodiscard]] TempOp temp_op(double temp_k) const {
        TempOp op;
        if (temp_k == tnom_k_) {
            op.phi = phi_;
            op.vto = vto_;
            op.kp = kp_;
            return op;
        }
        const double vt = temp_k * kVtPerK;
        const double vtnom = tnom_k_ * kVtPerK;
        const double fact2 = temp_k / phys::kRefTempK;
        const double fact1 = tnom_k_ / phys::kRefTempK;
        const double egfet_t = phys::egfet(temp_k);
        const double egfet_1 = phys::egfet(tnom_k_);
        const double arg = -egfet_t / (2.0 * phys::kBoltz * temp_k) +
                           1.1150877 / (phys::kBoltz * (phys::kRefTempK + phys::kRefTempK));
        const double pbfact = -2.0 * vt * (1.5 * std::log(fact2) + phys::kQ * arg);
        const double arg1 = -egfet_1 / (phys::kBoltz * 2.0 * tnom_k_) +
                            1.1150877 / (2.0 * phys::kBoltz * phys::kRefTempK);
        const double pbfact1 = -2.0 * vtnom * (1.5 * std::log(fact1) + phys::kQ * arg1);
        // Phi(T): the surface potential's temperature shift.
        const double phio = (phi_ - pbfact1) / fact1;
        op.phi = fact2 * phio + pbfact;
        // Vto(T): tVbi + type*gamma*sqrt(tPhi), the tlev=0 chain.
        const double type = pnp_ ? -1.0 : 1.0;
        const double tvbi = vto_ - type * (gamma_ * std::sqrt(phi_)) +
                            0.5 * (egfet_1 - egfet_t) +
                            type * 0.5 * (op.phi - phi_);
        op.vto = tvbi + type * gamma_ * std::sqrt(op.phi);
        if (op.phi < 0.1) {
            op.phi = 0.1;  // mos1temp.c's MAX(.1, phi) guard
        }
        // Kp(T): tTransconductance = Kp / (T/Tnom)^1.5.
        const double ratio = temp_k / tnom_k_;
        const double ratio4 = ratio * std::sqrt(ratio);
        op.kp = kp_ / ratio4;
        return op;
    }

    // The level-1 channel evaluation at one bias point (the shared body
    // of load_dc, load_ac and load_noise): Id and its node-voltage
    // derivatives, with the gmin channel floor folded in. The returned
    // `id` is the REAL-FRAME drain current (PMOS-negated); the
    // derivatives are d(that current)/d(real node voltage).
    struct ChannelEval {
        double id = 0.0;
        double did_dvg = 0.0, did_dvd = 0.0, did_dvs = 0.0, did_dvb = 0.0;
    };
    [[nodiscard]] ChannelEval channel_eval(double vgs, double vds, double vbs,
                                           double gmin, const TempOp& op) const {
        const double sgn = pnp_ ? -1.0 : 1.0;
        // Body effect: Vth = Vto + gamma*(sqrt(2*phi - Vbs) - sqrt(2*phi)).
        double vth = op.vto;
        if (gamma_ != 0.0) {
            const double arg1 = std::max(2 * op.phi - vbs, 1e-12);
            const double arg2 = std::max(2 * op.phi, 1e-12);
            vth += gamma_ * (std::sqrt(arg1) - std::sqrt(arg2));
        }
        const double vgs_vth = vgs - vth;  // "overdrive voltage"
        ChannelEval e;
        if (vgs_vth <= 0.0) {
            // Cutoff: Id = 0 (only gmin leakage).
        } else {
            const double beta = op.kp * w_ / l_;  // Kp(T)*W/L
            const double vdsat = vgs_vth;         // saturation voltage
            if (vds < vdsat) {
                // Triode: Id = beta*(Vgs-Vth - Vds/2)*Vds*(1+lambda*Vds)
                e.id = beta * (vgs_vth - vds / 2.0) * vds * (1.0 + lambda_ * vds);
                e.did_dvg = beta * vds * (1.0 + lambda_ * vds);
                e.did_dvd = beta * ((vgs_vth - vds) * (1.0 + lambda_ * vds) +
                                    (vgs_vth - vds / 2.0) * vds * lambda_);
                e.did_dvs = -e.did_dvg - e.did_dvd;
                e.did_dvb = 0.0;  // body effect on Vth (simplified)
            } else {
                // Saturation: Id = 0.5*beta*(Vgs-Vth)^2*(1+lambda*Vds)
                e.id = 0.5 * beta * vgs_vth * vgs_vth * (1.0 + lambda_ * vds);
                e.did_dvg = beta * vgs_vth * (1.0 + lambda_ * vds);  // gm
                e.did_dvd = 0.5 * beta * vgs_vth * vgs_vth * lambda_;  // gds
                e.did_dvs = -e.did_dvg - e.did_dvd;
                e.did_dvb = 0.0;  // body effect (simplified)
            }
        }
        // Add gmin channel (D-S conductance floor for graph/convergence).
        e.id += gmin * vds;
        e.did_dvd += gmin;
        e.did_dvs -= gmin;
        // Apply polarity (PMOS: Id flows S to D, so negate). The sgn
        // factors cancel in the derivative chain rule, same as the BJT.
        e.id *= sgn;
        return e;
    }

    // --- Meyer gate capacitances (§6.2, SPICE3 DEVqmeyer) -------------------
    // True when any gate cap exists: the oxide channel cap (TOX given)
    // or an overlap term (CGSO/CGDO/CGBO).
    [[nodiscard]] bool has_caps() const {
        return cox_total() > 0.0 || cgso_ * w_ > 0.0 || cgdo_ * w_ > 0.0 ||
               cgbo_ * l_ > 0.0;
    }
    // The total oxide cap over the channel (0 without TOX - ngspice's
    // oxideCapFactor == 0 leaves only the overlaps, mos1temp.c).
    [[nodiscard]] double cox_total() const {
        return cox_factor_ > 0.0 ? cox_factor_ * w_ * l_ : 0.0;
    }

    // SPICE3's DEVqmeyer: the HALF-capacitances at the mirror-frame gate
    // voltages (the caller doubles them and adds the overlaps). vdsat is
    // floored at 0.025 V (mos1load's MAGIC_VDS guard).
    struct MeyerHalf {
        double cgs = 0.0, cgd = 0.0, cgb = 0.0;
    };
    [[nodiscard]] static MeyerHalf meyer_half(double vgs, double vgd, double von,
                                              double vdsat_in, double phi,
                                              double cox) {
        MeyerHalf m;
        const double vgst = vgs - von;
        const double vdsat = std::max(vdsat_in, 0.025);
        if (vgst <= -phi) {
            m.cgb = cox / 2.0;
        } else if (vgst <= -phi / 2.0) {
            m.cgb = -vgst * cox / (2.0 * phi);
        } else if (vgst <= 0.0) {
            m.cgb = -vgst * cox / (2.0 * phi);
            m.cgs = vgst * cox / (1.5 * phi) + cox / 3.0;
            const double vds = vgs - vgd;
            if (vds < vdsat) {
                const double vddif = 2.0 * vdsat - vds;
                const double vddif1 = vdsat - vds;
                const double vddif2 = vddif * vddif;
                m.cgd = m.cgs * (1.0 - vdsat * vdsat / vddif2);
                m.cgs = m.cgs * (1.0 - vddif1 * vddif1 / vddif2);
            }
        } else {
            const double vds = vgs - vgd;
            if (vdsat <= vds) {
                m.cgs = cox / 3.0;
            } else {
                const double vddif = 2.0 * vdsat - vds;
                const double vddif1 = vdsat - vds;
                const double vddif2 = vddif * vddif;
                m.cgd = cox * (1.0 - vdsat * vdsat / vddif2) / 3.0;
                m.cgs = cox * (1.0 - vddif1 * vddif1 / vddif2) / 3.0;
            }
        }
        return m;
    }

    // The FULL gate caps at one bias point: 2*meyer (the mode swap for
    // reversed drain current, mos1load) + the overlaps. The threshold
    // carries the body effect at `vbs`.
    struct GateCaps {
        double cgs = 0.0, cgd = 0.0, cgb = 0.0;
    };
    [[nodiscard]] GateCaps gate_caps(double vgs, double vds, double vbs,
                                     const TempOp& op) const {
        double vth = op.vto;
        if (gamma_ != 0.0) {
            const double arg1 = std::max(2 * op.phi - vbs, 1e-12);
            const double arg2 = std::max(2 * op.phi, 1e-12);
            vth += gamma_ * (std::sqrt(arg1) - std::sqrt(arg2));
        }
        const double von = vth;
        const double vdsat = vgs - von;
        const double vgd = vgs - vds;
        const double cox = cox_total();
        // mode swap: vds >= 0 keeps (gs, gd); vds < 0 swaps them.
        MeyerHalf m = (vds >= 0.0)
                          ? meyer_half(vgs, vgd, von, vdsat, op.phi, cox)
                          : meyer_half(vgd, vgs, von, vdsat, op.phi, cox);
        GateCaps c;
        if (vds >= 0.0) {
            c.cgs = 2.0 * m.cgs + cgso_ * w_;
            c.cgd = 2.0 * m.cgd + cgdo_ * w_;
        } else {
            c.cgs = 2.0 * m.cgd + cgso_ * w_;
            c.cgd = 2.0 * m.cgs + cgdo_ * w_;
        }
        c.cgb = 2.0 * m.cgb + cgbo_ * l_;
        return c;
    }

    void load_dc(DcLoadContext& cx) const override {
        const double sgn = pnp_ ? -1.0 : 1.0;  // PMOS = pnp_ flag reused
        const TempOp op = temp_op(load_temp_k(cx.temp_k()));
        const double vg = cx.node_v(g_), vs = cx.node_v(s_);
        const double vd = cx.node_v(d_), vb = cx.node_v(b_);
        const ChannelEval e = channel_eval(sgn * (vg - vs), sgn * (vd - vs),
                                           sgn * (vb - vs), cx.gmin(), op);
        const double dic_dvg = e.did_dvg;
        const double dic_dvd = e.did_dvd;
        const double dic_dvs = e.did_dvs;
        const double dic_dvb = e.did_dvb;
        // Norton offset in terms of node voltages. `e.id` is already the
        // real-frame drain current - no second sgn here (the M4c PMOS bug
        // the analytic row caught: a double polarity negation flipped the
        // PMOS rhs constant while the G entries stayed right).
        const double id_const = e.id -
            (dic_dvg * vg + dic_dvd * vd + dic_dvs * vs + dic_dvb * vb);
        // G matrix: current leaving D = Id (into the drain).
        cx.add_entry(d_, g_, dic_dvg);
        cx.add_entry(d_, d_, dic_dvd);
        cx.add_entry(d_, s_, dic_dvs);
        cx.add_entry(d_, b_, dic_dvb);
        // Source gets -Id (current leaves source).
        cx.add_entry(s_, g_, -dic_dvg);
        cx.add_entry(s_, d_, -dic_dvd);
        cx.add_entry(s_, s_, -dic_dvs);
        cx.add_entry(s_, b_, -dic_dvb);
        // RHS: the MOSFET pulls Id from D, pushes -Id into S.
        cx.add_rhs(d_, -id_const);
        cx.add_rhs(s_, id_const);
    }

    // §7.4 (M5): the small-signal channel at the OP - the SAME conductance
    // entries as load_dc, evaluated at the frozen bias in the state slice
    // (kVgsPrev/kVdsPrev/kVbsPrev). A gap until now: the MOSFET stamped
    // NOTHING into the AC system, so any .ac deck carrying one saw an
    // open channel (gain 0). The Meyer caps (§6.2) join as j*omega*C at
    // the same frozen OP.
    void load_ac(AcLoadContext& cx) const override {
        const TempOp op = temp_op(load_temp_k(cx.temp_k()));
        const ChannelEval e = channel_eval(cx.st(kVgsPrev), cx.st(kVdsPrev),
                                           cx.st(kVbsPrev), 0.0, op);
        cx.add_entry(d_, g_, {e.did_dvg, 0.0});
        cx.add_entry(d_, d_, {e.did_dvd, 0.0});
        cx.add_entry(d_, s_, {e.did_dvs, 0.0});
        cx.add_entry(d_, b_, {e.did_dvb, 0.0});
        cx.add_entry(s_, g_, {-e.did_dvg, 0.0});
        cx.add_entry(s_, d_, {-e.did_dvd, 0.0});
        cx.add_entry(s_, s_, {-e.did_dvs, 0.0});
        cx.add_entry(s_, b_, {-e.did_dvb, 0.0});
        // §6.2 (M5): the Meyer caps at the frozen OP, as j*omega*C.
        if (has_caps()) {
            const GateCaps c = gate_caps(cx.st(kVgsPrev), cx.st(kVdsPrev),
                                         cx.st(kVbsPrev), op);
            cx.add_conductance(g_, s_, {0.0, cx.omega * c.cgs});
            cx.add_conductance(g_, d_, {0.0, cx.omega * c.cgd});
            cx.add_conductance(g_, b_, {0.0, cx.omega * c.cgb});
        }
    }

    // §6.2 (M5): the transient companion - the DC channel stamp plus the
    // three Meyer-cap Nortons (geq in parallel with sgn*jeq; the charges
    // live in the mirror frame, so the constant current flips with the
    // polarity - see the state-layout comment).
    void load_tran(DcLoadContext& cx, const TranStep&) const override {
        load_dc(cx);
        if (!has_caps()) {
            return;
        }
        const double sgn = pnp_ ? -1.0 : 1.0;
        cx.add_conductance(g_, s_, cx.st(kGsGeq));
        cx.add_current(s_, g_, sgn * cx.st(kGsJeq));
        cx.add_conductance(g_, d_, cx.st(kGdGeq));
        cx.add_current(d_, g_, sgn * cx.st(kGdJeq));
        cx.add_conductance(g_, b_, cx.st(kGbGeq));
        cx.add_current(b_, g_, sgn * cx.st(kGbJeq));
    }


    // --- parameter model (§3.5a) -----------------------------------------
    [[nodiscard]] std::vector<ParamSpec> param_specs() const override {
        return {};  // L and W are construction data, not run parameters
    }
    [[nodiscard]] ws::core::Status check_params(const double*) const override {
        return {};
    }

    // --- state model (§3.5b) ---------------------------------------------
    [[nodiscard]] std::size_t state_slots() const override {
        return has_caps() ? 42 : 5;
    }
    [[nodiscard]] bool is_dynamic() const override { return has_caps(); }

    // §7.5 (M5, mos1noi.c): _thermal channel noise 4kT·(2/3·gm) between
    // drain and source (the level-1 nlev<3 form - (8/3)kT·gm as a
    // density), plus _1overf flicker. The DESIGN's per-model formula
    // KF·|Id|^AF/f^EF (no geometry/Cox term - the model carries no TOX
    // dependency here; a documented deviation from ngspice's nlev=0
    // geometry form). The OP rides state[kVgsPrev]/[kVdsPrev].
    void load_noise(NoiseContext& cx) const override {
        const double t_k = load_temp_k(cx.temp_k);
        const TempOp op = temp_op(t_k);
        const ChannelEval e = channel_eval(cx.st(kVgsPrev), cx.st(kVdsPrev),
                                           cx.st(kVbsPrev), 0.0, op);
        const double gm = e.did_dvg;
        // channel_eval returns the real-frame Id; the mirror-frame value
        // drives the flicker exponent (|Id| - polarity-free either way).
        const double sid = 2.0 / 3.0 * std::abs(gm);
        cx.add_source("thermal", d_, s_, 4.0 * phys::kBoltz * t_k * sid);
        if (kf_ != 0.0 && cx.freq > 0.0 && e.id != 0.0) {
            cx.add_source("1overf", d_, s_,
                          kf_ * std::pow(std::abs(e.id), af_) /
                              std::pow(cx.freq, ef_));
        }
    }

    // --- §6.2 (M5): the Meyer-cap companion hooks --------------------------

    // The mirror-frame terminal voltages from a solution vector.
    [[nodiscard]] double mirror_vgs(const std::vector<double>& x) const {
        return (pnp_ ? -1.0 : 1.0) * v_across(x, g_, s_);
    }
    [[nodiscard]] double mirror_vds(const std::vector<double>& x) const {
        return (pnp_ ? -1.0 : 1.0) * v_across(x, d_, s_);
    }
    [[nodiscard]] double mirror_vbs(const std::vector<double>& x) const {
        return (pnp_ ? -1.0 : 1.0) * v_across(x, b_, s_);
    }

    // The run temperature reaches begin_step through the folded mirror
    // (the driver folds once per step attempt, §3.5b). Guarded: the
    // 5-slot no-caps slice ends below kTempK (fold_all only calls this on
    // dynamic devices, but the guard keeps the invariant local).
    void fold_state(double* st, const RunState& /*rs*/, const Circuit& c) const override {
        if (has_caps()) {
            st[kTempK] = phys::c_to_k(c.temp);
        }
    }
    [[nodiscard]] ws::core::Status link_dynamic(const Circuit& /*c*/,
                                                std::size_t /*self*/) override {
        return {};
    }

    // One companion per terminal pair, coefficients frozen at the last
    // COMMITTED bias (the Cjo precedent). The caps all evaluate at the
    // same (vgs, vds, vbs) recovered from the committed pair voltages.
    void begin_step(const TranStep& step, double* st) override {
        if (!has_caps()) {
            return;
        }
        const TempOp op = temp_op(st[kTempK]);
        const double vgs = st[kGsV];
        const double vds = vgs - st[kGdV];   // vgd = vgs - vds
        const double vbs = vgs - st[kGbV];   // vgb = vgs - vbs
        // (vgb = vg - vb and vgs = vg - vs, so vgb - vgs = vs - vb = -vbs.)
        const GateCaps c = gate_caps(vgs, vds, vbs, op);
        struct Pair {
            std::size_t v, q, i, geq, jeq;
            double cap;
        };
        const Pair pairs[3] = {
            {kGsV, kGsQ, kGsI, kGsGeq, kGsJeq, c.cgs},
            {kGdV, kGdQ, kGdI, kGdGeq, kGdJeq, c.cgd},
            {kGbV, kGbQ, kGbI, kGbGeq, kGbJeq, c.cgb},
        };
        for (const Pair& pr : pairs) {
            if (step.be) {
                st[pr.geq] = pr.cap / step.dt;
                st[pr.jeq] = -st[pr.q] / step.dt;
            } else {
                st[pr.geq] = 2.0 * pr.cap / step.dt;
                st[pr.jeq] = -(st[pr.geq] * st[pr.v] + st[pr.i]);
            }
        }
    }

    // SPICE3's charge evolution: q += (v_new - v_old) * C(v_new) - the
    // cap at the NEW bias carries the step's charge increment.
    void commit(const std::vector<double>& x, std::size_t /*nc*/,
                const TranStep& step, double* st) override {
        if (!has_caps()) {
            return;
        }
        const TempOp op = temp_op(st[kTempK]);
        const double vgs = mirror_vgs(x);
        const double vds = mirror_vds(x);
        const double vbs = mirror_vbs(x);
        const GateCaps c = gate_caps(vgs, vds, vbs, op);
        // v[2] is vgb: vgb = vgs - vbs (vgb - vgs = vs - vb = -vbs).
        const double v[3] = {vgs, vgs - vds, vgs - vbs};
        const struct {
            std::size_t v, q, i, geq, jeq, hn, h;
        } pairs[3] = {
            {kGsV, kGsQ, kGsI, kGsGeq, kGsJeq, kGsHistN, kGsHist},
            {kGdV, kGdQ, kGdI, kGdGeq, kGdJeq, kGdHistN, kGdHist},
            {kGbV, kGbQ, kGbI, kGbGeq, kGbJeq, kGbHistN, kGbHist},
        };
        const double caps[3] = {c.cgs, c.cgd, c.cgb};
        for (std::size_t k = 0; k < 3; ++k) {
            const auto& pr = pairs[k];
            st[pr.q] = st[pr.q] + (v[k] - st[pr.v]) * caps[k];
            st[pr.v] = v[k];
            st[pr.i] = st[pr.geq] * v[k] + st[pr.jeq];
            // LTE history (3 (t, q) pairs, oldest first); BE restarts.
            if (step.be) {
                st[pr.hn] = 0.0;
            } else {
                const double n = st[pr.hn];
                if (n < 3.0) {
                    const std::size_t idx = static_cast<std::size_t>(n);
                    st[pr.h + idx * 2] = step.time;
                    st[pr.h + idx * 2 + 1] = st[pr.q];
                    st[pr.hn] = n + 1.0;
                } else {
                    for (std::size_t j = 0; j < 2; ++j) {
                        st[pr.h + j * 2] = st[pr.h + (j + 1) * 2];
                        st[pr.h + j * 2 + 1] = st[pr.h + (j + 1) * 2 + 1];
                    }
                    st[pr.h + 4] = step.time;
                    st[pr.h + 5] = st[pr.q];
                }
            }
        }
    }

    // The §6.3 LTE test per pair, over the candidate charge
    // q_new = q + (v(x) - v_comp) * C(v(x)).
    [[nodiscard]] bool lte_ok(const TranStep& step, const std::vector<double>& x,
                              std::size_t /*nc*/, double reltol, double chgtol,
                              double trtol, const double* st, double* ratio) const override {
        if (!has_caps()) {
            if (ratio != nullptr) {
                *ratio = 0.0;
            }
            return true;
        }
        const TempOp op = temp_op(st[kTempK]);
        const double vgs = mirror_vgs(x);
        const double vds = mirror_vds(x);
        const double vbs = mirror_vbs(x);
        const GateCaps c = gate_caps(vgs, vds, vbs, op);
        // v[2] is vgb: vgb = vgs - vbs (vgb - vgs = vs - vb = -vbs).
        const double v[3] = {vgs, vgs - vds, vgs - vbs};
        const struct {
            std::size_t v, q, hn, h;
        } pairs[3] = {
            {kGsV, kGsQ, kGsHistN, kGsHist},
            {kGdV, kGdQ, kGdHistN, kGdHist},
            {kGbV, kGbQ, kGbHistN, kGbHist},
        };
        const double caps[3] = {c.cgs, c.cgd, c.cgb};
        double worst = 0.0;
        for (std::size_t k = 0; k < 3; ++k) {
            const auto& pr = pairs[k];
            const double q_new = st[pr.q] + (v[k] - st[pr.v]) * caps[k];
            const double n = st[pr.hn];
            if (n < 3.0) {
                continue;
            }
            const double t0 = st[pr.h], q0 = st[pr.h + 1];
            const double t1 = st[pr.h + 2], q1 = st[pr.h + 3];
            const double t2 = st[pr.h + 4], q2 = st[pr.h + 5];
            const double t3 = step.time, q3 = q_new;
            if (!(t1 > t0) || !(t2 > t1) || !(t3 > t2) || !(step.dt > 0)) {
                continue;
            }
            const double d1a = (q1 - q0) / (t1 - t0);
            const double d1b = (q2 - q1) / (t2 - t1);
            const double d1c = (q3 - q2) / (t3 - t2);
            const double d2a = (d1b - d1a) / (t2 - t0);
            const double d2b = (d1c - d1b) / (t3 - t1);
            const double dd3 = (d2b - d2a) / (t3 - t0);
            const double eps = step.dt * step.dt * step.dt * std::abs(dd3) / 2.0;
            const double tolq =
                trtol * (reltol * std::max(std::abs(q3), std::abs(st[pr.q])) + chgtol);
            if (tolq > 0.0) {
                worst = std::max(worst, eps / tolq);
            }
        }
        if (ratio != nullptr) {
            *ratio = worst;
        }
        return worst <= 1.0;
    }

    // The t=0 seed: the OP's mirror voltages, TRANOP charges q = v*C(v)
    // (the DOUBLED caps + overlaps), zero companion current.
    void seed_state(const std::vector<double>& x, std::size_t /*nc*/,
                    double* st) override {
        if (!has_caps()) {
            return;
        }
        const TempOp op = temp_op(st[kTempK]);
        const double vgs = mirror_vgs(x);
        const double vds = mirror_vds(x);
        const double vbs = mirror_vbs(x);
        const GateCaps c = gate_caps(vgs, vds, vbs, op);
        // v[2] is vgb: vgb = vgs - vbs (vgb - vgs = vs - vb = -vbs).
        const double v[3] = {vgs, vgs - vds, vgs - vbs};
        const double caps[3] = {c.cgs, c.cgd, c.cgb};
        const std::size_t vs[3] = {kGsV, kGdV, kGbV};
        const std::size_t qs[3] = {kGsQ, kGdQ, kGbQ};
        const std::size_t is[3] = {kGsI, kGdI, kGbI};
        const std::size_t hns[3] = {kGsHistN, kGdHistN, kGbHistN};
        for (std::size_t k = 0; k < 3; ++k) {
            st[vs[k]] = v[k];
            st[qs[k]] = v[k] * caps[k];
            st[is[k]] = 0.0;
            st[hns[k]] = 0.0;
        }
    }

    // Track Id for nonlinear_residual + Vgs/Vds for limiting (M4c: no
    // fetlim yet — the MOS is quadratic, converges without limiting).
    [[nodiscard]] bool update_state(const std::vector<double>& x,
                                    std::size_t /*nc*/, double* st,
                                    const std::vector<double>* x_prev = nullptr,
                                    const double* /*params*/ = nullptr,
                                    double temp_k = 300.15) override {
        const std::vector<double>& x_load =
            (x_prev != nullptr && !x_prev->empty()) ? *x_prev : x;
        const TempOp op = temp_op(load_temp_k(temp_k));
        const double sgn = pnp_ ? -1.0 : 1.0;
        const double vgs = sgn * (v_across(x_load, g_, s_));
        const double vds = sgn * (v_across(x_load, d_, s_));
        // Compute Id at the current iterate (for residual tracking).
        double vth = op.vto;
        if (gamma_ != 0.0) {
            const double vbs = sgn * (v_across(x_load, b_, s_));
            const double arg1 = std::max(2 * op.phi - vbs, 1e-12);
            const double arg2 = std::max(2 * op.phi, 1e-12);
            vth += gamma_ * (std::sqrt(arg1) - std::sqrt(arg2));
        }
        const double vgs_vth = vgs - vth;
        double id = 0.0;
        if (vgs_vth > 0.0) {
            const double beta = op.kp * w_ / l_;
            if (vds < vgs_vth) {
                id = beta * (vgs_vth - vds / 2.0) * vds * (1.0 + lambda_ * vds);
            } else {
                id = 0.5 * beta * vgs_vth * vgs_vth * (1.0 + lambda_ * vds);
            }
        }
        id *= sgn;
        st[kIPrev] = st[kICur];
        st[kICur] = id;
        st[kVgsPrev] = vgs;
        st[kVdsPrev] = vds;
        st[kVbsPrev] = sgn * (v_across(x_load, b_, s_));
        return false;  // no limiting in M4c (MOS is quadratic)
    }

    [[nodiscard]] NonlinResid nonlinear_residual(const double* st) const override {
        const double i_cur = st[kICur];
        const double i_prev = st[kIPrev];
        return {std::abs(i_cur - i_prev), std::max(std::abs(i_cur), std::abs(i_prev))};
    }

    // .model name NMOS|PMOS(Level=1 Vto= Kp= Lambda= Gamma= Phi= ...)
    ws::core::Status apply_model(const ModelDef& model,
                                 NodeResolver& /*nodes*/) override {
        if (model.type != "nmos" && model.type != "pmos") {
            return spice_fail(dev_diag(
                ErrorKind::Semantic, "spice.semantic.model_type",
                "MOSFET " + name_ + " needs NMOS or PMOS, got '" + model.type + "'",
                model.loc));
        }
        pnp_ = (model.type == "pmos");
        model_name_ = model.name.text;
        // ngspice defaults: Vto=0, Kp=20µA/V², Lambda=0, Gamma=0, Phi=0.6,
        // Tnom=27°C (§7.7).
        double vto = 0.0, kp = 20e-6, lambda = 0.0, gamma = 0.0, phi = 0.6, tnom = 27.0;
        double kf_val = 0.0, af_val = 1.0, ef_val = 1.0;
        double tox_val = 0.0, cgso_val = 0.0, cgdo_val = 0.0, cgbo_val = 0.0;
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
            if (key == "vto") vto = v;
            else if (key == "kp") kp = v;
            else if (key == "lambda") lambda = v;
            else if (key == "gamma") gamma = v;
            else if (key == "phi") phi = v;
            else if (key == "tnom") tnom = v;
            else if (key == "kf") kf_val = v;
            else if (key == "af") af_val = v;
            else if (key == "ef") ef_val = v;
            else if (key == "tox") tox_val = v;
            else if (key == "cgso") cgso_val = v;
            else if (key == "cgdo") cgdo_val = v;
            else if (key == "cgbo") cgbo_val = v;
            else if (key == "level" || key == "u0" ||
                     key == "cj" || key == "mj" || key == "cjsw" ||
                     key == "mjsw" || key == "pb" || key == "js" ||
                     key == "cbd" || key == "cbs" || key == "nsub" ||
                     key == "nss" || key == "tpg" || key == "ld" ||
                     key == "xl" || key == "wd" || key == "xw") {
                // Accepted but not implemented (BSIM, junction cap
                // tables, geometry shrinks).
            } else {
                return spice_fail(dev_diag(
                    ErrorKind::Semantic, "spice.semantic.model_param",
                    "model '" + model.name.text + "': unknown parameter '" + key +
                        "' (Level, Vto, Kp, Lambda, Gamma, Phi, + MOS set)",
                    toks[i].loc));
            }
        }
        vto_ = vto; kp_ = kp; lambda_ = lambda; gamma_ = gamma; phi_ = phi;
        tnom_k_ = phys::c_to_k(tnom);
        kf_ = kf_val; af_ = af_val; ef_ = ef_val;
        // §6.2 (M5): the oxide cap factor (0 without TOX - only the
        // overlaps then, mos1temp.c) and the overlap caps per meter.
        cox_factor_ = (tox_val > 0.0)
                          ? 3.9 * 8.854214871e-12 / tox_val
                          : 0.0;
        cgso_ = cgso_val;
        cgdo_ = cgdo_val;
        cgbo_ = cgbo_val;
        return {};
    }

    [[nodiscard]] bool write_card(std::string& out,
                                  const CardWriter& w) const override {
        out += name_ + " " + w.node(d_) + " " + w.node(g_) + " " + w.node(s_) +
               " " + w.node(b_) + " " + model_name_ +
               " L=" + Device::CardWriter::num(l_) +
               " W=" + Device::CardWriter::num(w_) + "\n";
        return true;
    }

private:
    static double v_across(const std::vector<double>& x, NodeId p, NodeId n) {
        const double vp = (p == kGround) ? 0.0 : x[p];
        const double vn = (n == kGround) ? 0.0 : x[n];
        return vp - vn;
    }

    std::string name_;
    NodeId d_, g_, s_, b_;
    double l_, w_;
    // §7.7: per-instance temp= override (°C; ngspice's M TEMP).
    bool has_temp_inst_ = false;
    double temp_inst_c_ = 27.0;
    // Model fields:
    double vto_ = 0.0;
    double kp_ = 20e-6;
    double lambda_ = 0.0;
    double gamma_ = 0.0;
    double phi_ = 0.6;
    double tnom_k_ = 300.15;  // model nominal temperature, K (§7.7)
    double kf_ = 0.0;         // flicker coefficient (§7.5; 0 = silent)
    double af_ = 1.0;         // flicker current exponent (§7.5)
    double ef_ = 1.0;         // flicker frequency exponent (§7.5)
    double cox_factor_ = 0.0;  // oxide cap per area, F/m^2 (§6.2; 0 = no TOX)
    double cgso_ = 0.0;        // gate-source overlap, F/m (§6.2)
    double cgdo_ = 0.0;        // gate-drain overlap, F/m (§6.2)
    double cgbo_ = 0.0;        // gate-bulk overlap, F/m (§6.2)
    bool pnp_ = false;  // PMOS
    std::string model_name_;
};

}  // namespace

ws::core::Result<std::unique_ptr<Device>> make_mosfet(const DeviceCard& card,
                                                       FactoryServices& s) {
    // M name D G S B model [L=...] [W=...] [AD=] [AS=] ...
    WS_TRY_ASSIGN(const std::vector<NodeId> n, take_nodes(card, s.nodes, 4, 0));
    constexpr std::size_t model_idx = 4;
    if (card.rest.size() <= model_idx) {
        return spice_fail(dev_diag(ErrorKind::Semantic, "spice.semantic.arity",
                                   "MOSFET " + card.name.text +
                                       " takes four nodes (D G S B) and a model name",
                                   card.loc));
    }
    double l = 100e-6, w = 100e-6;  // ngspice defaults
    bool has_temp_inst = false;
    double temp_inst = 27.0;
    for (std::size_t i = model_idx + 1; i < card.rest.size(); ++i) {
        const std::string lower = to_lower(card.rest[i].text);
        if (lower.rfind("l=", 0) == 0) {
            if (const ws::core::Result<double> r = parse_number(card.rest[i].text.substr(2))) {
                l = *r;
            }
        } else if (lower.rfind("w=", 0) == 0) {
            if (const ws::core::Result<double> r = parse_number(card.rest[i].text.substr(2))) {
                w = *r;
            }
        } else if (lower.rfind("temp=", 0) == 0) {
            // §7.7 (M5): per-instance temperature override (°C).
            if (const ws::core::Result<double> r = parse_number(card.rest[i].text.substr(5))) {
                has_temp_inst = true;
                temp_inst = *r;
            }
        }
        // AD, AS, PD, PS, NRD, NRS, OFF, IC= — accepted, ignored in M4c.
    }
    auto dev = std::make_unique<Mosfet>(card.name.text, n[0], n[1], n[2], n[3], l, w,
                                        has_temp_inst, temp_inst);
    dev->loc = card.loc;
    s.models.push_back(ModelRequest{dev.get(), card.rest[model_idx]});
    return dev;
}

}  // namespace ws::spice
