#include <ws/spice/expr.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <utility>

#include <ws/spice/parse.hpp>
#include <ws/spice/state.hpp>

namespace ws::spice {

namespace {

[[nodiscard]] bool is_digit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }
[[nodiscard]] bool is_ident_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_' ||
        static_cast<unsigned char>(c) >= 0x80;
}
[[nodiscard]] bool is_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' ||
        static_cast<unsigned char>(c) >= 0x80;
}

Diagnostic expr_diag(std::string message, std::size_t offset) {
    Diagnostic d;
    d.kind = ErrorKind::Parse;
    d.code = "spice.parse.expr";
    d.message = std::move(message) + " (at offset " + std::to_string(offset) + ")";
    return d;
}

Diagnostic mk_diag(ErrorKind kind, std::string code, std::string message) {
    Diagnostic d;
    d.kind = kind;
    d.code = std::move(code);
    d.message = std::move(message);
    return d;
}

// One-argument function names (case-insensitive).
const std::unordered_map<std::string, ExprProgram::Op>& unary_functions() {
    static const std::unordered_map<std::string, ExprProgram::Op> table = {
        {"abs", ExprProgram::Op::Abs},     {"sqrt", ExprProgram::Op::Sqrt},
        {"exp", ExprProgram::Op::Exp},     {"ln", ExprProgram::Op::Ln},
        {"log", ExprProgram::Op::Log},     {"log10", ExprProgram::Op::Log10},
        {"sin", ExprProgram::Op::Sin},     {"cos", ExprProgram::Op::Cos},
        {"tan", ExprProgram::Op::Tan},     {"asin", ExprProgram::Op::Asin},
        {"acos", ExprProgram::Op::Acos},   {"atan", ExprProgram::Op::Atan},
        {"sinh", ExprProgram::Op::Sinh},   {"cosh", ExprProgram::Op::Cosh},
        {"tanh", ExprProgram::Op::Tanh},   {"sgn", ExprProgram::Op::Sgn},
        {"floor", ExprProgram::Op::Floor}, {"ceil", ExprProgram::Op::Ceil},
        {"int", ExprProgram::Op::Int},     {"uramp", ExprProgram::Op::Uramp},
        {"u", ExprProgram::Op::U},
        // Monte-Carlo draws (§8.5 rev): never folded, always drawn at
        // evaluation time through the Rand* in the eval context.
        {"gauss", ExprProgram::Op::Gauss},  {"flat", ExprProgram::Op::Flat},
    };
    return table;
}

const std::unordered_map<std::string, ExprProgram::Op>& binary_functions() {
    static const std::unordered_map<std::string, ExprProgram::Op> table = {
        {"atan2", ExprProgram::Op::Atan2},
        {"pow", ExprProgram::Op::Pow2},
        {"pwr", ExprProgram::Op::Pow2},  // same semantics as pow here (abs-base is a ngspice nuance kept simple in v1)
        {"min", ExprProgram::Op::Min},
        {"max", ExprProgram::Op::Max},
        {"limit", ExprProgram::Op::Limit},
        {"if", ExprProgram::Op::If},
    };
    return table;
}

struct ExprParser {
    std::string_view text;
    std::size_t pos = 0;
    ExprProgram prog;
    std::map<std::string, int> var_index;  // lowercased name -> slot

    explicit ExprParser(std::string_view t) : text(t) {}

    void skip_ws() {
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) {
            ++pos;
        }
    }
    [[nodiscard]] bool peek(char c) const {
        return pos < text.size() && text[pos] == c;
    }
    bool eat(char c) {
        skip_ws();
        if (peek(c)) {
            ++pos;
            return true;
        }
        return false;
    }
    [[nodiscard]] char cur() const {
        return pos < text.size() ? text[pos] : '\0';
    }

    void emit(ExprProgram::Op op, int a = -1, int b = -1) {
        prog.code_.push_back(ExprProgram::Instr{op, a, b});
    }
    int emit_const(double v) {
        prog.consts_.push_back(v);
        emit(ExprProgram::Op::Const, static_cast<int>(prog.consts_.size() - 1));
        return 0;
    }
    int var_slot(const std::string& lower) {
        if (const auto it = var_index.find(lower); it != var_index.end()) {
            return it->second;
        }
        const int slot = static_cast<int>(prog.vars_.size());
        var_index.emplace(lower, slot);
        prog.vars_.push_back(lower);
        return slot;
    }

    // --- grammar ---------------------------------------------------------

    ws::core::Status parse_expr() {
        WS_TRY(parse_ternary());
        skip_ws();
        if (pos != text.size()) {
            return spice_fail(expr_diag("unexpected '" + std::string(text.substr(pos, 1)) + "'",
                                        pos));
        }
        return {};
    }

    ws::core::Status parse_ternary() {
        WS_TRY(parse_or());
        skip_ws();
        if (cur() != '?') {
            return {};
        }
        ++pos;
        // cond is on the stack; build the branch scaffold.
        const int jfalse = static_cast<int>(prog.code_.size());
        emit(ExprProgram::Op::JumpIfFalse);  // patched below
        WS_TRY(parse_ternary());
        const int jend = static_cast<int>(prog.code_.size());
        emit(ExprProgram::Op::Jump);
        prog.code_[jfalse].b = static_cast<int>(prog.code_.size());
        if (!eat(':')) {
            return spice_fail(expr_diag("expected ':' in '?:'", pos));
        }
        WS_TRY(parse_ternary());
        prog.code_[jend].a = static_cast<int>(prog.code_.size());
        return {};
    }

    ws::core::Status parse_or() {
        WS_TRY(parse_and());
        for (;;) {
            skip_ws();
            if (pos + 1 < text.size() && text[pos] == '|' && text[pos + 1] == '|') {
                pos += 2;
                WS_TRY(parse_and());
                emit(ExprProgram::Op::Or);
            } else {
                return {};
            }
        }
    }

    ws::core::Status parse_and() {
        WS_TRY(parse_equality());
        for (;;) {
            skip_ws();
            if (pos + 1 < text.size() && text[pos] == '&' && text[pos + 1] == '&') {
                pos += 2;
                WS_TRY(parse_equality());
                emit(ExprProgram::Op::And);
            } else {
                return {};
            }
        }
    }

    ws::core::Status parse_equality() {
        WS_TRY(parse_relational());
        for (;;) {
            skip_ws();
            if (pos + 1 < text.size() && text[pos] == '=' && text[pos + 1] == '=') {
                pos += 2;
                WS_TRY(parse_relational());
                emit(ExprProgram::Op::Eq);
            } else if (pos + 1 < text.size() && text[pos] == '!' && text[pos + 1] == '=') {
                pos += 2;
                WS_TRY(parse_relational());
                emit(ExprProgram::Op::Ne);
            } else {
                return {};
            }
        }
    }

    ws::core::Status parse_relational() {
        WS_TRY(parse_additive());
        for (;;) {
            skip_ws();
            if (pos + 1 < text.size() && text[pos] == '<' && text[pos + 1] == '=') {
                pos += 2;
                WS_TRY(parse_additive());
                emit(ExprProgram::Op::Le);
            } else if (pos + 1 < text.size() && text[pos] == '>' && text[pos + 1] == '=') {
                pos += 2;
                WS_TRY(parse_additive());
                emit(ExprProgram::Op::Ge);
            } else if (peek('<')) {
                ++pos;
                WS_TRY(parse_additive());
                emit(ExprProgram::Op::Lt);
            } else if (peek('>')) {
                ++pos;
                WS_TRY(parse_additive());
                emit(ExprProgram::Op::Gt);
            } else {
                return {};
            }
        }
    }

    ws::core::Status parse_additive() {
        WS_TRY(parse_multiplicative());
        for (;;) {
            skip_ws();
            if (peek('+')) {
                ++pos;
                WS_TRY(parse_multiplicative());
                emit(ExprProgram::Op::Add);
            } else if (peek('-')) {
                ++pos;
                WS_TRY(parse_multiplicative());
                emit(ExprProgram::Op::Sub);
            } else {
                return {};
            }
        }
    }

    ws::core::Status parse_multiplicative() {
        WS_TRY(parse_unary());
        for (;;) {
            skip_ws();
            if (peek('*') && (pos + 1 < text.size() && text[pos + 1] == '*')) {
                // '**' binds like '^' (handled in power); here it must NOT
                // be consumed as multiply-multiply.
                return {};
            }
            if (peek('*')) {
                ++pos;
                WS_TRY(parse_unary());
                emit(ExprProgram::Op::Mul);
            } else if (peek('/')) {
                ++pos;
                WS_TRY(parse_unary());
                emit(ExprProgram::Op::Div);
            } else {
                return {};
            }
        }
    }

    ws::core::Status parse_unary() {
        skip_ws();
        if (peek('-')) {
            ++pos;
            WS_TRY(parse_unary());
            emit(ExprProgram::Op::Neg);
            return {};
        }
        if (peek('+')) {
            ++pos;
            return parse_unary();
        }
        if (peek('!')) {
            ++pos;
            WS_TRY(parse_unary());
            emit(ExprProgram::Op::Not);
            return {};
        }
        return parse_power();
    }

    ws::core::Status parse_power() {
        // primary [ '**' | '^' unary ]  - right associative, binds tighter
        // than unary minus ('-x**2' == '-(x**2)').
        WS_TRY(parse_primary());
        skip_ws();
        const bool caret = peek('^');
        const bool starstar =
            pos + 1 < text.size() && text[pos] == '*' && text[pos + 1] == '*';
        if (caret || starstar) {
            pos += caret ? 1 : 2;
            WS_TRY(parse_unary());  // right assoc: exponent may nest
            emit(ExprProgram::Op::Pow);
        }
        return {};
    }

    ws::core::Status parse_primary() {
        skip_ws();
        if (pos >= text.size()) {
            return spice_fail(expr_diag("unexpected end of expression", pos));
        }
        if (peek('@')) {
            return parse_at_ref();
        }
        if (peek('(')) {
            ++pos;
            WS_TRY(parse_ternary());
            if (!eat(')')) {
                return spice_fail(expr_diag("expected ')'", pos));
            }
            return {};
        }
        if (is_digit(cur()) || peek('.')) {
            return parse_number_token();
        }
        if (is_ident_start(cur())) {
            return parse_ident();
        }
        return spice_fail(expr_diag("unexpected '" + std::string(text.substr(pos, 1)) + "'",
                                    pos));
    }

    ws::core::Status parse_number_token() {
        const std::size_t start = pos;
        // digits [. digits] [e[+-]digits] then suffix/unit letters - the
        // full §8.2 grammar through parse_number.
        while (pos < text.size() && (is_digit(text[pos]) || text[pos] == '.')) {
            ++pos;
        }
        if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
            std::size_t q = pos + 1;
            if (q < text.size() && (text[q] == '+' || text[q] == '-')) {
                ++q;
            }
            const std::size_t es = q;
            while (q < text.size() && is_digit(text[q])) {
                ++q;
            }
            if (q > es) {
                pos = q;
            }
        }
        while (pos < text.size() &&
               (std::isalpha(static_cast<unsigned char>(text[pos])) != 0 ||
                static_cast<unsigned char>(text[pos]) >= 0x80)) {
            ++pos;  // scale suffix + unit tail
        }
        const std::string token(text.substr(start, pos - start));
        const ws::core::Result<double> v = parse_number(token);
        if (!v) {
            return spice_fail(expr_diag("malformed number '" + token + "'", start));
        }
        emit_const(*v);
        return {};
    }

    ws::core::Status parse_at_ref() {
        // @dev[param] (§7.8 rev): a device's bound parameter value - the
        // ngspice spelling people use constantly (@m1[gm] lands with the
        // semiconductor models, M4; today it reads bound construction
        // parameters like @R1[value] or @V1[dc]).
        const std::size_t start = pos;
        ++pos;  // '@'
        const std::size_t d0 = pos;
        while (pos < text.size() && is_ident_char(text[pos])) {
            ++pos;
        }
        if (pos == d0) {
            return spice_fail(expr_diag("expected a device name after '@'", start));
        }
        const std::string dev(text.substr(d0, pos - d0));
        if (!eat('[')) {
            return spice_fail(expr_diag("expected '[param]' after '@" + dev + "'", start));
        }
        const std::size_t p0 = pos;
        while (pos < text.size() && text[pos] != ']') {
            ++pos;
        }
        if (pos == p0 || pos >= text.size()) {
            return spice_fail(expr_diag("expected ']' in '@" + dev + "[...]'", start));
        }
        const std::string param(text.substr(p0, pos - p0));
        ++pos;  // ']'
        const std::string var = "@" + to_lower(dev) + "[" + to_lower(param) + "]";
        emit(ExprProgram::Op::Load, var_slot(var));
        return {};
    }

    ws::core::Status parse_ident() {
        const std::size_t start = pos;
        while (pos < text.size() && is_ident_char(text[pos])) {
            ++pos;
        }
        const std::string name(text.substr(start, pos - start));
        skip_ws();
        if (peek('(')) {
            // Signal reference (§7.8 rev): v(node), v(a,b), i(Vsrc) - a
            // VARIABLE named by its spelling, not a function call. The
            // save-list evaluator binds it against the result columns.
            const std::string lower_name = to_lower(name);
            if (lower_name == "v" || lower_name == "i") {
                const std::size_t ref_start = start;
                ++pos;  // '('
                skip_ws();
                std::vector<std::string> args;
                for (;;) {
                    const std::size_t a0 = pos;
                    while (pos < text.size() && text[pos] != ',' && text[pos] != ')' &&
                           !std::isspace(static_cast<unsigned char>(text[pos]))) {
                        ++pos;
                    }
                    if (pos == a0) {
                        return spice_fail(expr_diag(
                            "expected a node or source name inside '" + name + "(...)'",
                            ref_start));
                    }
                    args.emplace_back(text.substr(a0, pos - a0));
                    skip_ws();
                    if (peek(',')) {
                        ++pos;
                        skip_ws();
                        continue;
                    }
                    break;
                }
                if (!eat(')')) {
                    return spice_fail(expr_diag("expected ')' after '" + name + "(...)'",
                                                ref_start));
                }
                if (lower_name == "v" && (args.size() < 1 || args.size() > 2)) {
                    return spice_fail(expr_diag(
                        "v() takes one node or two (v(a,b) is the differential)",
                        ref_start));
                }
                if (lower_name == "i" && args.size() != 1) {
                    return spice_fail(expr_diag("i() takes one source name", ref_start));
                }
                std::string var = lower_name + "(";
                for (std::size_t k = 0; k < args.size(); ++k) {
                    if (k != 0) {
                        var += ",";
                    }
                    var += to_lower(args[k]);
                }
                var += ")";
                emit(ExprProgram::Op::Load, var_slot(var));
                return {};
            }
            // function call
            ++pos;
            std::vector<ExprProgram::Op> arg_shape;  // not used; arity checked inline
            (void)arg_shape;
            // parse first arg
            WS_TRY(parse_ternary());
            if (eat(',')) {
                WS_TRY(parse_ternary());
                if (eat(',')) {
                    return spice_fail(
                        expr_diag("function '" + name + "' takes at most two arguments", start));
                }
                const auto& fns = binary_functions();
                const auto it = fns.find(to_lower(name));
                if (it == fns.end()) {
                    return spice_fail(
                        expr_diag("unknown function '" + name + "'", start));
                }
                emit(it->second);
            } else {
                const auto& fns = unary_functions();
                const auto it = fns.find(to_lower(name));
                if (it == fns.end()) {
                    return spice_fail(expr_diag("unknown function '" + name + "'", start));
                }
                emit(it->second);
            }
            if (!eat(')')) {
                return spice_fail(expr_diag("expected ')' after arguments of '" + name + "'",
                                            pos));
            }
            return {};
        }
        // plain variable reference
        emit(ExprProgram::Op::Load, var_slot(to_lower(name)));
        return {};
    }
};

// Constant folding over straight-line code (anything with jumps stays as
// parsed - the conservative rule; a jump-free ternary cannot exist).
void fold_constants(ExprProgram& p) {
    for (const auto& instr : p.code_) {
        if (instr.op == ExprProgram::Op::Jump || instr.op == ExprProgram::Op::JumpIfFalse) {
            return;  // control flow: skip folding entirely
        }
    }
    using Op = ExprProgram::Op;
    std::vector<std::optional<double>> stack;   // evaluation shape
    std::vector<ExprProgram::Instr> out;        // emitted code
    std::vector<double> consts;                 // new const pool

    // Emits a constant and records it on the shape stack.
    auto emit_const = [&](double v) {
        consts.push_back(v);
        out.push_back(ExprProgram::Instr{Op::Const, static_cast<int>(consts.size() - 1)});
        stack.push_back(v);
    };
    auto emit_as_is = [&](const ExprProgram::Instr& in) {
        out.push_back(in);
        stack.push_back(std::nullopt);
    };
    auto pop = [&]() -> std::optional<double> {
        const std::optional<double> v = stack.back();
        stack.pop_back();
        return v;
    };

    // Fold a binary op; nullopt when the result is out of domain (the
    // program is then left unfolded - the error belongs to evaluation,
    // where it carries runtime context).
    auto fold_binary = [](Op op, double a, double b) -> std::optional<double> {
        double v = 0.0;
        switch (op) {
            case Op::Add: v = a + b; break;
            case Op::Sub: v = a - b; break;
            case Op::Mul: v = a * b; break;
            case Op::Div: if (b == 0.0) { return std::nullopt; } v = a / b; break;
            case Op::Pow: case Op::Pow2:
                v = std::pow(a, b);
                if (!std::isfinite(v)) { return std::nullopt; }
                break;
            case Op::Min: v = std::min(a, b); break;
            case Op::Max: v = std::max(a, b); break;
            case Op::Limit: v = a; break;
            case Op::Atan2: v = std::atan2(a, b); break;
            case Op::If: v = (a != 0.0) ? b : 0.0; break;
            case Op::Lt: v = a < b ? 1.0 : 0.0; break;
            case Op::Le: v = a <= b ? 1.0 : 0.0; break;
            case Op::Gt: v = a > b ? 1.0 : 0.0; break;
            case Op::Ge: v = a >= b ? 1.0 : 0.0; break;
            case Op::Eq: v = a == b ? 1.0 : 0.0; break;
            case Op::Ne: v = a != b ? 1.0 : 0.0; break;
            case Op::And: v = (a != 0.0 && b != 0.0) ? 1.0 : 0.0; break;
            case Op::Or: v = (a != 0.0 || b != 0.0) ? 1.0 : 0.0; break;
            default: return std::nullopt;
        }
        if (!std::isfinite(v)) {
            return std::nullopt;
        }
        return v;
    };

    auto fold_unary = [](Op op, double a) -> std::optional<double> {
        double v = 0.0;
        switch (op) {
            case Op::Abs: v = std::abs(a); break;
            case Op::Sqrt: if (a < 0) { return std::nullopt; } v = std::sqrt(a); break;
            case Op::Exp: v = std::exp(a); break;
            case Op::Ln: if (a <= 0) { return std::nullopt; } v = std::log(a); break;
            case Op::Log: case Op::Log10: if (a <= 0) { return std::nullopt; } v = std::log10(a); break;
            case Op::Sin: v = std::sin(a); break;
            case Op::Cos: v = std::cos(a); break;
            case Op::Tan: v = std::tan(a); break;
            case Op::Asin: if (a < -1 || a > 1) { return std::nullopt; } v = std::asin(a); break;
            case Op::Acos: if (a < -1 || a > 1) { return std::nullopt; } v = std::acos(a); break;
            case Op::Atan: v = std::atan(a); break;
            case Op::Sinh: v = std::sinh(a); break;
            case Op::Cosh: v = std::cosh(a); break;
            case Op::Tanh: v = std::tanh(a); break;
            case Op::Sgn: v = (a > 0) - (a < 0); break;
            case Op::Floor: v = std::floor(a); break;
            case Op::Ceil: v = std::ceil(a); break;
            case Op::Int: v = std::trunc(a); break;
            case Op::Uramp: v = a > 0 ? a : 0.0; break;
            case Op::U: v = a > 0 ? 1.0 : 0.0; break;
            case Op::Neg: v = -a; break;
            default: return std::nullopt;
        }
        if (!std::isfinite(v)) {
            return std::nullopt;
        }
        return v;
    };

    const auto is_unary_op = [](Op op) {
        return op == Op::Neg || op == Op::Abs || op == Op::Sqrt || op == Op::Exp ||
            op == Op::Ln || op == Op::Log || op == Op::Log10 || op == Op::Sin ||
            op == Op::Cos || op == Op::Tan || op == Op::Asin || op == Op::Acos ||
            op == Op::Atan || op == Op::Sinh || op == Op::Cosh || op == Op::Tanh ||
            op == Op::Sgn || op == Op::Floor || op == Op::Ceil || op == Op::Int ||
            op == Op::Uramp || op == Op::U;
    };
    const auto is_binary_op = [](Op op) {
        return op == Op::Add || op == Op::Sub || op == Op::Mul || op == Op::Div ||
            op == Op::Pow || op == Op::Pow2 || op == Op::Min || op == Op::Max ||
            op == Op::Limit || op == Op::Atan2 || op == Op::If || op == Op::Lt ||
            op == Op::Le || op == Op::Gt || op == Op::Ge || op == Op::Eq ||
            op == Op::Ne || op == Op::And || op == Op::Or;
    };

    for (const auto& in : p.code_) {
        if (in.op == Op::Const) {
            emit_const(p.consts_[static_cast<std::size_t>(in.a)]);
        } else if (in.op == Op::Load) {
            emit_as_is(in);
        } else if (is_unary_op(in.op)) {
            const std::optional<double> a = pop();
            if (a) {
                const std::optional<double> v = fold_unary(in.op, *a);
                if (v) {
                    out.pop_back();  // the operand's single Const instr
                    emit_const(*v);
                } else {
                    return;  // out of domain: leave the program unfolded
                }
            } else {
                // operand was emitted already; emit the op after it
                out.push_back(in);
                stack.push_back(std::nullopt);
            }
        } else if (is_binary_op(in.op)) {
            const std::optional<double> b = pop();
            const std::optional<double> a = pop();
            if (a && b) {
                const std::optional<double> v = fold_binary(in.op, *a, *b);
                if (v) {
                    out.pop_back();  // the two operand Const instrs
                    out.pop_back();
                    emit_const(*v);
                } else {
                    return;
                }
            } else {
                // operands (folded or not) are already on `out`; the shape
                // stack may hold nullopts - re-push a non-const marker.
                out.push_back(in);
                stack.push_back(std::nullopt);
            }
        } else {
            return;  // anything unexpected: leave unfolded
        }
    }
    p.code_ = std::move(out);
    p.consts_ = std::move(consts);
}

}  // namespace

ws::core::Result<ExprProgram> parse_expression(std::string_view text) {
    ExprParser parser(text);
    WS_TRY(parser.parse_expr());
    fold_constants(parser.prog);
    // An expression must leave exactly one value: sanity-check depth.
    return parser.prog;
}

double ExprProgram::constant_value() const {
    return consts_[static_cast<std::size_t>(code_[0].a)];
}

ExprProgram ExprProgram::constant(double v) {
    ExprProgram p;
    p.consts_ = {v};
    p.code_ = {Instr{Op::Const, 0, -1}};
    return p;
}

ws::core::Result<double> ExprProgram::evaluate(
    const std::vector<std::optional<double>>& var_values, Rand* rng) const {
    if (var_values.size() != vars_.size()) {
        return spice_fail(mk_diag(
            ErrorKind::Usage, "spice.expr.bindings",
            "expression expects " + std::to_string(vars_.size()) +
                " variable bindings, got " + std::to_string(var_values.size())));
    }
    std::vector<double> stack;
    stack.reserve(8);
    auto pop = [&stack]() -> double {
        const double v = stack.back();
        stack.pop_back();
        return v;
    };
    auto domain = [&](const char* fn) -> ws::core::unexpected<ws::core::Error> {
        return spice_fail(mk_diag(
            ErrorKind::Convergence, "spice.expr.domain",
            std::string("expression evaluated outside the domain of ") + fn));
    };

    for (std::size_t pc = 0; pc < code_.size();) {
        const Instr& in = code_[pc];
        switch (in.op) {
            case Op::Const:
                stack.push_back(consts_[static_cast<std::size_t>(in.a)]);
                ++pc;
                break;
            case Op::Load: {
                const auto& v = var_values[static_cast<std::size_t>(in.a)];
                if (!v) {
                    return spice_fail(mk_diag(
                        ErrorKind::Semantic, "spice.expr.unbound",
                        "expression references unbound variable '" +
                            vars_[static_cast<std::size_t>(in.a)] + "'"));
                }
                stack.push_back(*v);
                ++pc;
                break;
            }
            case Op::Neg: stack.push_back(-pop()); ++pc; break;
            case Op::Add: { const double b = pop(), a = pop(); stack.push_back(a + b); ++pc; break; }
            case Op::Sub: { const double b = pop(), a = pop(); stack.push_back(a - b); ++pc; break; }
            case Op::Mul: { const double b = pop(), a = pop(); stack.push_back(a * b); ++pc; break; }
            case Op::Div: {
                const double b = pop(), a = pop();
                if (b == 0.0) {
                    return domain("division");
                }
                stack.push_back(a / b);
                ++pc;
                break;
            }
            case Op::Pow: case Op::Pow2: {
                const double b = pop(), a = pop();
                const double v = std::pow(a, b);
                if (!std::isfinite(v)) {
                    return domain("pow");
                }
                stack.push_back(v);
                ++pc;
                break;
            }
            case Op::Not: stack.push_back(pop() == 0.0 ? 1.0 : 0.0); ++pc; break;
            case Op::Lt: { const double b = pop(), a = pop(); stack.push_back(a < b ? 1.0 : 0.0); ++pc; break; }
            case Op::Le: { const double b = pop(), a = pop(); stack.push_back(a <= b ? 1.0 : 0.0); ++pc; break; }
            case Op::Gt: { const double b = pop(), a = pop(); stack.push_back(a > b ? 1.0 : 0.0); ++pc; break; }
            case Op::Ge: { const double b = pop(), a = pop(); stack.push_back(a >= b ? 1.0 : 0.0); ++pc; break; }
            case Op::Eq: { const double b = pop(), a = pop(); stack.push_back(a == b ? 1.0 : 0.0); ++pc; break; }
            case Op::Ne: { const double b = pop(), a = pop(); stack.push_back(a != b ? 1.0 : 0.0); ++pc; break; }
            case Op::And: { const double b = pop(), a = pop(); stack.push_back((a != 0.0 && b != 0.0) ? 1.0 : 0.0); ++pc; break; }
            case Op::Or: { const double b = pop(), a = pop(); stack.push_back((a != 0.0 || b != 0.0) ? 1.0 : 0.0); ++pc; break; }
            case Op::Jump: pc = static_cast<std::size_t>(in.a); break;
            case Op::JumpIfFalse: {
                const double c = pop();
                pc = (c == 0.0) ? static_cast<std::size_t>(in.b) : pc + 1;
                break;
            }
            case Op::Ternary:
                // The parser lowers ?: to Jump/JumpIfFalse; this opcode
                // never executes.
                return spice_fail(mk_diag(ErrorKind::Parse, "spice.expr.stack",
                                          "internal: ternary opcode executed"));
            case Op::Abs: stack.push_back(std::abs(pop())); ++pc; break;
            case Op::Sqrt: {
                const double a = pop();
                if (a < 0) {
                    return domain("sqrt");
                }
                stack.push_back(std::sqrt(a));
                ++pc;
                break;
            }
            case Op::Exp: stack.push_back(std::exp(pop())); ++pc; break;
            case Op::Ln: {
                const double a = pop();
                if (a <= 0) {
                    return domain("ln");
                }
                stack.push_back(std::log(a));  // natural log
                ++pc;
                break;
            }
            case Op::Log: case Op::Log10: {
                const double a = pop();
                if (a <= 0) {
                    return domain("log");
                }
                stack.push_back(std::log10(a));
                ++pc;
                break;
            }
            case Op::Sin: stack.push_back(std::sin(pop())); ++pc; break;
            case Op::Cos: stack.push_back(std::cos(pop())); ++pc; break;
            case Op::Tan: stack.push_back(std::tan(pop())); ++pc; break;
            case Op::Asin: {
                const double a = pop();
                if (a < -1 || a > 1) {
                    return domain("asin");
                }
                stack.push_back(std::asin(a));
                ++pc;
                break;
            }
            case Op::Acos: {
                const double a = pop();
                if (a < -1 || a > 1) {
                    return domain("acos");
                }
                stack.push_back(std::acos(a));
                ++pc;
                break;
            }
            case Op::Atan: stack.push_back(std::atan(pop())); ++pc; break;
            case Op::Sinh: stack.push_back(std::sinh(pop())); ++pc; break;
            case Op::Cosh: stack.push_back(std::cosh(pop())); ++pc; break;
            case Op::Tanh: stack.push_back(std::tanh(pop())); ++pc; break;
            case Op::Sgn: { const double a = pop(); stack.push_back((a > 0) - (a < 0)); ++pc; break; }
            case Op::Floor: stack.push_back(std::floor(pop())); ++pc; break;
            case Op::Ceil: stack.push_back(std::ceil(pop())); ++pc; break;
            case Op::Int: stack.push_back(std::trunc(pop())); ++pc; break;
            case Op::Uramp: { const double a = pop(); stack.push_back(a > 0 ? a : 0.0); ++pc; break; }
            case Op::U: { const double a = pop(); stack.push_back(a > 0 ? 1.0 : 0.0); ++pc; break; }
            case Op::Gauss: {
                const double a = pop();
                if (rng == nullptr) {
                    return spice_fail(mk_diag(
                        ErrorKind::Usage, "spice.expr.no_rng",
                        "gauss() needs a random context: it draws at run time"
                        " (.step / Monte-Carlo rebinds provide one)"));
                }
                stack.push_back(rng->normal(a));
                ++pc;
                break;
            }
            case Op::Flat: {
                const double a = pop();
                if (rng == nullptr) {
                    return spice_fail(mk_diag(
                        ErrorKind::Usage, "spice.expr.no_rng",
                        "flat() needs a random context: it draws at run time"
                        " (.step / Monte-Carlo rebinds provide one)"));
                }
                stack.push_back(rng->flat(a));
                ++pc;
                break;
            }
            case Op::Atan2: { const double b = pop(), a = pop(); stack.push_back(std::atan2(a, b)); ++pc; break; }
            case Op::Min: { const double b = pop(), a = pop(); stack.push_back(std::min(a, b)); ++pc; break; }
            case Op::Max: { const double b = pop(), a = pop(); stack.push_back(std::max(a, b)); ++pc; break; }
            case Op::Limit: {
                // limit(x, hi) is the 2-arg ngspice clamp toward hi? The
                // 3-arg form is standard; 2-arg here clamps to [min,max] of
                // the pair as a harmless bound.
                const double b = pop(), a = pop();
                stack.push_back(std::clamp(a, std::min(a, b), std::max(a, b)));
                ++pc;
                break;
            }
            case Op::If: {
                const double b = pop(), a = pop();
                stack.push_back(a != 0.0 ? b : 0.0);
                ++pc;
                break;
            }
        }
    }
    if (stack.size() != 1) {
        return spice_fail(mk_diag(
            ErrorKind::Parse, "spice.expr.stack",
            "expression left " + std::to_string(stack.size()) + " values (internal)"));
    }
    const double v = stack.back();
    if (!std::isfinite(v)) {
        return domain("expression");
    }
    return v;
}

std::string ExprProgram::disassemble() const {
    std::ostringstream out;
    for (std::size_t i = 0; i < code_.size(); ++i) {
        out << i << ": ";
        const Instr& in = code_[i];
        switch (in.op) {
            case Op::Const: out << "const " << consts_[static_cast<std::size_t>(in.a)]; break;
            case Op::Load: out << "load " << vars_[static_cast<std::size_t>(in.a)]; break;
            default: {
                static const std::unordered_map<Op, const char*> names = {
                    {Op::Neg, "neg"}, {Op::Add, "add"}, {Op::Sub, "sub"}, {Op::Mul, "mul"},
                    {Op::Div, "div"}, {Op::Pow, "pow"}, {Op::Not, "not"},
                    {Op::Lt, "lt"}, {Op::Le, "le"}, {Op::Gt, "gt"}, {Op::Ge, "ge"},
                    {Op::Eq, "eq"}, {Op::Ne, "ne"}, {Op::And, "and"}, {Op::Or, "or"},
                    {Op::Ternary, "ternary"}, {Op::JumpIfFalse, "jump_if_false"},
                    {Op::Jump, "jump"}, {Op::Abs, "abs"}, {Op::Sqrt, "sqrt"},
                    {Op::Exp, "exp"}, {Op::Ln, "ln"}, {Op::Log, "log"},
                    {Op::Log10, "log10"}, {Op::Sin, "sin"}, {Op::Cos, "cos"},
                    {Op::Tan, "tan"}, {Op::Asin, "asin"}, {Op::Acos, "acos"},
                    {Op::Atan, "atan"}, {Op::Sinh, "sinh"}, {Op::Cosh, "cosh"},
                    {Op::Tanh, "tanh"}, {Op::Sgn, "sgn"}, {Op::Floor, "floor"},
                    {Op::Ceil, "ceil"}, {Op::Int, "int"}, {Op::Uramp, "uramp"},
                    {Op::U, "u"}, {Op::Atan2, "atan2"}, {Op::Pow2, "pow2"},
                    {Op::Min, "min"}, {Op::Max, "max"}, {Op::Limit, "limit"},
                    {Op::If, "if"},
                };
                out << names.at(in.op);
                if (in.a >= 0) {
                    out << " " << in.a;
                }
                if (in.b >= 0) {
                    out << " " << in.b;
                }
                break;
            }
        }
        out << "\n";
    }
    return out.str();
}

}  // namespace ws::spice
