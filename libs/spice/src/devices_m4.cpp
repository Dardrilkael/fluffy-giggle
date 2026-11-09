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

#include "bsim6_core.hpp"
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

// --- Q (BJT, §3.6/§5.3/§11 M4a: Ebers-Moll; M4b junction caps; M7 Tf/Tr) ----------

class Bjt final : public Device {
public:
    // M4a state slots (always present):
    static constexpr std::size_t kVbePrev = 0;
    static constexpr std::size_t kVbcPrev = 1;
    static constexpr std::size_t kICur = 2;
    static constexpr std::size_t kIPrev = 3;
    // M4b BE junction companion (when Cje > 0 or Tf > 0):
    static constexpr std::size_t kVbeComp = 4;
    static constexpr std::size_t kQbeComp = 5;
    static constexpr std::size_t kIbeComp = 6;
    static constexpr std::size_t kGeqBe = 7;
    static constexpr std::size_t kJeqBe = 8;
    // M4b BC junction companion (when Cjc > 0 or Tr > 0):
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
    // M7 TT slots (present in every dynamic layout): the BE cross
    // conductance (the dQ_be/dVbc Early coupling - the companion's extra
    // unknown) and the folded temp mirrors the charge companions evaluate
    // at (the diode-TT precedent: begin_step/commit see no load context,
    // so the instance temp's Is(T)*area, Nf*Vt(T) and Nr*Vt(T) fold once
    // per step attempt and can never disagree with the load paths).
    static constexpr std::size_t kGeqcbBe = 30;
    static constexpr std::size_t kIsEffT = 31;
    static constexpr std::size_t kNVtBeT = 32;
    static constexpr std::size_t kNVtBcT = 33;

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
        // subtracting the V-dependent (G matrix) part. The reference is
        // the node triple CONSISTENT with the clamped junction voltages
        // (the diode's vd_lim precedent; SPICE3's bjtload evaluates every
        // companion at the limited vbe/vbc) - the raw iterate may have
        // run far past the clamp, and evaluating the offset there pins
        // it outside forever: a current-forced base (I1 into the base)
        // cold-starts at |v_b| ~ source/gmin ~ 1e8 and the whole §5.2
        // ladder then fails (found landing M7 - the fT-pole gates bias
        // the base with a current source; every M4a deck was
        // voltage-driven, which hid it). The rows sum to zero (the
        // currents depend on junction voltages only), so the offset is
        // shift-invariant and any consistent triple gives the same
        // stamp; with no clamp active the triple IS the raw node
        // voltages, byte-identical to the M4a form.
        const double vb_ref = cx.node_v(e_) + sgn * vbe;
        const double vc_ref = vb_ref - sgn * vbc;
        const double ve_ref = cx.node_v(e_);
        const double ic_eq = ic - (dic_dvb * vb_ref + dic_dvc * vc_ref +
                                    dic_dve * ve_ref);
        const double ib_eq = ib - (dib_dvb * vb_ref + dib_dvc * vc_ref +
                                    dib_dve * ve_ref);
        const double ie_eq = ie - (die_dvb * vb_ref + die_dvc * vc_ref +
                                    die_dve * ve_ref);
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

    // load_ac: small-signal conductances at the OP (frozen Vbe/Vbc from
    // state, set by update_state during the .ac OP solve) + the junction
    // caps (M4b depletion, M7 transit-time diffusion) as j*omega*C.
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
        // M7: the transit-time diffusion caps ride the same stamps, and
        // the BE charge's Early dependence adds the transcapacitance
        // cross dQ_be/dVbc coupling the (b,e) port to v(b)-v(c) (the
        // level-77 precedent: the full transcapacitance matrix,
        // conservation by construction; ngspice's bjtacld.c stamps only
        // the scalar capbe - identical here whenever Vaf = Var = 0,
        // where the cross is exactly 0).
        if (has_dyn_be()) {
            double cj_be = cje_ > 0.0 ? c_dep_be(vbe, cje_ * area) : 0.0;
            if (tf_ > 0.0 &&
                live_tt_be(vbe, vbc, is_eff, nf_ * op.vt) > phys::kChargeFloor) {
                cj_be += c_tt_be_vbe(vbe, vbc, is_eff, nf_ * op.vt);
                const double c_x = c_tt_be_vbc(vbe, is_eff, nf_ * op.vt);
                if (c_x != 0.0) {
                    cx.add_entry(b_, b_, {0.0, cx.omega * c_x});
                    cx.add_entry(b_, c_, {0.0, -cx.omega * c_x});
                    cx.add_entry(e_, b_, {0.0, -cx.omega * c_x});
                    cx.add_entry(e_, c_, {0.0, cx.omega * c_x});
                }
            }
            if (cj_be != 0.0) {
                cx.add_conductance(b_, e_, {0.0, cx.omega * cj_be});
            }
        }
        if (has_dyn_bc()) {
            double cj_bc = cjc_ > 0.0 ? c_dep_bc(vbc, cjc_ * area) : 0.0;
            if (tr_ > 0.0 &&
                live_tt_bc(vbc, is_eff, nr_ * op.vt) > phys::kChargeFloor) {
                cj_bc += c_tt_bc(vbc, is_eff, nr_ * op.vt);
            }
            if (cj_bc != 0.0) {
                cx.add_conductance(b_, c_, {0.0, cx.omega * cj_bc});
            }
        }
    }

    // M4b: load_tran stamps the DC terminal currents PLUS the junction
    // charge companions (geq_be/jeq_be between B and E, geq_bc/jeq_bc
    // between B and C; M7 folds the transit-time diffusion charges into
    // the same companions). The BE companion also carries the Early
    // cross conductance kGeqcbBe: the BE charge depends on Vbc, so the
    // charging current into the base picks up geqcb*(v_b - v_c) - a
    // VCCS-shaped stamp between the (b,e) pair and the (b,c) pair.
    void load_tran(DcLoadContext& cx, const TranStep&) const override {
        load_dc(cx);  // DC terminal-current stamp (M4a)
        if (has_dyn_be()) {
            cx.add_conductance(b_, e_, cx.st(kGeqBe));
            cx.add_current(e_, b_, cx.st(kJeqBe));
            const double gx = cx.st(kGeqcbBe);
            if (gx != 0.0) {
                cx.add_entry(b_, b_, gx);
                cx.add_entry(b_, c_, -gx);
                cx.add_entry(e_, b_, -gx);
                cx.add_entry(e_, c_, gx);
            }
        }
        if (has_dyn_bc()) {
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
    // M4a: 4 slots. Dynamic (Cje/Cjc depletion or Tf/Tr transit time):
    // 34 slots - the M4b companion/LTE/fold layout plus the M7 cross
    // conductance and the three folded temp mirrors. One layout for
    // every dynamic combination (the diode-TT 18-slot precedent: no
    // per-feature slot juggling, the untouched slots just read 0).
    [[nodiscard]] std::size_t state_slots() const override {
        return has_dyn_be() || has_dyn_bc() ? 34 : 4;
    }
    [[nodiscard]] bool is_dynamic() const override {
        return has_dyn_be() || has_dyn_bc();
    }

    void fold_state(double* st, const RunState& rs, const Circuit& c) const override {
        if (!has_dyn_be() && !has_dyn_bc()) return;
        const double area = rs.params()[c.param_span[self_index_].begin];
        st[kCjeEff] = cje_ * area;
        st[kCjcEff] = cjc_ * area;
        // The TT charges evaluate at the instance's effective temperature
        // (load_temp_k: the per-instance temp= override, else the
        // circuit's elaboration temperature) - the diode-TT fold
        // precedent, so the companion can never disagree with the load
        // paths. Every temp change re-elaborates (§7.7), so the fold
        // stays current.
        const TempOp op = temp_op(area, load_temp_k(phys::c_to_k(c.temp)));
        st[kIsEffT] = op.is_eff;
        st[kNVtBeT] = nf_ * op.vt;
        st[kNVtBcT] = nr_ * op.vt;
    }
    [[nodiscard]] ws::core::Status link_dynamic(const Circuit& /*c*/,
                                                 std::size_t self) override {
        self_index_ = self;
        return {};
    }

    // --- M4b/M7 dynamic hooks (junction charge companions) ----------------
    // The per-junction charge is the depletion part (M4b) PLUS the
    // transit-time diffusion part (M7); the BE charge also depends on Vbc
    // through the Early factor (the cross conductance kGeqcbBe). The
    // companion algebra is the diode-TT architecture (M5e): the charge
    // floor silences a numerically dead diffusion charge, the consistent
    // tangent linearizes at the committed point (the -Q/dt origin form
    // is exact only for q = C*v, and the diffusion charge is exponential
    // - Q = Vt*C, a factor 27 off at 0.7 V; this also retires the M4b
    // BE-branch debt noted in STATUS), and the trap memory rides I_comp.
    void begin_step(const TranStep& step, double* st) override {
        if (!has_dyn_be() && !has_dyn_bc()) return;
        if (has_dyn_be()) {
            const double vbe_c = st[kVbeComp];
            // The cross reference: the accepted Vbc of the last solved
            // step. kVbcComp holds it when the BC junction is dynamic;
            // kVbcPrev always exists and carries the same accepted value
            // (update_state ran on the converged iterate, where pnjlim
            // is inactive).
            const double vbc_ref = st[kVbcPrev];
            const double cje_eff = st[kCjeEff];
            // The diffusion charge floor (phys::kChargeFloor, the SPICE3
            // chgtol): once the stored charge drops under it, C = dQ/dV
            // collapses with it while the terminal current stays
            // milliamp-scale - the remaining dynamics are the
            // reverse-recovery snap. With no depletion cap either, the
            // honest handoff is a fully silent companion (the circuit
            // pins the node algebraically; a dead charge's trap memory
            // is exactly the ringing poison).
            const bool tt_dead =
                live_tt_be(vbe_c, vbc_ref, st[kIsEffT], st[kNVtBeT]) <=
                phys::kChargeFloor;
            if (tt_dead && cje_eff <= 0.0) {
                st[kGeqBe] = 0.0;
                st[kJeqBe] = 0.0;
                st[kGeqcbBe] = 0.0;
            } else {
                const double c = c_dep_be(vbe_c, cje_eff) +
                                 (tt_dead ? 0.0
                                          : c_tt_be_vbe(vbe_c, vbc_ref,
                                                        st[kIsEffT], st[kNVtBeT]));
                const double c_x =
                    tt_dead ? 0.0
                            : c_tt_be_vbc(vbe_c, st[kIsEffT], st[kNVtBeT]);
                if (step.be) {
                    st[kGeqBe] = c / step.dt;
                    st[kGeqcbBe] = c_x / step.dt;
                    // The consistent BE linearization of i = (q(v) -
                    // Q_comp)/dt frozen at the committed point: the
                    // tangent carries q(V_comp) - c*V_comp and the two
                    // Q's cancel exactly (begin_step runs after commit).
                    st[kJeqBe] = -(st[kGeqBe] * vbe_c + st[kGeqcbBe] * vbc_ref);
                } else {
                    st[kGeqBe] = 2.0 * c / step.dt;
                    st[kGeqcbBe] = 2.0 * c_x / step.dt;
                    st[kJeqBe] = -(st[kGeqBe] * vbe_c + st[kGeqcbBe] * vbc_ref +
                                   st[kIbeComp]);
                }
            }
        }
        if (has_dyn_bc()) {
            const double vbc_c = st[kVbcComp];
            const double cjc_eff = st[kCjcEff];
            const bool tt_dead =
                live_tt_bc(vbc_c, st[kIsEffT], st[kNVtBcT]) <= phys::kChargeFloor;
            if (tt_dead && cjc_eff <= 0.0) {
                st[kGeqBc] = 0.0;
                st[kJeqBc] = 0.0;
            } else {
                const double c = c_dep_bc(vbc_c, cjc_eff) +
                                 (tt_dead ? 0.0
                                          : c_tt_bc(vbc_c, st[kIsEffT], st[kNVtBcT]));
                if (step.be) {
                    st[kGeqBc] = c / step.dt;
                    st[kJeqBc] = -st[kGeqBc] * vbc_c;
                } else {
                    st[kGeqBc] = 2.0 * c / step.dt;
                    st[kJeqBc] = -(st[kGeqBc] * vbc_c + st[kIbcComp]);
                }
            }
        }
    }

    void commit(const std::vector<double>& x, std::size_t /*nc*/,
                const TranStep& step, double* st) override {
        if (!has_dyn_be() && !has_dyn_bc()) return;
        const double vbe = v_across(x, b_, e_);
        const double vbc = v_across(x, b_, c_);
        if (has_dyn_be()) {
            const double q = q_dep_be(vbe, st[kCjeEff]) +
                             q_tt_be(vbe, vbc, st[kIsEffT], st[kNVtBeT]);
            // The companion current at the accepted point (the trap
            // memory; the cross term included - it is the same charge).
            double i_comp = st[kGeqBe] * vbe + st[kGeqcbBe] * vbc + st[kJeqBe];
            // The snap guard (the diode-TT precedent): when the step
            // drained the tangent-implied diffusion charge past its
            // floor (|dv| > Vt off a live charge - the reverse-recovery
            // snap), the frozen companion's current is a fiction;
            // committing it rings the trap memory forever. The honest
            // memory is the implied rate (q_new - Q_comp)/dt - what the
            // step ACTUALLY did to the charge.
            if (live_tt_be(st[kVbeComp], st[kVbcPrev], st[kIsEffT],
                           st[kNVtBeT]) > phys::kChargeFloor) {
                const double c_dir =
                    c_tt_be_vbe(st[kVbeComp], st[kVbcPrev], st[kIsEffT], st[kNVtBeT]);
                const double q_hat =
                    live_tt_be(st[kVbeComp], st[kVbcPrev], st[kIsEffT], st[kNVtBeT]) +
                    c_dir * (vbe - st[kVbeComp]);
                if (q_hat <= 0.0) {
                    i_comp = (q - st[kQbeComp]) / step.dt;
                }
            }
            st[kVbeComp] = vbe; st[kQbeComp] = q;
            st[kIbeComp] = i_comp;
            push_hist(st, kHistNBe, kHistBe, step.time, q, step.be);
        }
        if (has_dyn_bc()) {
            const double q = q_dep_bc(vbc, st[kCjcEff]) +
                             q_tt_bc(vbc, st[kIsEffT], st[kNVtBcT]);
            double i_comp = st[kGeqBc] * vbc + st[kJeqBc];
            if (live_tt_bc(st[kVbcComp], st[kIsEffT], st[kNVtBcT]) >
                phys::kChargeFloor) {
                const double c_dir =
                    c_tt_bc(st[kVbcComp], st[kIsEffT], st[kNVtBcT]);
                const double q_hat =
                    live_tt_bc(st[kVbcComp], st[kIsEffT], st[kNVtBcT]) +
                    c_dir * (vbc - st[kVbcComp]);
                if (q_hat <= 0.0) {
                    i_comp = (q - st[kQbcComp]) / step.dt;
                }
            }
            st[kVbcComp] = vbc; st[kQbcComp] = q;
            st[kIbcComp] = i_comp;
            push_hist(st, kHistNBc, kHistBc, step.time, q, step.be);
        }
    }

    [[nodiscard]] bool lte_ok(const TranStep& step, const std::vector<double>& x,
                              std::size_t /*nc*/, double reltol, double chgtol,
                              double trtol, const double* st, double* ratio) const override {
        if (!has_dyn_be() && !has_dyn_bc()) { if (ratio) *ratio = 0.0; return true; }
        const double vbe = v_across(x, b_, e_);
        const double vbc = v_across(x, b_, c_);
        double worst = 0.0;
        if (has_dyn_be()) {
            const double q = q_dep_be(vbe, st[kCjeEff]) +
                             q_tt_be(vbe, vbc, st[kIsEffT], st[kNVtBeT]);
            double r = 0.0;
            dd3_lte_check(st + kHistNBe, st + kHistBe, step, q, st[kQbeComp],
                          reltol, chgtol, trtol, &r);
            worst = std::max(worst, r);
        }
        if (has_dyn_bc()) {
            const double q = q_dep_bc(vbc, st[kCjcEff]) +
                             q_tt_bc(vbc, st[kIsEffT], st[kNVtBcT]);
            double r = 0.0;
            dd3_lte_check(st + kHistNBc, st + kHistBc, step, q, st[kQbcComp],
                          reltol, chgtol, trtol, &r);
            worst = std::max(worst, r);
        }
        if (ratio) *ratio = worst;
        return worst <= 1.0;
    }

    void seed_state(const std::vector<double>& x, std::size_t /*nc*/, double* st) override {
        if (!has_dyn_be() && !has_dyn_bc()) return;
        const double vbe = v_across(x, b_, e_);
        const double vbc = v_across(x, b_, c_);
        if (has_dyn_be()) {
            st[kVbeComp] = vbe;
            st[kQbeComp] = q_dep_be(vbe, st[kCjeEff]) +
                           q_tt_be(vbe, vbc, st[kIsEffT], st[kNVtBeT]);
            st[kIbeComp] = 0.0;
            st[kHistNBe] = 0.0;
        }
        if (has_dyn_bc()) {
            st[kVbcComp] = vbc;
            st[kQbcComp] = q_dep_bc(vbc, st[kCjcEff]) +
                           q_tt_bc(vbc, st[kIsEffT], st[kNVtBcT]);
            st[kIbcComp] = 0.0;
            st[kHistNBc] = 0.0;
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
    // (Xti, Eg, Xtb - Is(T), the beta exponent); M7 adds the transit
    // times Tf/Tr (the diffusion charges, bjtload.c's qbe/qbc form);
    // the remaining Gummel-Poon params (Ikf, Ikr, the Tf bias/Temp
    // modulation Xtf/Itf/Vtf, the excess phase Ptf, ...) are accepted
    // but not implemented - they land with the device-physics follow-ups.
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
        double tf_val = 0.0, tr_val = 0.0;
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
            else if (key == "tf") tf_val = v;
            else if (key == "tr") tr_val = v;
            // Accepted but not implemented (the device-physics follow-ups:
            // high-current roll-off, the Tf bias/temperature modulation,
            // excess phase, parasitic resistances, the leak diodes):
            else if (key == "ikf" || key == "ikr" ||
                     key == "xtf" || key == "itf" || key == "vtf" ||
                     key == "ptf" ||
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
        // Transit times: 0 (the default) disables each diffusion charge;
        // negative values flip the stored charge's sign - reject loudly
        // (the diode-TT precedent; ngspice rejects Tf/Tr < 0 too).
        if (!(tf_val >= 0.0) || !std::isfinite(tf_val) ||
            !(tr_val >= 0.0) || !std::isfinite(tr_val)) {
            return spice_fail(dev_diag(
                ErrorKind::Semantic, "spice.semantic.q_tt",
                "model '" + model.name.text + "' needs Tf >= 0 and Tr >= 0"
                " (0 disables the transit-time diffusion charge)",
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
        // M7: transit times (Tf/Tr > 0 enables the diffusion charges).
        tf_ = tf_val;
        tr_ = tr_val;
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

    // --- M4b/M7: junction charges ----------------------------------------
    // Per-junction dynamic gates: the companion exists when either the
    // depletion cap (Cje/Cjc) or the transit time (Tf/Tr) is live.
    [[nodiscard]] bool has_dyn_be() const { return cje_ > 0.0 || tf_ > 0.0; }
    [[nodiscard]] bool has_dyn_bc() const { return cjc_ > 0.0 || tr_ > 0.0; }
    [[nodiscard]] bool has_tt() const { return tf_ > 0.0 || tr_ > 0.0; }

    // --- M7: the transit-time diffusion charges (bjtload.c) --------------
    // The forward (BE) charge: Q_be = Tf*If*oef at Vbe > 0 (SPICE3
    // divides the forward diode current by the base charge qb = 1/oef -
    // the stored charge tracks the Early base-width modulation), plain
    // Tf*If at Vbe <= 0; the reverse (BC) charge Q_bc = Tr*Ir carries no
    // base-charge scaling (SPICE3 likewise). The ngspice form floors at
    // -Tf*Is / -Tr*Is in reverse (the generation component is not
    // stored); live_* reports the above-floor part the charge floor and
    // the snap guard reason about. The _vbe/_vbc partials carry the
    // Early chain exactly (the cross term is dQ_be/dVbc).
    [[nodiscard]] double q_tt_be(double vbe, double vbc, double is_eff,
                                 double vte_be) const {
        if (tf_ <= 0.0) return 0.0;
        const double q = tf_ * is_eff *
                         (std::exp(safe_exp_arg(vbe / vte_be)) - 1.0);
        return vbe > 0.0 ? q * (1.0 - vbc * inv_vaf_ - vbe * inv_var_) : q;
    }
    [[nodiscard]] double live_tt_be(double vbe, double vbc, double is_eff,
                                    double vte_be) const {
        if (tf_ <= 0.0) return 0.0;
        const double live = tf_ * is_eff * std::exp(safe_exp_arg(vbe / vte_be));
        return vbe > 0.0 ? live * (1.0 - vbc * inv_vaf_ - vbe * inv_var_)
                         : live;
    }
    [[nodiscard]] double c_tt_be_vbe(double vbe, double vbc, double is_eff,
                                     double vte_be) const {
        if (tf_ <= 0.0) return 0.0;
        const double ex = std::exp(safe_exp_arg(vbe / vte_be));
        if (vbe > 0.0) {
            const double iff = is_eff * (ex - 1.0);
            const double oef = 1.0 - vbc * inv_vaf_ - vbe * inv_var_;
            return tf_ * (is_eff / vte_be * ex * oef - iff * inv_var_);
        }
        return tf_ * is_eff / vte_be * ex;
    }
    [[nodiscard]] double c_tt_be_vbc(double vbe, double is_eff,
                                     double vte_be) const {
        if (tf_ <= 0.0 || vbe <= 0.0) return 0.0;
        const double ex = std::exp(safe_exp_arg(vbe / vte_be));
        return -tf_ * is_eff * (ex - 1.0) * inv_vaf_;
    }
    [[nodiscard]] double q_tt_bc(double vbc, double is_eff, double vte_bc) const {
        if (tr_ <= 0.0) return 0.0;
        return tr_ * is_eff *
               (std::exp(safe_exp_arg(vbc / vte_bc)) - 1.0);
    }
    [[nodiscard]] double live_tt_bc(double vbc, double is_eff, double vte_bc) const {
        if (tr_ <= 0.0) return 0.0;
        return tr_ * is_eff * std::exp(safe_exp_arg(vbc / vte_bc));
    }
    [[nodiscard]] double c_tt_bc(double vbc, double is_eff, double vte_bc) const {
        if (tr_ <= 0.0) return 0.0;
        return tr_ * is_eff / vte_bc * std::exp(safe_exp_arg(vbc / vte_bc));
    }

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
    // M4b: junction cap params (Cje/Cjc > 0 enables the depletion
    // companions)
    double cje_ = 0.0, cjc_ = 0.0;
    double mje_ = 0.5, mjc_ = 0.5;
    double vje_ = 0.75, vjc_ = 0.75;
    double fc_ = 0.5;
    // M7: transit times (Tf/Tr > 0 enables the diffusion charges; the
    // BE charge carries the Early base-charge scaling, the BC does not).
    double tf_ = 0.0, tr_ = 0.0;
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
    // level 77 (BSIM6-style core) block: the folded temperature, the
    // committed Vdsat (the dynamic hooks reuse it instead of re-solving),
    // then the SAME (V, Q, I, geq, jeq) + LTE-history shape as the Meyer
    // pairs over the intrinsic pairs (g-s', g-d', g-b).
    static constexpr std::size_t kB6TempK = 5;
    static constexpr std::size_t kB6Vdsat = 6;
    static constexpr std::size_t kB6GsV = 7, kB6GsQ = 8, kB6GsI = 9, kB6GsGeq = 10,
                                 kB6GsJeq = 11;
    static constexpr std::size_t kB6GdV = 12, kB6GdQ = 13, kB6GdI = 14,
                                 kB6GdGeq = 15, kB6GdJeq = 16;
    static constexpr std::size_t kB6GbV = 17, kB6GbQ = 18, kB6GbI = 19,
                                 kB6GbGeq = 20, kB6GbJeq = 21;
    static constexpr std::size_t kB6GsHistN = 22, kB6GsHist = 23;  // 6 slots
    static constexpr std::size_t kB6GdHistN = 29, kB6GdHist = 30;  // 6 slots
    static constexpr std::size_t kB6GbHistN = 36, kB6GbHist = 37;  // 6 slots
    static constexpr std::size_t kB6VdsatFrozen = 43;  // the Vdsat the
    // step's companions froze on: update_state refreshes kB6Vdsat inside
    // the Newton loop, so begin_step snapshots it here for commit/lte_ok -
    // a mid-step swap desynced the committed charges from the solved
    // companion (a constant spurious gate current was the symptom).
    static constexpr std::size_t kB6Slots = 44;

    Mosfet(std::string display, NodeId d, NodeId g, NodeId s, NodeId b,
           double l, double w, bool has_temp_inst, double temp_inst_c)
        : name_(std::move(display)), d_(d), g_(g), s_(s), b_(b), l_(l), w_(w),
          has_temp_inst_(has_temp_inst), temp_inst_c_(temp_inst_c) {}

    // The intrinsic channel terminals: with RDSW > 0 the channel hangs on
    // the internal 'name#d'/'name#s' nodes behind the series resistances
    // (the diode-Rs topology precedent); without it they ARE the terminals.
    [[nodiscard]] NodeId chan_d() const { return rdsw_ > 0.0 ? dp_ : d_; }
    [[nodiscard]] NodeId chan_s() const { return rdsw_ > 0.0 ? sp_ : s_; }

    [[nodiscard]] const std::string& name() const override { return name_; }
    [[nodiscard]] char kind() const override { return 'M'; }
    [[nodiscard]] std::vector<NodeId> terminals() const override {
        if (level_ == 77 && rdsw_ > 0.0) {
            return {d_, dp_, g_, s_, sp_, b_};
        }
        return {d_, g_, s_, b_};
    }
    // The MOSFET's channel connects D-S (through the controlled current
    // source); the gate is capacitive only (no DC path). The body junction
    // (B-S, B-D) is reverse-biased (a diode) — for the graph check, treat
    // D-S as a conductance path (the channel is always slightly conducting
    // via gmin). The gate has no DC path (open). Level 77 with RDSW adds
    // the d-d'-s'-s resistor chain (the diode-Rs graph precedent).
    [[nodiscard]] std::vector<std::pair<NodeId, NodeId>> conductance_edges()
        const override {
        if (level_ == 77 && rdsw_ > 0.0) {
            return {{d_, dp_}, {dp_, sp_}, {sp_, s_}};
        }
        return {{d_, s_}};  // channel (weak but present via gmin)
    }

    // §8.5: the b6 psi/Vdsat failure channel (the T-line overflow
    // precedent - the one-shot latch lives on the device because
    // load_error has no state slice).
    [[nodiscard]] ws::core::Status load_error(
        const std::vector<double>&, const TranStep*, const double*,
        std::size_t) const override {
        if (level_ == 77 && b6_failed_) {
            b6_failed_ = false;
            return spice_fail(dev_diag(
                ErrorKind::Convergence, "spice.convergence.b6_core",
                "BSIM6 core of " + name_ +
                    " failed to converge (surface potential or Vdsat solve)",
                loc));
        }
        return {};
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

    // --- level 77: the BSIM6-style core (src/bsim6_core.hpp) -----------------
    //
    // The wrapper maps the REAL node voltages into the core's MIRROR frame
    // (all four negated for PMOS), handles reverse drain operation by the
    // source/drain swap (the core itself is symmetric; the Vdseff layer is
    // not, so it always sees vds >= 0), and maps the results back: the
    // current and charges flip with sgn, the partials do not (the level-1
    // polarity lesson: d(sgn*f)/d(v_real) = df/dv_mirror).
    struct B6Eval {
        double id = 0.0;                     // real-frame drain current
        double did[4] = {};                  // d/d(vg, vd, vs, vb) real
        double qg = 0.0, qb = 0.0, qs = 0.0, qd = 0.0;  // real-frame, C
        double dqg[4] = {}, dqb[4] = {}, dqs[4] = {}, dqd[4] = {};
        double vdseff = 0.0;
        double vdsat = 0.0;
        bool ok = true;
    };

    // The three gate pair charges (real frame): qgs = -Qs, qgd = -Qd,
    // qgb = -Qb (qgs+qgd+qgb = Qg by construction - conservation).
    struct B6Pairs {
        double q[3] = {};       // qgs, qgd, qgb
        double dq[3][4] = {};   // their node-voltage partials
    };
    [[nodiscard]] static B6Pairs b6_pairs(const B6Eval& e) {
        B6Pairs p;
        p.q[0] = -e.qs;
        p.q[1] = -e.qd;
        p.q[2] = -e.qb;
        for (int k = 0; k < 4; ++k) {
            p.dq[0][k] = -e.dqs[k];
            p.dq[1][k] = -e.dqd[k];
            p.dq[2][k] = -e.dqb[k];
        }
        return p;
    }

    [[nodiscard]] B6Eval b6_wrap(double vg, double vd, double vs, double vb,
                                 double temp_k,
                                 double vdsat_fixed = -1.0) const {
        const double sgn = pnp_ ? -1.0 : 1.0;
        const bsim6::Derived dv = bsim6::derive(b6_, temp_k);
        const double vgbp = sgn * (vg - vb) - dv.vfb;
        const double vsb = sgn * (vs - vb);
        const double vds = sgn * (vd - vs);
        bsim6::EvalOpts opts;
        opts.vdsat_fixed = vdsat_fixed;
        B6Eval out;
        if (vds >= 0.0) {
            const bsim6::Eval e =
                bsim6::evaluate(b6_, dv, l_, w_, vgbp, vsb, vds, opts);
            if (!e.ok) {
                out.ok = false;
                return out;
            }
            out.id = sgn * e.id;
            for (int k = 0; k < 4; ++k) {
                out.did[k] = e.did[k];
                out.dqg[k] = e.dqg[k];
                out.dqb[k] = e.dqb[k];
                out.dqs[k] = e.dqs[k];
                out.dqd[k] = e.dqd[k];
            }
            out.qg = sgn * e.qg;
            out.qb = sgn * e.qb;
            out.qs = sgn * e.qs;
            out.qd = sgn * e.qd;
            out.vdseff = e.vdseff;
            out.vdsat = e.vdsat;
            return out;
        }
        // Reverse mode: evaluate with S/D swapped, then unswap. Column
        // permutation: real vd is the swapped vs and vice versa.
        const bsim6::Eval e = bsim6::evaluate(b6_, dv, l_, w_, vgbp, vsb + vds,
                                              -vds, opts);
        if (!e.ok) {
            out.ok = false;
            return out;
        }
        static const int perm[4] = {0, 2, 1, 3};
        out.id = -sgn * e.id;
        for (int k = 0; k < 4; ++k) {
            out.did[k] = -e.did[perm[k]];
            out.dqg[k] = e.dqg[perm[k]];
            out.dqb[k] = e.dqb[perm[k]];
        }
        // The swapped source IS the real drain.
        out.qs = sgn * e.qd;
        out.qd = sgn * e.qs;
        for (int k = 0; k < 4; ++k) {
            out.dqs[k] = e.dqd[perm[k]];
            out.dqd[k] = e.dqs[perm[k]];
        }
        out.vdseff = e.vdseff;
        out.vdsat = e.vdsat;
        return out;
    }

    // The mirror-frame pair voltages (vgs, vgd, vgb) from a solution.
    void b6_pair_volts(const std::vector<double>& x, double v[3]) const {
        const double sgn = pnp_ ? -1.0 : 1.0;
        v[0] = sgn * v_across(x, g_, chan_s());
        v[1] = sgn * v_across(x, g_, chan_d());
        v[2] = sgn * v_across(x, g_, b_);
    }

    // The pair charges + their mirror-frame node partials at given mirror
    // pair voltages, reusing a committed Vdsat (the hooks' linearization
    // point). v = {vgs, vgd, vgb}; columns (vg, vd, vs, vb) mirror.
    [[nodiscard]] B6Pairs b6_pairs_at(const double v[3], double vdsat,
                                      double temp_k, bool* ok) const {
        B6Pairs p;
        const bsim6::Derived dv = bsim6::derive(b6_, temp_k);
        bsim6::EvalOpts opts;
        opts.vdsat_fixed = vdsat;
        const double vgbp = v[2] - dv.vfb;
        const double vsb = v[2] - v[0];  // vgb - vgs = vs - vb
        const double vds = v[0] - v[1];  // vgs - vgd
        bsim6::Eval e;
        if (vds >= 0.0) {
            e = bsim6::evaluate(b6_, dv, l_, w_, vgbp, vsb, vds, opts);
        } else {
            // Reverse: swapped evaluation, then unswap.
            const bsim6::Eval es =
                bsim6::evaluate(b6_, dv, l_, w_, vgbp, vsb + vds, -vds, opts);
            e = es;
            if (es.ok) {
                static const int perm[4] = {0, 2, 1, 3};
                e.qs = es.qd;
                e.qd = es.qs;
                for (int k = 0; k < 4; ++k) {
                    e.dqs[k] = es.dqd[perm[k]];
                    e.dqd[k] = es.dqs[perm[k]];
                }
            }
        }
        *ok = e.ok;
        if (!e.ok) {
            return p;
        }
        p.q[0] = -e.qs;
        p.q[1] = -e.qd;
        p.q[2] = -e.qb;
        for (int k = 0; k < 4; ++k) {
            p.dq[0][k] = -e.dqs[k];
            p.dq[1][k] = -e.dqd[k];
            p.dq[2][k] = -e.dqb[k];
        }
        // The overlap caps ride the SAME pair charges (and therefore the
        // same companions + LTE): qgs += CGSO*W*vgs, etc.
        p.q[0] += b6_.cgso * w_ * v[0];
        p.q[1] += b6_.cgdo * w_ * v[1];
        p.q[2] += b6_.cgbo * l_ * v[2];
        p.dq[0][0] += b6_.cgso * w_;
        p.dq[1][0] += b6_.cgdo * w_;
        p.dq[2][0] += b6_.cgbo * l_;
        p.dq[0][2] -= b6_.cgso * w_;
        p.dq[1][1] -= b6_.cgdo * w_;
        p.dq[2][3] -= b6_.cgbo * l_;
        return p;
    }

    void b6_load_dc(DcLoadContext& cx) const {
        const NodeId cd = chan_d(), cs = chan_s();
        const B6Eval e = b6_wrap(cx.node_v(g_), cx.node_v(cd), cx.node_v(cs),
                                 cx.node_v(b_), load_temp_k(cx.temp_k()));
        if (!e.ok) {
            b6_failed_ = true;  // reported through load_error
            return;
        }
        // Channel Norton on the intrinsic nodes (the level-1 pattern).
        const double vg = cx.node_v(g_), vd = cx.node_v(cd),
                     vs = cx.node_v(cs), vb = cx.node_v(b_);
        const double id_const = e.id - (e.did[0] * vg + e.did[1] * vd +
                                        e.did[2] * vs + e.did[3] * vb);
        cx.add_entry(cd, g_, e.did[0]);
        cx.add_entry(cd, cd, e.did[1]);
        cx.add_entry(cd, cs, e.did[2]);
        cx.add_entry(cd, b_, e.did[3]);
        cx.add_entry(cs, g_, -e.did[0]);
        cx.add_entry(cs, cd, -e.did[1]);
        cx.add_entry(cs, cs, -e.did[2]);
        cx.add_entry(cs, b_, -e.did[3]);
        cx.add_rhs(cd, -id_const);
        cx.add_rhs(cs, id_const);
        // The gmin channel floor (the graph/convergence precedent).
        cx.add_conductance(cd, cs, cx.gmin());
        // Series resistances (constant stamps).
        if (rdsw_ > 0.0) {
            const double g = 1.0 / (0.5 * rdsw_ / (w_ * 1e6));
            cx.add_conductance(d_, dp_, g);
            cx.add_conductance(s_, sp_, g);
        }
    }

    void b6_load_ac(AcLoadContext& cx) const {
        const NodeId cd = chan_d(), cs = chan_s();
        const double sgn = pnp_ ? -1.0 : 1.0;
        // The frozen OP rides the shared mirror slots (update_state):
        // reconstruct real node voltages with vb = 0 (vg = sgn*(Vgs-Vbs),
        // vd = sgn*(Vds-Vbs), vs = -sgn*Vbs - the wrapper re-mirrors).
        const B6Eval e = b6_wrap(
            sgn * (cx.st(kVgsPrev) - cx.st(kVbsPrev)),
            sgn * (cx.st(kVdsPrev) - cx.st(kVbsPrev)),
            -sgn * cx.st(kVbsPrev), 0.0, load_temp_k(cx.temp_k()));
        if (!e.ok) {
            b6_failed_ = true;
            return;
        }
        // Channel conductances (real entries, the level-1 shape).
        cx.add_entry(cd, g_, {e.did[0], 0.0});
        cx.add_entry(cd, cd, {e.did[1], 0.0});
        cx.add_entry(cd, cs, {e.did[2], 0.0});
        cx.add_entry(cd, b_, {e.did[3], 0.0});
        cx.add_entry(cs, g_, {-e.did[0], 0.0});
        cx.add_entry(cs, cd, {-e.did[1], 0.0});
        cx.add_entry(cs, cs, {-e.did[2], 0.0});
        cx.add_entry(cs, b_, {-e.did[3], 0.0});
        if (rdsw_ > 0.0) {
            const double g = 1.0 / (0.5 * rdsw_ / (w_ * 1e6));
            cx.add_conductance(d_, dp_, {g, 0.0});
            cx.add_conductance(s_, sp_, {g, 0.0});
        }
        // The transcapacitance stamp: each pair (g,n_k) carries q_k; the
        // current the node g must SUPPLY to the pair is dq_k/dt, so
        // Y[g][j] += j*omega*dq_k/dv_j and the return node gets the same
        // with the opposite sign (the plain-cap convention: a linear q=C*v
        // reduces to add_conductance's +j*omega*C diagonal). Row g summed
        // over the pairs reproduces +j*omega*dQg/dv_j.
        const B6Pairs pr = b6_pairs(e);
        const NodeId ret[3] = {cs, cd, b_};
        const NodeId cols[4] = {g_, cd, cs, b_};
        for (int p = 0; p < 3; ++p) {
            for (int j = 0; j < 4; ++j) {
                const std::complex<double> y = {0.0, cx.omega * pr.dq[p][j]};
                cx.add_entry(g_, cols[j], y);
                cx.add_entry(ret[p], cols[j], -y);
            }
        }
        // Overlaps as plain caps.
        cx.add_conductance(g_, cs, {0.0, cx.omega * b6_.cgso * w_});
        cx.add_conductance(g_, cd, {0.0, cx.omega * b6_.cgdo * w_});
        cx.add_conductance(g_, b_, {0.0, cx.omega * b6_.cgbo * l_});
    }

    void b6_load_tran(DcLoadContext& cx) const {
        b6_load_dc(cx);
        const NodeId cd = chan_d(), cs = chan_s();
        const double sgn = pnp_ ? -1.0 : 1.0;
        cx.add_conductance(g_, cs, cx.st(kB6GsGeq));
        cx.add_current(cs, g_, sgn * cx.st(kB6GsJeq));
        cx.add_conductance(g_, cd, cx.st(kB6GdGeq));
        cx.add_current(cd, g_, sgn * cx.st(kB6GdJeq));
        cx.add_conductance(g_, b_, cx.st(kB6GbGeq));
        cx.add_current(b_, g_, sgn * cx.st(kB6GbJeq));
    }

    // The b6 companions: each pair's charge is a STATE FUNCTION of the
    // terminal voltages (no incremental tracking - the charge-based
    // advantage over Meyer: Q is always q_pair(v), conservation exact).
    // begin_step linearizes at the committed bias with the committed
    // Vdsat; the (V,Q,I,geq,jeq) bookkeeping follows the Cjo/Meyer house
    // convention (trap: geq = 2C/dt, jeq = -(geq*V+I); BE: geq = C/dt,
    // jeq = -Q/dt), where C is the pair's self-transcapacitance
    // d(q_pair)/d(v_pair) from the core's analytic Jacobian.
    void b6_begin_step(const TranStep& step, double* st) {
        st[kB6VdsatFrozen] = st[kB6Vdsat];
        double v[3] = {st[kB6GsV], st[kB6GdV], st[kB6GbV]};
        bool ok = true;
        const B6Pairs p = b6_pairs_at(v, st[kB6VdsatFrozen], st[kB6TempK], &ok);
        if (!ok) {
            b6_failed_ = true;
            return;
        }
        // Self-caps: d(qgs)/d(vgs)|vgd,vgb = d(-Qs)/d(vs), etc.
        const double cap[3] = {-p.dq[0][2], -p.dq[1][1], -p.dq[2][3]};
        const std::size_t slot[3][5] = {
            {kB6GsV, kB6GsQ, kB6GsI, kB6GsGeq, kB6GsJeq},
            {kB6GdV, kB6GdQ, kB6GdI, kB6GdGeq, kB6GdJeq},
            {kB6GbV, kB6GbQ, kB6GbI, kB6GbGeq, kB6GbJeq},
        };
        for (int k = 0; k < 3; ++k) {
            const double c = cap[k] > 0.0 ? cap[k] : 0.0;
            if (step.be) {
                st[slot[k][3]] = c / step.dt;
                st[slot[k][4]] = -st[slot[k][1]] / step.dt;
            } else {
                st[slot[k][3]] = 2.0 * c / step.dt;
                st[slot[k][4]] = -(st[slot[k][3]] * st[slot[k][0]] + st[slot[k][2]]);
            }
        }
    }

    void b6_commit(const std::vector<double>& x, const TranStep& step,
                   double* st) {
        double v[3];
        b6_pair_volts(x, v);
        bool ok = true;
        const B6Pairs p = b6_pairs_at(v, st[kB6VdsatFrozen], st[kB6TempK], &ok);
        if (!ok) {
            b6_failed_ = true;
            return;
        }
        const std::size_t slot[3][5] = {
            {kB6GsV, kB6GsQ, kB6GsI, kB6GsGeq, kB6GsJeq},
            {kB6GdV, kB6GdQ, kB6GdI, kB6GdGeq, kB6GdJeq},
            {kB6GbV, kB6GbQ, kB6GbI, kB6GbGeq, kB6GbJeq},
        };
        const std::size_t hn[3] = {kB6GsHistN, kB6GdHistN, kB6GbHistN};
        const std::size_t h[3] = {kB6GsHist, kB6GdHist, kB6GbHist};
        for (int k = 0; k < 3; ++k) {
            const auto& sl = slot[k];
            st[sl[1]] = p.q[k];
            st[sl[0]] = v[k];
            st[sl[2]] = st[sl[3]] * v[k] + st[sl[4]];
            if (step.be) {
                st[hn[k]] = 0.0;
            } else {
                const double n = st[hn[k]];
                if (n < 3.0) {
                    const auto idx = static_cast<std::size_t>(n);
                    st[h[k] + idx * 2] = step.time;
                    st[h[k] + idx * 2 + 1] = st[sl[1]];
                    st[hn[k]] = n + 1.0;
                } else {
                    for (std::size_t j = 0; j < 2; ++j) {
                        st[h[k] + j * 2] = st[h[k] + (j + 1) * 2];
                        st[h[k] + j * 2 + 1] = st[h[k] + (j + 1) * 2 + 1];
                    }
                    st[h[k] + 4] = step.time;
                    st[h[k] + 5] = st[sl[1]];
                }
            }
        }
    }

    [[nodiscard]] bool b6_lte_ok(const TranStep& step, const std::vector<double>& x,
                                 double reltol, double chgtol, double trtol,
                                 const double* st, double* ratio) const {
        double v[3];
        b6_pair_volts(x, v);
        bool ok = true;
        const B6Pairs p = b6_pairs_at(v, st[kB6VdsatFrozen], st[kB6TempK], &ok);
        if (!ok) {
            if (ratio != nullptr) {
                *ratio = std::numeric_limits<double>::infinity();
            }
            return false;
        }
        const std::size_t qsl[3] = {kB6GsQ, kB6GdQ, kB6GbQ};
        const std::size_t hn[3] = {kB6GsHistN, kB6GdHistN, kB6GbHistN};
        const std::size_t h[3] = {kB6GsHist, kB6GdHist, kB6GbHist};
        double worst = 0.0;
        for (int k = 0; k < 3; ++k) {
            const double n = st[hn[k]];
            if (n < 3.0) {
                continue;
            }
            const double q3 = p.q[k];
            const double t0 = st[h[k]], q0 = st[h[k] + 1];
            const double t1 = st[h[k] + 2], q1 = st[h[k] + 3];
            const double t2 = st[h[k] + 4], q2 = st[h[k] + 5];
            const double t3 = step.time;
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
            const double tolq = trtol * (reltol * std::max(std::abs(q3), std::abs(st[qsl[k]])) + chgtol);
            if (tolq > 0.0) {
                worst = std::max(worst, eps / tolq);
            }
        }
        if (ratio != nullptr) {
            *ratio = worst;
        }
        return worst <= 1.0;
    }

    void b6_seed_state(const std::vector<double>& x, double* st) {
        // The t=0 OP: pair charges at the OP bias, a FRESH Vdsat for the
        // companions, zero companion currents.
        const NodeId cd = chan_d(), cs = chan_s();
        const B6Eval e = b6_wrap(v_across(x, g_, kGround), v_across(x, cd, kGround),
                                 v_across(x, cs, kGround), v_across(x, b_, kGround),
                                 st[kB6TempK]);
        if (!e.ok) {
            b6_failed_ = true;
            return;
        }
        st[kB6Vdsat] = e.vdsat;
        st[kB6VdsatFrozen] = e.vdsat;
        double v[3];
        b6_pair_volts(x, v);
        bool ok = true;
        const B6Pairs p = b6_pairs_at(v, e.vdsat, st[kB6TempK], &ok);
        if (!ok) {
            b6_failed_ = true;
            return;
        }
        const std::size_t vsl[3] = {kB6GsV, kB6GdV, kB6GbV};
        const std::size_t qsl[3] = {kB6GsQ, kB6GdQ, kB6GbQ};
        const std::size_t isl[3] = {kB6GsI, kB6GdI, kB6GbI};
        const std::size_t hn[3] = {kB6GsHistN, kB6GdHistN, kB6GbHistN};
        for (int k = 0; k < 3; ++k) {
            st[vsl[k]] = v[k];
            st[qsl[k]] = p.q[k];
            st[isl[k]] = 0.0;
            st[hn[k]] = 0.0;
        }
    }

    void load_dc(DcLoadContext& cx) const override {
        if (level_ == 77) {
            b6_load_dc(cx);
            return;
        }
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
        if (level_ == 77) {
            b6_load_ac(cx);
            return;
        }
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
        if (level_ == 77) {
            b6_load_tran(cx);
            return;
        }
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
        if (level_ == 77) {
            return kB6Slots;
        }
        return has_caps() ? 42 : 5;
    }
    [[nodiscard]] bool is_dynamic() const override {
        return level_ == 77 ? true : has_caps();
    }

    // §7.5 (M5, mos1noi.c): _thermal channel noise 4kT·(2/3·gm) between
    // drain and source (the level-1 nlev<3 form - (8/3)kT·gm as a
    // density), plus _1overf flicker. The DESIGN's per-model formula
    // KF·|Id|^AF/f^EF (no geometry/Cox term - the model carries no TOX
    // dependency here; a documented deviation from ngspice's nlev=0
    // geometry form). The OP rides state[kVgsPrev]/[kVdsPrev].
    void load_noise(NoiseContext& cx) const override {
        if (level_ == 77) {
            return;  // the b6 noise model is the follow-up (documented)
        }
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
        if (level_ == 77) {
            st[kB6TempK] = phys::c_to_k(c.temp);
            return;
        }
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
        if (level_ == 77) {
            b6_begin_step(step, st);
            return;
        }
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
        if (level_ == 77) {
            b6_commit(x, step, st);
            return;
        }
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
        if (level_ == 77) {
            const NodeId cd = chan_d(), cs = chan_s();
            const double sgn = pnp_ ? -1.0 : 1.0;
            const double vgs = sgn * v_across(x_load, g_, cs);
            const double vds = sgn * v_across(x_load, cd, cs);
            const double vbs = sgn * v_across(x_load, b_, cs);
            const B6Eval e =
                b6_wrap(v_across(x_load, g_, kGround), v_across(x_load, cd, kGround),
                        v_across(x_load, cs, kGround), v_across(x_load, b_, kGround),
                        load_temp_k(temp_k));
            b6_failed_ = b6_failed_ || !e.ok;
            st[kIPrev] = st[kICur];
            st[kICur] = e.ok ? e.id : 0.0;
            st[kVgsPrev] = vgs;
            st[kVdsPrev] = vds;
            st[kVbsPrev] = vbs;
            if (e.ok) {
                st[kB6Vdsat] = e.vdsat;
            }
            return false;  // the SP core is smooth; no limiting
        }
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
    // Level=77 selects the BSIM6-style core (bsim6_core.hpp): TOX, EPSROX,
    // NDEP, VTH0, U0, UA, UB, VSAT, RDSW, PCLM, XJ, CGSO/CGDO/CGBO, TNOM
    // implemented; the long BSIM4/6 tail is accepted-but-ignored so
    // production decks parse (the diode-TT precedent; STATUS lists them).
    ws::core::Status apply_model(const ModelDef& model,
                                 NodeResolver& nodes) override {
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
        double level_val = 1.0;
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
            else if (key == "tox") { tox_val = v; b6_.tox = v; }
            else if (key == "cgso") { cgso_val = v; b6_.cgso = v; }
            else if (key == "cgdo") { cgdo_val = v; b6_.cgdo = v; }
            else if (key == "cgbo") { cgbo_val = v; b6_.cgbo = v; }
            else if (key == "level") level_val = v;
            else if (key == "version") { /* 77 lineage marker; ignored */ }
            else if (key == "epsrox") b6_.epsrox = v;
            else if (key == "ndep") b6_.ndep_cm3 = v;
            else if (key == "vth0") b6_.vth0 = v;
            else if (key == "u0") b6_.u0 = v;
            else if (key == "ua") b6_.ua = v;
            else if (key == "ub") b6_.ub = v;
            else if (key == "vsat") b6_.vsat = v;
            else if (key == "rdsw") b6_.rdsw = v;
            else if (key == "pclm") b6_.pclm = v;
            else if (key == "xj") b6_.xj = v;
            else if (key == "cj" || key == "mj" || key == "cjsw" ||
                     key == "mjsw" || key == "pb" || key == "js" ||
                     key == "cbd" || key == "cbs" || key == "nsub" ||
                     key == "nss" || key == "tpg" || key == "ld" ||
                     key == "xl" || key == "wd" || key == "xw" ||
                     // The BSIM6 tail: accepted so real decks parse, not
                     // implemented (STATUS sharp edge lists the set).
                     key == "k1" || key == "k2" || key == "k3" ||
                     key == "k3b" || key == "w0" || key == "dvt0" ||
                     key == "dvt1" || key == "dvt2" || key == "dvt0w" ||
                     key == "dvt1w" || key == "eta0" || key == "etab" ||
                     key == "dsub" || key == "cdsc" || key == "cdscd" ||
                     key == "cdscb" || key == "nfactor" || key == "voff" ||
                     key == "cit" || key == "a0" || key == "ags" ||
                     key == "b0" || key == "bsat" ||
                     key == "pdiblc1" || key == "pdiblc2" || key == "pdiblcb" ||
                     key == "drout" || key == "pvag" || key == "delta" ||
                     key == "rdswmin" || key == "rdw" || key == "rsw" ||
                     key == "prwg" || key == "prwb" || key == "nch" ||
                     key == "ngate" || key == "phin" || key == "mobmod" ||
                     key == "capmod" || key == "noimod" || key == "paramchk" ||
                     key == "lint" || key == "wint" ||
                     key == "ll" || key == "lln" || key == "wl" ||
                     key == "wln" || key == "lwl" || key == "ww" ||
                     key == "wwn" || key == "wwn") {
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
        if (level_val != 1.0 && level_val != 77.0) {
            return spice_fail(dev_diag(
                ErrorKind::Semantic, "spice.semantic.mos_level",
                "model '" + model.name.text + "': Level " +
                    Device::CardWriter::num(level_val) +
                    " is not implemented (1 = Shichman-Hodges, 77 = BSIM6)",
                model.loc));
        }
        level_ = static_cast<int>(level_val);
        if (level_ == 77) {
            if (!(b6_.tox > 0.0) || !std::isfinite(b6_.tox)) {
                return spice_fail(dev_diag(
                    ErrorKind::Semantic, "spice.semantic.mos_b6",
                    "model '" + model.name.text + "': Level 77 needs Tox > 0",
                    model.loc));
            }
            if (!(b6_.ndep_cm3 > 0.0) || !(b6_.u0 > 0.0)) {
                return spice_fail(dev_diag(
                    ErrorKind::Semantic, "spice.semantic.mos_b6",
                    "model '" + model.name.text +
                        "': Level 77 needs Ndep > 0 and U0 > 0",
                    model.loc));
            }
            if (b6_.rdsw < 0.0 || b6_.vsat < 0.0) {
                return spice_fail(dev_diag(
                    ErrorKind::Semantic, "spice.semantic.mos_b6",
                    "model '" + model.name.text +
                        "': Level 77 needs Rdsw >= 0 and Vsat >= 0",
                    model.loc));
            }
            if (b6_.pclm < 0.0 || !(b6_.xj > 0.0)) {
                return spice_fail(dev_diag(
                    ErrorKind::Semantic, "spice.semantic.mos_b6",
                    "model '" + model.name.text +
                        "': Level 77 needs Pclm >= 0 (0 disables the CLM "
                        "layer) and Xj > 0",
                    model.loc));
            }
            b6_.tnom_c = tnom;
            rdsw_ = b6_.rdsw;
            dp_ = kGround;
            sp_ = kGround;
            if (rdsw_ > 0.0) {
                // The diode-Rs topology: internal nodes behind half the
                // total Rds each side, claimed before the §6.6 checks.
                Token tok;
                tok.text = name_ + "#d";
                tok.loc = model.loc;
                dp_ = nodes.node(tok);
                tok.text = name_ + "#s";
                sp_ = nodes.node(tok);
            }
        }
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
    // Level 77: the BSIM6-style core (bsim6_core.hpp) + its series-R
    // internal nodes (kGround when RDSW = 0 - no nodes exist at all, the
    // diode-Rs precedent) and the psi/Vdsat failure latch (the T-line
    // overflow precedent: load_error has no state access).
    int level_ = 1;
    bsim6::Params b6_;
    double rdsw_ = 0.0;
    NodeId dp_ = kGround, sp_ = kGround;
    mutable bool b6_failed_ = false;
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
