// RunState: the per-run parameter + state arenas (§3.5 rev). Binding is
// the "cheap re-elaboration": every device's parameter expressions
// re-evaluate against the run's .param scope (a .step point's override,
// a Monte-Carlo draw), the values are validated exactly as a card value
// would be, and the state slices re-seed from construction data. The
// stamp pattern never depends on any of it - that is the §7.6-rev reuse
// guarantee.

#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include <ws/spice/circuit.hpp>
#include <ws/spice/state.hpp>

namespace ws::spice {

double Rand::normal(double sigma) {
    // Box-Muller: two uniforms -> one standard normal (the second is
    // cached for the next call; a single draw per gauss() keeps run-to-run
    // reproducibility keyed on the seed alone).
    if (have_spare_) {
        have_spare_ = false;
        return spare_ * sigma;
    }
    double u1 = unit();
    const double u2 = unit();
    while (u1 <= 0.0) {
        u1 = unit();  // log(0) guard; measure-zero event
    }
    const double r = std::sqrt(-2.0 * std::log(u1));
    const double theta = 2.0 * 3.14159265358979323846 * u2;
    spare_ = r * std::sin(theta);
    have_spare_ = true;
    return r * std::cos(theta) * sigma;
}

namespace {

Diagnostic state_diag(ErrorKind kind, std::string code, std::string message) {
    Diagnostic d;
    d.kind = kind;
    d.code = std::move(code);
    d.message = std::move(message);
    return d;
}

}  // namespace

ws::core::Result<RunState> RunState::bind(const Circuit& c, const ParamTable* scope,
                                          Rand* rng) {
    RunState rs;
    rs.params_.assign(c.param_total, 0.0);
    rs.state_.assign(c.state_total, 0.0);
    rs.pins_.clear();

    const ParamTable& table = (scope != nullptr) ? *scope : c.params;
    // Draw coherence WITHOUT a cache clear: the first evaluation of a
    // gauss()/flat() parameter caches, so every device AND every analysis
    // of the run sees the same draw (§8.5 rev). A .step point hands bind
    // a FRESH scope per point (the driver's table), so each point draws
    // anew; a re-definition (a stepped override, Simulator::rebind with
    // an updated table) clears that name's cache by construction.

    for (std::size_t i = 0; i < c.devices.size(); ++i) {
        const Device& dev = *c.devices[i];
        const auto& span = c.param_span[i];
        for (std::size_t j = 0; j < span.count; ++j) {
            const ParamSpec& spec = c.param_table[span.begin + j];
            const ws::core::Result<std::vector<std::optional<double>>> bindings =
                table.bind(spec.source, rng);
            if (!bindings) {
                // The scope's own error (a cycle, an unknown param) already
                // names the chain; add the device context on top.
                Diagnostic d = state_diag(ErrorKind::Semantic, "spice.param.bind",
                                          "device '" + dev.name() + "' parameter '" +
                                              spec.name + "': " +
                                              bindings.error().message);
                return spice_fail(std::move(d));
            }
            const ws::core::Result<double> v = spec.source.evaluate(*bindings, rng);
            if (!v) {
                Diagnostic d = state_diag(
                    ErrorKind::Semantic, "spice.param.eval",
                    "device '" + dev.name() + "' parameter '" + spec.name + "': " +
                        v.error().message);
                return spice_fail(std::move(d));
            }
            rs.params_[span.begin + j] = *v;
        }
        if (const ws::core::Status st = dev.check_params(rs.params_.data() + span.begin);
            !st) {
            // check_params failures arrive pre-wrapped as spice errors
            // carrying the device name; pass them through untouched.
            return ws::core::unexpected(st.error());
        }
        dev.init_state(rs.state_.data() + c.state_span[i].begin);
    }
    return rs;
}

void RunState::reset_state(const Circuit& c) {
    std::fill(state_.begin(), state_.end(), 0.0);
    for (std::size_t i = 0; i < c.devices.size(); ++i) {
        c.devices[i]->init_state(state_.data() + c.state_span[i].begin);
    }
}

void RunState::apply_pins(const Circuit& c) {
    for (const Pin& p : pins_) {
        if (slot_valid(c, p.device, p.slot)) {
            params_[c.param_span[p.device].begin + p.slot] = p.value;
        }
    }
}

bool RunState::slot_valid(const Circuit& c, std::size_t device, std::size_t slot) {
    return device < c.devices.size() && slot < c.param_span[device].count;
}

std::optional<std::size_t> RunState::device_index(const Circuit& c, const std::string& name) {
    const std::string lower = to_lower(name);
    for (std::size_t i = 0; i < c.devices.size(); ++i) {
        if (to_lower(c.devices[i]->name()) == lower) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<std::pair<std::size_t, std::size_t>> RunState::param_slot(
    const Circuit& c, const std::string& device, const std::string& param) {
    const std::optional<std::size_t> di = device_index(c, device);
    if (!di) {
        return std::nullopt;
    }
    const auto& span = c.param_span[*di];
    const std::string lower = to_lower(param);
    for (std::size_t j = 0; j < span.count; ++j) {
        if (to_lower(c.param_table[span.begin + j].name) == lower) {
            return std::make_pair(*di, j);
        }
    }
    return std::nullopt;
}

}  // namespace ws::spice
