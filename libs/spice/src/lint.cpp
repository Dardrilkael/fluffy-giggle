// The lint pass (§9.3 rev). The number-trap scanner works on raw value
// TOKENS (before parsing collapses them to doubles - once parsed, the
// damage is invisible), replicating the §8.2 longest-match suffix scan
// only to classify which scale factor a token actually carries.

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include <ws/spice/circuit.hpp>
#include <ws/spice/lint.hpp>
#include <ws/spice/parse.hpp>

namespace ws::spice {

namespace {

// The scale suffix a numeric token carries (MEG/MIL before M, §8.2), or
// nullopt when the token is not numeric or carries no suffix.
std::optional<std::string> scale_suffix_of(const std::string& text) {
    // Numeric prefix: sign, digits with optional '.', optional exponent.
    std::size_t i = 0;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
        ++i;
    }
    bool digits = false;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])) != 0) {
        ++i;
        digits = true;
    }
    if (i < text.size() && text[i] == '.') {
        ++i;
        while (i < text.size() &&
               std::isdigit(static_cast<unsigned char>(text[i])) != 0) {
            ++i;
            digits = true;
        }
    }
    if (!digits) {
        return std::nullopt;
    }
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        const std::size_t save = i;
        ++i;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
            ++i;
        }
        bool exp_digits = false;
        while (i < text.size() &&
               std::isdigit(static_cast<unsigned char>(text[i])) != 0) {
            ++i;
            exp_digits = true;
        }
        if (!exp_digits) {
            i = save;  // 'e' starts the unit tail, not an exponent
        }
    }
    const std::string rest = to_lower(text.substr(i));
    // Longest-match first (§8.2): MEG and MIL before M.
    if (rest.rfind("meg", 0) == 0) {
        return std::string("meg");
    }
    if (rest.rfind("mil", 0) == 0) {
        return std::string("mil");
    }
    if (!rest.empty()) {
        const char c = rest.front();
        if (std::string("tgkmunpf").find(c) != std::string::npos) {
            return std::string(1, c);
        }
    }
    return std::nullopt;
}

void push_warning(std::vector<Warning>& warnings, SourceLoc loc, std::string code,
                  std::string message, std::string hint) {
    Warning w;
    w.diag.kind = ErrorKind::Parse;
    w.diag.code = std::move(code);
    w.diag.message = std::move(message);
    w.diag.hint = std::move(hint);
    w.diag.loc = std::move(loc);
    warnings.push_back(std::move(w));
}

}  // namespace

void lint_number_traps(const DeckAst& ast, std::vector<Warning>& warnings) {
    for (const DeviceCard& card : ast.devices) {
        if (card.rest.size() < 3) {
            continue;  // arity errors belong to elaboration
        }
        const char letter = static_cast<char>(std::toupper(
            static_cast<unsigned char>(card.name.text.empty()
                                           ? ' '
                                           : card.name.text[0])));
        // The primary value token per device shape (§8.3): two nodes then
        // the value for R/C/L/V/I; four nodes for E/G; control + value
        // for F/H. Anything past it (ic=, Rser=) carries its own value.
        std::size_t value_at = 2;
        if (letter == 'E' || letter == 'G' || letter == 'S') {
            value_at = 4;
        } else if (letter == 'F' || letter == 'H' || letter == 'W') {
            value_at = 3;
        }
        for (std::size_t i = value_at; i < card.rest.size(); ++i) {
            const std::optional<std::string> suffix =
                scale_suffix_of(card.rest[i].text);
            if (!suffix) {
                continue;
            }
            const bool is_value = (i == value_at);
            if (*suffix == "m" && letter == 'R' && is_value) {
                char ohms[32];
                std::snprintf(ohms, sizeof(ohms), "%.6g",
                              std::stod(card.rest[i].text) * 1e-3);
                push_warning(
                    warnings, card.rest[i].loc, "spice.warn.milli_resistor",
                    "resistor " + card.name.text + " value '" + card.rest[i].text +
                        "' carries M = milli (1e-3) - it is " + ohms + " ohms",
                    "mega-ohms are spelled MEG ('" + card.rest[i].text +
                        "' -> 10MEG style); ngspice reads M the same way");
            } else if (*suffix == "f" && is_value) {
                if (letter == 'C') {
                    push_warning(warnings, card.rest[i].loc, "spice.warn.femto_cap",
                                "capacitor " + card.name.text + " value '" +
                                    card.rest[i].text +
                                    "' carries F = femto (1e-15), not farads",
                                "farads are the derived unit - write 100u for"
                                " 100 microfarads");
                } else {
                    push_warning(
                        warnings, card.rest[i].loc, "spice.warn.femto_value",
                        "device " + card.name.text + " value '" + card.rest[i].text +
                            "' carries F = femto (1e-15) - farads are not a value"
                            " unit outside capacitors",
                        "check the intended scale (1e-15 is rarely it)");
                }
            }
        }
    }
}

ws::core::Status lint_deck(const DeckAst& ast, std::vector<Warning>& warnings) {
    lint_number_traps(ast, warnings);
    // The full elaboration surfaces the §6.6 topology findings and the
    // unused-model warnings; its circuit is discarded - nothing simulates.
    ws::core::Result<Circuit> circuit = elaborate_deck(ast, kDefaultTemp, warnings);
    if (!circuit) {
        return ws::core::unexpected(circuit.error());
    }
    return {};
}

}  // namespace ws::spice
