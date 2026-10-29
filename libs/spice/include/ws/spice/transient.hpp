#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <ws/core/result.hpp>
#include <ws/spice/analysis.hpp>
#include <ws/spice/error.hpp>
#include <ws/spice/results.hpp>
#include <ws/spice/run.hpp>

// The steppable, cancellable transient (DESIGN.md §6 rev): SPICE can't do
// this, and it is what live UIs (circsim-ui), closed-loop controllers in
// C++ (power electronics + embedded control), and early abort on a
// diverging run all need:
//
//   auto run = sim.begin_transient(Tran{.tstop = 10_ms}).value();
//   run.set_stop_token(token);            // any thread may cancel
//   run.set_progress([&](double t, double stop) { ui.draw(run); });
//   for (double t = 1_ms; t <= 10_ms; t += 1_ms) {
//       run.advance_to(t).value();        // whole accepted steps up to t
//       sim.set_source("V1", controller.step(run.solution()));
//   }
//   Table res = run.take_table();
//
// Between advance_to() calls the owning Simulator's set()/set_source()
// take effect on the very next step - parameters fold per step attempt
// (§3.5b), so no re-solve machinery is involved. Cancellation leaves the
// run resumable; advance_to() again continues from the same state.

namespace ws::spice {

// A shared cancel flag: copy it, hand it to the run, request_stop() from
// anywhere (a UI thread, a watchdog). Checks are per step attempt.
class StopToken {
public:
    StopToken() : flag_(std::make_shared<std::atomic<bool>>(false)) {}

    void request_stop() { flag_->store(true, std::memory_order_relaxed); }
    [[nodiscard]] bool stop_requested() const {
        return flag_->load(std::memory_order_relaxed);
    }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

// One .ic node value for begin_transient (§6.5: enforced during the t=0
// OP without UIC; the state directly under UIC).
struct IcNode {
    std::string node;
    double volts = 0.0;
};

class Simulator;

class TransientRun {
public:
    TransientRun(TransientRun&&) = default;
    TransientRun& operator=(TransientRun&&) = default;
    ~TransientRun();

    // Marches to wall time `t` (clamped into (now, tstop]): whole
    // accepted steps only - a step is never split to land on `t`
    // exactly, so breakpoints, LTE and BE restarts keep their semantics.
    // Returns ok when done, cancelled, or the target is reached; real
    // failures (timestep below dtmin, non-convergence) propagate.
    [[nodiscard]] ws::core::Status advance_to(double t);

    // The transient is complete (tstop reached).
    [[nodiscard]] bool done() const;
    // A stop token / progress hook asked to stop; resumable.
    [[nodiscard]] bool cancelled() const;
    [[nodiscard]] double now() const;
    [[nodiscard]] double tstop() const;

    // The accepted solution at now(): node voltages then branch currents
    // (the MNA unknown order; v = x[node], i = x[node_count + branch]).
    [[nodiscard]] const std::vector<double>& solution() const;

    // Samples so far (copy), or the whole table moved out.
    [[nodiscard]] Table table() const;
    [[nodiscard]] Table take_table();
    [[nodiscard]] AnalysisReport report() const;

    void set_stop_token(StopToken st);
    // Called after every accepted step with (now, tstop).
    void set_progress(std::function<void(double now, double tstop)> cb);

private:
    friend class Simulator;
    TransientRun() = default;
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ws::spice
