#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ws/core/result.hpp>
#include <ws/spice/error.hpp>

// The expression language (DESIGN.md §8.5) shared by .param, behavioral
// sources (M4) and device value={} forms: + - * / ** ^, unary ±,
// comparisons, ?:, && || !, the function set, numbers, and variable
// references. Expressions parse ONCE into a flat stack program with
// constants folded at parse time; variable references are collected by
// name and bound by the evaluator (.param scope; time/temp/freq in the
// analyses that define them).

namespace ws::spice {

class ExprProgram {
public:
    enum class Op : unsigned char {
        Const,   // a = const index
        Load,    // a = var index
        Neg,
        Add,
        Sub,
        Mul,
        Div,
        Pow,     // ** and ^
        Not,     // !  (1.0 / 0.0)
        Lt, Le, Gt, Ge, Eq, Ne,   // 1.0 / 0.0
        And, Or,                 // short-circuit semantics, 1.0 / 0.0
        Ternary, // cond ? a : b - a/b are code addresses (see below)
        JumpIfFalse,  // a = condition slot behavior: pop cond, jump b if false
        Jump,         // a = code address
        // functions (1 arg)
        Abs, Sqrt, Exp, Ln, Log, Log10, Sin, Cos, Tan, Asin, Acos, Atan,
        Sinh, Cosh, Tanh, Sgn, Floor, Ceil, Int, Uramp, U,
        // functions (2 args)
        Atan2, Pow2 /*pow(a,b) synonym*/, Min, Max, Limit, If /*like ternary fn*/,
    };

    struct Instr {
        Op op;
        int a = -1;
        int b = -1;
    };

    // Variable references by name, in first-seen order; the evaluator
    // supplies values slot-for-slot.
    const std::vector<std::string>& vars() const { return vars_; }
    const std::vector<Instr>& code() const { return code_; }

    // Evaluates with `var_values` sized vars().size(); a null entry is an
    // unbound variable (the caller's error to report, with its own
    // context). Returns NaN-free results or an error for domain problems
    // (sqrt/ln of negative, division by zero...) per §8.5.
    [[nodiscard]] ws::core::Result<double> evaluate(
        const std::vector<std::optional<double>>& var_values) const;

    // True when the expression is a lone constant (common: .param x=3).
    [[nodiscard]] bool is_constant() const {
        return code_.size() == 1 && code_[0].op == Op::Const;
    }
    [[nodiscard]] double constant_value() const;  // valid iff is_constant()

    // Debug/test stringifier.
    [[nodiscard]] std::string disassemble() const;

    // A program that evaluates to a fixed value (instance-parameter
    // overrides after evaluation in the caller's scope).
    [[nodiscard]] static ExprProgram constant(double v);

private:
    // The parser (src/expr.cpp) fills these directly - a plain data
    // carrier, not an invariant-bearing class.
public:
    std::vector<Instr> code_;
    std::vector<double> consts_;
    std::vector<std::string> vars_;
};

// Parses `text` (a full expression; surrounding whitespace allowed).
// Errors carry ErrorKind::Parse with the offending offset in the message.
[[nodiscard]] ws::core::Result<ExprProgram> parse_expression(std::string_view text);

}  // namespace ws::spice
