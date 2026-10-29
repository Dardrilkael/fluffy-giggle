// Simulator (DESIGN.md §2.1 rev): the handle-driven run over an immutable
// Circuit. Thin by design: it converts Analysis aggregates into the card
// structs the drivers consume and owns the (single) RunState; every real
// behavior lives in the shared drivers, so the CLI path and this path
// cannot drift apart.

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "drivers.hpp"

#include <ws/spice/analysis.hpp>
#include <ws/spice/circuit.hpp>
#include <ws/spice/deck.hpp>
#include <ws/spice/run.hpp>
#include <ws/spice/simulator.hpp>
#include <ws/spice/state.hpp>

namespace ws::spice {

namespace {

Diagnostic sim_diag(ErrorKind kind, std::string code, std::string message) {
    Diagnostic d;
    d.kind = kind;
    d.code = std::move(code);
    d.message = std::move(message);
    return d;
}

// A constant-valued expression (sweep bounds, .tran times).
ExprProgram constant_expr(double v) { return ExprProgram::constant(v); }

}  // namespace

ws::core::Result<Simulator> Simulator::create(Circuit circuit, RunSettings settings) {
    Simulator sim;
    sim.circuit_ = std::make_unique<Circuit>(std::move(circuit));
    sim.settings_ = settings;
    WS_TRY_ASSIGN(sim.state_, RunState::bind(*sim.circuit_));
    return sim;
}

ws::core::Result<Table> Simulator::run(const Analysis& a) {
    warnings_.clear();
    AnalysisReport report;
    ws::core::Result<Table> t = std::visit(
        [this, &report](const auto& analysis) -> ws::core::Result<Table> {
            using A = std::decay_t<decltype(analysis)>;
            if constexpr (std::is_same_v<A, OpAnalysis>) {
                return drivers::run_op(*circuit_, state_, settings_, report);
            } else if constexpr (std::is_same_v<A, DcAnalysis>) {
                DcCard card;
                card.first.source.text = analysis.source;
                card.first.start = constant_expr(analysis.start);
                card.first.stop = constant_expr(analysis.stop);
                card.first.step = constant_expr(analysis.step);
                if (analysis.outer) {
                    ::ws::spice::DcSweep outer;  // the card struct, not the leg
                    outer.source.text = analysis.outer->source;
                    outer.start = constant_expr(analysis.outer->start);
                    outer.stop = constant_expr(analysis.outer->stop);
                    outer.step = constant_expr(analysis.outer->step);
                    card.second = std::move(outer);
                }
                return drivers::run_dc(nullptr, *circuit_, card, settings_, warnings_,
                                       report);
            } else if constexpr (std::is_same_v<A, TranAnalysis>) {
                TranCard card;
                // tstep 0 = "tstop/100" (the card form always spells it;
                // the aggregate default keeps Tran{.tstop = ...} tidy).
                card.tstep = constant_expr(analysis.tstep > 0.0
                                               ? analysis.tstep
                                               : analysis.tstop / 100.0);
                card.tstop = constant_expr(analysis.tstop);
                if (analysis.tstart) {
                    card.tstart = constant_expr(*analysis.tstart);
                    card.has_tstart = true;
                }
                if (analysis.tmax) {
                    card.tmax = constant_expr(*analysis.tmax);
                    card.has_tmax = true;
                }
                card.uic = analysis.uic;
                return drivers::run_tran(*circuit_, state_, card, settings_,
                                         circuit_->params, {}, report);
            } else {
                AcCard card;
                switch (analysis.sweep) {
                    case AcAnalysis::Sweep::Dec: card.sweep = AcCard::Sweep::Dec; break;
                    case AcAnalysis::Sweep::Oct: card.sweep = AcCard::Sweep::Oct; break;
                    case AcAnalysis::Sweep::Lin: card.sweep = AcCard::Sweep::Lin; break;
                }
                card.n = constant_expr(static_cast<double>(analysis.n));
                card.fstart = constant_expr(analysis.fstart);
                card.fstop = constant_expr(analysis.fstop);
                return drivers::run_ac(*circuit_, state_, card, settings_, circuit_->params,
                                       report);
            }
        },
        a);
    if (t) {
        last_report_ = report;
    }
    return t;
}

ws::core::Status Simulator::set(const ParamHandle& h, double value) {
    const auto slot = RunState::param_slot(*circuit_, h.device, h.param);
    if (!slot) {
        Diagnostic d = sim_diag(
            ErrorKind::Usage, "spice.usage.param_handle",
            "no parameter '" + h.param + "' on device '" + h.device + "'");
        d.names.push_back(h.device);
        return spice_fail(std::move(d));
    }
    state_.pin(slot->first, slot->second, value);
    state_.apply_pins(*circuit_);
    return {};
}

ws::core::Status Simulator::set_source(std::string_view source, double value) {
    const auto di = RunState::device_index(*circuit_, std::string(source));
    if (!di || (circuit_->devices[*di]->kind() != 'V' &&
                circuit_->devices[*di]->kind() != 'I')) {
        Diagnostic d = sim_diag(ErrorKind::Usage, "spice.usage.source_handle",
                                "'" + std::string(source) + "' is not a V or I source");
        return spice_fail(std::move(d));
    }
    const auto& span = circuit_->param_span[*di];
    if (span.count < 2) {
        Diagnostic d = sim_diag(ErrorKind::Usage, "spice.usage.source_handle",
                                "source '" + std::string(source) +
                                    "' has no parameter slots");
        return spice_fail(std::move(d));
    }
    state_.pin(*di, 0, value);   // dc
    state_.pin(*di, 1, 1.0);     // force: the DC value overrides the waveform
    state_.apply_pins(*circuit_);
    return {};
}

ws::core::Status Simulator::release_source(std::string_view source) {
    const auto di = RunState::device_index(*circuit_, std::string(source));
    if (!di) {
        Diagnostic d = sim_diag(ErrorKind::Usage, "spice.usage.source_handle",
                                "unknown source '" + std::string(source) + "'");
        return spice_fail(std::move(d));
    }
    state_.unpin(*di, 0);
    state_.unpin(*di, 1);
    // Restore the scoped values for both slots.
    WS_TRY(rebind(nullptr, nullptr));
    return {};
}

ws::core::Status Simulator::rebind(const ParamTable* scope, Rand* rng) {
    WS_TRY_ASSIGN(RunState fresh, RunState::bind(*circuit_, scope, rng));
    // Pins survive rebinding (§3.5a): the user's set()/set_source() wins
    // over the scope's value.
    fresh.pins_ = state_.pins_;
    fresh.apply_pins(*circuit_);
    state_ = std::move(fresh);
    return {};
}

std::optional<double> Simulator::param_value(const ParamHandle& h) const {
    const auto slot = RunState::param_slot(*circuit_, h.device, h.param);
    if (!slot) {
        return std::nullopt;
    }
    return state_.params_[circuit_->param_span[slot->first].begin + slot->second];
}

}  // namespace ws::spice
