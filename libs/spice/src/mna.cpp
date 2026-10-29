#include "mna.hpp"

#include <utility>
#include <vector>

namespace ws::spice {

namespace detail {

// The pattern recorder (§3.2): records (row, col), ignores values; ground
// entries never reach the builder (ground row/col dropped, §4.1).
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

private:
    ws::math::SparsePatternBuilder& builder_;
    std::size_t node_count_;
};

// The numeric context: entries accumulate into the values array, rhs into
// the vector. A numeric write outside the recorded pattern is a
// programmer error (§3.2) - SparseMatrix::add asserts it.
class NumericContext final : public DcLoadContext {
public:
    NumericContext(ws::math::SparseMatrix<double>& matrix, std::vector<double>& rhs,
                   std::size_t node_count)
        : matrix_(matrix), rhs_(rhs), node_count_(node_count) {}

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

private:
    ws::math::SparseMatrix<double>& matrix_;
    std::vector<double>& rhs_;
    std::size_t node_count_;
};

}  // namespace detail

MnaSystem::MnaSystem(std::size_t node_count, std::size_t branch_count)
    : node_count_(node_count), branch_count_(branch_count),
      rhs_(node_count + branch_count, 0.0) {}

ws::core::Status MnaSystem::setup(const std::vector<std::unique_ptr<Device>>& devices) {
    {
        detail::PatternContext ctx(builder_, node_count_);
        for (const auto& dev : devices) {
            dev->load_dc(ctx);
        }
    }
    return analyze();
}

void MnaSystem::load(const std::vector<std::unique_ptr<Device>>& devices) {
    zero();
    if (matrix_) {
        detail::NumericContext ctx(*matrix_, rhs_, node_count_);
        for (const auto& dev : devices) {
            dev->load_dc(ctx);
        }
    }
}

ws::core::Status MnaSystem::analyze() {
    const ws::math::SparsePattern pattern = builder_.build(dim());
    matrix_.emplace(pattern);
    return lu_.analyze(std::move(pattern));
}

void MnaSystem::zero() {
    if (matrix_) {
        matrix_->zero();
    }
    std::fill(rhs_.begin(), rhs_.end(), 0.0);
    ++load_count_;
}

ws::core::Status MnaSystem::solve(std::vector<double>& x) {
    x.assign(dim(), 0.0);
    WS_TRY(lu_.factorize(*matrix_));
    return lu_.solve(rhs_, x);
}

}  // namespace ws::spice
