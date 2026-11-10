#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <ws/circsim/library.hpp>
#include <ws/circsim/text.hpp>

#include "json_helpers.hpp"
#include "util.hpp"

namespace ws::circsim {

namespace json = ws::settings::json;

// as_number/as_integer/as_text/as_bool/as_pos/pos_value ride
// json_helpers.hpp; lower/is_identifier ride util.hpp; number_text rides
// fmt_num (text.hpp) - locale-independent by construction.

namespace {

// A number formatted for a params default (accepted as bare JSON number
// on load; written back as text so re-loads stay stringly like the model).
std::string number_text(double v) {
    return fmt_num(v);
}

// --- migration chain ----------------------------------------------------------
// v0 = "no version field at all" (or a pre-release file); steps may edit
// the raw root, unknown fields ride through untouched.

void migrate_0_to_1(json::Object& root, std::vector<Diag>& diags) {
    if (root.find("symbols") == nullptr) {
        root.set("symbols", json::Value::array());
    }
    diags.push_back(make_diag(
        Severity::Note, "circsim.symbols.migrated",
        "library carried no version; migrated to version 1 (defaults filled)",
        ""));
}

using Migrator = void (*)(json::Object&, std::vector<Diag>&);
constexpr Migrator kMigrations[] = {
    migrate_0_to_1,
};
constexpr int kOldestVersion = 0;

// --- sub-object readers ---------------------------------------------------------

// The fields a PinDef/ParamEmit/ParamSpec/DrawPrim reader recognizes; a
// key outside the set warns (dropped, not preserved - these objects are
// tight, and a typo here is something the author wants to hear about).
void warn_unknown_fields(const json::Object& o,
                         std::initializer_list<std::string_view> known,
                         const std::string& context,
                         const std::string& origin,
                         std::vector<Diag>& diags) {
    for (const auto& [key, val] : o) {
        (void)val;
        if (std::find(known.begin(), known.end(), key) != known.end()) {
            continue;
        }
        diags.push_back(make_diag(
            Severity::Warning, "circsim.symbols.bad_field",
            context + ": field '" + key +
                "' is not recognized here; dropped (unknown fields survive"
                " only at the symbol and root level)",
            origin));
    }
}

bool read_pin(const json::Object& o, PinDef& pin, const std::string& key,
              const std::string& origin, std::vector<Diag>& diags) {
    const auto name = as_text(o.find("name"));
    if (!name.has_value() || name->empty()) {
        diags.push_back(make_diag(
            Severity::Warning, "circsim.symbols.bad_field",
            "symbol '" + key + "': a pin without a 'name' is dropped",
            origin));
        return false;
    }
    pin.name = *name;
    if (const auto pos = as_pos(o.find("pos"))) {
        pin.dx = pos->x;
        pin.dy = pos->y;
    } else if (o.find("pos") != nullptr) {
        diags.push_back(make_diag(
            Severity::Warning, "circsim.symbols.bad_field",
            "symbol '" + key + "' pin '" + pin.name +
                "': 'pos' is not [x, y]; defaulting to [0, 0]",
            origin));
    }
    if (const auto dir = as_integer(o.find("dir"))) {
        pin.dir = static_cast<int>(*dir);
    } else if (o.find("dir") != nullptr) {
        diags.push_back(make_diag(
            Severity::Warning, "circsim.symbols.bad_field",
            "symbol '" + key + "' pin '" + pin.name +
                "': 'dir' is not an integer; defaulting to 180",
            origin));
    }
    warn_unknown_fields(o, {"name", "pos", "dir"},
                        "symbol '" + key + "' pin '" + pin.name + "'", origin,
                        diags);
    return true;
}

bool read_emit(const json::Object& o, ParamEmit& pe,
               const std::string& key, const std::string& origin,
               std::vector<Diag>& diags) {
    const auto param = as_text(o.find("param"));
    if (!param.has_value() || param->empty()) {
        diags.push_back(make_diag(
            Severity::Warning, "circsim.symbols.bad_field",
            "symbol '" + key + "': an emit entry without a 'param' is dropped",
            origin));
        return false;
    }
    pe.key = *param;
    if (const auto prefix = as_text(o.find("prefix"))) {
        pe.prefix = *prefix;
    } else if (o.find("prefix") != nullptr) {
        diags.push_back(make_diag(
            Severity::Warning, "circsim.symbols.bad_field",
            "symbol '" + key + "' emit '" + pe.key +
                "': 'prefix' is not text; defaulting to none",
            origin));
    }
    if (const auto optional = as_bool(o.find("optional"))) {
        pe.optional = *optional;
    } else if (o.find("optional") != nullptr) {
        diags.push_back(make_diag(
            Severity::Warning, "circsim.symbols.bad_field",
            "symbol '" + key + "' emit '" + pe.key +
                "': 'optional' is not a bool; defaulting to true",
            origin));
    }
    warn_unknown_fields(o, {"param", "prefix", "optional"},
                        "symbol '" + key + "' emit '" + pe.key + "'", origin,
                        diags);
    return true;
}

// One entry of a multi-card symbol's "cards" list: the device letter,
// the name suffix, the pin indices it connects, its own emit tail and
// the sibling-name flag (see symbols.hpp CardEmit). Pin indices are
// bounds-checked against the symbol's pin count by the caller (the
// pins parse before the cards do).
bool read_card(const json::Object& o, CardEmit& ce, const std::string& key,
               std::size_t pin_count, const std::string& origin,
               std::vector<Diag>& diags) {
    const auto letter = as_text(o.find("letter"));
    if (!letter.has_value() || letter->size() != 1 ||
        (*letter)[0] < 'A' || (*letter)[0] > 'Z') {
        diags.push_back(make_diag(
            Severity::Warning, "circsim.symbols.bad_field",
            "symbol '" + key + "': a cards entry without an uppercase"
                               " 'letter' (\"L\", \"K\", ...) is dropped",
            origin));
        return false;
    }
    ce.letter = (*letter)[0];
    if (const auto suffix = as_text(o.find("suffix"))) {
        ce.suffix = *suffix;
    } else if (o.find("suffix") != nullptr) {
        diags.push_back(make_diag(
            Severity::Warning, "circsim.symbols.bad_field",
            "symbol '" + key + "' card '" + *letter +
                "': 'suffix' is not text; defaulting to none",
            origin));
    }
    if (const json::Value* pins = o.find("pins");
        pins != nullptr && pins->is_array()) {
        for (const json::Value& e : *pins->as_array()) {
            const auto idx = as_integer(&e);
            if (!idx.has_value() || *idx < 0 ||
                static_cast<std::size_t>(*idx) >= pin_count) {
                diags.push_back(make_diag(
                    Severity::Warning, "circsim.symbols.bad_field",
                    "symbol '" + key + "' card '" + *letter +
                        "': pin index " +
                        (idx.has_value() ? std::to_string(*idx)
                                         : std::string("not a number")) +
                        " is not one of the symbol's " +
                        std::to_string(pin_count) + " pins; dropped",
                    origin));
                return false;
            }
            ce.pins.push_back(static_cast<std::uint32_t>(*idx));
        }
    } else if (o.find("pins") != nullptr) {
        diags.push_back(make_diag(
            Severity::Warning, "circsim.symbols.bad_field",
            "symbol '" + key + "': 'pins' is not an array; ignored", origin));
    }
    if (const json::Value* emit = o.find("emit");
        emit != nullptr && emit->is_array()) {
        for (const json::Value& e : *emit->as_array()) {
            ParamEmit pe;
            if (e.is_object() &&
                read_emit(*e.as_object(), pe, key, origin, diags)) {
                ce.emit.push_back(std::move(pe));
            } else if (!e.is_object()) {
                diags.push_back(make_diag(
                    Severity::Warning, "circsim.symbols.bad_field",
                    "symbol '" + key + "': an emit entry is not an object;"
                                       " skipped",
                    origin));
            }
        }
    } else if (o.find("emit") != nullptr) {
        diags.push_back(make_diag(
            Severity::Warning, "circsim.symbols.bad_field",
            "symbol '" + key + "': 'emit' is not an array; ignored", origin));
    }
    if (const auto siblings = as_bool(o.find("name_siblings"))) {
        ce.name_siblings = *siblings;
    } else if (o.find("name_siblings") != nullptr) {
        diags.push_back(make_diag(
            Severity::Warning, "circsim.symbols.bad_field",
            "symbol '" + key + "': 'name_siblings' is not a bool;"
                               " defaulting to false",
            origin));
    }
    warn_unknown_fields(o, {"letter", "suffix", "pins", "emit", "name_siblings"},
                        "symbol '" + key + "' card '" + *letter + "'", origin,
                        diags);
    return true;
}

bool read_param_spec(const json::Object& o, ParamSpec& ps,
                     const std::string& key, const std::string& origin,
                     std::vector<Diag>& diags) {
    const auto pk = as_text(o.find("key"));
    if (!pk.has_value() || pk->empty()) {
        diags.push_back(make_diag(
            Severity::Warning, "circsim.symbols.bad_field",
            "symbol '" + key +
                "': a params entry without a 'key' is dropped",
            origin));
        return false;
    }
    ps.key = *pk;
    if (const auto type = as_text(o.find("type"))) {
        if (*type == "number") {
            ps.type = ParamSpec::Type::Number;
        } else if (*type == "choice") {
            ps.type = ParamSpec::Type::Choice;
        } else if (*type != "text") {
            diags.push_back(make_diag(
                Severity::Warning, "circsim.symbols.bad_field",
                "symbol '" + key + "' param '" + ps.key + "': unknown type '" +
                    *type + "'; treating as text",
                origin));
        }
    }
    if (const auto label = as_text(o.find("label"))) {
        ps.label = *label;
    } else if (o.find("label") != nullptr) {
        diags.push_back(make_diag(
            Severity::Warning, "circsim.symbols.bad_field",
            "symbol '" + key + "' param '" + ps.key +
                "': 'label' is not text; defaulting to the key",
            origin));
    }
    if (const json::Value* def = o.find("default"); def != nullptr) {
        if (const auto text = as_text(def)) {
            ps.default_value = *text;
        } else if (const auto n = as_number(def)) {
            ps.default_value = number_text(*n);  // "default": 50 -> "50"
        } else {
            diags.push_back(make_diag(
                Severity::Warning, "circsim.symbols.bad_field",
                "symbol '" + key + "' param '" + ps.key +
                    "': 'default' is neither text nor number; ignored",
                origin));
        }
    }
    if (const json::Value* choices = o.find("choices");
        choices != nullptr && choices->is_array()) {
        for (const json::Value& c : *choices->as_array()) {
            if (const auto text = as_text(&c)) {
                ps.choices.push_back(*text);
            } else {
                diags.push_back(make_diag(
                    Severity::Warning, "circsim.symbols.bad_field",
                    "symbol '" + key + "' param '" + ps.key +
                        "': a choice is not text; dropped",
                    origin));
            }
        }
    } else if (o.find("choices") != nullptr) {
        diags.push_back(make_diag(
            Severity::Warning, "circsim.symbols.bad_field",
            "symbol '" + key + "' param '" + ps.key +
                "': 'choices' is not an array; ignored",
            origin));
    }
    warn_unknown_fields(o, {"key", "type", "label", "default", "choices"},
                        "symbol '" + key + "' param '" + ps.key + "'", origin,
                        diags);
    return true;
}

// Reads [numbers...] with the wanted arity; nullopt on any mismatch.
std::optional<std::vector<double>> number_array(const json::Value* v,
                                                std::size_t want) {
    if (v == nullptr || !v->is_array() || v->as_array() == nullptr ||
        v->as_array()->size() != want) {
        return std::nullopt;
    }
    std::vector<double> out;
    for (const json::Value& e : *v->as_array()) {
        const auto n = as_number(&e);
        if (!n.has_value()) {
            return std::nullopt;
        }
        out.push_back(*n);
    }
    return out;
}

std::optional<std::vector<Pos>> pos_array(const json::Value* v,
                                          std::size_t want_min) {
    if (v == nullptr || !v->is_array() || v->as_array() == nullptr ||
        v->as_array()->size() < want_min) {
        return std::nullopt;
    }
    std::vector<Pos> out;
    for (const json::Value& e : *v->as_array()) {
        const auto p = as_pos(&e);
        if (!p.has_value()) {
            return std::nullopt;
        }
        out.push_back(*p);
    }
    return out;
}

bool read_draw(const json::Object& o, DrawPrim& d, const std::string& key,
               const std::string& origin, std::vector<Diag>& diags) {
    if (o.size() != 1) {
        diags.push_back(make_diag(
            Severity::Warning, "circsim.symbols.bad_draw",
            "symbol '" + key + "': a draw entry must hold exactly one shape"
                                " key; entry dropped",
            origin));
        return false;
    }
    const auto& [shape, val] = *o.begin();
    const auto bad = [&](const char* why) {
        diags.push_back(make_diag(
            Severity::Warning, "circsim.symbols.bad_draw",
            "symbol '" + key + "': " + shape + " " + why + "; entry dropped",
            origin));
        return false;
    };
    if (shape == "line") {
        d.kind = DrawPrim::Kind::Line;
        const auto pts = pos_array(&val, 2);
        if (!pts.has_value() || pts->size() != 2) {
            return bad("needs exactly two [x, y] points");
        }
        d.pts = *pts;
    } else if (shape == "poly") {
        d.kind = DrawPrim::Kind::Poly;
        const auto pts = pos_array(&val, 2);
        if (!pts.has_value()) {
            return bad("needs two or more [x, y] points");
        }
        d.pts = *pts;
    } else if (shape == "polyfill") {
        d.kind = DrawPrim::Kind::PolyFill;
        const auto pts = pos_array(&val, 3);
        if (!pts.has_value()) {
            return bad("needs three or more [x, y] points");
        }
        d.pts = *pts;
    } else if (shape == "rect") {
        d.kind = DrawPrim::Kind::Rect;
        const auto n = number_array(&val, 4);
        if (!n.has_value()) {
            return bad("needs [x1, y1, x2, y2]");
        }
        d.pts = {Pos{(*n)[0], (*n)[1]}, Pos{(*n)[2], (*n)[3]}};
    } else if (shape == "circle" || shape == "dot") {
        d.kind = shape == "circle" ? DrawPrim::Kind::Circle
                                   : DrawPrim::Kind::Dot;
        const auto n = number_array(&val, 3);
        if (!n.has_value()) {
            return bad("needs [cx, cy, r]");
        }
        if ((*n)[2] < 0.0) {
            return bad("needs a non-negative radius");
        }
        d.pts = {Pos{(*n)[0], (*n)[1]}};
        d.r = (*n)[2];
    } else if (shape == "arc") {
        d.kind = DrawPrim::Kind::Arc;
        const auto n = number_array(&val, 5);
        if (!n.has_value()) {
            return bad("needs [cx, cy, r, a0, a1]");
        }
        if ((*n)[2] < 0.0) {
            return bad("needs a non-negative radius");
        }
        d.pts = {Pos{(*n)[0], (*n)[1]}};
        d.r = (*n)[2];
        d.a0 = (*n)[3];
        d.a1 = (*n)[4];
    } else if (shape == "text") {
        d.kind = DrawPrim::Kind::Text;
        if (val.is_array() && val.as_array() != nullptr &&
            val.as_array()->size() == 3) {
            const json::Array& a = *val.as_array();
            const auto x = as_number(&a[0]);
            const auto y = as_number(&a[1]);
            const auto t = as_text(&a[2]);
            if (x.has_value() && y.has_value() && t.has_value()) {
                d.pts = {Pos{*x, *y}};
                d.text = *t;
            } else {
                return bad("needs [x, y, \"text\"]");
            }
        } else {
            return bad("needs [x, y, \"text\"]");
        }
    } else {
        diags.push_back(make_diag(
            Severity::Warning, "circsim.symbols.bad_draw",
            "symbol '" + key + "': unknown shape '" + shape +
                "' (line, poly, rect, circle, arc, dot, text); entry dropped",
            origin));
        return false;
    }
    return true;
}

// --- one symbol ----------------------------------------------------------------

struct SymbolReader {
    SymbolDef def;
    bool ok = false;
};

SymbolReader read_symbol(const json::Object& o, const std::string& origin,
                         std::vector<Diag>& diags) {
    SymbolReader r;
    const auto key = as_text(o.find("key"));
    if (!key.has_value() || key->empty()) {
        diags.push_back(make_diag(
            Severity::Error, "circsim.symbols.bad_symbol",
            "a symbol without a 'key' cannot be addressed; skipped", origin));
        return r;
    }
    r.def.key = *key;
    r.ok = true;
    const std::string k = *key;  // for messages

    if (const auto name = as_text(o.find("name"))) {
        r.def.name = *name;
    } else if (o.find("name") != nullptr) {
        diags.push_back(make_diag(
            Severity::Warning, "circsim.symbols.bad_field",
            "symbol '" + k + "': 'name' is not text; defaulting to empty",
            origin));
    }
    if (const json::Value* spice = o.find("spice"); spice != nullptr) {
        if (const auto letter = as_text(spice)) {
            if (letter->empty()) {
                r.def.spice_letter = 0;  // explicit "never exported"
            } else if (std::isalpha(static_cast<unsigned char>((*letter)[0]))) {
                r.def.spice_letter = (*letter)[0];
            } else {
                diags.push_back(make_diag(
                    Severity::Warning, "circsim.symbols.bad_field",
                    "symbol '" + k + "': 'spice' must be a letter (\"" +
                        *letter + "\"); treated as not exported",
                    origin));
            }
        } else {
            diags.push_back(make_diag(
                Severity::Warning, "circsim.symbols.bad_field",
                "symbol '" + k + "': 'spice' is not text; treated as not"
                                " exported",
                origin));
        }
    }
    if (const auto ref = as_text(o.find("ref"))) {
        r.def.ref_prefix = *ref;
    } else if (o.find("ref") != nullptr) {
        diags.push_back(make_diag(
            Severity::Warning, "circsim.symbols.bad_field",
            "symbol '" + k + "': 'ref' is not text; defaulting to empty",
            origin));
    }
    if (const json::Value* net = o.find("fixed_net"); net != nullptr) {
        if (const auto name = as_text(net)) {
            r.def.fixed_net = *name;
        } else if (net->is_null()) {
            r.def.fixed_net = std::nullopt;
        } else {
            diags.push_back(make_diag(
                Severity::Warning, "circsim.symbols.bad_field",
                "symbol '" + k + "': 'fixed_net' is not text; ignored",
                origin));
        }
    }
    if (const json::Value* v = o.find("value"); v != nullptr) {
        if (const auto b = as_bool(v)) {
            r.def.needs_value = *b;
        } else {
            diags.push_back(make_diag(
                Severity::Warning, "circsim.symbols.bad_field",
                "symbol '" + k + "': 'value' is not a bool; defaulting to"
                                " true",
                origin));
        }
    }
    if (const json::Value* v = o.find("model"); v != nullptr) {
        if (const auto b = as_bool(v)) {
            r.def.needs_model = *b;
        } else {
            diags.push_back(make_diag(
                Severity::Warning, "circsim.symbols.bad_field",
                "symbol '" + k + "': 'model' is not a bool; defaulting to"
                                " false",
                origin));
        }
    }
    // The model KIND the `model` param names (stage 25): a .csym
    // semiconductor names which library shelf its picker offers and
    // ERC checks against. Unknown strings ride as-is - the built-in
    // library grows kinds the editor has not met yet, and a wrong
    // kind simply picks nothing (find_builtin_model still answers
    // by name at export time).
    if (const json::Value* v = o.find("model_kind"); v != nullptr) {
        if (const auto t = as_text(v)) {
            r.def.model_kind = *t;
        } else {
            diags.push_back(make_diag(
                Severity::Warning, "circsim.symbols.bad_field",
                "symbol '" + k + "': 'model_kind' is not text; ignored",
                origin));
        }
    }
    if (const json::Value* pins = o.find("pins");
        pins != nullptr && pins->is_array()) {
        for (const json::Value& e : *pins->as_array()) {
            PinDef pin;
            if (e.is_object() &&
                read_pin(*e.as_object(), pin, k, origin, diags)) {
                r.def.pins.push_back(std::move(pin));
            } else if (!e.is_object()) {
                diags.push_back(make_diag(
                    Severity::Warning, "circsim.symbols.bad_field",
                    "symbol '" + k + "': a pin entry is not an object;"
                                    " dropped",
                    origin));
            }
        }
    } else if (o.find("pins") != nullptr) {
        diags.push_back(make_diag(
            Severity::Warning, "circsim.symbols.bad_field",
            "symbol '" + k + "': 'pins' is not an array; the symbol exports"
                            " no connections",
            origin));
    }
    if (const json::Value* emit = o.find("emit");
        emit != nullptr && emit->is_array()) {
        for (const json::Value& e : *emit->as_array()) {
            ParamEmit pe;
            if (e.is_object() &&
                read_emit(*e.as_object(), pe, k, origin, diags)) {
                r.def.emit.push_back(std::move(pe));
            } else if (!e.is_object()) {
                diags.push_back(make_diag(
                    Severity::Warning, "circsim.symbols.bad_field",
                    "symbol '" + k + "': an emit entry is not an object;"
                                    " dropped",
                    origin));
            }
        }
    } else if (o.find("emit") != nullptr) {
        diags.push_back(make_diag(
            Severity::Warning, "circsim.symbols.bad_field",
            "symbol '" + k + "': 'emit' is not an array; ignored", origin));
    }
    // The multi-card layout (symbols.hpp CardEmit): parses after the
    // pins so each entry's pin indices can be bounds-checked.
    if (const json::Value* cards = o.find("cards");
        cards != nullptr && cards->is_array()) {
        for (const json::Value& e : *cards->as_array()) {
            CardEmit ce;
            if (e.is_object() &&
                read_card(*e.as_object(), ce, k, r.def.pins.size(), origin,
                          diags)) {
                r.def.card_emits.push_back(std::move(ce));
            } else if (!e.is_object()) {
                diags.push_back(make_diag(
                    Severity::Warning, "circsim.symbols.bad_field",
                    "symbol '" + k + "': a cards entry is not an object;"
                                    " dropped",
                    origin));
            }
        }
    } else if (o.find("cards") != nullptr) {
        diags.push_back(make_diag(
            Severity::Warning, "circsim.symbols.bad_field",
            "symbol '" + k + "': 'cards' is not an array; ignored", origin));
    }
    if (const json::Value* params = o.find("params");
        params != nullptr && params->is_array()) {
        for (const json::Value& e : *params->as_array()) {
            ParamSpec ps;
            if (e.is_object() &&
                read_param_spec(*e.as_object(), ps, k, origin, diags)) {
                r.def.params.push_back(std::move(ps));
            } else if (!e.is_object()) {
                diags.push_back(make_diag(
                    Severity::Warning, "circsim.symbols.bad_field",
                    "symbol '" + k + "': a params entry is not an object;"
                                    " dropped",
                    origin));
            }
        }
    } else if (o.find("params") != nullptr) {
        diags.push_back(make_diag(
            Severity::Warning, "circsim.symbols.bad_field",
            "symbol '" + k + "': 'params' is not an array; ignored",
            origin));
    }
    if (const json::Value* draw = o.find("draw");
        draw != nullptr && draw->is_array()) {
        for (const json::Value& e : *draw->as_array()) {
            DrawPrim d;
            if (e.is_object() &&
                read_draw(*e.as_object(), d, k, origin, diags)) {
                r.def.draw.push_back(std::move(d));
            } else if (!e.is_object()) {
                diags.push_back(make_diag(
                    Severity::Warning, "circsim.symbols.bad_draw",
                    "symbol '" + k + "': a draw entry is not an object;"
                                    " dropped",
                    origin));
            }
        }
    } else if (o.find("draw") != nullptr) {
        diags.push_back(make_diag(
            Severity::Warning, "circsim.symbols.bad_field",
            "symbol '" + k + "': 'draw' is not an array; ignored", origin));
    }
    // Everything else rides unknown (silently - the extension surface,
    // exactly the .csim rule).
    for (const auto& [field, val] : o) {
        if (field != "key" && field != "name" && field != "spice" &&
            field != "ref" && field != "fixed_net" && field != "value" &&
            field != "model" && field != "model_kind" &&
            field != "pins" && field != "emit" &&
            field != "cards" && field != "params" && field != "draw") {
            r.def.unknown.set(field, val);
        }
    }
    return r;
}

}  // namespace

// --- the library ----------------------------------------------------------------

SymbolLibrary::SymbolLibrary() {
    symbols_ = builtin_symbols();
    builtin_count_ = symbols_.size();
    source_.assign(symbols_.size(), 0);
    reindex();
}

const SymbolDef* SymbolLibrary::find(std::string_view key) const {
    // The index answers in one lower + one lookup; the old linear scan
    // lowercased BOTH strings on every comparison (~100k constructions
    // for a 1,000-part schematic against a 100-symbol table, every net
    // build / ERC pass / export).
    const auto it = by_key_.find(lower(key));
    return it == by_key_.end() ? nullptr : &symbols_[it->second];
}

void SymbolLibrary::reindex() {
    by_key_.clear();
    for (std::size_t i = 0; i < symbols_.size(); ++i) {
        by_key_[lower(symbols_[i].key)] = i;  // later wins, as layering does
    }
}

ws::core::Status SymbolLibrary::load_text(std::string_view text,
                                          std::string_view origin_view,
                                          std::vector<Diag>& diags) {
    const std::string origin(origin_view);
    const auto parsed = json::parse(text);
    if (!parsed) {
        return ws::core::unexpected<ws::core::Error>(ws::core::make_error(
            1, "circsim",
            std::string(origin.empty() ? "" : std::string(origin) + ": ") +
                "not a symbol library: " + parsed.error().message));
    }
    if (!parsed->is_object()) {
        return ws::core::unexpected<ws::core::Error>(ws::core::make_error(
            1, "circsim",
            std::string(origin.empty() ? "" : std::string(origin) + ": ") +
                "not a symbol library: the top level is not an object"));
    }
    json::Object root = *parsed->as_object();

    const auto format = as_text(root.find("format"));
    if (!format.has_value() || *format != kCsymLibFormat) {
        return ws::core::unexpected<ws::core::Error>(ws::core::make_error(
            1, "circsim",
            std::string(origin.empty() ? "" : std::string(origin) + ": ") +
                "not a symbol library: \"format\" is not \"" +
                std::string(kCsymLibFormat) + "\""));
    }

    int version = kOldestVersion;
    if (const auto v = as_integer(root.find("version"))) {
        version = static_cast<int>(*v);
    } else if (root.find("version") != nullptr) {
        diags.push_back(make_diag(
            Severity::Warning, "circsim.symbols.bad_field",
            "'version' is not an integer; treated as version 0", origin));
    }
    if (version > kCsymLibVersion) {
        diags.push_back(make_diag(
            Severity::Warning, "circsim.symbols.future_version",
            "library says version " + std::to_string(version) +
                ", this build knows " + std::to_string(kCsymLibVersion) +
                "; reading what it understands",
            origin));
    } else if (version < kCsymLibVersion) {
        for (int v = version; v < kCsymLibVersion; ++v) {
            kMigrations[v - kOldestVersion](root, diags);
        }
    }

    // Root unknowns merge into the library's carrier (a later file's key
    // overrides in place; first position kept - the Object rule).
    for (const auto& [field, val] : root) {
        if (field != "format" && field != "version" && field != "symbols") {
            unknown.set(field, val);
        }
    }

    const json::Value* symbols = root.find("symbols");
    if (symbols == nullptr) {
        return {};  // the migration fills it; an explicit null means empty
    }
    if (!symbols->is_array()) {
        diags.push_back(make_diag(
            Severity::Warning, "circsim.symbols.bad_field",
            "'symbols' is not an array; the library contributes nothing",
            origin));
        return {};
    }
    for (const json::Value& e : *symbols->as_array()) {
        if (!e.is_object()) {
            diags.push_back(make_diag(
                Severity::Warning, "circsim.symbols.bad_symbol",
                "a symbol entry is not an object; skipped", origin));
            continue;
        }
        const SymbolReader r = read_symbol(*e.as_object(), origin, diags);
        if (!r.ok) {
            continue;
        }
        // Layering: same key (ci) overrides in place; new keys append.
        // The index carries the lookup (one lower + one find).
        const std::string key = lower(r.def.key);
        const auto it = by_key_.find(key);
        if (it != by_key_.end()) {
            const std::size_t at = it->second;
            diags.push_back(make_diag(
                Severity::Warning, "circsim.symbols.override",
                "symbol '" + r.def.key + "' redefines " +
                    (source_[at] == 0 ? "the built-in '" + r.def.key + "'"
                                      : "an earlier definition of '" +
                                            r.def.key + "'") +
                    "; the later definition wins",
                origin));
            symbols_[at] = std::move(r.def);
            source_[at] = 1;
        } else {
            by_key_[key] = symbols_.size();
            symbols_.push_back(std::move(r.def));
            source_.push_back(1);
        }
    }
    return {};
}

// --- the writer -------------------------------------------------------------------

std::string SymbolLibrary::save(bool include_builtin) const {
    json::Object root;
    root.set("format", json::Value::text(kCsymLibFormat));
    root.set("version", json::Value::integer(kCsymLibVersion));

    json::Array out;
    // The user-library cut is by SOURCE, not position: an override of a
    // built-in keeps the built-in's slot (index < builtin_count_), and it
    // is library data - it must survive a round trip.
    for (std::size_t i = 0; i < symbols_.size(); ++i) {
        if (!include_builtin && source_[i] == 0) {
            continue;
        }
        const SymbolDef& s = symbols_[i];
        json::Object o;
        o.set("key", json::Value::text(s.key));
        o.set("name", json::Value::text(s.name));
        o.set("spice", json::Value::text(
                           s.spice_letter == 0
                               ? std::string()
                               : std::string(1, s.spice_letter)));
        o.set("ref", json::Value::text(s.ref_prefix));
        json::Array pins;
        for (const PinDef& pin : s.pins) {
            json::Object p;
            p.set("name", json::Value::text(pin.name));
            p.set("pos", pos_value(Pos{pin.dx, pin.dy}));
            p.set("dir", json::Value::integer(pin.dir));
            pins.push_back(json::Value::object(std::move(p)));
        }
        o.set("pins", json::Value::array(std::move(pins)));
        if (s.fixed_net.has_value()) {
            o.set("fixed_net", json::Value::text(*s.fixed_net));
        }
        o.set("value", json::Value::boolean(s.needs_value));
        o.set("model", json::Value::boolean(s.needs_model));
        // The kind rides only when the symbol takes one, so every
        // classic .csym stays byte-identical.
        if (!s.model_kind.empty()) {
            o.set("model_kind", json::Value::text(s.model_kind));
        }
        json::Array emit;
        for (const ParamEmit& pe : s.emit) {
            json::Object e;
            e.set("param", json::Value::text(pe.key));
            e.set("prefix", json::Value::text(pe.prefix));
            e.set("optional", json::Value::boolean(pe.optional));
            emit.push_back(json::Value::object(std::move(e)));
        }
        o.set("emit", json::Value::array(std::move(emit)));
        // The multi-card layout rides only when the symbol has one, so
        // every classic .csym stays byte-identical.
        if (!s.card_emits.empty()) {
            json::Array cards;
            for (const CardEmit& ce : s.card_emits) {
                json::Object c;
                c.set("letter", json::Value::text(std::string(1, ce.letter)));
                c.set("suffix", json::Value::text(ce.suffix));
                json::Array cpins;
                for (const std::uint32_t pin : ce.pins) {
                    cpins.push_back(json::Value::integer(pin));
                }
                c.set("pins", json::Value::array(std::move(cpins)));
                json::Array cemit;
                for (const ParamEmit& pe : ce.emit) {
                    json::Object e;
                    e.set("param", json::Value::text(pe.key));
                    e.set("prefix", json::Value::text(pe.prefix));
                    e.set("optional", json::Value::boolean(pe.optional));
                    cemit.push_back(json::Value::object(std::move(e)));
                }
                c.set("emit", json::Value::array(std::move(cemit)));
                c.set("name_siblings", json::Value::boolean(ce.name_siblings));
                cards.push_back(json::Value::object(std::move(c)));
            }
            o.set("cards", json::Value::array(std::move(cards)));
        }
        json::Array params;
        for (const ParamSpec& ps : s.params) {
            json::Object p;
            p.set("key", json::Value::text(ps.key));
            const char* type = "text";
            if (ps.type == ParamSpec::Type::Number) {
                type = "number";
            } else if (ps.type == ParamSpec::Type::Choice) {
                type = "choice";
            }
            p.set("type", json::Value::text(type));
            p.set("label", json::Value::text(ps.label));
            p.set("default", json::Value::text(ps.default_value));
            if (!ps.choices.empty()) {
                json::Array choices;
                for (const std::string& c : ps.choices) {
                    choices.push_back(json::Value::text(c));
                }
                p.set("choices", json::Value::array(std::move(choices)));
            }
            params.push_back(json::Value::object(std::move(p)));
        }
        o.set("params", json::Value::array(std::move(params)));
        json::Array draw;
        for (const DrawPrim& d : s.draw) {
            json::Object e;
            switch (d.kind) {
                case DrawPrim::Kind::Line: {
                    if (d.pts.size() < 2) {
                        break;
                    }
                    json::Array pts{pos_value(d.pts[0]), pos_value(d.pts[1])};
                    e.set("line", json::Value::array(std::move(pts)));
                    break;
                }
                case DrawPrim::Kind::Poly: {
                    json::Array pts;
                    for (const Pos& p : d.pts) {
                        pts.push_back(pos_value(p));
                    }
                    e.set("poly", json::Value::array(std::move(pts)));
                    break;
                }
                case DrawPrim::Kind::PolyFill: {
                    if (d.pts.size() < 3) {
                        break;
                    }
                    json::Array pts;
                    for (const Pos& p : d.pts) {
                        pts.push_back(pos_value(p));
                    }
                    e.set("polyfill", json::Value::array(std::move(pts)));
                    break;
                }
                case DrawPrim::Kind::Rect: {
                    if (d.pts.size() < 2) {
                        break;
                    }
                    json::Array n{json::Value::real(d.pts[0].x),
                                  json::Value::real(d.pts[0].y),
                                  json::Value::real(d.pts[1].x),
                                  json::Value::real(d.pts[1].y)};
                    e.set("rect", json::Value::array(std::move(n)));
                    break;
                }
                case DrawPrim::Kind::Circle:
                case DrawPrim::Kind::Dot: {
                    if (d.pts.empty()) {
                        break;
                    }
                    json::Array n{json::Value::real(d.pts[0].x),
                                  json::Value::real(d.pts[0].y),
                                  json::Value::real(d.r)};
                    e.set(d.kind == DrawPrim::Kind::Circle ? "circle" : "dot",
                          json::Value::array(std::move(n)));
                    break;
                }
                case DrawPrim::Kind::Arc: {
                    if (d.pts.empty()) {
                        break;
                    }
                    json::Array n{json::Value::real(d.pts[0].x),
                                  json::Value::real(d.pts[0].y),
                                  json::Value::real(d.r),
                                  json::Value::real(d.a0),
                                  json::Value::real(d.a1)};
                    e.set("arc", json::Value::array(std::move(n)));
                    break;
                }
                case DrawPrim::Kind::Text: {
                    if (d.pts.empty()) {
                        break;
                    }
                    json::Array n{json::Value::real(d.pts[0].x),
                                  json::Value::real(d.pts[0].y),
                                  json::Value::text(d.text)};
                    e.set("text", json::Value::array(std::move(n)));
                    break;
                }
            }
            draw.push_back(json::Value::object(std::move(e)));
        }
        o.set("draw", json::Value::array(std::move(draw)));
        for (const auto& [field, val] : s.unknown) {
            o.set(field, val);
        }
        out.push_back(json::Value::object(std::move(o)));
    }
    root.set("symbols", json::Value::array(std::move(out)));
    for (const auto& [field, val] : unknown) {
        root.set(field, val);
    }
    return json::write(json::Value::object(std::move(root)));
}

const SymbolLibrary& builtin_library() {
    static const SymbolLibrary lib;
    return lib;
}

}  // namespace ws::circsim
