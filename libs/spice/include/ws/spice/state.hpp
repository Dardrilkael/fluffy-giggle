#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <ws/core/result.hpp>
#include <ws/spice/circuit.hpp>
#include <ws/spice/error.hpp>
#include <ws/spice/expr.hpp>

// The per-run arenas (DESIGN.md §3.5, revised 2026-10-29). A RunState is
// everything a single simulation run owns that a Circuit does not:
//
//   params  - the bound parameter values (a resistor's ohms, a source's
//             DC value, gains), one slice per device. Rebound from the
//             Circuit's parameter expressions whenever a run starts or a
//             .step point / Monte-Carlo sample changes the .param scope;
//             a value write can never invalidate the symbolic pattern, so
//             rebinding never re-analyzes (§7.6 rev).
//   state   - the dynamic evolution (companion coefficients, charge/flux
//             histories, switch positions), one fixed-size slice per
//             device. Zeroed at bind; mutated only by the run that owns
//             it. Copying the two vectors IS snapshot/restore.
//   pinned  - user overrides (Simulator::set / set_source) that survive
//             rebinding.
//
// Two RunStates over one Circuit never touch each other: that is the
// whole parallel-safety argument for shared-Circuit .step / Monte-Carlo /
// sweep runs (multithreaded DRIVERS remain a backlog item; the state
// model is what makes them possible).

namespace ws::spice {

// Deterministic, seedable random source for Monte-Carlo expressions
// (gauss()/flat(), §8.5). A plain mt19937_64 wrapper: no global state,
// copyable, and a recorded seed reproduces a run bit-for-bit (§10.5).
class Rand {
public:
    explicit Rand(std::uint64_t seed = 0x5EED'C0FF'EE01ull)
        : engine_(seed) {}

    // Uniform in [0, 1).
    double unit() { return std::uniform_real_distribution<double>{}(engine_); }
    // Standard normal, mean 0, standard deviation sigma (Box-Muller).
    double normal(double sigma);
    // Uniform in [-range/2, +range/2).
    double flat(double range) { return range * (unit() - 0.5); }

    void reseed(std::uint64_t seed) { engine_.seed(seed); }

private:
    std::mt19937_64 engine_;
    bool have_spare_ = false;  // Box-Muller second normal
    double spare_ = 0.0;
};

// The two arenas + the override bookkeeping. Default-constructible so
// callers can move-assign a bound state into place.
class RunState {
public:
    RunState() = default;

    // Binds every device's parameter expressions against `scope` (the
    // Circuit's own .param scope when null), validates them through the
    // owning device (check_params - a Monte-Carlo draw obeys the same
    // positivity rules as a hand-written card), seeds state slices from
    // construction data, and clears pins. `rng`, when given, serves
    // gauss()/flat() inside the expressions - each run of a Monte-Carlo
    // family binds with its own Rand so draws are per-run and
    // reproducible from the recorded seed.
    [[nodiscard]] static ws::core::Result<RunState> bind(
        const Circuit& c, const ParamTable* scope = nullptr, Rand* rng = nullptr);

    // Zeroes every state slice and re-seeds from construction data,
    // leaving bound parameter values alone (a .dc warm start keeps its
    // values; only dynamics reset).
    void reset_state(const Circuit& c);

    // True when `p` is a valid slot for `c`.
    [[nodiscard]] static bool slot_valid(const Circuit& c, std::size_t device,
                                          std::size_t slot);

    [[nodiscard]] std::size_t param_total() const { return params_.size(); }
    [[nodiscard]] std::size_t state_total() const { return state_.size(); }

    // Direct slice access (indexing as laid out by the Circuit).
    [[nodiscard]] const std::vector<double>& params() const { return params_; }
    [[nodiscard]] std::vector<double>& params() { return params_; }
    [[nodiscard]] const std::vector<double>& state() const { return state_; }
    [[nodiscard]] std::vector<double>& state() { return state_; }

    [[nodiscard]] const double* param_slice(const Circuit& c, std::size_t device) const {
        const auto& s = c.param_span[device];
        return params_.data() + s.begin;
    }
    [[nodiscard]] double* param_slice(const Circuit& c, std::size_t device) {
        const auto& s = c.param_span[device];
        return params_.data() + s.begin;
    }
    [[nodiscard]] const double* state_slice(const Circuit& c, std::size_t device) const {
        const auto& s = c.state_span[device];
        return state_.data() + s.begin;
    }
    [[nodiscard]] double* state_slice(const Circuit& c, std::size_t device) {
        const auto& s = c.state_span[device];
        return state_.data() + s.begin;
    }

    // A parameter override that survives rebinding (Simulator::set).
    struct Pin {
        std::size_t device;
        std::size_t slot;
        double value;
    };
    [[nodiscard]] const std::vector<Pin>& pins() const { return pins_; }
    void pin(std::size_t device, std::size_t slot, double value) {
        for (Pin& p : pins_) {
            if (p.device == device && p.slot == slot) {
                p.value = value;
                return;
            }
        }
        pins_.push_back({device, slot, value});
    }
    void unpin(std::size_t device, std::size_t slot) {
        for (std::size_t i = 0; i < pins_.size(); ++i) {
            if (pins_[i].device == device && pins_[i].slot == slot) {
                pins_.erase(pins_.begin() + static_cast<std::ptrdiff_t>(i));
                return;
            }
        }
    }
    // Re-applies pins to the bound values (bind leaves slots at their
    // scoped values; this overlays the user's overrides).
    void apply_pins(const Circuit& c);

    // Index of a device by (case-insensitive) name; nullopt when absent.
    [[nodiscard]] static std::optional<std::size_t> device_index(const Circuit& c,
                                                                 const std::string& name);

    // The device/slot coordinates of a named parameter, e.g. ("R1",
    // "value") or ("V1", "dc"); nullopt when the device or the parameter
    // name does not exist. Names are case-insensitive.
    [[nodiscard]] static std::optional<std::pair<std::size_t, std::size_t>> param_slot(
        const Circuit& c, const std::string& device, const std::string& param);

    std::vector<double> params_;
    std::vector<double> state_;
    std::vector<Pin> pins_;
};

}  // namespace ws::spice
