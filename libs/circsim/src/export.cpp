#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ws/spice/deck.hpp>
#include <ws/circsim/export.hpp>

namespace ws::circsim {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return out;
}

std::string fmt_num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.12g", v);
    return buf;
}

bool is_identifier(std::string_view s) {
    if (s.empty()) {
        return false;
    }
    for (const char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

// A SPICE-safe node token: identifier characters only, never empty, never
// leading-digit ("+5V" -> "n5V", "in out" -> "inout"). Deterministic, so
// equal names keep mapping to equal tokens.
std::string node_token(std::string_view name) {
    std::string t;
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_';
        if (ok) {
            t += c;
        }
    }
    if (t.empty() || (t[0] >= '0' && t[0] <= '9')) {
        t = "n" + t;
    }
    return t;
}

// The element name for one exported part: the ref when it is a valid
// identifier with the symbol's letter; else a deterministic mangling
// (ERC has already flagged the ref).
std::string element_name(const Part& p, const SymbolDef& sym) {
    if (!p.ref.empty() && is_identifier(p.ref) &&
        static_cast<char>(std::toupper(static_cast<unsigned char>(p.ref[0]))) ==
            sym.spice_letter) {
        return p.ref;
    }
    std::string base(1, sym.spice_letter);
    if (!p.ref.empty()) {
        std::string kept;
        for (const char c : p.ref) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_') {
                kept += c;
            }
        }
        base += "_" + (kept.empty() ? std::to_string(p.id) : kept);
    } else {
        base += std::to_string(p.id);
    }
    return base;
}

Diag mapped_diag(std::string code, std::string message, std::string where) {
    Diag d;
    d.severity = Severity::Error;
    d.code = std::move(code);
    d.message = std::move(message);
    d.where = std::move(where);
    return d;
}

}  // namespace

bool CiLess::operator()(const std::string& a, const std::string& b) const {
    return lower(a) < lower(b);
}

// --- analysis cards -------------------------------------------------------------

std::string analysis_card(const AnalysisSpec& a) {
    if (const OpSpec* op = std::get_if<OpSpec>(&a)) {
        (void)op;
        return ".op";
    }
    if (const DcSpec* dc = std::get_if<DcSpec>(&a)) {
        return ".dc " + dc->source + " " + fmt_num(dc->start) + " " +
               fmt_num(dc->stop) + " " + fmt_num(dc->step);
    }
    if (const TranSpec* tran = std::get_if<TranSpec>(&a)) {
        const double tstep = tran->tstep > 0.0 ? tran->tstep
                                               : tran->tstop / 100.0;
        std::string card = ".tran " + fmt_num(tstep) + " " +
                           fmt_num(tran->tstop);
        if (tran->tstart.has_value()) {
            card += " " + fmt_num(*tran->tstart);
            if (tran->tmax.has_value()) {
                card += " " + fmt_num(*tran->tmax);
            }
        } else if (tran->tmax.has_value()) {
            card += " 0 " + fmt_num(*tran->tmax);
        }
        if (tran->uic) {
            card += " UIC";
        }
        return card;
    }
    if (const AcSpec* ac = std::get_if<AcSpec>(&a)) {
        const char* sweep = "dec";
        if (ac->sweep == AcSpec::Sweep::Oct) {
            sweep = "oct";
        } else if (ac->sweep == AcSpec::Sweep::Lin) {
            sweep = "lin";
        }
        return std::string(".ac ") + sweep + " " + fmt_num(ac->n) + " " +
               fmt_num(ac->fstart) + " " + fmt_num(ac->fstop);
    }
    const NoiseSpec* ns = std::get_if<NoiseSpec>(&a);
    const char* sweep = "dec";
    if (ns->sweep == NoiseSpec::Sweep::Oct) {
        sweep = "oct";
    } else if (ns->sweep == NoiseSpec::Sweep::Lin) {
        sweep = "lin";
    }
    std::string card = ".noise v(" + ns->output;
    if (!ns->ref.empty()) {
        card += "," + ns->ref;
    }
    card += ") " + ns->input + " " + sweep + " " + fmt_num(ns->n) + " " +
           fmt_num(ns->fstart) + " " + fmt_num(ns->fstop);
    if (ns->pts_per_summary > 0) {
        card += " " + std::to_string(ns->pts_per_summary);
    }
    return card;
}

ws::core::Result<AnalysisSpec> parse_analysis_spec(std::string_view text) {
    std::vector<std::string> tokens;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() &&
               std::isspace(static_cast<unsigned char>(text[i]))) {
            ++i;
        }
        std::size_t j = i;
        while (j < text.size() &&
               !std::isspace(static_cast<unsigned char>(text[j]))) {
            ++j;
        }
        if (j > i) {
            tokens.emplace_back(text.substr(i, j - i));
        }
        i = j;
    }
    if (tokens.empty()) {
        return ws::core::unexpected<ws::core::Error>(ws::core::make_error(
            2, "circsim", "--analysis needs a spec: 'op' | 'dc V1 0 5 0.1' | "
                              "'tran 1u 1m [tstart [tmax]] [uic]' | "
                              "'ac dec 10 1 100k' | "
                              "'noise v(out) V1 dec 10 1 100k'"));
    }
    const std::string kind = lower(tokens[0]);
    const auto need = [&](std::size_t count) {
        return tokens.size() == count;
    };
    const auto number = [&](std::size_t idx) {
        return ws::spice::parse_number(tokens[idx]);
    };

    if (kind == "op") {
        if (!need(1)) {
            return ws::core::unexpected<ws::core::Error>(
                ws::core::make_error(2, "circsim",
                                     "'op' takes no arguments"));
        }
        return AnalysisSpec{OpSpec{}};
    }
    if (kind == "dc") {
        if (tokens.size() != 5) {
            return ws::core::unexpected<ws::core::Error>(
                ws::core::make_error(2, "circsim",
                                     "'dc' takes: dc <source> <start> <stop> "
                                             "<step>"));
        }
        DcSpec dc;
        dc.source = tokens[1];
        for (std::size_t k = 0; k < 3; ++k) {
            const auto n = number(2 + k);
            if (!n) {
                return ws::core::unexpected<ws::core::Error>(ws::core::make_error(
                    2, "circsim", "bad number '" + tokens[2 + k] +
                                      "' in --analysis dc"));
            }
            (k == 0 ? dc.start : k == 1 ? dc.stop : dc.step) = *n;
        }
        return AnalysisSpec{dc};
    }
    if (kind == "tran") {
        if (tokens.size() < 3 || tokens.size() > 6) {
            return ws::core::unexpected<ws::core::Error>(
                ws::core::make_error(2, "circsim",
                                     "'tran' takes: tran <tstep> <tstop> "
                                             "[tstart [tmax]] [uic]"));
        }
        TranSpec t;
        bool uic = false;
        std::vector<double> nums;
        for (std::size_t k = 1; k < tokens.size(); ++k) {
            if (lower(tokens[k]) == "uic") {
                uic = true;
                continue;
            }
            const auto n = number(k);
            if (!n) {
                return ws::core::unexpected<ws::core::Error>(
                    ws::core::make_error(2, "circsim",
                                         "bad number '" + tokens[k] +
                                             "' in --analysis tran"));
            }
            nums.push_back(*n);
        }
        if (nums.size() < 2 || nums.size() > 4) {
            return ws::core::unexpected<ws::core::Error>(
                ws::core::make_error(2, "circsim",
                                     "'tran' takes: tran <tstep> <tstop> "
                                             "[tstart [tmax]] [uic]"));
        }
        t.tstep = nums[0];
        t.tstop = nums[1];
        if (nums.size() >= 3) {
            t.tstart = nums[2];
        }
        if (nums.size() == 4) {
            t.tmax = nums[3];
        }
        t.uic = uic;
        return AnalysisSpec{t};
    }
    if (kind == "ac") {
        if (tokens.size() != 5) {
            return ws::core::unexpected<ws::core::Error>(
                ws::core::make_error(2, "circsim",
                                     "'ac' takes: ac <dec|oct|lin> <n> "
                                             "<fstart> <fstop>"));
        }
        AcSpec ac;
        const std::string sweep = lower(tokens[1]);
        if (sweep == "dec") {
            ac.sweep = AcSpec::Sweep::Dec;
        } else if (sweep == "oct") {
            ac.sweep = AcSpec::Sweep::Oct;
        } else if (sweep == "lin") {
            ac.sweep = AcSpec::Sweep::Lin;
        } else {
            return ws::core::unexpected<ws::core::Error>(
                ws::core::make_error(2, "circsim",
                                     "ac sweep must be dec, oct or lin"));
        }
        const auto n = number(2);
        const auto f1 = number(3);
        const auto f2 = number(4);
        if (!n || !f1 || !f2) {
            return ws::core::unexpected<ws::core::Error>(
                ws::core::make_error(2, "circsim",
                                     "bad number in --analysis ac"));
        }
        ac.n = static_cast<int>(*n);
        ac.fstart = *f1;
        ac.fstop = *f2;
        return AnalysisSpec{ac};
    }
    if (kind == "noise") {
        // 'noise <out> <src> dec|oct|lin> <n> <fstart> <fstop> [pts]',
        // the output spelling either a bare node ("out") or the card's
        // own v(out[,ref]) form.
        if (tokens.size() < 7 || tokens.size() > 8) {
            return ws::core::unexpected<ws::core::Error>(
                ws::core::make_error(2, "circsim",
                                     "'noise' takes: noise <out> <src> "
                                             "<dec|oct|lin> <n> <fstart> "
                                             "<fstop> [pts]"));
        }
        // The output token: strip an optional v(...) wrapper.
        std::string out_text = tokens[1];
        std::string ref;
        if (out_text.size() >= 3 && lower(out_text.substr(0, 2)) == "v(" &&
            out_text.back() == ')') {
            out_text = out_text.substr(2, out_text.size() - 3);
        }
        const std::size_t comma = out_text.find(',');
        if (comma != std::string::npos) {
            ref = out_text.substr(comma + 1);
            out_text = out_text.substr(0, comma);
        }
        NoiseSpec ns;
        ns.output = out_text;
        ns.ref = ref;
        ns.input = tokens[2];
        const std::string sweep = lower(tokens[3]);
        if (sweep == "dec") {
            ns.sweep = NoiseSpec::Sweep::Dec;
        } else if (sweep == "oct") {
            ns.sweep = NoiseSpec::Sweep::Oct;
        } else if (sweep == "lin") {
            ns.sweep = NoiseSpec::Sweep::Lin;
        } else {
            return ws::core::unexpected<ws::core::Error>(
                ws::core::make_error(2, "circsim",
                                     "noise sweep must be dec, oct or lin"));
        }
        const auto n = ws::spice::parse_number(tokens[4]);
        const auto f1 = ws::spice::parse_number(tokens[5]);
        const auto f2 = ws::spice::parse_number(tokens[6]);
        if (!n || !f1 || !f2) {
            return ws::core::unexpected<ws::core::Error>(
                ws::core::make_error(2, "circsim",
                                     "bad number in --analysis noise"));
        }
        ns.n = static_cast<int>(*n);
        ns.fstart = *f1;
        ns.fstop = *f2;
        if (tokens.size() == 8) {
            const auto pts = ws::spice::parse_number(tokens[7]);
            if (!pts || *pts != std::floor(*pts) || *pts < 0.0) {
                return ws::core::unexpected<ws::core::Error>(
                    ws::core::make_error(
                        2, "circsim",
                        "noise summary cadence must be a whole number >= 0"));
            }
            ns.pts_per_summary = static_cast<int>(*pts);
        }
        return AnalysisSpec{ns};
    }
    return ws::core::unexpected<ws::core::Error>(
        ws::core::make_error(2, "circsim",
                             "unknown analysis '" + tokens[0] +
                                 "' (op, dc, tran, ac, noise)"));
}

// --- the deck export ---------------------------------------------------------------

ws::core::Result<SpiceExport> export_deck(
    const Schematic& s, const NetList& nets, const std::vector<AnalysisSpec>* analyses,
    std::vector<Diag>* diags, const SymbolLibrary* symbols) {
    SpiceExport out;
    const SymbolLibrary& lib =
        symbols != nullptr ? *symbols : builtin_library();

    // Node tokens per net (ground is "0"), collision-checked ci.
    std::map<std::string, std::uint32_t, CiLess> token_owner;
    for (const Net& n : nets.nets) {
        const std::string token = n.is_ground ? "0" : node_token(n.name);
        const auto it = token_owner.find(token);
        if (it != token_owner.end() && it->second != n.id) {
            const Net* other = nullptr;
            for (const Net& m : nets.nets) {
                if (m.id == it->second) {
                    other = &m;
                    break;
                }
            }
            return ws::core::unexpected<ws::core::Error>(ws::core::make_error(
                1, "circsim",
                "nets '" + n.name + "' and '" +
                    (other != nullptr ? other->name : "?") +
                    "' both export as node '" + token +
                    "'; rename one of them"));
        }
        token_owner[token] = n.id;
        out.node_net[token] = n.id;
    }
    const auto net_token = [&](const Net& n) {
        return n.is_ground ? "0" : node_token(n.name);
    };

    // Exported parts sorted by ref (ci, then id) for stable decks.
    struct Card {
        const Part* part;
        const SymbolDef* sym;
        std::string name;
    };
    std::vector<Card> cards;
    for (const Part& p : s.parts()) {
        const SymbolDef* sym = lib.find(p.symbol);
        if (sym == nullptr) {
            if (diags != nullptr) {
                diags->push_back(mapped_diag(
                    "circsim.export.unknown_symbol",
                    "part with unknown symbol '" + p.symbol +
                        "' exports no card",
                    p.ref.empty() ? p.symbol + " " + std::to_string(p.id)
                                  : p.ref));
            }
            continue;
        }
        if (sym->spice_letter == 0) {
            continue;  // power/ground ride their net names
        }
        cards.push_back({&p, sym, ""});
    }
    std::sort(cards.begin(), cards.end(), [](const Card& a, const Card& b) {
        const std::string ra = lower(a.part->ref);
        const std::string rb = lower(b.part->ref);
        if (ra != rb) {
            return ra < rb;
        }
        return a.part->id < b.part->id;
    });
    std::map<std::string, std::uint32_t, CiLess> used_names;
    for (Card& c : cards) {
        std::string name = element_name(*c.part, *c.sym);
        if (used_names.find(name) != used_names.end()) {
            std::string base = name;
            int suffix = 2;
            while (used_names.find(name) != used_names.end()) {
                name = base + "_" + std::to_string(suffix);
                ++suffix;
            }
        }
        used_names[name] = c.part->id;
        c.name = std::move(name);
        out.element_part[c.name] = c.part->id;
    }

    // Emission.
    std::string text;
    const auto push_line = [&](const std::string& line,
                               SpiceExport::Origin::Kind kind,
                               std::uint32_t id) {
        text += line;
        text += '\n';
        out.line_origins.push_back({kind, id});
    };

    push_line("circsim export", SpiceExport::Origin::Kind::None, 0);

    // The run temperature (§7.7): an unset temp emits nothing (SPICE's
    // 27°C default stands), and the card rides AFTER the directives
    // block so a user's own `.options temp=` (an escape hatch inside
    // the directives) still wins - deck order, last assignment lands.
    if (s.temp.has_value()) {
        push_line(".options temp=" + fmt_num(*s.temp),
                  SpiceExport::Origin::Kind::Temp, 0);
    }

    if (!s.directives.empty()) {
        std::string body = s.directives;
        if (!body.empty() && body.back() != '\n') {
            body += '\n';
        }
        std::size_t start = 0;
        while (start < body.size()) {
            std::size_t end = body.find('\n', start);
            if (end == std::string::npos) {
                end = body.size();
            }
            push_line(body.substr(start, end - start),
                      SpiceExport::Origin::Kind::Directives, 0);
            start = end + 1;
        }
    }

    for (const Card& c : cards) {
        std::string line = c.name;
        for (std::uint32_t i = 0; i < c.sym->pins.size(); ++i) {
            const Net* n = nets.net_of_pin(PinRef{c.part->id, i});
            if (n == nullptr) {
                line += " NC";  // net building dropped the pin's net; ERC
                                // has already flagged the cause
                continue;
            }
            line += " " + net_token(*n);
        }
        if (c.sym->needs_value && !c.part->value.empty()) {
            line += " " + c.part->value;
        }
        for (const ParamEmit& pe : c.sym->emit) {
            const auto it = c.part->params.find(pe.key);
            if (it == c.part->params.end() || it->second.empty()) {
                continue;
            }
            line += " " + pe.prefix + it->second;
        }
        push_line(line, SpiceExport::Origin::Kind::Part, c.part->id);
    }

    const std::vector<AnalysisSpec>& setup =
        analyses != nullptr ? *analyses : s.analysis.entries;
    for (const AnalysisSpec& a : setup) {
        push_line(analysis_card(a), SpiceExport::Origin::Kind::Analysis, 0);
    }
    push_line(".end", SpiceExport::Origin::Kind::None, 0);

    out.text = std::move(text);
    return out;
}

// --- the run path -------------------------------------------------------------------

namespace {

// "circsim export:12:3: error: ..." -> 12; nullopt when the message carries
// no location prefix (deck-global errors).
std::optional<int> leading_line(std::string_view message,
                                std::string_view deck_name) {
    if (message.substr(0, deck_name.size()) != deck_name) {
        return std::nullopt;
    }
    std::size_t i = deck_name.size();
    if (i >= message.size() || message[i] != ':') {
        return std::nullopt;
    }
    ++i;
    int line = 0;
    std::size_t digits = 0;
    while (i < message.size() && message[i] >= '0' && message[i] <= '9') {
        line = line * 10 + (message[i] - '0');
        ++i;
        ++digits;
    }
    if (digits == 0 || i >= message.size() || message[i] != ':') {
        return std::nullopt;
    }
    return line;
}

// One spice failure -> one circsim diagnostic: deck line first (part /
// directives / analysis), then element/node names in the message.
Diag map_spice_error(const Schematic& s, const SpiceExport& map,
                     const std::string& msg) {
    std::string where;
    std::string detail = msg;
    const auto line = leading_line(msg, map.deck_name);
    if (line.has_value() && *line >= 1 &&
        static_cast<std::size_t>(*line) <= map.line_origins.size()) {
        const SpiceExport::Origin& o =
            map.line_origins[static_cast<std::size_t>(*line) - 1];
        if (o.kind == SpiceExport::Origin::Kind::Part) {
            const Part* p = s.find_part(o.id);
            where = p != nullptr && !p->ref.empty()
                        ? p->ref
                        : "part " + std::to_string(o.id);
        } else if (o.kind == SpiceExport::Origin::Kind::Directives) {
            where = "directives";
        } else if (o.kind == SpiceExport::Origin::Kind::Analysis) {
            where = "analysis setup";
        } else if (o.kind == SpiceExport::Origin::Kind::Temp) {
            where = "simulation setup";
        }
        // Strip the "name:line:col: " prefix for the message body.
        const std::size_t sep = msg.find(": error:");
        if (sep != std::string::npos) {
            detail = msg.substr(sep + 2);  // "error: ..."
        }
    }
    if (where.empty()) {
        for (const auto& [name, part_id] : map.element_part) {
            if (name.size() >= 2 && msg.find(name) != std::string::npos) {
                const Part* p = s.find_part(part_id);
                where = p != nullptr && !p->ref.empty()
                            ? p->ref
                            : "part " + std::to_string(part_id);
                break;
            }
        }
    }
    if (where.empty()) {
        for (const auto& [token, net_id] : map.node_net) {
            if (token.size() >= 2 && token[0] >= 'A' &&
                msg.find(token) != std::string::npos) {
                where = "net " + token;
                break;
            }
        }
    }
    return mapped_diag("circsim.run.failed", detail, where);
}

}  // namespace

RunOutcome run_schematic(const Schematic& s,
                         const std::vector<AnalysisSpec>* override_analyses,
                         const ws::spice::IncludeResolver& include,
                         const SymbolLibrary* symbols) {
    RunOutcome out;

    NetBuild build = build_nets(s, symbols);
    out.problems = build.diags;
    out.nets = build.nets;

    std::vector<AnalysisSpec> effective;
    if (override_analyses != nullptr) {
        effective = *override_analyses;
    } else {
        effective = s.analysis.entries;
    }
    if (effective.empty()) {
        effective.push_back(OpSpec{});  // the friendly default: one .op
    }

    auto deck = export_deck(s, out.nets, &effective, &out.problems, symbols);
    if (!deck) {
        out.run = ws::core::unexpected<ws::core::Error>(deck.error());
        out.problems.push_back(mapped_diag(
            "circsim.export.failed", deck.error().message, ""));
        return out;
    }
    out.map = std::move(*deck);

    ws::spice::ParseOptions parse_opts;
    parse_opts.include_resolver = include;
    const auto read = ws::spice::read_deck(out.map.deck_name, out.map.text,
                                           parse_opts);
    if (!read) {
        out.run = ws::core::unexpected<ws::core::Error>(read.error());
        out.problems.push_back(map_spice_error(s, out.map,
                                                read.error().message));
        return out;
    }
    const auto ast = ws::spice::build_deck_ast(*read, out.warnings);
    if (!ast) {
        out.run = ws::core::unexpected<ws::core::Error>(ast.error());
        out.problems.push_back(map_spice_error(s, out.map,
                                                ast.error().message));
        return out;
    }
    out.run = ws::spice::run_deck(*ast, ws::spice::RunSettings{}, out.warnings);
    if (!out.run) {
        out.problems.push_back(
            map_spice_error(s, out.map, out.run.error().message));
    }
    return out;
}

// --- JSON results -----------------------------------------------------------------

std::string results_json(const ws::spice::RunOutput& run) {
    namespace json = ws::settings::json;
    json::Object root;
    root.set("format", json::Value::text("circsim-results"));
    root.set("version", json::Value::integer(1));

    json::Array tables;
    for (const ws::spice::Table& t : run.results.tables) {
        json::Object o;
        const char* analysis = ".op";
        switch (t.analysis) {
            case ws::spice::AnalysisKind::Dc: analysis = ".dc"; break;
            case ws::spice::AnalysisKind::Tran: analysis = ".tran"; break;
            case ws::spice::AnalysisKind::Ac: analysis = ".ac"; break;
            case ws::spice::AnalysisKind::Noise: analysis = ".noise"; break;
            case ws::spice::AnalysisKind::Op: break;
        }
        o.set("analysis", json::Value::text(analysis));
        json::Array sweep;
        for (const std::string& n : t.sweep_names) {
            sweep.push_back(json::Value::text(n));
        }
        o.set("sweep", json::Value::array(std::move(sweep)));
        json::Array columns;
        for (const std::string& n : t.sweep_names) {
            columns.push_back(json::Value::text(n));
        }
        for (const ws::spice::Signal& sig : t.signals) {
            columns.push_back(json::Value::text(sig.name));
        }
        o.set("columns", json::Value::array(std::move(columns)));
        json::Array rows;
        for (std::size_t i = 0; i < t.row_count(); ++i) {
            json::Array row;
            for (const double v : t.row(i)) {
                row.push_back(json::Value::real(v));
            }
            rows.push_back(json::Value::array(std::move(row)));
        }
        o.set("rows", json::Value::array(std::move(rows)));
        tables.push_back(json::Value::object(std::move(o)));
    }
    root.set("tables", json::Value::array(std::move(tables)));

    json::Array measurements;
    for (const ws::spice::Measurement& m : run.measurements) {
        json::Object o;
        o.set("name", json::Value::text(m.name));
        o.set("analysis", json::Value::text(m.analysis));
        if (m.value.has_value()) {
            o.set("value", json::Value::real(*m.value));
        }
        if (!m.note.empty()) {
            o.set("note", json::Value::text(m.note));
        }
        measurements.push_back(json::Value::object(std::move(o)));
    }
    root.set("measurements", json::Value::array(std::move(measurements)));
    return json::write(json::Value::object(std::move(root)));
}

}  // namespace ws::circsim
