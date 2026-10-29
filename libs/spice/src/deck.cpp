#include "cards.hpp"

#include <cctype>
#include <optional>
#include <utility>

namespace ws::spice::cards {

namespace {

Diagnostic card_diag(ErrorKind kind, std::string code, std::string message, SourceLoc loc) {
    Diagnostic d;
    d.kind = kind;
    d.code = std::move(code);
    d.message = std::move(message);
    d.loc = std::move(loc);
    return d;
}

// Milestone by device letter (§11): the not-yet story per kind.
const std::map<char, const char*>& device_milestones() {
    static const std::map<char, const char*> table = {
        {'D', "M3"}, {'B', "M4"}, {'Q', "M4"}, {'M', "M4"}, {'T', "M4"},
    };
    return table;
}

const std::map<std::string, const char*>& card_milestones() {
    static const std::map<std::string, const char*> table = {
        {".noise", "M5"},   {".step", "M5"},   {".meas", "M5"},
        {".save", "M5"},    {".print", "M5"},  {".probe", "M5"},
        {".nodeset", "M5"},
        {".lib", "backlog"},  {".global", "backlog"}, {".func", "backlog"},
        {".alter", "backlog"}, {".tf", "backlog"},    {".pz", "backlog"},
        {".four", "backlog"},  {".disto", "backlog"}, {".sens", "backlog"},
        {".pss", "backlog"},
    };
    return table;
}

// .options keys this build understands; everything else warns (§9.3).
bool known_option_key(const std::string& lower) {
    return lower == "reltol" || lower == "abstol" || lower == "vntol" ||
           lower == "gmin" || lower == "itl1" || lower == "itl2" || lower == "itl4" ||
           lower == "chgtol" || lower == "trtol" || lower == "temp" ||
           lower == "tnom" || lower == "solver" || lower == "gshunt";
}

// "name=expr" -> (name token, expression text). The expression side may be
// anything up to whitespace ('{...}' braces guard embedded spaces).
struct Assignment {
    Token name;
    std::string value;
};

[[nodiscard]] ws::core::Result<Assignment> split_assignment(const Token& token) {
    const std::size_t eq = token.text.find('=');
    if (eq == std::string::npos || eq == 0) {
        return spice_fail(card_diag(
            ErrorKind::Parse, "spice.parse.assignment",
            "expected name=value, got '" + token.text + "'", token.loc));
    }
    Assignment out;
    out.name.text = token.text.substr(0, eq);
    out.name.loc = token.loc;
    out.value = token.text.substr(eq + 1);
    // The LTspice form (§8.3): '{expr}' around the whole value. Both
    // spellings mean the same expression; unwrap so the parser never
    // sees the braces.
    if (out.value.size() >= 2 && out.value.front() == '{' && out.value.back() == '}') {
        out.value = out.value.substr(1, out.value.size() - 2);
    }
    return out;
}

[[nodiscard]] ws::core::Result<ExprProgram> expr_from_text(const std::string& text,
                                                           const SourceLoc& loc) {
    const ws::core::Result<ExprProgram> p = parse_expression(text);
    if (!p) {
        return spice_fail(card_diag(ErrorKind::Parse, "spice.parse.expr",
                                    p.error().message, loc));
    }
    return p;
}

}  // namespace

const char* unsupported_device_milestone(char letter) {
    const auto& table = device_milestones();
    const auto it = table.find(letter);
    return (it == table.end()) ? "backlog" : it->second;
}

ws::core::Result<ParsedCard> parse_card(const Record& rec, bool inside_subckt,
                                        std::vector<Warning>& warnings) {
    if (rec.tokens.empty()) {
        return spice_fail(card_diag(ErrorKind::Parse, "spice.parse.empty",
                                    "empty record", rec.loc));
    }
    ParsedCard card;
    card.loc = rec.loc;
    const Token& first = rec.tokens.front();
    const std::string first_lower = to_lower(first.text);

    if (!first_lower.empty() && first_lower[0] == '.') {
        // --- control cards ------------------------------------------------
        if (first_lower == ".op") {
            if (inside_subckt) {
                return spice_fail(card_diag(ErrorKind::Parse, "spice.parse.card_scope",
                                            "analysis card '.op' inside .subckt", rec.loc));
            }
            card.kind = ParsedCard::Kind::Op;
            card.op = OpCard{rec.loc};
            return card;
        }
        if (first_lower == ".tran") {
            if (inside_subckt) {
                return spice_fail(card_diag(ErrorKind::Parse, "spice.parse.card_scope",
                                            "analysis card '.tran' inside .subckt", rec.loc));
            }
            // .tran tstep tstop [tstart [tmax]] [UIC] - UIC may appear
            // anywhere after tstop (ngspice).
            TranCard tran;
            tran.loc = rec.loc;
            std::vector<Token> nums;
            for (std::size_t i = 1; i < rec.tokens.size(); ++i) {
                if (to_lower(rec.tokens[i].text) == "uic") {
                    tran.uic = true;
                    continue;
                }
                nums.push_back(rec.tokens[i]);
            }
            if (nums.size() < 2 || nums.size() > 4) {
                return spice_fail(card_diag(
                    ErrorKind::Parse, "spice.parse.tran_arity",
                    "'.tran' takes TSTEP TSTOP [TSTART [TMAX]] [UIC]", rec.loc));
            }
            WS_TRY_ASSIGN(tran.tstep, expr_from_text(nums[0].text, nums[0].loc));
            WS_TRY_ASSIGN(tran.tstop, expr_from_text(nums[1].text, nums[1].loc));
            if (nums.size() >= 3) {
                WS_TRY_ASSIGN(tran.tstart, expr_from_text(nums[2].text, nums[2].loc));
                tran.has_tstart = true;
            }
            if (nums.size() == 4) {
                WS_TRY_ASSIGN(tran.tmax, expr_from_text(nums[3].text, nums[3].loc));
                tran.has_tmax = true;
            }
            card.kind = ParsedCard::Kind::Tran;
            card.tran = std::move(tran);
            return card;
        }
        if (first_lower == ".ac") {
            if (inside_subckt) {
                return spice_fail(card_diag(ErrorKind::Parse, "spice.parse.card_scope",
                                            "analysis card '.ac' inside .subckt", rec.loc));
            }
            if (rec.tokens.size() != 5) {
                return spice_fail(card_diag(
                    ErrorKind::Parse, "spice.parse.ac_arity",
                    "'.ac' takes DEC|OCT|LIN N FSTART FSTOP", rec.loc));
            }
            AcCard ac;
            ac.loc = rec.loc;
            const std::string mode = to_lower(rec.tokens[1].text);
            if (mode == "dec") {
                ac.sweep = AcCard::Sweep::Dec;
            } else if (mode == "oct") {
                ac.sweep = AcCard::Sweep::Oct;
            } else if (mode == "lin") {
                ac.sweep = AcCard::Sweep::Lin;
            } else {
                return spice_fail(card_diag(
                    ErrorKind::Parse, "spice.parse.ac_mode",
                    "'.ac' sweep mode '" + rec.tokens[1].text +
                        "' is not one of dec, oct, lin",
                    rec.loc));
            }
            WS_TRY_ASSIGN(ac.n, expr_from_text(rec.tokens[2].text, rec.tokens[2].loc));
            WS_TRY_ASSIGN(ac.fstart, expr_from_text(rec.tokens[3].text, rec.tokens[3].loc));
            WS_TRY_ASSIGN(ac.fstop, expr_from_text(rec.tokens[4].text, rec.tokens[4].loc));
            card.kind = ParsedCard::Kind::Ac;
            card.ac = std::move(ac);
            return card;
        }
        if (first_lower == ".ic") {
            if (inside_subckt) {
                return spice_fail(card_diag(ErrorKind::Parse, "spice.parse.card_scope",
                                            "'.ic' inside .subckt", rec.loc));
            }
            if (rec.tokens.size() < 2) {
                return spice_fail(card_diag(ErrorKind::Parse, "spice.parse.ic_arity",
                                            "'.ic' needs v(node)=value ...", rec.loc));
            }
            IcCard ic;
            ic.loc = rec.loc;
            for (std::size_t i = 1; i < rec.tokens.size(); ++i) {
                const std::string& t = rec.tokens[i].text;
                const std::string lower = to_lower(t);
                // v(node)=value: the node name is everything between the
                // leading "v(" and the FIRST ')' - node names are single
                // tokens, so no nested parens to worry about.
                const std::size_t eq = t.find('=');
                bool ok = lower.rfind("v(", 0) == 0 && eq != std::string::npos;
                std::size_t close = std::string::npos;
                if (ok) {
                    close = t.find(')');
                    ok = close != std::string::npos && close > 2 && close < eq &&
                         eq + 1 < t.size();
                }
                if (!ok) {
                    return spice_fail(card_diag(
                        ErrorKind::Parse, "spice.parse.ic_form",
                        "'.ic' entries look like v(node)=value, got '" + t + "'",
                        rec.tokens[i].loc));
                }
                Token node;
                node.text = t.substr(2, close - 2);
                node.loc = rec.tokens[i].loc;
                WS_TRY_ASSIGN(ExprProgram expr,
                              expr_from_text(t.substr(eq + 1), rec.tokens[i].loc));
                ic.node_values.emplace_back(std::move(node), std::move(expr));
            }
            card.kind = ParsedCard::Kind::Ic;
            card.ic = std::move(ic);
            return card;
        }
        if (first_lower == ".dc") {
            if (inside_subckt) {
                return spice_fail(card_diag(ErrorKind::Parse, "spice.parse.card_scope",
                                            "analysis card '.dc' inside .subckt", rec.loc));
            }
            // .dc src start stop step [src2 start2 stop2 step2]
            if (rec.tokens.size() != 5 && rec.tokens.size() != 9) {
                return spice_fail(card_diag(
                    ErrorKind::Parse, "spice.parse.dc_arity",
                    "'.dc' takes SRC START STOP STEP [SRC2 START2 STOP2 STEP2]",
                    rec.loc));
            }
            DcCard dc;
            dc.loc = rec.loc;
            auto parse_sweep = [&](std::size_t base, DcSweep& out) -> ws::core::Status {
                out.source = rec.tokens[base];
                WS_TRY_ASSIGN(out.start, expr_from_text(rec.tokens[base + 1].text,
                                                        rec.tokens[base + 1].loc));
                WS_TRY_ASSIGN(out.stop, expr_from_text(rec.tokens[base + 2].text,
                                                       rec.tokens[base + 2].loc));
                WS_TRY_ASSIGN(out.step, expr_from_text(rec.tokens[base + 3].text,
                                                       rec.tokens[base + 3].loc));
                return {};
            };
            WS_TRY(parse_sweep(1, dc.first));
            if (rec.tokens.size() == 9) {
                DcSweep second;
                WS_TRY(parse_sweep(5, second));
                dc.second = std::move(second);
            }
            card.kind = ParsedCard::Kind::Dc;
            card.dc = std::move(dc);
            return card;
        }
        if (first_lower == ".param") {
            if (rec.tokens.size() < 2) {
                return spice_fail(card_diag(ErrorKind::Parse, "spice.parse.param_arity",
                                            "'.param' needs name=value", rec.loc));
            }
            card.kind = ParsedCard::Kind::Param;
            for (std::size_t i = 1; i < rec.tokens.size(); ++i) {
                WS_TRY_ASSIGN(Assignment a, split_assignment(rec.tokens[i]));
                WS_TRY_ASSIGN(ExprProgram expr, expr_from_text(a.value, a.name.loc));
                ParamDef def;
                def.name = std::move(a.name);
                def.expr = std::move(expr);
                card.params.push_back(std::move(def));
            }
            return card;
        }
        if (first_lower == ".model") {
            if (rec.tokens.size() < 3) {
                return spice_fail(card_diag(ErrorKind::Parse, "spice.parse.model_arity",
                                            "'.model' needs NAME TYPE (params...)", rec.loc));
            }
            // The type token may glue the opening paren - "SW(Ron=1" -
            // or carry a lone "(" in the next token; normalize to TYPE +
            // a flat param list whose entries may still carry parens
            // ("Vt=2)") - apply_model strips them.
            std::string type = rec.tokens[2].text;
            std::string glued_param;
            if (const std::size_t paren = type.find('('); paren != std::string::npos) {
                glued_param = type.substr(paren + 1);
                type = type.substr(0, paren);
            }
            const std::string type_lower = to_lower(type);
            if (type_lower != "r" && type_lower != "sw") {
                return spice_fail(card_diag(
                    ErrorKind::Parse, "spice.parse.model_type",
                    "model type '" + type +
                        "' is not supported in v1 (resistor models M5, switch models M2;"
                        " diode M3, BJT/MOS M4)",
                    rec.loc));
            }
            std::vector<Token> params;
            if (!glued_param.empty() && glued_param != ")") {
                Token t;
                t.text = glued_param;
                t.loc = rec.tokens[2].loc;
                params.push_back(std::move(t));
            }
            for (std::size_t i = 3; i < rec.tokens.size(); ++i) {
                params.push_back(rec.tokens[i]);
            }
            card.kind = ParsedCard::Kind::Model;
            card.model = ModelDef{rec.tokens[1], type_lower, std::move(params),
                                  rec.loc};
            return card;
        }
        if (first_lower == ".options") {
            // KEY VALUE pairs and ngspice's KEY=VALUE single tokens.
            card.kind = ParsedCard::Kind::Options;
            OptionsCard opts;
            opts.loc = rec.loc;
            bool pair_ok = true;
            for (std::size_t i = 1; i < rec.tokens.size(); ++i) {
                Token key = rec.tokens[i];
                Token value;
                const std::size_t eq = key.text.find('=');
                if (eq != std::string::npos && eq > 0 && eq + 1 < key.text.size()) {
                    value.text = key.text.substr(eq + 1);
                    value.loc = key.loc;
                    key.text = key.text.substr(0, eq);
                } else if (eq != std::string::npos) {
                    return spice_fail(card_diag(
                        ErrorKind::Parse, "spice.parse.options_arity",
                        "'.options' key '" + key.text + "' needs a value", rec.loc));
                } else if (i + 1 < rec.tokens.size()) {
                    value = rec.tokens[++i];
                } else {
                    pair_ok = false;
                    value = rec.tokens.back();
                }
                if (!pair_ok) {
                    return spice_fail(card_diag(
                        ErrorKind::Parse, "spice.parse.options_arity",
                        "'.options' takes KEY VALUE pairs (dangling key '" +
                            rec.tokens.back().text + "')",
                        rec.loc));
                }
                const std::string key_lower = to_lower(key.text);
                if (!known_option_key(key_lower)) {
                    Warning w;
                    w.diag = card_diag(ErrorKind::Parse, "spice.warn.options_ignored",
                                       "ignored .options key '" + key.text + "'",
                                       key.loc);
                    warnings.push_back(std::move(w));
                    continue;
                }
                opts.kv.emplace_back(key, value);
            }
            card.options = std::move(opts);
            return card;
        }
        if (first_lower == ".subckt") {
            if (inside_subckt) {
                return spice_fail(card_diag(
                    ErrorKind::Parse, "spice.parse.card_scope",
                    "nested .subckt definitions are not supported (flattening, 8.4)",
                    rec.loc));
            }
            if (rec.tokens.size() < 2) {
                return spice_fail(card_diag(ErrorKind::Parse, "spice.parse.subckt_arity",
                                            "'.subckt' needs a name", rec.loc));
            }
            card.kind = ParsedCard::Kind::Subckt;
            return card;
        }
        if (first_lower == ".ends") {
            card.kind = ParsedCard::Kind::Ends;
            return card;
        }
        const auto& milestones = card_milestones();
        if (const auto it = milestones.find(first_lower); it != milestones.end()) {
            return spice_fail(card_diag(
                ErrorKind::Parse, "spice.parse.unsupported_card",
                "card '" + first_lower + "' is not supported yet (" + it->second + ")",
                rec.loc));
        }
        return spice_fail(card_diag(ErrorKind::Parse, "spice.parse.unknown_card",
                                    "unknown card '" + first.text + "'", rec.loc));
    }

    // --- device instance cards ----------------------------------------------
    const std::string& name_text = first.text;
    if (name_text.size() < 2) {
        return spice_fail(card_diag(ErrorKind::Parse, "spice.parse.device_name",
                                    "'" + name_text + "' is not a device name", rec.loc));
    }
    const char letter =
        static_cast<char>(std::toupper(static_cast<unsigned char>(name_text[0])));
    const std::string supported = "RVIEFGHXCLKSW";
    if (supported.find(letter) == std::string::npos) {
        if (device_milestones().count(letter) != 0) {
            return spice_fail(card_diag(
                ErrorKind::Parse, "spice.parse.unsupported_device",
                std::string("device '") + letter + "' is not supported yet (" +
                    unsupported_device_milestone(letter) + ")",
                rec.loc));
        }
        return spice_fail(card_diag(ErrorKind::Parse, "spice.parse.unknown_device",
                                    "unknown device letter '" + name_text.substr(0, 1) + "'",
                                    rec.loc));
    }

    card.kind = ParsedCard::Kind::Device;
    card.letter = letter;
    card.device =
        DeviceCard{first, std::vector<Token>(rec.tokens.begin() + 1, rec.tokens.end()),
                   rec.loc};
    return card;
}

}  // namespace ws::spice::cards

namespace ws::spice {

namespace {

Diagnostic ast_diag(ErrorKind kind, std::string code, std::string message, SourceLoc loc) {
    Diagnostic d;
    d.kind = kind;
    d.code = std::move(code);
    d.message = std::move(message);
    d.loc = std::move(loc);
    return d;
}

// The classic SPICE footgun: a deck whose first line IS a device card
// ("V1 in 0 5" as line 1) silently loses that device to the title (§8.1:
// line 1 is always the title, exactly like ngspice). Warn when the title
// looks like a card, so the mystery zero output gets a name. Heuristic,
// deliberately narrow to keep false positives off ordinary English titles:
//   - a leading '.' (control card: ".op foo" as a title is always wrong)
//   - a device-letter name whose remainder contains a digit ("V1", "R12")
// Pure-alpha first tokens ("Running the show", "Voltage divider") never
// warn; a letter-named device without digits ("Vsrc") slips through.
void warn_when_title_looks_like_card(const Deck& deck, std::vector<Warning>& warnings) {
    if (deck.title.empty()) {
        return;
    }
    const std::size_t space = deck.title.find_first_of(" \t");
    const std::string first = deck.title.substr(0, space);
    bool suspicious = false;
    if (first.front() == '.') {
        suspicious = true;
    } else if (first.size() >= 2 && std::isalpha(static_cast<unsigned char>(first.front()))) {
        for (const char c : first.substr(1)) {
            if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
                suspicious = true;
                break;
            }
        }
    }
    if (!suspicious) {
        return;
    }
    Warning w;
    w.diag = ast_diag(
        ErrorKind::Parse, "spice.warn.title_looks_like_card",
        "the title line looks like a card ('" + first + "') but line 1 of a deck"
        " is always the title and is ignored",
        deck.title_loc);
    w.diag.hint = "add a title line (even blank text on line 1) before the first"
                  " card if '" + first + "' was meant as a device or control";
    warnings.push_back(std::move(w));
}

}  // namespace

ws::core::Result<DeckAst> build_deck_ast(const Deck& deck, std::vector<Warning>& warnings) {
    DeckAst ast;
    ast.title = deck.title;
    warn_when_title_looks_like_card(deck, warnings);

    std::vector<SubcktDef> open_subckts;  // 0 or 1 deep (nesting rejected)

    for (const Record& rec : deck.records) {
        const bool inside = !open_subckts.empty();
        ws::core::Result<cards::ParsedCard> parsed =
            cards::parse_card(rec, inside, warnings);
        if (!parsed) {
            return ws::core::unexpected(std::move(parsed.error()));
        }
        const cards::ParsedCard& card = *parsed;

        switch (card.kind) {
            using K = cards::ParsedCard::Kind;
            case K::Device:
                if (inside) {
                    open_subckts.back().body.push_back(rec);
                } else {
                    ast.devices.push_back(card.device);
                }
                break;
            case K::Param:
                if (inside) {
                    for (const ParamDef& def : card.params) {
                        open_subckts.back().defaults.push_back(def);
                    }
                } else {
                    for (const ParamDef& def : card.params) {
                        ast.params.push_back(def);
                    }
                }
                break;
            case K::Model:
                if (inside) {
                    // .model inside a subckt is local to it (8.4) - kept as
                    // a body record; elaboration picks it up per instance.
                    open_subckts.back().body.push_back(rec);
                } else {
                    ast.models.push_back(card.model);
                }
                break;
            case K::Options:
                if (inside) {
                    return spice_fail(ast_diag(ErrorKind::Parse, "spice.parse.card_scope",
                                                "'.options' inside .subckt", rec.loc));
                }
                ast.options.push_back(card.options);
                break;
            case K::Op: {
                AnalysisCard a;
                a.loc = card.loc;
                a.op = card.op;
                ast.analyses.push_back(std::move(a));
                break;
            }
            case K::Dc: {
                AnalysisCard a;
                a.loc = card.loc;
                a.dc = card.dc;
                ast.analyses.push_back(std::move(a));
                break;
            }
            case K::Tran: {
                AnalysisCard a;
                a.loc = card.loc;
                a.tran = card.tran;
                ast.analyses.push_back(std::move(a));
                break;
            }
            case K::Ac: {
                AnalysisCard a;
                a.loc = card.loc;
                a.ac = card.ac;
                ast.analyses.push_back(std::move(a));
                break;
            }
            case K::Ic: {
                if (inside) {
                    return spice_fail(ast_diag(
                        ErrorKind::Parse, "spice.parse.card_scope",
                        "'.ic' inside .subckt", rec.loc));
                }
                ast.ics.push_back(card.ic);
                break;
            }
            case K::Subckt: {
                SubcktDef def;
                def.loc = rec.loc;
                def.name = rec.tokens[1];
                // Ports until 'params:' or the first bare name=value (the
                // ngspice form); the rest are subckt parameter defaults.
                bool in_params = false;
                for (std::size_t i = 2; i < rec.tokens.size(); ++i) {
                    const std::string t = to_lower(rec.tokens[i].text);
                    if (!in_params && (t == "params:" ||
                                       rec.tokens[i].text.find('=') != std::string::npos)) {
                        in_params = true;
                    }
                    if (!in_params) {
                        def.ports.push_back(rec.tokens[i]);
                        continue;
                    }
                    if (t == "params:") {
                        continue;  // the keyword itself
                    }
                    const std::size_t eq = rec.tokens[i].text.find('=');
                    if (eq == std::string::npos || eq == 0) {
                        return spice_fail(ast_diag(
                            ErrorKind::Parse, "spice.parse.subckt_param",
                            "expected name=value in 'params:' section, got '" +
                                rec.tokens[i].text + "'",
                            rec.tokens[i].loc));
                    }
                    ParamDef pd;
                    pd.name.text = rec.tokens[i].text.substr(0, eq);
                    pd.name.loc = rec.tokens[i].loc;
                    WS_TRY_ASSIGN(pd.expr,
                                  ([&]() -> ws::core::Result<ExprProgram> {
                                      const ws::core::Result<ExprProgram> e =
                                          parse_expression(rec.tokens[i].text.substr(eq + 1));
                                      if (!e) {
                                          return spice_fail(ast_diag(
                                              ErrorKind::Parse, "spice.parse.expr",
                                              e.error().message, rec.tokens[i].loc));
                                      }
                                      return e;
                                  }()));
                    def.defaults.push_back(std::move(pd));
                }
                open_subckts.push_back(std::move(def));
                break;
            }
            case K::Ends: {
                if (open_subckts.empty()) {
                    return spice_fail(ast_diag(ErrorKind::Parse, "spice.parse.ends",
                                                "'.ends' with no open .subckt", rec.loc));
                }
                SubcktDef def = std::move(open_subckts.back());
                open_subckts.pop_back();
                if (rec.tokens.size() >= 2) {
                    if (to_lower(rec.tokens[1].text) != to_lower(def.name.text)) {
                        return spice_fail(ast_diag(
                            ErrorKind::Parse, "spice.parse.ends_mismatch",
                            "'.ends " + rec.tokens[1].text +
                                "' does not match .subckt " + def.name.text,
                            rec.loc));
                    }
                }
                ast.subckts.emplace(to_lower(def.name.text), std::move(def));
                break;
            }
        }
    }

    if (!open_subckts.empty()) {
        return spice_fail(ast_diag(
            ErrorKind::Parse, "spice.parse.subckt_unterminated",
            "unterminated .subckt '" + open_subckts.back().name.text + "'",
            open_subckts.back().loc));
    }
    return ast;
}

// --- ParamTable ---------------------------------------------------------------

void ParamTable::define(ParamDef def) {
    const std::string key = to_lower(def.name.text);
    defs_[key] = std::move(def);
    cache_.erase(key);  // a replaced definition invalidates its value
}

ParamTable ParamTable::with_overrides(const std::vector<ParamDef>& overrides) const {
    ParamTable child(this);
    for (const ParamDef& def : overrides) {
        child.define(def);
    }
    return child;
}

bool ParamTable::knows(const std::string& lower) const {
    if (defs_.count(lower) != 0) {
        return true;
    }
    return parent_ != nullptr && parent_->knows(lower);
}

bool ParamTable::knows_local(const std::string& lower) const {
    return defs_.count(lower) != 0;
}

void ParamTable::clear_cache() const {
    cache_.clear();
    if (parent_ != nullptr) {
        parent_->clear_cache();
    }
}

ws::core::Result<double> ParamTable::value(const std::string& lower) const {
    std::vector<std::string> active;
    return resolve(lower, active);
}

ws::core::Result<double> ParamTable::resolve(const std::string& lower,
                                             std::vector<std::string>& active) const {
    for (const std::string& a : active) {
        if (a == lower) {
            std::string chain;
            for (const std::string& link : active) {
                chain += (chain.empty() ? "" : " -> ") + link;
            }
            Diagnostic d;
            d.kind = ErrorKind::Semantic;
            d.code = "spice.param.cycle";
            d.message = "parameter cycle: " + chain + " -> " + lower;
            d.names.push_back(lower);
            return spice_fail(std::move(d));
        }
    }
    const auto it = defs_.find(lower);
    if (it != defs_.end()) {
        if (const auto hit = cache_.find(lower); hit != cache_.end()) {
            return hit->second;
        }
        active.push_back(lower);
        const ws::core::Result<std::vector<std::optional<double>>> bindings =
            bind_active(it->second.expr, active);
        if (!bindings) {
            active.pop_back();
            return ws::core::unexpected(bindings.error());
        }
        const ws::core::Result<double> v = it->second.expr.evaluate(*bindings);
        active.pop_back();
        if (!v) {
            Diagnostic d;
            d.kind = ErrorKind::Semantic;
            d.code = "spice.param.eval";
            d.message = "parameter '" + lower + "': " + v.error().message;
            return spice_fail(std::move(d));
        }
        cache_.emplace(lower, *v);
        return v;
    }
    if (parent_ != nullptr) {
        return parent_->resolve(lower, active);
    }
    Diagnostic d;
    d.kind = ErrorKind::Semantic;
    d.code = "spice.param.unknown";
    d.message = "unknown parameter '" + lower + "'";
    return spice_fail(std::move(d));
}

ws::core::Result<std::vector<std::optional<double>>> ParamTable::bind(
    const ExprProgram& prog) const {
    std::vector<std::string> active;
    return bind_active(prog, active);
}

ws::core::Result<std::vector<std::optional<double>>> ParamTable::bind_active(
    const ExprProgram& prog, std::vector<std::string>& active) const {
    std::vector<std::optional<double>> out(prog.vars().size());
    for (std::size_t i = 0; i < prog.vars().size(); ++i) {
        if (knows(prog.vars()[i])) {
            WS_TRY_ASSIGN(out[i], resolve(prog.vars()[i], active));
        }
    }
    return out;
}

}  // namespace ws::spice
