// The JFET (device letter J, the last classic SPICE3 semiconductor): the
// Shichman-Hodges junction FET - the diode/BJT pattern applied one more
// time. Two gate pn-junctions (gate-source and gate-drain) modeled as
// Shockley diodes with the diode's pnjlim, plus fetlim (SPICE3 devsup.c's
// DEVfetlim, the FET-specific limiter that keeps vgs/vgd within a moving
// window of Vto), a quadratic channel with the classic four regions
// (normal/inverse x cutoff/linear/saturation, jfetload.c's original
// SPICE3 section - the Sydney University b/Bfac refinement is not
// carried), depletion junction caps (Cgs/Cgd over Pb with the Fc
// linear extension - the diode Cjo equations with m = 0.5), the full
// SPICE3 temperature model (jfettemp.c: Is(T) with the XTI quirk, the
// Pb(T) pbfact chain, Cgs/Cgd(T), Vto(T) through Vtotc/Tcv, Beta(T)
// through Betatce/Bex), parasitic Rd/Rs on internal nodes (the diode-Rs
// topology), and the jfetnoi.c noise set (rd/rs thermal, the 2/3*gm
// channel, KF/AF flicker).
//
// Channel equations (the NJF "mirror frame", N-channel signs; PJF
// negates the terminal mapping via sgn = -1):
//   vgs = v(g) - v(s'),  vgd = v(g) - v(d'),  vds = vgs - vgd
//   Normal mode (vds >= 0), vgst = vgs - Vto:
//     cutoff   vgst <= 0:                 Id = 0,           gm = 0,  gds = 0
//     linear   0 < vds < vgst:            Id = B*(1+l*vds)*vds*(2*vgst - vds)
//     sat      vds >= vgst:               Id = B*(1+l*vds)*vgst^2
//   Inverse mode (vds < 0), vgdt = vgd - Vto (source and drain swap):
//     cutoff   vgdt <= 0:                 Id = 0
//     linear   0 < -vds < vgdt:           Id = B*(1-l*vds)*vds*(2*vgdt + vds)
//     sat      -vds >= vgdt:              Id = -B*(1-l*vds)*vgdt^2
//   with B = Beta(T)*area, l = Lambda. gm/gds are the hand derivatives
//   (jfetload.c's twob form).
//
// Gate junctions: Is(T)*area through vgs/vgd on plain Vt (the SPICE3
// JFET quirk: the emission coefficient N rides ONLY the Is(T) exponent
// denominator - the junction itself evaluates at Vt, never N*Vt), with
// the cubic reverse extrapolation below -3*Vt (jfetload.c) and the gmin
// floor. vcrit = Vt*ln(Vt/(sqrt(2)*Is(T))) is the pnjlim knee.
//
// State layout (state_slots == 4, or 33 when Cgs/Cgd > 0):
//   [0] Vgs_prev  - the gs junction's limiter vold (the CLAMPED Vgs)
//   [1] Vgd_prev  - the gd junction's limiter vold (the CLAMPED Vgd)
//   [2] Id_cur    - channel current at the current load (the residual)
//   [3] Id_prev   - channel current at the previous load
//   [4..8]   gs companion (V, Q, I, geq, jeq)       the BJT-Cje
//   [9..13]  gd companion (V, Q, I, geq, jeq)       architecture
//   [14..20] gs LTE history (n + 3 (t, q) pairs)   (one block per
//   [21..27] gd LTE history (n + 3 (t, q) pairs)    junction)
//   [28] Cgs_eff   - folded Cgs(T)*area            the diode-TT fold
//   [29] Cgd_eff   - folded Cgd(T)*area             precedent (begin_step
//   [30] Gate_pot  - folded Pb(T)                    sees no load context)
//   [31] CorDepCap - folded Fc*Pb(T)
//   [32] F1        - folded Pb(T)*(1-e^{.5*xfc})/.5
//
// Rd/Rs > 0 claim the internal nodes <name>#d / <name>#s in apply_model
// (the diode's Rs-precedent topology; resolve_models runs before the
// 6.6 graph checks so the nodes join them like any other). The channel
// and the gate junctions live on the prime nodes; Rd = Rs = 0 (the
// defaults) keeps the stamps directly on the terminals.

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

// Voltage across a terminal pair from the solution vector (ground -> 0).
[[nodiscard]] double v_across(const std::vector<double>& x, NodeId p, NodeId n) {
    const double vp = (p == kGround) ? 0.0 : x[p];
    const double vn = (n == kGround) ? 0.0 : x[n];
    return vp - vn;
}

constexpr double kVtPerK = phys::kKoverQ;

// 5.3 pnjlim - the FULL SPICE3 form, not the diode's simplified one.
// The diode/BJT copy dropped the negative-direction branch ("the icheck
// first-time flag is folded into the always-directional form"); for the
// JFET that branch is load-bearing. A depletion device's channel is
// EXACTLY off below Vto, so a current-biased source follower whose first
// Newton iterate swings the source to +11 V (vgs = -11 V) walks into the
// gmin-regularized cutoff solution (vgs ~ -I/gmin ~ -1e8) and converges
// there - a self-consistent spurious answer the ladder can never rescue
// (Newton already "converged"). SPICE3's negative floor (vnew < 0 ->
// clamp to 2*vold - 1, or -vold - 1 when vold > 0) keeps the first wild
// iterate at vgs >= -1: still conducting for the default Vto = -2, so
// Newton tracks the true basin. That floor IS ngspice's answer to the
// same trap (its JFET pairs it with the MODEINITJCT vgs = -1 junction
// init; the floor reaches the same start from the cold x = 0). The
// positive branch is the repo's proven simplified form (vold +/- 2*Vt
// past vcrit - same role as the diode's).
[[nodiscard]] double pnjlim(double vnew, double vold, double vt, double vcrit) {
    if (vnew > vcrit && std::abs(vnew - vold) > 2.0 * vt) {
        return vold + ((vnew > vold) ? 2.0 * vt : -2.0 * vt);
    }
    if (vnew < 0.0) {
        // SPICE3's reverse-direction floor (devsup.c DEVpnjlim's else).
        const double floor_arg = (vold > 0.0) ? (-vold - 1.0) : (2.0 * vold - 1.0);
        if (vnew < floor_arg) {
            return floor_arg;
        }
    }
    return vnew;
}

// fetlim (SPICE3 devsup.c's DEVfetlim, Alan Gillespie's vtstlo fix,
// translated verbatim): the FET channel limiter. Keeps the per-iteration
// vgs/vgd excursion within a moving window of Vto (tsthi = 2*|vold-Vto|+2
// out in the ON region, vtstlo = |vold-Vto|+1 in the OFF/middle regions)
// so the quadratic channel and its region boundaries are crossed in
// bounded steps - the companion of pnjlim for a threshold device.
[[nodiscard]] double fetlim(double vnew, double vold, double vto) {
    const double vtsthi = std::fabs(2.0 * (vold - vto)) + 2.0;
    const double vtstlo = std::fabs(vold - vto) + 1.0;
    const double vtox = vto + 3.5;
    const double delv = vnew - vold;
    if (vold >= vto) {
        if (vold >= vtox) {
            if (delv <= 0.0) {
                // going off
                if (vnew >= vtox) {
                    if (-delv > vtstlo) {
                        return vold - vtstlo;
                    }
                } else {
                    return std::max(vnew, vto + 2.0);
                }
            } else {
                // staying on
                if (delv >= vtsthi) {
                    return vold + vtsthi;
                }
            }
        } else {
            // middle region
            if (delv <= 0.0) {
                return std::max(vnew, vto - 0.5);
            }
            return std::min(vnew, vto + 4.0);
        }
    } else {
        // off
        if (delv <= 0.0) {
            if (-delv > vtsthi) {
                return vold - vtsthi;
            }
        } else {
            const double vtemp = vto + 0.5;
            if (vnew <= vtemp) {
                if (delv > vtstlo) {
                    return vold + vtstlo;
                }
            } else {
                return vtemp;
            }
        }
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

// --- J (JFET) ---------------------------------------------------------------------

class Jfet final : public Device {
public:
    // Limiter/residual slots (always present).
    static constexpr std::size_t kVgsPrev = 0;
    static constexpr std::size_t kVgdPrev = 1;
    static constexpr std::size_t kICur = 2;
    static constexpr std::size_t kIPrev = 3;
    // Gate-cap companions (present when has_caps()).
    static constexpr std::size_t kVgsComp = 4;
    static constexpr std::size_t kQgsComp = 5;
    static constexpr std::size_t kIgsComp = 6;
    static constexpr std::size_t kGeqGs = 7;
    static constexpr std::size_t kJeqGs = 8;
    static constexpr std::size_t kVgdComp = 9;
    static constexpr std::size_t kQgdComp = 10;
    static constexpr std::size_t kIgdComp = 11;
    static constexpr std::size_t kGeqGd = 12;
    static constexpr std::size_t kJeqGd = 13;
    // LTE history (3 (t, q) pairs per junction).
    static constexpr std::size_t kHistNGs = 14;
    static constexpr std::size_t kHistGs = 15;   // 6 slots
    static constexpr std::size_t kHistNGd = 21;
    static constexpr std::size_t kHistGd = 22;   // 6 slots
    // Folded cap model mirrors (the diode-TT fold precedent).
    static constexpr std::size_t kCgsEff = 28;
    static constexpr std::size_t kCgdEff = 29;
    static constexpr std::size_t kGatePotT = 30;
    static constexpr std::size_t kCorDepCap = 31;
    static constexpr std::size_t kF1T = 32;

    Jfet(std::string display, NodeId d, NodeId g, NodeId s, ExprProgram area,
         bool has_ic, double ic_vds, double ic_vgs, bool has_temp_inst,
         double temp_inst_c)
        : name_(std::move(display)), d_(d), g_(g), s_(s), area_(std::move(area)),
          has_ic_(has_ic), ic_vds_(ic_vds), ic_vgs_(ic_vgs),
          has_temp_inst_(has_temp_inst), temp_inst_c_(temp_inst_c) {}

    [[nodiscard]] const std::string& name() const override { return name_; }
    [[nodiscard]] char kind() const override { return 'J'; }
    [[nodiscard]] std::vector<NodeId> terminals() const override {
        return {d_, g_, s_};
    }
    // The DC-path graph: both gate junctions conduct a little at any bias
    // (ggs = Is/Vt + gmin at zero bias), so the gate reaches the source
    // and drain primes through them; Rd/Rs add the terminal-to-prime
    // edges when the internal nodes exist. In cutoff the channel itself
    // is exactly open (gds = 0, no gmin floor - ngspice's JFET likewise
    // floors only the junctions), and the gate junctions still carry the
    // d-g-s path.
    [[nodiscard]] std::vector<std::pair<NodeId, NodeId>> conductance_edges()
        const override {
        std::vector<std::pair<NodeId, NodeId>> edges;
        edges.reserve(4);
        edges.push_back({g_, dp()});
        edges.push_back({g_, sp()});
        if (dp_ != kGround) {
            edges.push_back({d_, dp_});
        }
        if (sp_ != kGround) {
            edges.push_back({s_, sp_});
        }
        return edges;
    }

    // --- nodes: the primes are the terminals unless Rd/Rs split them ------
    [[nodiscard]] NodeId dp() const { return (dp_ != kGround) ? dp_ : d_; }
    [[nodiscard]] NodeId sp() const { return (sp_ != kGround) ? sp_ : s_; }

    // 7.7: the effective temperature for this load - the instance temp=
    // override when given, else the run's (elaboration) temperature.
    [[nodiscard]] double load_temp_k(double ctx_temp_k) const {
        return has_temp_inst_ ? phys::c_to_k(temp_inst_c_) : ctx_temp_k;
    }

    // The temp-dependent model quantities at one (area, temperature),
    // jfettemp.c's chain. At T == Tnom every field equals the frozen
    // model value (the explicit shortcut keeps default decks bit-for-bit
    // - the pbfact chain at T == Tnom is only approximately the identity
    // because egfet(REFTEMP) and the 1.1150877 constant differ in the
    // 5th digit).
    struct TempOp {
        double vt = 0.02588;       // Vt(T)
        double is_eff = 1e-14;     // Is(T)*area (gate junctions)
        double vcrit = 0.7;        // pnjlim knee
        double vto = -2.0;         // Vto(T)
        double beta_eff = 1e-4;    // Beta(T)*area
        double gate_pot = 1.0;     // Pb(T)
        double cor_dep_cap = 0.5;  // Fc*Pb(T)
        double f1 = 0.808;         // Pb(T)*(1 - sqrt(1-Fc))/(1/2)
        double cgs_eff = 0.0;      // Cgs(T)*area
        double cgd_eff = 0.0;      // Cgd(T)*area
    };
    [[nodiscard]] TempOp temp_op(double area, double temp_k) const {
        TempOp op;
        op.vt = temp_k * kVtPerK;
        if (temp_k == tnom_k_) {
            // Frozen Tnom values (bit-identical to the model card).
            op.is_eff = is_ * area;
            const double arg = op.vt / (std::sqrt(2.0) * is_);
            op.vcrit = (arg > 1.0) ? op.vt * std::log(arg) : op.vt;
            op.vto = vto_;
            op.beta_eff = beta_ * area;
            op.gate_pot = pb_;
            op.cor_dep_cap = fc_ * pb_;
            op.f1 = pb_ * (1.0 - std::exp(0.5 * xfc())) / 0.5;
            op.cgs_eff = cgs_ * area;
            op.cgd_eff = cgd_ * area;
            return op;
        }
        // Is(T) (jfettemp.c, the SPICE3 quirk: the (T/Tnom)^XTI factor
        // applies only when XTI is explicitly on the model card; N rides
        // only the Eg exponent's denominator).
        const double ratio1 = temp_k / tnom_k_ - 1.0;
        const double vtn = n_ * op.vt;
        double is_t = is_ * std::exp(ratio1 * eg_ / vtn);
        if (xti_given_) {
            is_t *= std::pow(1.0 + ratio1, xti_);
        }
        op.is_eff = is_t * area;
        const double arg = op.vt / (std::sqrt(2.0) * is_t);
        op.vcrit = (arg > 1.0) ? op.vt * std::log(arg) : op.vt;
        // Vto(T): Vtotc (V/C) when given, else Tcv (negated - both spell
        // the threshold's drift, jfettemp.c).
        op.vto = vtotc_given_ ? vto_ + vtotc_ * (temp_k - tnom_k_)
                              : vto_ - tcv_ * (temp_k - tnom_k_);
        // Beta(T): Betatce (percent/C through 1.01^x) when given, else
        // the Bex mobility exponent (T/Tnom)^Bex.
        op.beta_eff = betatce_given_
                          ? beta_ * std::pow(1.01, betatce_ * (temp_k - tnom_k_))
                          : beta_ * std::pow(temp_k / tnom_k_, bex_);
        op.beta_eff *= area;
        // Pb(T): the SPICE3 pbfact chain (the mos1temp precedent,
        // jfettemp.c's spelling) plus the Cgs/Cgd cjfact grading.
        const double vt_tnom = tnom_k_ * kVtPerK;
        const double fact1 = tnom_k_ / phys::kRefTempK;
        const double fact2 = temp_k / phys::kRefTempK;
        const double kt1 = phys::kBoltz * tnom_k_;
        const double egfet1 = phys::egfet(tnom_k_);
        const double arg1 =
            -egfet1 / (kt1 + kt1) +
            1.1150877 / (phys::kBoltz * (phys::kRefTempK + phys::kRefTempK));
        const double pbfact1 =
            -2.0 * vt_tnom * (1.5 * std::log(fact1) + phys::kQ * arg1);
        const double pbo = (pb_ - pbfact1) / fact1;
        const double gmaold = (pb_ - pbo) / pbo;
        const double cjfact = 1.0 / (1.0 + 0.5 * (4e-4 * (tnom_k_ - phys::kRefTempK) -
                                                  gmaold));
        const double kt = phys::kBoltz * temp_k;
        const double egfet_t = phys::egfet(temp_k);
        const double argt =
            -egfet_t / (kt + kt) +
            1.1150877 / (phys::kBoltz * (phys::kRefTempK + phys::kRefTempK));
        const double pbfact =
            -2.0 * op.vt * (1.5 * std::log(fact2) + phys::kQ * argt);
        op.gate_pot = fact2 * pbo + pbfact;
        const double gmanew = (op.gate_pot - pbo) / pbo;
        const double cjfact1 =
            1.0 + 0.5 * (4e-4 * (temp_k - phys::kRefTempK) - gmanew);
        op.cgs_eff = cgs_ * area * cjfact * cjfact1;
        op.cgd_eff = cgd_ * area * cjfact * cjfact1;
        op.cor_dep_cap = fc_ * op.gate_pot;
        op.f1 = op.gate_pot * (1.0 - std::exp(0.5 * xfc())) / 0.5;
        return op;
    }

    // --- the shared DC evaluation (load_dc / update_state / noise) --------

    struct GateEval {
        double igs = 0.0;   // gs junction current, g -> s
        double ggs = 0.0;   // its conductance
        double igd = 0.0;   // gd junction current, g -> d
        double ggd = 0.0;   // its conductance
    };
    // The gate junctions at the (limited) mirror-frame voltages,
    // jfetload.c: plain Vt (never N*Vt), the cubic reverse extrapolation
    // below -3*Vt, gmin folded in.
    [[nodiscard]] GateEval gate_eval(double vgs, double vgd, double gmin,
                                     const TempOp& op) const {
        GateEval e;
        if (vgs < -3.0 * op.vt) {
            const double arg = std::pow(3.0 * op.vt / (vgs * phys::kE), 3.0);
            e.igs = -op.is_eff * (1.0 + arg) + gmin * vgs;
            e.ggs = op.is_eff * 3.0 * arg / vgs + gmin;
        } else {
            const double evgs = std::exp(safe_exp_arg(vgs / op.vt));
            e.ggs = op.is_eff * evgs / op.vt + gmin;
            e.igs = op.is_eff * (evgs - 1.0) + gmin * vgs;
        }
        if (vgd < -3.0 * op.vt) {
            const double arg = std::pow(3.0 * op.vt / (vgd * phys::kE), 3.0);
            e.igd = -op.is_eff * (1.0 + arg) + gmin * vgd;
            e.ggd = op.is_eff * 3.0 * arg / vgd + gmin;
        } else {
            const double evgd = std::exp(safe_exp_arg(vgd / op.vt));
            e.ggd = op.is_eff * evgd / op.vt + gmin;
            e.igd = op.is_eff * (evgd - 1.0) + gmin * vgd;
        }
        return e;
    }

    struct ChannelEval {
        double cdrain = 0.0;  // channel current, d' -> s' (mirror frame)
        double gm = 0.0;      // d(cdrain)/d(vgs)
        double gds = 0.0;     // d(cdrain)/d(vds)
    };
    // The four-region channel at the (limited) mirror-frame voltages -
    // jfetload.c's original SPICE3 section (b = 1, bFac = 0: the Sydney
    // form degenerates to exactly these equations).
    [[nodiscard]] ChannelEval channel_eval(double vgs, double vds,
                                           const TempOp& op) const {
        ChannelEval e;
        if (vds >= 0.0) {
            // normal mode
            const double vgst = vgs - op.vto;
            if (vgst <= 0.0) {
                return e;  // cutoff: exactly off (no gmin floor)
            }
            const double betap = op.beta_eff * (1.0 + lambda_ * vds);
            const double twob = betap + betap;
            if (vgst <= vds) {
                // saturation
                e.cdrain = betap * vgst * vgst;
                e.gm = twob * vgst;
                e.gds = lambda_ * op.beta_eff * vgst * vgst;
            } else {
                // linear
                e.cdrain = betap * vds * (vgst + vgst - vds);
                e.gm = twob * vds;
                e.gds = twob * (vgst - vds) +
                        lambda_ * op.beta_eff * vds * (vgst + vgst - vds);
            }
        } else {
            // inverse mode (source and drain swap)
            const double vgdt = vgs - vds - op.vto;  // vgd - vto
            if (vgdt <= 0.0) {
                return e;  // inverse cutoff
            }
            const double betap = op.beta_eff * (1.0 - lambda_ * vds);
            const double twob = betap + betap;
            if (vgdt <= -vds) {
                // inverse saturation
                e.cdrain = -betap * vgdt * vgdt;
                e.gm = -twob * vgdt;
                e.gds = lambda_ * op.beta_eff * vgdt * vgdt - e.gm;
            } else {
                // inverse linear
                e.cdrain = betap * vds * (vgdt + vgdt + vds);
                e.gm = twob * vds;
                e.gds = twob * vgdt -
                        lambda_ * op.beta_eff * vds * (vgdt + vgdt + vds);
            }
        }
        return e;
    }

    // --- the stamp (3.6: linearize at the current iterate) ----------------
    // The ngspice jfetload stamps, translated 1:1 (the sgn factors of the
    // PJF terminal mapping cancel in the G entries exactly as the BJT's
    // do; only the Norton ceq offsets carry sgn). With Rd/Rs > 0 the
    // channel and junctions sit on the prime nodes; the series
    // conductances area/Rd and area/Rs tie the terminals to them.
    void load_dc(DcLoadContext& cx) const override {
        const double area = cx.par(kAreaParam);
        const TempOp op = temp_op(area, load_temp_k(cx.temp_k()));
        const double sgn = pjp_ ? -1.0 : 1.0;
        const NodeId dpe = dp(), spe = sp();
        const double vgs_raw = sgn * (cx.node_v(g_) - cx.node_v(spe));
        const double vgd_raw = sgn * (cx.node_v(g_) - cx.node_v(dpe));
        const double vgs = fetlim(
            pnjlim(vgs_raw, cx.st(kVgsPrev), op.vt, op.vcrit), cx.st(kVgsPrev),
            op.vto);
        const double vgd = fetlim(
            pnjlim(vgd_raw, cx.st(kVgdPrev), op.vt, op.vcrit), cx.st(kVgdPrev),
            op.vto);
        const double vds = vgs - vgd;
        const GateEval ge = gate_eval(vgs, vgd, cx.gmin(), op);
        const ChannelEval ch = channel_eval(vgs, vds, op);
        const double gdpr = (rd_ > 0.0) ? area / rd_ : 0.0;
        const double gspr = (rs_ > 0.0) ? area / rs_ : 0.0;
        // Terminal currents at the limited point (mirror frame): the gate
        // carries both junctions; the drain prime carries the channel
        // minus the gd junction current; the source prime the remainder.
        const double cg = ge.igs + ge.igd;
        const double cd = ch.cdrain - ge.igd;
        // Norton offsets (the constant parts, sgn-scaled).
        const double ceqgd = sgn * (ge.igd - ge.ggd * vgd);
        const double ceqgs = sgn * ((cg - ge.igd) - ge.ggs * vgs);
        const double cdreq = sgn * ((cd + ge.igd) - ch.gds * vds - ch.gm * vgs);
        // G matrix (accumulating; the Rd/Rs stamps self-fold when the
        // primes ARE the terminals because gdpr/gspr are then 0).
        if (rd_ > 0.0) {
            cx.add_conductance(d_, dp_, gdpr);
        }
        if (rs_ > 0.0) {
            cx.add_conductance(s_, sp_, gspr);
        }
        cx.add_entry(g_, dpe, -ge.ggd);
        cx.add_entry(g_, spe, -ge.ggs);
        cx.add_entry(g_, g_, ge.ggd + ge.ggs);
        cx.add_entry(dpe, g_, ch.gm - ge.ggd);
        cx.add_entry(dpe, dpe, ch.gds + ge.ggd);
        cx.add_entry(dpe, spe, -ch.gds - ch.gm);
        cx.add_entry(spe, g_, -ge.ggs - ch.gm);
        cx.add_entry(spe, spe, ch.gds + ch.gm + ge.ggs);
        cx.add_entry(spe, dpe, -ch.gds);
        // RHS: the Norton offsets (current injected INTO the nodes).
        cx.add_rhs(g_, -ceqgs - ceqgd);
        cx.add_rhs(dpe, -cdreq + ceqgd);
        cx.add_rhs(spe, cdreq + ceqgs);
    }

    // load_ac: the small-signal conductances at the frozen OP (state
    // kVgsPrev/kVgdPrev, set by update_state on the converged .ac OP)
    // plus the gate depletion caps as j*omega*C. The junction
    // conductances fold the run's gmin (cx.gmin()) - unlike the diode's
    // load_ac (which drops it), a JFET's gate is reverse-biased in every
    // normal operating point, and without the floor its column is
    // numerically dead (ggd ~ 1e-23 at vgd = -12 V): the complex
    // factorize fails as singular. The floor is the same one the OP
    // stamp carried (ngspice's small-signal matrix inherits the OP's
    // gmin likewise).
    void load_ac(AcLoadContext& cx) const override {
        const double area = cx.par(kAreaParam);
        const TempOp op = temp_op(area, load_temp_k(cx.temp_k()));
        const NodeId dpe = dp(), spe = sp();
        // The parasitic resistances are part of the small-signal path
        // too (area/Rd, area/Rs between the terminal and its prime -
        // the load_dc stamps); without them a terminal whose only
        // neighbor is its prime through Rs is structurally isolated in
        // the .ac matrix.
        if (rd_ > 0.0) {
            cx.add_conductance(d_, dp_, area / rd_);
        }
        if (rs_ > 0.0) {
            cx.add_conductance(s_, sp_, area / rs_);
        }
        const double vgs = cx.st(kVgsPrev);
        const double vgd = cx.st(kVgdPrev);
        const double vds = vgs - vgd;
        const GateEval ge = gate_eval(vgs, vgd, cx.gmin(), op);
        const ChannelEval ch = channel_eval(vgs, vds, op);
        cx.add_entry(g_, dpe, {-ge.ggd, 0.0});
        cx.add_entry(g_, spe, {-ge.ggs, 0.0});
        cx.add_entry(g_, g_, {ge.ggd + ge.ggs, 0.0});
        cx.add_entry(dpe, g_, {ch.gm - ge.ggd, 0.0});
        cx.add_entry(dpe, dpe, {ch.gds + ge.ggd, 0.0});
        cx.add_entry(dpe, spe, {-ch.gds - ch.gm, 0.0});
        cx.add_entry(spe, g_, {-ge.ggs - ch.gm, 0.0});
        cx.add_entry(spe, spe, {ch.gds + ch.gm + ge.ggs, 0.0});
        cx.add_entry(spe, dpe, {-ch.gds, 0.0});
        if (has_caps()) {
            const double cgs = c_dep(vgs, op.cgs_eff, op.gate_pot, op.cor_dep_cap);
            const double cgd = c_dep(vgd, op.cgd_eff, op.gate_pot, op.cor_dep_cap);
            if (cgs != 0.0) {
                cx.add_conductance(g_, spe, {0.0, cx.omega * cgs});
            }
            if (cgd != 0.0) {
                cx.add_conductance(g_, dpe, {0.0, cx.omega * cgd});
            }
        }
    }

    // load_tran: the DC stamp plus the gate-cap charge companions (the
    // BJT-Cje architecture: geq in parallel with jeq per junction, the
    // currents flowing g -> s'/d' like the junction currents themselves).
    void load_tran(DcLoadContext& cx, const TranStep&) const override {
        load_dc(cx);
        if (!has_caps()) return;
        cx.add_conductance(g_, sp(), cx.st(kGeqGs));
        cx.add_current(sp(), g_, cx.st(kJeqGs));
        cx.add_conductance(g_, dp(), cx.st(kGeqGd));
        cx.add_current(dp(), g_, cx.st(kJeqGd));
    }

    // 7.5 (jfetnoi.c): _rd/_rs thermal (area/Rd, area/Rs between the
    // terminal and its prime), _id channel 4kT*(2/3)*gm (the nlev<3
    // classic form, between the primes), _1overf flicker KF*|Id|^AF/f on
    // the drain current (KF = 0, the default, is silent).
    void load_noise(NoiseContext& cx) const override {
        const double area = cx.par(kAreaParam);
        const TempOp op = temp_op(area, load_temp_k(cx.temp_k));
        const double sgn = pjp_ ? -1.0 : 1.0;
        const double vgs = cx.st(kVgsPrev);
        const double vgd = cx.st(kVgdPrev);
        const double vds = vgs - vgd;
        const GateEval ge = gate_eval(vgs, vgd, 0.0, op);
        const ChannelEval ch = channel_eval(vgs, vds, op);
        const double t_k = load_temp_k(cx.temp_k);
        if (rd_ > 0.0) {
            cx.add_source("rd", dp(), d_, 4.0 * phys::kBoltz * t_k * area / rd_);
        }
        if (rs_ > 0.0) {
            cx.add_source("rs", sp(), s_, 4.0 * phys::kBoltz * t_k * area / rs_);
        }
        cx.add_source("id", dp(), sp(),
                      4.0 * phys::kBoltz * t_k * (2.0 / 3.0) * std::abs(ch.gm));
        if (kf_ != 0.0 && cx.freq > 0.0) {
            const double cd = sgn * (ch.cdrain - ge.igd);
            if (cd != 0.0) {
                cx.add_source("1overf", dp(), sp(),
                              kf_ * std::pow(std::abs(cd), af_) / cx.freq);
            }
        }
    }

    // --- parameter model (3.5a) ------------------------------------------
    [[nodiscard]] std::vector<ParamSpec> param_specs() const override {
        return {ParamSpec{"area", area_}};
    }
    [[nodiscard]] ws::core::Status check_params(const double* p) const override {
        if (!(p[kAreaParam] > 0.0) || !std::isfinite(p[kAreaParam])) {
            return spice_fail(dev_diag(
                ErrorKind::Semantic, "spice.semantic.j_area",
                "JFET " + name_ + " needs a positive finite area", loc));
        }
        return {};
    }

    // --- state model (3.5b) ----------------------------------------------
    [[nodiscard]] std::size_t state_slots() const override {
        return has_caps() ? 33 : 4;
    }
    [[nodiscard]] bool is_dynamic() const override { return has_caps(); }

    // Folds the cap-model mirrors once per step attempt (the diode-TT
    // fold precedent: begin_step/commit/lte_ok see no load context, so
    // the temp-dependent Cgs/Cgd/Pb/Fc*Pb/F1 fold into the state slice
    // where every companion path reads the same values). link_dynamic
    // sets self_index_.
    void fold_state(double* st, const RunState& rs, const Circuit& c) const override {
        if (!has_caps()) return;
        const double area = rs.params()[c.param_span[self_index_].begin];
        const TempOp op = temp_op(area, load_temp_k(phys::c_to_k(c.temp)));
        st[kCgsEff] = op.cgs_eff;
        st[kCgdEff] = op.cgd_eff;
        st[kGatePotT] = op.gate_pot;
        st[kCorDepCap] = op.cor_dep_cap;
        st[kF1T] = op.f1;
    }
    [[nodiscard]] ws::core::Status link_dynamic(const Circuit& /*c*/,
                                                std::size_t self) override {
        self_index_ = self;
        return {};
    }

    // --- dynamic hooks (the BJT-Cje architecture, depletion charges only) --

    void begin_step(const TranStep& step, double* st) override {
        if (!has_caps()) return;
        const double cgs_eff = st[kCgsEff];
        const double cgd_eff = st[kCgdEff];
        const double c_gs = c_dep(st[kVgsComp], cgs_eff, st[kGatePotT],
                                  st[kCorDepCap]);
        const double c_gd = c_dep(st[kVgdComp], cgd_eff, st[kGatePotT],
                                  st[kCorDepCap]);
        if (step.be) {
            // The consistent BE tangent at the committed point (the
            // diode-Cjo form: commit stores the true Q, so the two Q's
            // cancel exactly).
            st[kGeqGs] = c_gs / step.dt;
            st[kJeqGs] = -st[kGeqGs] * st[kVgsComp];
            st[kGeqGd] = c_gd / step.dt;
            st[kJeqGd] = -st[kGeqGd] * st[kVgdComp];
        } else {
            st[kGeqGs] = 2.0 * c_gs / step.dt;
            st[kJeqGs] = -(st[kGeqGs] * st[kVgsComp] + st[kIgsComp]);
            st[kGeqGd] = 2.0 * c_gd / step.dt;
            st[kJeqGd] = -(st[kGeqGd] * st[kVgdComp] + st[kIgdComp]);
        }
    }

    void commit(const std::vector<double>& x, std::size_t /*nc*/,
                const TranStep& step, double* st) override {
        if (!has_caps()) return;
        const double sgn = pjp_ ? -1.0 : 1.0;
        const double vgs = sgn * v_across(x, g_, sp());
        const double vgd = sgn * v_across(x, g_, dp());
        st[kVgsComp] = vgs;
        st[kQgsComp] = q_dep(vgs, st[kCgsEff], st[kGatePotT], st[kCorDepCap],
                             st[kF1T]);
        st[kIgsComp] = st[kGeqGs] * vgs + st[kJeqGs];
        st[kVgdComp] = vgd;
        st[kQgdComp] = q_dep(vgd, st[kCgdEff], st[kGatePotT], st[kCorDepCap],
                             st[kF1T]);
        st[kIgdComp] = st[kGeqGd] * vgd + st[kJeqGd];
        // LTE history (3 (t, q) pairs per junction, oldest first).
        push_hist(st, kHistNGs, kHistGs, step.time, st[kQgsComp], step.be);
        push_hist(st, kHistNGd, kHistGd, step.time, st[kQgdComp], step.be);
    }

    [[nodiscard]] bool lte_ok(const TranStep& step, const std::vector<double>& x,
                              std::size_t /*nc*/, double reltol, double chgtol,
                              double trtol, const double* st,
                              double* ratio) const override {
        if (!has_caps()) {
            if (ratio) *ratio = 0.0;
            return true;
        }
        const double sgn = pjp_ ? -1.0 : 1.0;
        const double vgs = sgn * v_across(x, g_, sp());
        const double vgd = sgn * v_across(x, g_, dp());
        const double q_gs = q_dep(vgs, st[kCgsEff], st[kGatePotT],
                                  st[kCorDepCap], st[kF1T]);
        const double q_gd = q_dep(vgd, st[kCgdEff], st[kGatePotT],
                                  st[kCorDepCap], st[kF1T]);
        if (ratio) *ratio = 0.0;
        bool ok = dd3_lte(st, kHistNGs, kHistGs, step, q_gs, reltol, chgtol,
                          trtol, ratio);
        double ratio_gd = 0.0;
        ok = dd3_lte(st, kHistNGd, kHistGd, step, q_gd, reltol, chgtol, trtol,
                     &ratio_gd) &&
             ok;
        if (ratio != nullptr && ratio_gd > *ratio) {
            *ratio = ratio_gd;
        }
        return ok;
    }

    void seed_state(const std::vector<double>& x, std::size_t /*nc*/,
                    double* st) override {
        if (!has_caps()) return;
        const double sgn = pjp_ ? -1.0 : 1.0;
        const double vgs = sgn * v_across(x, g_, sp());
        const double vgd = sgn * v_across(x, g_, dp());
        st[kVgsComp] = vgs;
        st[kQgsComp] = q_dep(vgs, st[kCgsEff], st[kGatePotT], st[kCorDepCap],
                             st[kF1T]);
        st[kIgsComp] = 0.0;
        st[kVgdComp] = vgd;
        st[kQgdComp] = q_dep(vgd, st[kCgdEff], st[kGatePotT], st[kCorDepCap],
                             st[kF1T]);
        st[kIgdComp] = 0.0;
        st[kHistNGs] = 0.0;
        st[kHistNGd] = 0.0;
    }

    // Advances both junctions' limiter volds (the CLAMPED values, the
    // 5.3 contract: the load and this hook compute the same limited pair
    // against the same vold) and the channel-current residual trackers.
    // Returns true when either limiter is clamping - forces another
    // iteration so convergence is never declared at a stale
    // linearization point.
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
        const double sgn = pjp_ ? -1.0 : 1.0;
        const double vgs_raw = sgn * v_across(x_load, g_, sp());
        const double vgd_raw = sgn * v_across(x_load, g_, dp());
        const double vgs_lim =
            fetlim(pnjlim(vgs_raw, st[kVgsPrev], op.vt, op.vcrit), st[kVgsPrev],
                   op.vto);
        const double vgd_lim =
            fetlim(pnjlim(vgd_raw, st[kVgdPrev], op.vt, op.vcrit), st[kVgdPrev],
                   op.vto);
        const ChannelEval ch = channel_eval(vgs_lim, vgs_lim - vgd_lim, op);
        st[kIPrev] = st[kICur];
        st[kICur] = ch.cdrain;
        st[kVgsPrev] = vgs_lim;
        st[kVgdPrev] = vgd_lim;
        return vgs_lim != vgs_raw || vgd_lim != vgd_raw;
    }

    // 5.1: |dId| since the previous load, normalized by max(|Id|).
    [[nodiscard]] NonlinResid nonlinear_residual(const double* st) const override {
        const double i_cur = st[kICur];
        const double i_prev = st[kIPrev];
        return {std::abs(i_cur - i_prev), std::max(std::abs(i_cur), std::abs(i_prev))};
    }

    // --- terminal-current probe (7.8): the i(<name>) column ---------------
    // The drain terminal current at the ACCEPTED solution. With Rd > 0
    // the honest reading is the series resistor's own current (the
    // diode-Rs precedent: it never exponentiates); without it, the
    // channel minus the gd junction current at the raw terminal
    // voltages (gmin excluded - the solver's crutch, not the device).
    [[nodiscard]] bool probes_current() const override { return true; }
    [[nodiscard]] double terminal_current(const std::vector<double>& x,
                                          std::size_t /*node_count*/,
                                          const double* params,
                                          double temp_k = 300.15) const override {
        const double area = params != nullptr ? params[kAreaParam] : 1.0;
        if (dp_ != kGround) {
            return v_across(x, d_, dp_) * area / rd_;
        }
        const TempOp op = temp_op(area, load_temp_k(temp_k));
        const double sgn = pjp_ ? -1.0 : 1.0;
        const double vgs = sgn * v_across(x, g_, sp());
        const double vgd = sgn * v_across(x, g_, dp());
        const GateEval ge = gate_eval(vgs, vgd, 0.0, op);
        const ChannelEval ch = channel_eval(vgs, vgs - vgd, op);
        return sgn * (ch.cdrain - ge.igd);
    }

    // .model name NJF|PJF (Vto= Beta= Lambda= Rd= Rs= Cgs= Cgd= Pb= Is=
    // N= Fc= Tnom= Vtotc= Tcv= Betatce= Bex= Xti= Eg= Kf= Af=). The
    // SPICE3 defaults (jfetset.c): Vto=-2, Beta=1e-4, Lambda=0, Rd=Rs=0,
    // Cgs=Cgd=0, Pb=1, Is=1e-14, N=1, Fc=0.5, all temp coefficients 0,
    // Xti=3, Eg=1.11, Kf=0, Af=1. Nlev/Gdsnoi (the ngspice noise
    // selectors) are accepted silently - the classic 2/3*gm channel form
    // is the only one this model carries.
    ws::core::Status apply_model(const ModelDef& model,
                                 NodeResolver& nodes) override {
        if (model.type != "njf" && model.type != "pjf") {
            return spice_fail(dev_diag(
                ErrorKind::Semantic, "spice.semantic.model_type",
                "JFET " + name_ + " needs an NJF or PJF model, got type '" +
                    model.type + "'",
                model.loc));
        }
        pjp_ = (model.type == "pjf");
        model_name_ = model.name.text;
        double vto_val = -2.0, beta_val = 1e-4, lambda_val = 0.0;
        double rd_val = 0.0, rs_val = 0.0;
        double cgs_val = 0.0, cgd_val = 0.0, pb_val = 1.0, fc_val = 0.5;
        double is_val = 1e-14, n_val = 1.0;
        double tnom = 27.0;
        double vtotc_val = 0.0, tcv_val = 0.0;
        double betatce_val = 0.0, bex_val = 0.0;
        double xti_val = 3.0, eg_val = 1.11;
        double kf_val = 0.0, af_val = 1.0;
        bool vtotc_given = false, betatce_given = false, xti_given = false;
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
                        "model '" + model.name.text + "': '" + text +
                            "' needs a value",
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
            if (key == "vto" || key == "vt0") vto_val = v;
            else if (key == "beta") beta_val = v;
            else if (key == "lambda") lambda_val = v;
            else if (key == "rd") rd_val = v;
            else if (key == "rs") rs_val = v;
            else if (key == "cgs") cgs_val = v;
            else if (key == "cgd") cgd_val = v;
            else if (key == "pb") pb_val = v;
            else if (key == "is") is_val = v;
            else if (key == "n") n_val = v;
            else if (key == "fc") fc_val = v;
            else if (key == "tnom") tnom = v;
            else if (key == "vtotc") { vtotc_val = v; vtotc_given = true; }
            else if (key == "tcv") tcv_val = v;
            else if (key == "betatce") { betatce_val = v; betatce_given = true; }
            else if (key == "bex") bex_val = v;
            else if (key == "xti") { xti_val = v; xti_given = true; }
            else if (key == "eg") eg_val = v;
            else if (key == "kf") kf_val = v;
            else if (key == "af") af_val = v;
            // The ngspice noise selectors: accepted so real decks parse
            // cleanly; the classic 2/3*gm channel noise (nlev < 3) is
            // the only form this model carries.
            else if (key == "nlev" || key == "gdsnoi") {
            } else {
                return spice_fail(dev_diag(
                    ErrorKind::Semantic, "spice.semantic.model_param",
                    "model '" + model.name.text + "': unknown parameter '" + key +
                        "' (Vto, Beta, Lambda, Rd, Rs, Cgs, Cgd, Pb, Is, N, Fc,"
                        " Tnom, Vtotc, Tcv, Betatce, Bex, Xti, Eg, Kf, Af)",
                    toks[i].loc));
            }
        }
        if (!(beta_val > 0.0) || !std::isfinite(beta_val)) {
            return spice_fail(dev_diag(
                ErrorKind::Semantic, "spice.semantic.j_beta",
                "model '" + model.name.text + "' needs Beta > 0", model.loc));
        }
        if (!(is_val > 0.0) || !std::isfinite(is_val)) {
            return spice_fail(dev_diag(
                ErrorKind::Semantic, "spice.semantic.j_is",
                "model '" + model.name.text + "' needs Is > 0", model.loc));
        }
        if (!(n_val > 0.0) || !std::isfinite(n_val)) {
            return spice_fail(dev_diag(
                ErrorKind::Semantic, "spice.semantic.j_n",
                "model '" + model.name.text + "' needs N > 0", model.loc));
        }
        // Parasitic resistances: 0 (the defaults) disable the internal
        // nodes; negative values are meaningless - reject loudly (the
        // diode-Rs precedent).
        if (!(rd_val >= 0.0) || !std::isfinite(rd_val) ||
            !(rs_val >= 0.0) || !std::isfinite(rs_val)) {
            return spice_fail(dev_diag(
                ErrorKind::Semantic, "spice.semantic.j_rds",
                "model '" + model.name.text + "' needs Rd >= 0 and Rs >= 0"
                " (0 disables the parasitic resistance)",
                model.loc));
        }
        // Depletion caps: 0 (the defaults) disables the charge model.
        if (!(cgs_val >= 0.0) || !std::isfinite(cgs_val) ||
            !(cgd_val >= 0.0) || !std::isfinite(cgd_val)) {
            return spice_fail(dev_diag(
                ErrorKind::Semantic, "spice.semantic.j_cap",
                "model '" + model.name.text + "' needs Cgs >= 0 and Cgd >= 0"
                " (0 disables the gate capacitances)",
                model.loc));
        }
        // SPICE3 clamps Fc at .95 (jfettemp.c warns and limits); a Fc
        // outside [0, .95] is a deck error here - reject loudly rather
        // than silently rewrite the model.
        if (!(fc_val >= 0.0) || !(fc_val <= 0.95) || !std::isfinite(fc_val)) {
            return spice_fail(dev_diag(
                ErrorKind::Semantic, "spice.semantic.j_fc",
                "model '" + model.name.text + "' needs 0 <= Fc <= 0.95"
                " (SPICE3's depletion-cap coefficient ceiling)",
                model.loc));
        }
        if (!(pb_val > 0.0) || !std::isfinite(pb_val)) {
            return spice_fail(dev_diag(
                ErrorKind::Semantic, "spice.semantic.j_pb",
                "model '" + model.name.text + "' needs Pb > 0", model.loc));
        }
        vto_ = vto_val;
        beta_ = beta_val;
        lambda_ = lambda_val;
        rd_ = rd_val;
        rs_ = rs_val;
        cgs_ = cgs_val;
        cgd_ = cgd_val;
        pb_ = pb_val;
        fc_ = fc_val;
        is_ = is_val;
        n_ = n_val;
        tnom_k_ = phys::c_to_k(tnom);
        vtotc_ = vtotc_val;
        vtotc_given_ = vtotc_given;
        tcv_ = tcv_val;
        betatce_ = betatce_val;
        betatce_given_ = betatce_given;
        bex_ = bex_val;
        xti_ = xti_val;
        xti_given_ = xti_given;
        eg_ = eg_val;
        kf_ = kf_val;
        af_ = af_val;
        // The prime nodes exist only when the parasitic resistances do
        // (the diode-mid precedent: an always-present internal node
        // would flood every deck's save list). apply_model runs BEFORE
        // the 6.6 graph checks, so claimed nodes join them like any
        // other.
        dp_ = kGround;
        sp_ = kGround;
        if (rd_ > 0.0) {
            Token tok;
            tok.text = name_ + "#d";
            tok.loc = model.loc;
            dp_ = nodes.node(tok);
        }
        if (rs_ > 0.0) {
            Token tok;
            tok.text = name_ + "#s";
            tok.loc = model.loc;
            sp_ = nodes.node(tok);
        }
        return {};
    }

    [[nodiscard]] bool has_ic() const override { return has_ic_; }
    // The card's ic= carries VDS,VGS; expose VDS as the primary ic value
    // (the BJT-Vbe precedent: the .ic/UIC machinery reads one number,
    // the card round-trips both).
    [[nodiscard]] double ic_value() const override { return ic_vds_; }

    [[nodiscard]] bool write_card(std::string& out,
                                  const CardWriter& w) const override {
        out += name_ + " " + w.node(d_) + " " + w.node(g_) + " " + w.node(s_) +
               " " + model_name_;
        const double area = w.params(*this)[kAreaParam];
        if (area != 1.0) {
            out += " " + Device::CardWriter::num(area);
        }
        if (has_ic_) {
            out += " ic=" + Device::CardWriter::num(ic_vds_) + "," +
                   Device::CardWriter::num(ic_vgs_);
        }
        out += "\n";
        return true;
    }

private:
    static constexpr std::size_t kAreaParam = 0;

    // True when either gate cap exists (gates the companion hooks).
    [[nodiscard]] bool has_caps() const { return cgs_ > 0.0 || cgd_ > 0.0; }

    // xfc = ln(1 - Fc) - the SPICE3 cap-model constant.
    [[nodiscard]] double xfc() const { return std::log(1.0 - fc_); }

    // The depletion capacitance and charge at one junction voltage
    // (jfetload.c's qgs/qgd block, m = 0.5 fixed): below Fc*Pb the
    // sqrt(1 - v/Pb) depletion law, above it the Fc linear extension
    // (f2 = e^{1.5*xfc}, f3 = 1 - 1.5*Fc - both Fc-only constants).
    [[nodiscard]] double c_dep(double v, double cz, double gate_pot,
                               double cor_dep_cap) const {
        if (cz <= 0.0) return 0.0;
        if (v < cor_dep_cap) {
            double arg = 1.0 - v / gate_pot;
            if (arg < 1e-12) arg = 1e-12;  // the Vd=Vj singularity guard
            return cz / std::sqrt(arg);
        }
        const double f2 = std::exp(1.5 * xfc());
        return cz / f2 * (1.0 - 1.5 * fc_ + v / (2.0 * gate_pot));
    }
    [[nodiscard]] double q_dep(double v, double cz, double gate_pot,
                               double cor_dep_cap, double f1) const {
        if (cz <= 0.0) return 0.0;
        if (v < cor_dep_cap) {
            double arg = 1.0 - v / gate_pot;
            if (arg < 1e-12) arg = 1e-12;
            return 2.0 * gate_pot * cz * (1.0 - std::sqrt(arg));
        }
        const double f2 = std::exp(1.5 * xfc());
        const double f3 = 1.0 - 1.5 * fc_;
        const double fcpb2 = cor_dep_cap * cor_dep_cap;
        return cz * f1 + cz / f2 *
                             (f3 * (v - cor_dep_cap) +
                              (v * v - fcpb2) / (4.0 * gate_pot));
    }

    // The divided-difference LTE (the diode/BJT form): the third
    // difference of the charge history against chgtol-scaled tolq.
    [[nodiscard]] static bool dd3_lte(const double* st, std::size_t n_slot,
                                      std::size_t hist_slot, const TranStep& step,
                                      double q_new, double reltol, double chgtol,
                                      double trtol, double* ratio) {
        const double n = st[n_slot];
        if (n < 3.0) {
            return true;  // not enough history
        }
        const double t0 = st[hist_slot], q0 = st[hist_slot + 1];
        const double t1 = st[hist_slot + 2], q1 = st[hist_slot + 3];
        const double t2 = st[hist_slot + 4], q2 = st[hist_slot + 5];
        const double t3 = step.time, q3 = q_new;
        if (!(t1 > t0) || !(t2 > t1) || !(t3 > t2) || !(step.dt > 0)) {
            return true;
        }
        const double d1a = (q1 - q0) / (t1 - t0);
        const double d1b = (q2 - q1) / (t2 - t1);
        const double d1c = (q3 - q2) / (t3 - t2);
        const double d2a = (d1b - d1a) / (t2 - t0);
        const double d2b = (d1c - d1b) / (t3 - t1);
        const double dd3 = (d2b - d2a) / (t3 - t0);
        const double eps = step.dt * step.dt * step.dt * std::abs(dd3) / 2.0;
        const double tolq = trtol * (reltol * std::max(std::abs(q3), std::abs(q2)) +
                                     chgtol);
        if (ratio != nullptr) {
            *ratio = (tolq > 0) ? (eps / tolq)
                                : std::numeric_limits<double>::infinity();
        }
        return tolq > 0 && eps <= tolq;
    }

    static void push_hist(double* st, std::size_t n_slot, std::size_t hist_slot,
                          double t, double q, bool be) {
        if (be) {
            st[n_slot] = 0.0;  // BE restart invalidates the trap history
            return;
        }
        const double n = st[n_slot];
        if (n < 3.0) {
            const std::size_t idx = static_cast<std::size_t>(n);
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

    std::string name_;
    NodeId d_, g_, s_;
    // P-channel flag (PJF: the terminal mapping negates; NJF frame inside).
    bool pjp_ = false;
    // The Rd/Rs prime nodes (kGround when the resistance is 0 - no node
    // exists at all, the stamps fold onto the terminals).
    NodeId dp_ = kGround;
    NodeId sp_ = kGround;
    ExprProgram area_;
    bool has_ic_;
    double ic_vds_;
    double ic_vgs_;
    // 7.7: per-instance temp= override (C).
    bool has_temp_inst_ = false;
    double temp_inst_c_ = 27.0;
    // Model fields (set by apply_model):
    double vto_ = -2.0;      // threshold voltage (NJF frame)
    double beta_ = 1e-4;     // transconductance parameter (A/V^2)
    double lambda_ = 0.0;    // channel-length modulation (1/V)
    double rd_ = 0.0;        // drain parasitic resistance (0 = no #d node)
    double rs_ = 0.0;        // source parasitic resistance (0 = no #s node)
    double cgs_ = 0.0;       // zero-bias gate-source cap (F)
    double cgd_ = 0.0;       // zero-bias gate-drain cap (F)
    double pb_ = 1.0;        // gate junction potential (V)
    double fc_ = 0.5;        // forward-bias depletion cap coefficient
    double is_ = 1e-14;      // gate junction saturation current
    double n_ = 1.0;         // emission coefficient (rides Is(T) only)
    double tnom_k_ = 300.15; // model nominal temperature, K
    double vtotc_ = 0.0;     // Vto temp coefficient (V/C)
    bool vtotc_given_ = false;
    double tcv_ = 0.0;       // the alternative Vto temp coefficient (negated)
    double betatce_ = 0.0;   // Beta temp coefficient (percent/C, via 1.01^x)
    bool betatce_given_ = false;
    double bex_ = 0.0;       // mobility temp exponent ((T/Tnom)^Bex)
    double xti_ = 3.0;       // Is temp exponent (applied only when given)
    bool xti_given_ = false;
    double eg_ = 1.11;       // energy gap (eV, the Is(T) exponent)
    double kf_ = 0.0;        // flicker coefficient (0 = silent)
    double af_ = 1.0;        // flicker exponent
    std::size_t self_index_ = 0;  // filled by link_dynamic (for fold_state)
    std::string model_name_;
};

}  // namespace

ws::core::Result<std::unique_ptr<Device>> make_jfet(const DeviceCard& card,
                                                    FactoryServices& s) {
    // J name drain gate source model [area] [off] [ic=Vds,Vgs] [temp=]
    WS_TRY_ASSIGN(const std::vector<NodeId> n, take_nodes(card, s.nodes, 3, 0));
    if (card.rest.size() < 4) {
        return spice_fail(dev_diag(
            ErrorKind::Semantic, "spice.semantic.arity",
            "JFET " + card.name.text + " takes three nodes and a model name",
            card.loc));
    }
    ExprProgram area_prog;
    bool has_area = false;
    bool has_ic = false;
    double ic_vds = 0.0, ic_vgs = 0.0;
    bool has_temp_inst = false;
    double temp_inst = 27.0;
    for (std::size_t i = 4; i < card.rest.size(); ++i) {
        const std::string lower = to_lower(card.rest[i].text);
        if (lower.rfind("ic=", 0) == 0) {
            has_ic = true;
            const std::string v = card.rest[i].text.substr(3);
            const std::size_t comma = v.find(',');
            if (const ws::core::Result<double> r = parse_number(v.substr(0, comma))) {
                ic_vds = *r;
            }
            if (comma != std::string::npos) {
                if (const ws::core::Result<double> r =
                        parse_number(v.substr(comma + 1))) {
                    ic_vgs = *r;
                }
            }
            continue;
        }
        if (lower == "off") {
            // ngspice: start with the junctions at -1 V. With a cold
            // start (x = 0) the device already begins at vgs = 0 (a
            // depletion device's "on" side); the ladder carries the
            // stiff case - accept and ignore (the diode-off precedent).
            continue;
        }
        if (lower.rfind("temp=", 0) == 0) {
            // 7.7: per-instance temperature override (C).
            if (const ws::core::Result<double> r =
                    parse_number(card.rest[i].text.substr(5))) {
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
            ErrorKind::Semantic, "spice.semantic.j_extra",
            "JFET " + card.name.text +
                " takes three nodes, a model, an optional area, ic=Vds,Vgs,"
                " off, temp=",
            card.loc));
    }
    if (!has_area) {
        Token one;
        one.text = "1.0";
        WS_TRY_ASSIGN(area_prog, resolve_value_program(one, s.params));
    }
    auto dev = std::make_unique<Jfet>(card.name.text, n[0], n[1], n[2],
                                      std::move(area_prog), has_ic, ic_vds, ic_vgs,
                                      has_temp_inst, temp_inst);
    dev->loc = card.loc;
    s.models.push_back(ModelRequest{dev.get(), card.rest[3]});
    return dev;
}

}  // namespace ws::spice
