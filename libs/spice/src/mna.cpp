#include "mna.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace ws::spice {

namespace detail {

// The pattern recorder (§3.2): records (row, col), ignores values; ground
// entries never reach the builder (ground row/col dropped, §4.1). §3.6
// (M3): node_v/branch_i/gmin return 0/default - a linearization around
// zero produces the right stamp POSITIONS (only coefficients depend on
// the iterate), which is all the dry run records.
class PatternContext final : public DcLoadContext {
public:
    PatternContext(ws::math::SparsePatternBuilder& builder, std::size_t node_count)
        : builder_(builder), node_count_(node_count) {}

    void add_entry(std::size_t row, std::size_t col, double) override {
        if (row == kGround || col == kGround) {
            return;
        }
        builder_.add(row, col);
    }
    void add_rhs(std::size_t, double) override {}
    [[nodiscard]] std::size_t branch_row(BranchId br) const override {
        return node_count_ + br;
    }
    [[nodiscard]] bool pattern_mode() const override { return true; }
    [[nodiscard]] double node_v(NodeId) const override { return 0.0; }
    [[nodiscard]] double branch_i(BranchId) const override { return 0.0; }
    [[nodiscard]] double gmin() const override { return 1e-12; }

private:
    ws::math::SparsePatternBuilder& builder_;
    std::size_t node_count_;
};

// The numeric context: entries accumulate into the values array, rhs into
// the vector. A numeric write outside the recorded pattern is a
// programmer error (§3.2) - SparseMatrix::add asserts it. §3.6 (M3): the
// current iterate is exposed to nonlinear devices through node_v/branch_i;
// gmin is the run's junction-parallel conductance floor.
class NumericContext final : public DcLoadContext {
public:
    NumericContext(ws::math::SparseMatrix<double>& matrix, std::vector<double>& rhs,
                   std::size_t node_count, const std::vector<double>* x, double gmin)
        : matrix_(matrix), rhs_(rhs), node_count_(node_count), x_(x), gmin_(gmin) {}

    void add_entry(std::size_t row, std::size_t col, double v) override {
        if (row == kGround || col == kGround) {
            return;
        }
        matrix_.add(row, col, v);
    }
    void add_rhs(std::size_t row, double v) override {
        if (row == kGround) {
            return;
        }
        rhs_[row] += v;
    }
    [[nodiscard]] std::size_t branch_row(BranchId br) const override {
        return node_count_ + br;
    }
    [[nodiscard]] bool pattern_mode() const override { return false; }
    [[nodiscard]] double node_v(NodeId n) const override {
        if (n == kGround || x_ == nullptr) {
            return 0.0;
        }
        return (*x_)[n];
    }
    [[nodiscard]] double branch_i(BranchId br) const override {
        if (x_ == nullptr) {
            return 0.0;
        }
        return (*x_)[node_count_ + br];
    }
    [[nodiscard]] double gmin() const override { return gmin_; }

private:
    ws::math::SparseMatrix<double>& matrix_;
    std::vector<double>& rhs_;
    std::size_t node_count_;
    const std::vector<double>* x_;
    double gmin_;
};

// The AC pattern twin: same coordinates, complex values ignored.
class AcPatternContext final : public AcLoadContext {
public:
    AcPatternContext(ws::math::SparsePatternBuilder& builder, std::size_t node_count)
        : builder_(builder), node_count_(node_count) {}

    void add_entry(std::size_t row, std::size_t col, std::complex<double>) override {
        if (row == kGround || col == kGround) {
            return;
        }
        builder_.add(row, col);
    }
    void add_rhs(std::size_t, std::complex<double>) override {}
    [[nodiscard]] std::size_t branch_row(BranchId br) const override {
        return node_count_ + br;
    }

private:
    ws::math::SparsePatternBuilder& builder_;
    std::size_t node_count_;
};

// The AC numeric context: complex values into the complex matrix/rhs.
class AcNumericContext final : public AcLoadContext {
public:
    AcNumericContext(ws::math::SparseMatrix<std::complex<double>>& matrix,
                     std::vector<std::complex<double>>& rhs, std::size_t node_count,
                     double w)
        : matrix_(matrix), rhs_(rhs), node_count_(node_count) {
        this->omega = w;
    }

    void add_entry(std::size_t row, std::size_t col, std::complex<double> v) override {
        if (row == kGround || col == kGround) {
            return;
        }
        matrix_.add(row, col, v);
    }
    void add_rhs(std::size_t row, std::complex<double> v) override {
        if (row == kGround) {
            return;
        }
        rhs_[row] += v;
    }
    [[nodiscard]] std::size_t branch_row(BranchId br) const override {
        return node_count_ + br;
    }

private:
    ws::math::SparseMatrix<std::complex<double>>& matrix_;
    std::vector<std::complex<double>>& rhs_;
    std::size_t node_count_;
};

}  // namespace detail

MnaSystem::MnaSystem(std::size_t node_count, std::size_t branch_count,
                     const Circuit& circuit)
    : node_count_(node_count), branch_count_(branch_count), circuit_(circuit),
      rhs_(node_count + branch_count, 0.0),
      ac_rhs_(node_count + branch_count, {0.0, 0.0}) {
    // Pattern-mode scratch: the largest device's slices, zeroed. Stamp
    // POSITIONS never depend on values (§3.2), so zeroed scratch is as
    // good as any for the dry run.
    std::size_t max_params = 0;
    std::size_t max_state = 0;
    for (const auto& span : circuit.param_span) {
        max_params = std::max(max_params, span.count);
    }
    for (const auto& span : circuit.state_span) {
        max_state = std::max(max_state, span.count);
    }
    scratch_params_.assign(max_params, 0.0);
    scratch_state_.assign(max_state, 0.0);
}

ws::core::Status MnaSystem::setup(const std::vector<std::unique_ptr<Device>>& devices) {
    {
        // §3.2: the pattern is the UNION of what any load phase can touch.
        // Coefficient VALUES change with dt/omega/method; POSITIONS never
        // do, so the dry run stamps with zeroed scratch slices.
        detail::PatternContext ctx(builder_, node_count_);
        for (std::size_t i = 0; i < devices.size(); ++i) {
            ctx.params_ = scratch_params_.data();
            ctx.state_ = scratch_state_.data();
            devices[i]->load_dc(ctx);
        }
        const TranStep nominal{0.0, 1e-9, false, 1e-9, 1.0};
        for (std::size_t i = 0; i < devices.size(); ++i) {
            ctx.params_ = scratch_params_.data();
            ctx.state_ = scratch_state_.data();
            devices[i]->load_tran(ctx, nominal);
        }
        detail::AcPatternContext ac_ctx(builder_, node_count_);
        for (std::size_t i = 0; i < devices.size(); ++i) {
            ac_ctx.params_ = scratch_params_.data();
            // §3.6 (M3): nonlinear devices' load_ac reads state (a diode's
            // Gd_op from Vd_prev); in pattern mode the scratch slice is
            // zeroed, which produces a finite small Gd - the right
            // POSITIONS, which is all the dry run records.
            ac_ctx.state_ = scratch_state_.data();
            devices[i]->load_ac(ac_ctx);
        }
        // §5.2: every node diagonal joins the pattern (a legal structural
        // zero) so the convergence ladder's gmin/source/PTC shunts can
        // write (n, n) without tripping the §3.2 assert. Storage, not
        // flops.
        for (std::size_t n = 0; n < node_count_; ++n) {
            builder_.add(n, n);
        }
    }
    return analyze();
}

ws::core::Status MnaSystem::setup_ac() {
    if (pattern_.n == 0) {
        ws::core::Error e = ws::core::make_error(
            static_cast<std::int32_t>(1), "math",
            "MnaSystem::setup_ac before setup()");
        return ws::core::unexpected(std::move(e));
    }
    ac_matrix_.emplace(pattern_);
    ws::math::SparsePattern copy = pattern_;
    return ac_lu_.analyze(std::move(copy));
}

void MnaSystem::load(const std::vector<std::unique_ptr<Device>>& devices,
                      const RunState& rs, const std::vector<double>* x,
                      double gmin) {
    zero();
    if (matrix_) {
        detail::NumericContext ctx(*matrix_, rhs_, node_count_, x, gmin);
        for (std::size_t i = 0; i < devices.size(); ++i) {
            ctx.params_ = rs.params_.data() + circuit_.param_span[i].begin;
            ctx.state_ = rs.state_.data() + circuit_.state_span[i].begin;
            devices[i]->load_dc(ctx);
        }
    }
}

void MnaSystem::load_tran(const std::vector<std::unique_ptr<Device>>& devices,
                          const RunState& rs, const TranStep& step,
                          const std::vector<double>* x, double gmin) {
    zero();
    if (matrix_) {
        detail::NumericContext ctx(*matrix_, rhs_, node_count_, x, gmin);
        for (std::size_t i = 0; i < devices.size(); ++i) {
            ctx.params_ = rs.params_.data() + circuit_.param_span[i].begin;
            ctx.state_ = rs.state_.data() + circuit_.state_span[i].begin;
            devices[i]->load_tran(ctx, step);
        }
    }
}

void MnaSystem::load_ac(const std::vector<std::unique_ptr<Device>>& devices,
                        const RunState& rs, double omega) {
    zero_ac();
    if (ac_matrix_) {
        detail::AcNumericContext ctx(*ac_matrix_, ac_rhs_, node_count_, omega);
        ctx.omega = omega;
        for (std::size_t i = 0; i < devices.size(); ++i) {
            ctx.params_ = rs.params_.data() + circuit_.param_span[i].begin;
            // §3.6 (M3): nonlinear devices read their OP linearization
            // point back through state (a diode's Gd_op from Vd_prev).
            ctx.state_ = rs.state_.data() + circuit_.state_span[i].begin;
            devices[i]->load_ac(ctx);
        }
    }
}

ws::core::Status MnaSystem::analyze() {
    pattern_ = builder_.build(dim());
    matrix_.emplace(pattern_);
    ws::math::SparsePattern copy = pattern_;
    return lu_.analyze(std::move(copy));
}

void MnaSystem::zero() {
    if (matrix_) {
        matrix_->zero();
    }
    std::fill(rhs_.begin(), rhs_.end(), 0.0);
    ++load_count_;
}

void MnaSystem::zero_ac() {
    if (ac_matrix_) {
        ac_matrix_->zero();
    }
    std::fill(ac_rhs_.begin(), ac_rhs_.end(), std::complex<double>{0.0, 0.0});
}

ws::core::Status MnaSystem::solve(std::vector<double>& x) {
    x.assign(dim(), 0.0);
    WS_TRY(lu_.factorize(*matrix_));
    return lu_.solve(rhs_, x);
}

ws::core::Status MnaSystem::solve_ac(std::vector<std::complex<double>>& x) {
    x.assign(dim(), {0.0, 0.0});
    WS_TRY(ac_lu_.factorize(*ac_matrix_));
    return ac_lu_.solve(ac_rhs_, x);
}

void MnaSystem::force_node(std::size_t node, double volts, double gforce) {
    if (node == kGround || node >= dim() || !matrix_) {
        return;
    }
    matrix_->add(node, node, gforce);
    rhs_[node] += gforce * volts;
}

double MnaSystem::max_diagonal() const {
    double best = 0.0;
    if (matrix_) {
        for (std::size_t i = 0; i < dim(); ++i) {
            const std::size_t s = matrix_->slot(i, i);
            if (s != ws::math::SparseMatrix<double>::npos) {
                best = std::max(best, std::abs(matrix_->value_at_slot(s)));
            }
        }
    }
    return best;
}

}  // namespace ws::spice
