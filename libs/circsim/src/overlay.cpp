// overlay (patch 0012): the interactive canvas's pure math - see
// overlay.hpp for the contracts. The implementations moved here from
// canvas_panel.cpp's anonymous namespace so the locked rule holds
// (no ui include anywhere in libs/circsim) and the rules that decide
// pixels answer under the headless gate.

#include <ws/circsim/overlay.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include <ws/spice/deck.hpp>
#include <ws/spice/parse.hpp>

#include <ws/circsim/models.hpp>
#include <ws/circsim/plot.hpp>

#include "util.hpp"

namespace ws::circsim {

namespace {

// The r2d packing, produced locally: this library must not include
// the ui headers just to spell a color word.
[[nodiscard]] constexpr std::uint32_t pack_rgba(int r, int g, int b,
                                                int a) {
    return (static_cast<std::uint32_t>(r) << 24) |
           (static_cast<std::uint32_t>(g) << 16) |
           (static_cast<std::uint32_t>(b) << 8) |
           static_cast<std::uint32_t>(a);
}

// The ladder's nearest entry (the values are exact binary fractions,
// but a drifted rate still finds its home).
[[nodiscard]] std::size_t nearest_rate(double rate) {
    std::size_t best = 2;  // 1x, the default's own entry
    double best_d = -1.0;
    for (std::size_t i = 0; i < std::size(kOverlayRates); ++i) {
        const double d = std::fabs(kOverlayRates[i] - rate);
        if (best_d < 0.0 || d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

// --- the S/W model's thresholds (the blade walk's constants) -------------------

// What one .model SW card says: the S reads Vt/Vh, the W It/Ih.
struct SwThresholds {
    double vt = 0.0;
    double vh = 0.0;
    double it = 0.0;
    double ih = 0.0;
};

// The param tokens of one .model card -> the thresholds. The walk over
// the tokens mirrors the SW device's own apply_model (devices_m2):
// paren glue "(Ron=1" / "Roff=1meg)", "key=value" one token, "key
// value" across two, "key = value" across three. Values ride
// parse_number ("2.5", "0.4m", "1meg"); a token that cannot spell a
// number makes the whole card unusable (the run would have said the
// same). Keys the blade does not read (Ron/Roff) are consumed and
// skipped - only the thresholds answer here.
[[nodiscard]] std::optional<SwThresholds> sw_thresholds_of(
    const std::vector<ws::spice::Token>& toks) {
    SwThresholds out;
    for (std::size_t i = 0; i < toks.size(); ++i) {
        std::string text = toks[i].text;
        while (!text.empty() && text.front() == '(') {
            text.erase(text.begin());
        }
        while (!text.empty() && text.back() == ')') {
            text.pop_back();
        }
        if (text.empty()) {
            continue;
        }
        std::string key;
        std::string value;
        const std::size_t eq = text.find('=');
        if (eq != std::string::npos && eq > 0) {
            key = lower(text.substr(0, eq));
            value = text.substr(eq + 1);
        } else {
            key = lower(text);
            std::size_t next = i + 1;
            if (next < toks.size() && toks[next].text == "=") {
                ++next;
            }
            if (next >= toks.size()) {
                return std::nullopt;  // a key without a value
            }
            value = toks[next].text;
            i = next;
        }
        while (!value.empty() && value.back() == ')') {
            value.pop_back();
        }
        const ws::core::Result<double> r = ws::spice::parse_number(value);
        if (!r) {
            return std::nullopt;
        }
        if (key == "vt") {
            out.vt = *r;
        } else if (key == "vh") {
            out.vh = *r;
        } else if (key == "it") {
            out.it = *r;
        } else if (key == "ih") {
            out.ih = *r;
        }
        // Ron/Roff and friends: consumed, not answered.
    }
    return out;
}

// The thresholds of the model `name` names, in the export's own
// resolution order: the directives block's card first (the user's own
// wins), else the built-in model library's card, else nothing. Both
// spellings go through ws::spice's real deck reader - continuations,
// comments and glued parens read exactly as the run read them. (An
// .include inside directives resolves through the RUN's own resolver,
// which this walk does not carry: such a card answers nothing here and
// the blade rests open - the honest degradation, never a wrong flip.)
[[nodiscard]] std::optional<SwThresholds> sw_thresholds_for(
    const Schematic& s, const std::string& name) {
    const auto from_deck_text = [&name](const std::string& text) {
        // Line 1 of a deck is its title - the leading comment keeps
        // the first real card off it.
        const auto read =
            ws::spice::read_deck("switch overlay", "* overlay\n" + text,
                                 ws::spice::ParseOptions{});
        if (!read) {
            return std::optional<SwThresholds>{};
        }
        std::vector<ws::spice::Warning> warnings;
        const auto ast = ws::spice::build_deck_ast(*read, warnings);
        if (!ast) {
            return std::optional<SwThresholds>{};
        }
        for (const ws::spice::ModelDef& m : ast->models) {
            if (lower(m.name.text) == lower(name) && m.type == "sw") {
                return sw_thresholds_of(m.params);
            }
        }
        return std::optional<SwThresholds>{};
    };
    if (!s.directives.empty()) {
        if (const std::optional<SwThresholds> t =
                from_deck_text(s.directives);
            t.has_value()) {
            return t;
        }
    }
    if (const ModelCard* card = find_builtin_model(name);
        card != nullptr) {
        return from_deck_text(card->text);
    }
    return std::nullopt;
}

}  // namespace

std::span<const double> overlay_sweep_axis(const ws::spice::Table& t) {
    // The leading .step family axes are not the analysis's own x;
    // step_axes counts them, so the own axis is the first one past
    // them (a plain table's step_axes is 0 and the answer is axis(0),
    // the historical behavior).
    if (t.sweep_axes.empty()) {
        return {};
    }
    const std::size_t k =
        std::min(t.step_axes, t.sweep_axes.size() - 1u);
    return t.axis(k);
}

bool overlay_scrubbable(const ws::spice::Table& t) {
    return (t.sweep == ws::spice::SweepKind::Time ||
            t.sweep == ws::spice::SweepKind::Swept) &&
           !overlay_sweep_axis(t).empty();
}

std::size_t overlay_playhead_sample(const ws::spice::Table& t,
                                    double playhead) {
    const std::size_t rows = t.row_count();
    if (rows == 0) {
        return 0;
    }
    if (!overlay_scrubbable(t)) {
        return rows - 1;  // the sweep's end - the settled answer
    }
    const std::span<const double> axis = overlay_sweep_axis(t);
    if (axis.empty()) {
        return rows - 1;
    }
    // The last sample at or before the playhead (a .tran/.dc axis is
    // strictly increasing; upper_bound lands past it).
    const auto it = std::upper_bound(axis.begin(), axis.end(), playhead);
    if (it == axis.begin()) {
        return 0;
    }
    return static_cast<std::size_t>(it - axis.begin()) - 1;
}

OverlayWindow overlay_window(const ws::spice::Table& t) {
    OverlayWindow w;
    const std::span<const double> axis = overlay_sweep_axis(t);
    if (!axis.empty()) {
        w.t0 = axis.front();
        w.t1 = axis.back();
    }
    return w;
}

std::string overlay_axis_readout(const ws::spice::Table& t,
                                 double playhead) {
    switch (t.sweep) {
        case ws::spice::SweepKind::Time:
            // The strip's own spelling, kept: the SI value with a
            // trailing 's', no axis name (time needs no introduction).
            return format_eng(playhead) + "s";
        case ws::spice::SweepKind::Swept: {
            // "V1 = 3.4V": the swept source names itself, and its
            // leading letter carries the unit (V is volts, I is
            // amps - the DcSpec source grammar's own rule).
            std::string name =
                t.sweep_names.size() > t.step_axes
                    ? t.sweep_names[t.step_axes]
                    : std::string{"sweep"};
            const char unit =
                !name.empty() && (name[0] == 'I' || name[0] == 'i') ? 'A'
                                                                     : 'V';
            return name + " = " + format_eng(playhead) + unit;
        }
        case ws::spice::SweepKind::Frequency:
            return format_eng(playhead) + "Hz";
        case ws::spice::SweepKind::None:
            break;
    }
    return {};
}

double overlay_scrub(const OverlayWindow& w, double frac) {
    if (!(w.t1 > w.t0)) {
        return w.t0;
    }
    const double f = std::clamp(frac, 0.0, 1.0);
    return w.t0 + f * (w.t1 - w.t0);
}

double overlay_step(double t, const OverlayWindow& w, double dt,
                    double rate, double loop_seconds) {
    const double span = w.t1 - w.t0;
    if (!(span > 0.0) || !(dt > 0.0)) {
        return std::clamp(t, w.t0, w.t1);
    }
    const double pace =
        span * std::max(rate, 0.0) / std::max(loop_seconds, 1e-9);
    t += dt * pace;
    while (t > w.t1) {
        t -= span;  // loop
    }
    if (t < w.t0) {
        t = w.t0;
    }
    return t;
}

double overlay_rate_next(double rate) {
    const std::size_t i = nearest_rate(rate);
    return kOverlayRates[(i + 1u) % std::size(kOverlayRates)];
}

const char* overlay_rate_label(double rate) {
    switch (nearest_rate(rate)) {
    case 0:
        return "1/4x";
    case 1:
        return "1/2x";
    case 3:
        return "2x";
    default:
        return "1x";
    }
}

std::uint32_t overlay_voltage_color(double v, double vmax) {
    const double u =
        std::clamp(v / std::max(vmax, 1e-9), -1.0, 1.0);
    const double a = std::sqrt(std::fabs(u));
    constexpr double kBase[3] = {122.0, 126.0, 134.0};  // neutral ink
    constexpr double kHot[3] = {255.0, 86.0, 60.0};     // positive: red
    constexpr double kCold[3] = {86.0, 150.0, 255.0};   // negative: blue
    const double* hot = u >= 0.0 ? kHot : kCold;
    int rgb[3];
    for (int i = 0; i < 3; ++i) {
        rgb[i] = static_cast<int>(
            std::lround(kBase[i] + (hot[i] - kBase[i]) * a));
    }
    return pack_rgba(rgb[0], rgb[1], rgb[2], 242);
}

double overlay_led_brightness(double amps) {
    // The glow's full-scale and floor: 20 mA reads as full-on,
    // sub-100 uA conduction is dark (the gmin ghost must not glow).
    constexpr double kFullAmps = 0.020;
    constexpr double kFloorAmps = 100e-6;
    const double a = std::fabs(amps);  // sign-blind
    if (a < kFloorAmps) {
        return 0.0;
    }
    return std::sqrt(std::min(1.0, a / kFullAmps));
}

OverlaySnapshot overlay_snapshot(const ws::spice::Table& t,
                                 const SpiceExport& map,
                                 double playhead) {
    OverlaySnapshot snap;
    snap.sample = overlay_playhead_sample(t, playhead);
    // net id -> node token (the export map is token -> net; invert
    // once - the first token a net answers wins, the deck's own
    // one-node-per-net spelling).
    std::unordered_map<std::uint32_t, std::string> token_of;
    token_of.reserve(map.node_net.size());
    for (const auto& [tok, net_id] : map.node_net) {
        token_of.emplace(net_id, tok);
    }
    for (const auto& [net_id, tok] : token_of) {
        const std::span<const double> col = t.signal("v(" + tok + ")");
        if (col.size() > snap.sample &&
            std::isfinite(col[snap.sample])) {
            const double v = col[snap.sample];
            snap.volts.emplace(net_id, v);
            snap.vmax = std::max(snap.vmax, std::fabs(v));
        }
    }
    // part id -> the element's probe current at the same sample (only
    // devices with a probe column answer; resistors stay absent).
    for (const auto& [name, part_id] : map.element_part) {
        const std::span<const double> col = t.signal("i(" + name + ")");
        if (col.size() > snap.sample &&
            std::isfinite(col[snap.sample])) {
            snap.amps.emplace(part_id, col[snap.sample]);
        }
    }
    snap.ok = !snap.volts.empty();
    return snap;
}

OverlaySwitch overlay_switch(const ws::spice::Table& t,
                             const SpiceExport& map, const NetList& nets,
                             const Schematic& s, const Part& p,
                             const SymbolDef& sym, double playhead) {
    OverlaySwitch out;  // unknown -> the blade rests open
    if (t.row_count() == 0) {
        return out;
    }
    const std::size_t sample = overlay_playhead_sample(t, playhead);

    // The control signal, resolved onto its column pair: v(c+) - v(c-)
    // for an S (the pins by name, the nets through the run's own
    // build, ground answering 0 V), i(<vs>) for a W (the deck's own
    // spelling - element_part is CiLess, so a differently-cased param
    // still finds the column the table actually named).
    std::span<const double> pos;
    std::span<const double> neg;
    std::span<const double> cur;
    double thr = 0.0;
    double band = 0.0;
    if (sym.spice_letter == 'S') {
        const auto pin_index = [&](const char* name) {
            for (std::size_t i = 0; i < sym.pins.size(); ++i) {
                if (sym.pins[i].name == name) {
                    return static_cast<int>(i);
                }
            }
            return -1;
        };
        const int cp = pin_index("c+");
        const int cn = pin_index("c-");
        if (cp < 0 || cn < 0) {
            return out;  // not this shape (a .csym rename): rest open
        }
        // net id -> node token, inverted once (overlay_snapshot's own
        // rule: the first token a net answers wins).
        const auto column_of = [&](std::uint32_t pin_index_u,
                                   std::span<const double>& col) {
            const Net* n = nets.net_of_pin(
                PinRef{p.id, pin_index_u});
            if (n == nullptr) {
                return false;  // the pin joins no net (ERC's story)
            }
            if (n->is_ground) {
                return true;  // 0 V: no v(0) column exists, and none is
                              // needed - the span stays empty and reads 0
            }
            for (const auto& [tok, net_id] : map.node_net) {
                if (net_id == n->id) {
                    col = t.signal("v(" + tok + ")");
                    return !col.empty();
                }
            }
            return false;
        };
        if (!column_of(static_cast<std::uint32_t>(cp), pos) ||
            !column_of(static_cast<std::uint32_t>(cn), neg)) {
            return out;
        }
    } else if (sym.spice_letter == 'W') {
        const auto vs = p.params.find("vs");
        if (vs == p.params.end() || vs->second.empty()) {
            return out;
        }
        cur = t.signal("i(" + vs->second + ")");
        if (cur.empty()) {
            const auto it = map.element_part.find(vs->second);
            if (it != map.element_part.end()) {
                cur = t.signal("i(" + it->first + ")");
            }
        }
        if (cur.empty()) {
            return out;
        }
    } else {
        return out;  // not a switch the walk speaks
    }

    // The thresholds: the model the part names, in the export's own
    // resolution order.
    const auto model = p.params.find("model");
    if (model == p.params.end() || model->second.empty()) {
        return out;  // ERC's missing_model is the story; no run anyway
    }
    const std::optional<SwThresholds> th =
        sw_thresholds_for(s, model->second);
    if (!th.has_value()) {
        return out;
    }
    if (sym.spice_letter == 'S') {
        thr = th->vt;
        band = th->vh;
    } else {
        thr = th->it;
        band = th->ih;
    }

    // The walk itself - the device's exact hysteresis law
    // (devices_m2's hysteresis_flip), sample 0 through the playhead's,
    // starting open (the export writes no ic=on). A ground pin's empty
    // span reads 0 V (no v(0) column exists); any column that runs
    // short, or a non-finite control anywhere, answers unknown rather
    // than guess.
    const auto read = [&](std::span<const double> col,
                          std::size_t k) -> std::optional<double> {
        if (col.empty()) {
            return 0.0;  // the ground pin (column_of's own spelling)
        }
        if (k >= col.size()) {
            return std::nullopt;
        }
        return col[k];
    };
    bool on = false;
    for (std::size_t k = 0; k <= sample && k < t.row_count(); ++k) {
        const std::optional<double> a = read(pos, k);
        const std::optional<double> b = read(neg, k);
        const std::optional<double> c =
            cur.empty() ? std::optional<double>{0.0} : read(cur, k);
        if (!a.has_value() || !b.has_value() || !c.has_value()) {
            return out;
        }
        const double control = cur.empty() ? *a - *b : *c;
        if (!std::isfinite(control)) {
            return out;
        }
        if (!on && control > thr + band) {
            on = true;
        } else if (on && control < thr - band) {
            on = false;
        }
    }
    out.known = true;
    out.closed = on;
    return out;
}

}  // namespace ws::circsim
