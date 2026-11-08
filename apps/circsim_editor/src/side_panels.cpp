// apps/circsim_editor - the side/bottom panels (see side_panels.hpp).
//
// The Properties grid is the stage's ParamSpec story made visible: ref
// and value rows, one row per spec (a text field for Text/Number - ERC
// validates numbers on the next check - a combo for Choice), every
// change pushed as an undo command whose typing-run coalescing is the
// stack's own. The field buffers live in the Editor keyed by a stable
// string and refresh lazily when the edit epoch moves (a selection
// change or an undo landing), so typing never fights the model and an
// undo lands back in the field the next frame.
//
// The Problems rows locate through ws::circsim::locate.hpp - the
// back-map seam - and the jump selects and zooms in one act.

#include "side_panels.hpp"

#include "palette_groups.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <ws/core/assert.hpp>
#include <ws/circsim/plot.hpp>
#include <ws/input/input.hpp>
#include <ws/spice/parse.hpp>
#include <ws/ui/renderer2d.hpp>

namespace editor {
namespace {

// --- the field buffers (the Properties panel's editing state) ---------

// The buffer for `key`, refreshed from `current` when the epoch moved
// (selection change, undo/redo landing, first use). Typing frames keep
// the buffer: nothing moves the epoch while the user edits.
[[nodiscard]] char* field_buf(Editor& ed, const std::string& key,
                              const std::string& current) {
    FieldBuf& b = ed.fields[key];
    if (b.stamp != ed.field_stamp) {
        const std::size_t keep =
            std::min(current.size(), b.buf.size() - 1u);
        std::memcpy(b.buf.data(), current.data(), keep);
        b.buf[keep] = '\0';
        b.shadow = current;
        b.stamp = ed.field_stamp;
        b.combo_open = false;
    }
    return b.buf.data();
}

[[nodiscard]] FieldBuf& field_of(Editor& ed, const std::string& key) {
    return ed.fields[key];
}

// text_input plus the focus watch: the flag feeds the chord loop's
// rung choice, so the editor's bare panel keys never land in a field
// the keyboard is editing.
//
// `id_label` is the field's full identity - the DISPLAYED label, then
// "##" and the field's buffer key, so two fields that share a display
// label are two widgets to the gui: separate caret state, separate
// focus, separate Tab stops. The "##" suffix rides the id and never
// the display (id_stack.hpp's marker rule), which is what keeps a
// param labeled "value" (or "ref", or any duplicated label) from
// MERGING with the built-in value row - one widget id would mean one
// shared TextInputState and one focus answer for both rows, and
// typing in one would drag the other's caret. The focus check below
// derives the SAME id the widget did, so the watched flag is the
// field's own truth.
[[nodiscard]] bool text_input_tracked(Editor& ed, gui::GuiContext& gui,
                                     gui::LayoutCursor& layout,
                                     const gui::ChromeTheme& theme,
                                     const std::string& id_label, char* buf,
                                     int capacity, float width = -1.0f) {
    const bool changed =
        gui::text_input(gui, layout, theme, id_label.c_str(), buf,
                        capacity, width);
    if (gui.is_focused(gui.ids().id(id_label.c_str()))) {
        ed.text_field_focused = true;
    }
    return changed;
}

// The selection's stable key: what the per-field buffers hang from.
[[nodiscard]] std::string part_field_key(std::uint32_t id, int kind,
                                         const std::string& param) {
    std::string key = "p" + std::to_string(id);
    if (kind == 0) {
        return key + ".ref";
    }
    if (kind == 1) {
        return key + ".value";
    }
    return key + ".param." + param;
}

// Bumps the edit epoch when the selection moved (the panel's own
// watch - the app bumps it on undo/redo through the stack listener).
// The bump rides the Editor's own seam like every other one in this
// file: the panel never mutates the epoch directly.
void watch_selection(Editor& ed) {
    if (ed.fields_sel_kind != static_cast<int>(ed.sel.kind) ||
        ed.fields_sel_id != ed.sel.id) {
        ed.fields_sel_kind = static_cast<int>(ed.sel.kind);
        ed.fields_sel_id = ed.sel.id;
        ed.bump_edit_epoch();
    }
}

// --- small row helpers ---------------------------------------------------

void label_line(gui::LayoutCursor& layout, const gui::ChromeTheme& theme,
                const std::string& key, const std::string& value) {
    char line[192];
    std::snprintf(line, sizeof line, "%s: %s", key.c_str(),
                  value.c_str());
    gui::text(layout, theme, line);
}

// A dim, secondary line: hints, units, validation notes. Drawn the way
// the netlist rows are (one layout row + draw_text) so it costs a single
// row and never competes with the primary text.
void hint_line(gui::LayoutCursor& layout, const gui::ChromeTheme& theme,
               const std::string& text) {
    if (theme.font != nullptr && theme.font->line_height() > 0.0f) {
        const ws::ui::r2d::Font& font = *theme.font;
        const gui::Rect row =
            layout.next(layout.remaining_width(), font.line_height() + 2.0f);
        (void)ws::ui::r2d::draw_text(row.x, row.y, text.c_str(), font,
                                     gui::dim(theme.text, 0.6f));
    } else {
        // Fontless fallback: use the text widget which handles null font.
        // (The row reservation stands: the same cursor advance the
        // font path makes, so both branches cost one layout row.)
        layout.next(layout.remaining_width(), 16.0f);
        gui::text(layout, theme, text.c_str());
    }
}

// The panels' section break: a separator and a heading.
void panel_section(gui::LayoutCursor& layout, const gui::ChromeTheme& theme,
                   const char* title) {
    gui::separator(layout, theme);
    gui::text(layout, theme, title);
}

// "3 errors" / "1 error": the count with its noun.
[[nodiscard]] std::string counted(std::size_t n, const char* noun) {
    return std::to_string(n) + " " + noun + (n == 1 ? "" : "s");
}

// What a value field reads back as: "4.7k" says "= 4700". Empty when the
// text is not a SPICE number or already is the plain number (free-form
// values like "SIN(0 1 1k)" simply get no readback).
[[nodiscard]] std::string value_readback(const char* text) {
    const auto parsed = ws::spice::parse_number(text);
    if (!parsed.has_value()) {
        return {};
    }
    char plain[64];
    std::snprintf(plain, sizeof plain, "%.12g", *parsed);
    if (std::string_view{text} == plain) {
        return {};
    }
    return std::string{"= "} + plain;
}

[[nodiscard]] std::string endpoint_label(const Editor& ed,
                                         const ws::circsim::WireEndpoint& e) {
    if (const ws::circsim::PinRef* pin = std::get_if<ws::circsim::PinRef>(&e)) {
        const ws::circsim::Part* p =
            ed.schem() != nullptr ? ed.schem()->find_part(pin->part) : nullptr;
        const ws::circsim::SymbolDef* sym =
            p != nullptr ? ed.sym_of(*p) : nullptr;
        const std::string who =
            p != nullptr && !p->ref.empty()
                ? p->ref
                : (p != nullptr ? p->symbol + " " + std::to_string(p->id)
                                : "part " + std::to_string(pin->part));
        const std::string pin_name =
            sym != nullptr && pin->pin < sym->pins.size()
                ? sym->pins[pin->pin].name
                : std::to_string(pin->pin);
        return who + "." + pin_name;
    }
    const ws::circsim::JunctionRef* j =
        std::get_if<ws::circsim::JunctionRef>(&e);
    return "junction " + std::to_string(j != nullptr ? j->junction : 0);
}

// A text row that pushes the edit on every change (the typing-run
// coalescer makes this one undo step per run). The widget id embeds
// the buffer key (text_input_tracked's "##" rule): the built-in
// "ref"/"value" rows and a symbol's own param rows can share display
// labels without sharing a widget.
void part_text_row(Editor& ed, gui::GuiContext& gui,
                   gui::LayoutCursor& layout, const gui::ChromeTheme& theme,
                   const char* label, std::uint32_t part_id, int kind,
                   const std::string& param, const std::string& current) {
    const std::string key = part_field_key(part_id, kind, param);
    if (text_input_tracked(ed, gui, layout, theme,
                           std::string{label} + "##" + key,
                           field_buf(ed, key, current), 128)) {
        push_part_text(*ed.doc, part_id, kind, param,
                       std::string{field_of(ed, key).buf.data()},
                       std::string{"Edit "} + label);
    }
    if (kind == 1) {
        const std::string readback =
            value_readback(field_of(ed, key).buf.data());
        if (!readback.empty()) {
            hint_line(layout, theme, readback);
        }
    }
}

// --- the analysis editor ---------------------------------------------------

// The per-type field rows for one analysis entry; every valid change
// pushes SetAnalysis. Numbers parse through ws::spice's grammar; an
// invalid value keeps the old and notes the status line. The labels
// lead with the plain word and keep the SPICE token in parentheses
// ("step (tstep)", "begin (tstart)"): the panel teaches the deck's
// vocabulary while it speaks the user's.
void analysis_fields(Editor& ed, gui::GuiContext& gui,
                     gui::LayoutCursor& layout,
                     const gui::ChromeTheme& theme, std::size_t index) {
    const std::vector<ws::circsim::AnalysisSpec>& entries =
        ed.schem()->analysis.entries;
    if (index >= entries.size()) {
        return;
    }
    const ws::circsim::AnalysisSpec& spec = entries[index];
    const std::string key = "a" + std::to_string(index);

    // A number row over a member pointer: parse, land, push; a bad
    // value keeps the old and says so on the status line. The id
    // embeds the buffer key (skey + "." + label) - one analysis is
    // edited at a time, but the key still keeps the row's state its
    // own across analysis switches. `unit` (when given) adds the
    // engineering readback - "1e-06" says "= 1us" - the Value row's
    // own "= 4700" teaching with the row's unit spelled.
    const auto number_row = [&](const char* label, const std::string& skey,
                                double current, const auto& land,
                                const char* unit = nullptr) {
        char current_text[64];
        std::snprintf(current_text, sizeof current_text, "%.12g", current);
        const std::string key = skey + "." + label;
        char* buf = field_buf(ed, key, current_text);
        if (text_input_tracked(ed, gui, layout, theme,
                               std::string{label} + "##" + key, buf, 128)) {
            const auto parsed = ws::spice::parse_number(buf);
            if (parsed.has_value()) {
                land(*parsed);
            } else {
                ed.status(std::string{"'"} + buf +
                          "' is not a SPICE number - kept the old value");
            }
        }
        const auto shown = ws::spice::parse_number(buf);
        if (!shown.has_value()) {
            hint_line(layout, theme,
                      "Not a SPICE number - try 1u, 4.7k or 1meg");
        } else if (unit != nullptr && *shown != 0.0) {
            const std::string eng =
                ws::circsim::format_eng(*shown) + unit;
            if (eng != std::string{buf}) {
                hint_line(layout, theme, "= " + eng);
            }
        }
    };

    if (std::get_if<ws::circsim::OpSpec>(&spec) != nullptr) {
        gui::text(layout, theme, "Operating point: no settings needed");
        return;
    }
    if (const ws::circsim::DcSpec* dc =
            std::get_if<ws::circsim::DcSpec>(&spec)) {
        const std::string skey = key + ".dc";
        char* src = field_buf(ed, skey + ".source", dc->source);
        if (text_input_tracked(ed, gui, layout, theme, "source (swept)##" +
                                                              skey +
                                                              ".source",
                               src, 128)) {
            ws::circsim::DcSpec next = *dc;
            next.source = src;
            push_set_analysis(*ed.doc, index, next);
        }
        number_row("begin (start)", skey, dc->start, [&](double v) {
            ws::circsim::DcSpec next = *dc;
            next.start = v;
            push_set_analysis(*ed.doc, index, next);
        });
        number_row("end (stop)", skey, dc->stop, [&](double v) {
            ws::circsim::DcSpec next = *dc;
            next.stop = v;
            push_set_analysis(*ed.doc, index, next);
        });
        number_row("step (increment)", skey, dc->step, [&](double v) {
            ws::circsim::DcSpec next = *dc;
            next.step = v;
            push_set_analysis(*ed.doc, index, next);
        });
        hint_line(layout, theme,
                  "Sweeps the named source from begin to end, step apart");
        return;
    }
    if (const ws::circsim::TranSpec* tran =
            std::get_if<ws::circsim::TranSpec>(&spec)) {
        const std::string skey = key + ".tran";
        hint_line(layout, theme,
                  "Signals over time: step = the output interval, "
                  "begin/end = the window");
        // 0 spells "unset" for the optional pair (tstart/tmax), which
        // the card layer treats exactly like nullopt's defaults.
        number_row("step (tstep)", skey, tran->tstep, [&](double v) {
            ws::circsim::TranSpec next = *tran;
            next.tstep = v;
            push_set_analysis(*ed.doc, index, next);
        }, "s");
        number_row("end (tstop)", skey, tran->tstop, [&](double v) {
            ws::circsim::TranSpec next = *tran;
            next.tstop = v;
            push_set_analysis(*ed.doc, index, next);
        }, "s");
        number_row("begin (tstart)", skey, tran->tstart.value_or(0.0),
                   [&](double v) {
                       ws::circsim::TranSpec next = *tran;
                       next.tstart = v > 0.0 ? std::optional<double>{v}
                                             : std::nullopt;
                       push_set_analysis(*ed.doc, index, next);
                   }, "s");
        number_row("max step (tmax)", skey, tran->tmax.value_or(0.0),
                   [&](double v) {
                       ws::circsim::TranSpec next = *tran;
                       next.tmax = v > 0.0 ? std::optional<double>{v}
                                           : std::nullopt;
                       push_set_analysis(*ed.doc, index, next);
                   }, "s");
        hint_line(layout, theme,
                  "0 on begin/max step leaves them at SPICE's defaults");
        bool flag = tran->uic;
        if (gui::checkbox(gui, layout, theme, "uic (skip the bias point)",
                          flag)) {
            ws::circsim::TranSpec next = *tran;
            next.uic = flag;
            push_set_analysis(*ed.doc, index, next);
        }
        return;
    }
    if (const ws::circsim::AcSpec* ac =
            std::get_if<ws::circsim::AcSpec>(&spec)) {
        const std::string skey = key + ".ac";
        static const char* const kSweeps[] = {"dec", "oct", "lin"};
        int selected = static_cast<int>(ac->sweep);
        FieldBuf& sweep = field_of(ed, skey + ".sweep");
        if (sweep.stamp != ed.field_stamp) {
            sweep.stamp = ed.field_stamp;
            sweep.combo_open = false;
        }
        // The combo answers true only on a REAL move (its own
        // contract - `selected` is written and the return fires when
        // the picked item differs from the current one), so the
        // spec-copy and the push below recompute nothing when the
        // user re-picks the value that is already set: the dropdown
        // closes, the model stays untouched, no SetAnalysis lands.
        // `selected` re-derives from the live spec each frame by
        // design - the row is a view of the model, the model is the
        // truth.
        if (gui::combo(gui, layout, theme,
                       ("sweep##" + skey + ".sweep").c_str(), kSweeps, 3,
                       selected, sweep.combo_open)) {
            ws::circsim::AcSpec next = *ac;
            next.sweep = static_cast<ws::circsim::AcSpec::Sweep>(selected);
            push_set_analysis(*ed.doc, index, next);
        }
        char n_text[32];
        std::snprintf(n_text, sizeof n_text, "%d", ac->n);
        char* n_buf = field_buf(ed, skey + ".n", n_text);
        if (text_input_tracked(ed, gui, layout, theme, "points (n)##" + skey +
                                                             ".n",
                               n_buf, 128)) {
            char* end = nullptr;
            const long parsed = std::strtol(n_buf, &end, 10);
            if (end != nullptr && *end == '\0' && parsed > 0) {
                ws::circsim::AcSpec next = *ac;
                next.n = static_cast<int>(parsed);
                push_set_analysis(*ed.doc, index, next);
            } else {
                ed.status(std::string{"'"} + n_buf +
                          "' is not a point count - kept the old value");
            }
        }
        number_row("begin (fstart)", skey, ac->fstart, [&](double v) {
            ws::circsim::AcSpec next = *ac;
            next.fstart = v;
            push_set_analysis(*ed.doc, index, next);
        }, "Hz");
        number_row("end (fstop)", skey, ac->fstop, [&](double v) {
            ws::circsim::AcSpec next = *ac;
            next.fstop = v;
            push_set_analysis(*ed.doc, index, next);
        }, "Hz");
        hint_line(layout, theme,
                  "Small-signal sweep from begin to end frequency");
        return;
    }
}

}  // namespace

// --- the analysis list's readable face (side_panels.hpp) ----------------------

namespace {

// The dc sweep's numbers, plain (no engineering prefix - a 0.1 step
// should read "0.1", not "100m"; the swept source's unit is the
// user's, not the row's to guess).
[[nodiscard]] std::string plain_num(double v) {
    char text[64];
    std::snprintf(text, sizeof text, "%.12g", v);
    return text;
}

}  // namespace

std::string analysis_brief(const ws::circsim::AnalysisSpec& spec) {
    if (std::get_if<ws::circsim::OpSpec>(&spec) != nullptr) {
        return "Operating point (.op)";
    }
    if (std::get_if<ws::circsim::DcSpec>(&spec) != nullptr) {
        return "DC sweep (.dc)";
    }
    if (std::get_if<ws::circsim::TranSpec>(&spec) != nullptr) {
        return "Transient (.tran)";
    }
    return "AC sweep (.ac)";
}

std::string analysis_params_brief(const ws::circsim::AnalysisSpec& spec) {
    if (std::get_if<ws::circsim::OpSpec>(&spec) != nullptr) {
        return "the DC bias solution - no parameters";
    }
    if (const ws::circsim::DcSpec* dc =
            std::get_if<ws::circsim::DcSpec>(&spec)) {
        return "sweeps " + dc->source + " from " + plain_num(dc->start) +
               " to " + plain_num(dc->stop) + ", step " +
               plain_num(dc->step);
    }
    if (const ws::circsim::TranSpec* tran =
            std::get_if<ws::circsim::TranSpec>(&spec)) {
        // tstep 0 spells "tstop/100" - the card layer's own default
        // (analysis_card's rule, mirrored so the row never disagrees
        // with the deck it will export).
        const double tstep =
            tran->tstep > 0.0 ? tran->tstep : tran->tstop / 100.0;
        std::string s = "step " + ws::circsim::format_eng(tstep) + "s, " +
                        "stop " + ws::circsim::format_eng(tran->tstop) +
                        "s";
        if (tran->tstart.has_value()) {
            s += ", start " + ws::circsim::format_eng(*tran->tstart) + "s";
        }
        if (tran->tmax.has_value()) {
            s += ", max " + ws::circsim::format_eng(*tran->tmax) + "s";
        }
        if (tran->uic) {
            s += ", UIC";
        }
        return s;
    }
    const ws::circsim::AcSpec* ac =
        std::get_if<ws::circsim::AcSpec>(&spec);
    const char* sweep = "dec";
    if (ac->sweep == ws::circsim::AcSpec::Sweep::Oct) {
        sweep = "oct";
    } else if (ac->sweep == ws::circsim::AcSpec::Sweep::Lin) {
        sweep = "lin";
    }
    return std::to_string(ac->n) + " pts/" + sweep + ", " +
           ws::circsim::format_eng(ac->fstart) + "Hz to " +
           ws::circsim::format_eng(ac->fstop) + "Hz";
}

// --- the source waveform editor (the pure half: side_panels.hpp) -------------

namespace {

// Case-insensitive head match: does `text` start with `head`?
[[nodiscard]] bool starts_with_ci(const std::string& text, const char* head) {
    const std::size_t n = std::strlen(head);
    if (text.size() < n) {
        return false;
    }
    for (std::size_t i = 0; i < n; ++i) {
        const char a = static_cast<char>(std::tolower(
            static_cast<unsigned char>(text[i])));
        const char b = static_cast<char>(
            std::tolower(static_cast<unsigned char>(head[i])));
        if (a != b) {
            return false;
        }
    }
    return true;
}

// The numbers inside a "(...)" form: split on spaces/commas/tabs and
// parse each through ws::spice's grammar. False when a token is not a
// number (the caller answers Raw - never a silent rewrite).
[[nodiscard]] bool split_numbers(const std::string& inside,
                                 std::vector<double>& out) {
    std::size_t at = 0;
    while (at < inside.size()) {
        const std::size_t sep = inside.find_first_of(" \t,", at);
        const std::string token =
            inside.substr(at, sep == std::string::npos ? std::string::npos
                                                       : sep - at);
        if (!token.empty()) {
            const auto parsed = ws::spice::parse_number(token);
            if (!parsed.has_value()) {
                return false;
            }
            out.push_back(*parsed);
        }
        if (sep == std::string::npos) {
            break;
        }
        at = sep + 1;
    }
    return true;
}

}  // namespace

SourceWave parse_source_wave(const std::string& text) {
    SourceWave w;
    // Blank (or absent) is the DC-only form.
    const std::size_t first = text.find_first_not_of(" \t");
    if (first == std::string::npos) {
        w.kind = SourceWave::Kind::None;
        return w;
    }
    const std::size_t last = text.find_last_not_of(" \t");
    const std::string trimmed = text.substr(first, last - first + 1);

    struct Head {
        const char* name;
        SourceWave::Kind kind;
        std::size_t numbers;
    };
    static const Head kHeads[] = {
        {"SIN(", SourceWave::Kind::Sin, 6},
        {"PULSE(", SourceWave::Kind::Pulse, 7},
        {"EXP(", SourceWave::Kind::Exp, 6},
        {"PWL(", SourceWave::Kind::Pwl, 0},
    };
    for (const Head& head : kHeads) {
        if (!starts_with_ci(trimmed, head.name)) {
            continue;
        }
        // The inside: past the head, up to the LAST ')' (a missing one
        // is tolerated the way ws::spice's own tokenizer tolerates a
        // split-across-tokens form - the numbers are what matter).
        const std::size_t open = trimmed.find('(');
        std::size_t close = trimmed.find_last_of(')');
        if (close == std::string::npos || close < open) {
            close = trimmed.size();
        }
        const std::string inside =
            trimmed.substr(open + 1, close - open - 1);
        if (head.kind == SourceWave::Kind::Pwl) {
            // Free-form corners: kept verbatim, edited as text.
            w.kind = SourceWave::Kind::Pwl;
            w.raw = trimmed;
            return w;
        }
        std::vector<double> numbers;
        if (!split_numbers(inside, numbers) ||
            numbers.size() > head.numbers) {
            break;  // unreadable or over-long: the Raw escape below
        }
        w.kind = head.kind;
        for (std::size_t i = 0; i < head.numbers && i < numbers.size(); ++i) {
            w.p[i] = numbers[i];
        }
        return w;
    }
    w.kind = SourceWave::Kind::Raw;
    w.raw = trimmed;
    return w;
}

std::string compose_source_wave(const SourceWave& w) {
    switch (w.kind) {
        case SourceWave::Kind::None:
            return {};
        case SourceWave::Kind::Sin:
        case SourceWave::Kind::Pulse:
        case SourceWave::Kind::Exp: {
            const char* head = w.kind == SourceWave::Kind::Sin    ? "SIN("
                               : w.kind == SourceWave::Kind::Pulse ? "PULSE("
                                                                   : "EXP(";
            const std::size_t n =
                w.kind == SourceWave::Kind::Pulse ? 7 : 6;
            std::string out = head;
            for (std::size_t i = 0; i < n; ++i) {
                char number[64];
                std::snprintf(number, sizeof number, "%.12g", w.p[i]);
                if (i != 0) {
                    out += ' ';
                }
                out += number;
            }
            out += ')';
            return out;
        }
        case SourceWave::Kind::Pwl:
        case SourceWave::Kind::Raw:
            return w.raw;
    }
    return {};
}

std::string source_wave_baseline(const SourceWave& w) {
    switch (w.kind) {
        case SourceWave::Kind::Sin:
        case SourceWave::Kind::Pulse:
        case SourceWave::Kind::Exp: {
            char text[64];
            std::snprintf(text, sizeof text, "%.12g", w.p[0]);
            return text;
        }
        case SourceWave::Kind::Pwl: {
            // The first (t, v) corner's value, when the numbers read:
            // "PWL(0 3 1m 5)" answers 3. Anything else - no pair, a
            // bad number - answers "" (the row starts empty rather
            // than a guess).
            const std::size_t open = w.raw.find('(');
            if (open == std::string::npos) {
                return {};
            }
            const std::size_t close = w.raw.find_last_of(')');
            const std::string inside =
                w.raw.substr(open + 1,
                             close != std::string::npos && close > open
                                 ? close - open - 1
                                 : std::string::npos);
            std::vector<double> numbers;
            if (!split_numbers(inside, numbers) || numbers.size() < 2u) {
                return {};
            }
            char text[64];
            std::snprintf(text, sizeof text, "%.12g", numbers[1]);
            return text;
        }
        case SourceWave::Kind::None:
        case SourceWave::Kind::Raw:
            return {};
    }
    return {};
}

// --- the preview's sampler (the pure half: side_panels.hpp) --------------------

namespace {

// The curve forms' sample budget: SIN and EXP land on this many
// uniform points (their breaks ride beside it), ~2 px apart on a
// panel-width preview - past that the polyline is denser than the
// screen and the extra points are pure tessellation cost.
constexpr int kCurveSamples = 160;

// SIN's value at t: vo before the delay, then the damped oscillation
// ngspice's manual spells - the phase is DEGREES on the deck (SPICE's
// own convention) and radians inside sin().
[[nodiscard]] double sin_value(const SourceWave& w, double t) {
    const double vo = w.p[0], va = w.p[1], freq = w.p[2], td = w.p[3];
    const double theta = w.p[4], phase = w.p[5];
    if (t < td) {
        return vo;
    }
    const double since = t - td;
    const double damp = theta != 0.0 ? std::exp(-theta * since) : 1.0;
    const double rad = 2.0 * 3.14159265358979323846 * freq * since +
                       phase * 3.14159265358979323846 / 180.0;
    return vo + va * damp * std::sin(rad);
}

// One PULSE cycle's value at tau (seconds past the cycle's start);
// tau >= 0. The branch order IS the ngspice grammar's: ramp, plateau,
// fall, rest. A zero TR or TF never divides (tau < tr with tau >= 0
// requires tr > 0), so a zero-width edge is the instant jump its
// absent ramp means.
[[nodiscard]] double pulse_once(double tau, double v1, double v2,
                                double tr, double tf, double pw) {
    if (tau < tr) {
        return v1 + (v2 - v1) * tau / tr;
    }
    tau -= tr;
    if (tau < pw) {
        return v2;
    }
    tau -= pw;
    if (tau < tf) {
        return v2 + (v1 - v2) * tau / tf;
    }
    return v1;
}

// PULSE's value at t: v1 before the delay, then the cycle the modulo
// picks (PER > 0 repeats; PER <= 0 is the single shot ngspice draws
// once and holds v1 after).
[[nodiscard]] double pulse_value(const SourceWave& w, double t) {
    const double v1 = w.p[0], v2 = w.p[1], td = w.p[2];
    const double tr = w.p[3], tf = w.p[4], pw = w.p[5], per = w.p[6];
    if (t < td) {
        return v1;
    }
    const double since = t - td;
    if (per > 0.0) {
        return pulse_once(std::fmod(since, per), v1, v2, tr, tf, pw);
    }
    return pulse_once(since, v1, v2, tr, tf, pw);
}

// EXP's approach factor: 1 - e^(-dt/tau), the ngspice segment shape.
// tau == 0 is the instant step (the limit, not a division); the clamp
// keeps a negative tau's explosion finite-or-infinite but never NaN
// (e^x's own overflow answer), so the drawer's run-splitting is the
// whole story.
[[nodiscard]] double exp_factor(double dt, double tau) {
    if (tau == 0.0) {
        return 1.0;
    }
    const double x = -dt / tau;
    if (x > 700.0) {
        return -std::numeric_limits<double>::infinity();
    }
    if (x < -700.0) {
        return 1.0;  // e^x underflowed: settled
    }
    return 1.0 - std::exp(x);
}

// EXP's value at t: v1 until TD1, the rise toward v2 with TAU1, and
// past TD2 the fall's own approach SUBTRACTED from the risen value -
// ngspice's manual keeps both exponentials alive (the tail settles at
// v1, not v2: a rise-and-return, the EXP every scope shows).
[[nodiscard]] double exp_value(const SourceWave& w, double t) {
    const double v1 = w.p[0], v2 = w.p[1];
    const double td1 = w.p[2], tau1 = w.p[3], td2 = w.p[4], tau2 = w.p[5];
    if (t < td1) {
        return v1;
    }
    const double rise = v1 + (v2 - v1) * exp_factor(t - td1, tau1);
    if (t < td2) {
        return rise;
    }
    return rise + (v1 - v2) * exp_factor(t - td2, tau2);
}

// PWL's corners from the raw text: the numbers inside the parens,
// taken as (t, v) pairs. An odd tail is dropped (the mid-typing form
// previews the complete pairs; the deck itself stays verbatim for the
// text row). False when a token is not a number - no honest picture.
[[nodiscard]] bool pwl_corners(const SourceWave& w,
                               std::vector<double>& ts,
                               std::vector<double>& vs) {
    const std::size_t open = w.raw.find('(');
    std::size_t close = w.raw.find_last_of(')');
    if (close == std::string::npos ||
        (open != std::string::npos && close < open)) {
        close = w.raw.size();
    }
    const std::string inside = open == std::string::npos
                                   ? w.raw
                                   : w.raw.substr(open + 1,
                                                  close - open - 1);
    std::vector<double> nums;
    if (!split_numbers(inside, nums)) {
        return false;
    }
    const std::size_t pairs = nums.size() / 2u;
    if (pairs == 0u) {
        return false;  // not even one complete corner
    }
    ts.reserve(pairs);
    vs.reserve(pairs);
    for (std::size_t i = 0; i < pairs; ++i) {
        const double t = nums[2u * i];
        if (t < 0.0) {
            return false;  // before the window's start: no honest clip
        }
        ts.push_back(t);
        vs.push_back(nums[2u * i + 1u]);
    }
    return true;
}

// Adds one sample point (kept even when the value is non-finite - the
// drawer splits the line there, and the y fit skips it).
void push_point(std::vector<double>& ts, std::vector<double>& vs, double t,
                double v) {
    ts.push_back(t);
    vs.push_back(v);
}

// Adds a break's two faces: just-before (the segment ending there) and
// at (the segment starting there - the closed forms answer the right
// side at their boundaries). A continuous form's pair is two samples
// d apart with equal values; a zero-width edge's pair is the vertical
// line the shape deserves.
void push_break(std::vector<double>& ts, std::vector<double>& vs, double b,
                double d,
                const std::function<double(double)>& value_at) {
    if (b - d > 0.0) {
        push_point(ts, vs, b - d, value_at(b - d));
    }
    push_point(ts, vs, b, value_at(b));
}

}  // namespace

WavePreview sample_source_wave(const SourceWave& w, double dc_value) {
    WavePreview out;
    for (double v : w.p) {
        if (!std::isfinite(v)) {
            return out;  // an imported deck can carry what fields gate out
        }
    }
    // The samples per kind, then the shared finish below (sort, flat).
    std::function<double(double)> value_at;
    switch (w.kind) {
        case SourceWave::Kind::None: {
            if (!std::isfinite(dc_value)) {
                return out;  // no level to draw: the value row says it
            }
            out.t_end = 1.0;  // a flat line's window is arbitrary
            push_point(out.ts, out.vs, 0.0, dc_value);
            push_point(out.ts, out.vs, out.t_end, dc_value);
            break;
        }
        case SourceWave::Kind::Sin: {
            const double td = w.p[3];
            const double freq = w.p[2];
            if (freq > 0.0) {
                // Three cycles, or the delay plus one when the delay is
                // the longer story - the window shows the hold AND at
                // least a cycle of what follows it.
                out.t_end = std::max(3.0 / freq, td + 1.0 / freq);
            } else {
                out.t_end = std::max(td, 0.0) + 1.0;  // the offset line
            }
            if (!(out.t_end > 0.0)) {
                out.t_end = 1.0;
            }
            value_at = [&](double t) { return sin_value(w, t); };
            for (int i = 0; i < kCurveSamples; ++i) {
                const double t = out.t_end * static_cast<double>(i) /
                                 static_cast<double>(kCurveSamples - 1);
                push_point(out.ts, out.vs, t, value_at(t));
            }
            // The delay's jump (a phase != 0 sine steps at TD; a phase
            // == 0 one is continuous and the pair costs nothing).
            const double d = out.t_end * 1e-9;
            if (td > 0.0 && td < out.t_end) {
                push_break(out.ts, out.vs, td, d, value_at);
            }
            break;
        }
        case SourceWave::Kind::Pulse: {
            const double td = w.p[2];
            const double tr = w.p[3], tf = w.p[4], pw = w.p[5];
            const double per = w.p[6];
            const double single = tr + pw + tf;
            if (per > 0.0) {
                out.t_end = td + 2.0 * per;  // two full cycles
            } else if (single > 0.0) {
                // The single shot plus a quarter-length tail so the
                // return to v1 reads as the pulse's end, not a cliff.
                out.t_end = td + single + 0.25 * single;
            } else {
                out.t_end = td + 1.0;  // an instant spike: the rest line
            }
            if (!(out.t_end > 0.0)) {
                out.t_end = 1.0;
            }
            value_at = [&](double t) { return pulse_value(w, t); };
            push_point(out.ts, out.vs, 0.0, value_at(0.0));
            // Every cycle's four breaks (delay, ramp-end, plateau-end,
            // fall-end), each as its two faces.
            const double d = out.t_end * 1e-9;
            for (int k = 0; k < 1000; ++k) {
                const double base = td + static_cast<double>(k) * per;
                if (per > 0.0 && base >= out.t_end) {
                    break;  // past the window's cycles
                }
                const double offs[4] = {0.0, tr, tr + pw, tr + pw + tf};
                for (const double off : offs) {
                    const double b = base + off;
                    if (b > 0.0 && b < out.t_end) {
                        push_break(out.ts, out.vs, b, d, value_at);
                    }
                }
                if (per <= 0.0) {
                    break;  // the single shot has no second cycle
                }
            }
            push_point(out.ts, out.vs, out.t_end, value_at(out.t_end));
            break;
        }
        case SourceWave::Kind::Exp: {
            const double td1 = w.p[2], tau1 = w.p[3];
            const double td2 = w.p[4], tau2 = w.p[5];
            // The settling span: past each delay by 5 of its own tau
            // (95% settled), then a 5% tail so the last event is not
            // pinned to the frame's edge.
            double span = std::max({td1, td2, 0.0});
            if (tau1 != 0.0) {
                span = std::max(span, td1 + 5.0 * std::fabs(tau1));
            }
            if (tau2 != 0.0) {
                span = std::max(span, td2 + 5.0 * std::fabs(tau2));
            }
            out.t_end = span > 0.0 ? span * 1.05 : 1.0;
            value_at = [&](double t) { return exp_value(w, t); };
            for (int i = 0; i < kCurveSamples; ++i) {
                const double t = out.t_end * static_cast<double>(i) /
                                 static_cast<double>(kCurveSamples - 1);
                push_point(out.ts, out.vs, t, value_at(t));
            }
            // Both delays' faces (a zero tau steps there).
            const double d = out.t_end * 1e-9;
            for (const double b : {td1, td2}) {
                if (b > 0.0 && b < out.t_end) {
                    push_break(out.ts, out.vs, b, d, value_at);
                }
            }
            break;
        }
        case SourceWave::Kind::Pwl: {
            if (!pwl_corners(w, out.ts, out.vs)) {
                out.ts.clear();
                out.vs.clear();
                return out;  // not drawable
            }
            // ngspice holds v1 before the first corner; the window is
            // the corners' own extent.
            if (out.ts.front() > 0.0) {
                out.ts.insert(out.ts.begin(), 0.0);
                out.vs.insert(out.vs.begin(), out.vs.front());
            }
            out.t_end = out.ts.back();
            if (out.ts.size() < 2u || !(out.t_end > 0.0)) {
                out.ts.clear();
                out.vs.clear();
                return out;  // a single point is not a picture
            }
            break;
        }
        case SourceWave::Kind::Raw:
            return out;  // never a guess over a text kept verbatim
    }

    // Ascending order: the breaks ride beside the grid and the pairs
    // interleave. Equal times (a grid point ON a break) keep both -
    // duplicates are a zero-length segment, harmless to the polyline.
    std::vector<std::size_t> order(out.ts.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return out.ts[a] < out.ts[b];
    });
    std::vector<double> ts2;
    std::vector<double> vs2;
    ts2.reserve(out.ts.size());
    vs2.reserve(out.vs.size());
    for (const std::size_t i : order) {
        ts2.push_back(out.ts[i]);
        vs2.push_back(out.vs[i]);
    }
    out.ts = std::move(ts2);
    out.vs = std::move(vs2);

    // The shared finish: a finite sample must exist (a fully exploded
    // form has no picture either), and `flat` means every sample
    // finite AND equal - the one-line-one-label case (a non-finite
    // sample breaks the line, so it is not a labeled level either).
    bool any_finite = false;
    bool all_equal = true;
    double first = 0.0;
    for (const double v : out.vs) {
        if (!std::isfinite(v)) {
            all_equal = false;
            continue;
        }
        if (!any_finite) {
            first = v;
            any_finite = true;
        } else if (v != first) {
            all_equal = false;
        }
    }
    out.flat = any_finite && all_equal;
    out.drawable = any_finite && out.ts.size() >= 2u;
    return out;
}

// --- the directives split/join (side_panels.hpp's contract) -------------------

std::vector<std::string> split_directive_lines(const std::string& text) {
    // SEPARATOR semantics (the netlist panel's own loop shape): the
    // lines BETWEEN the newlines, so a trailing newline yields a
    // trailing EMPTY line and an empty text is one empty line. The
    // `at < size` loop this replaces dropped whatever followed the
    // final newline, so "a\n" reloaded as one line - and with the
    // join stripping the tail, a row cleared to empty vanished on
    // the next reload.
    std::vector<std::string> lines;
    if (text.empty()) {
        lines.push_back("");
        return lines;
    }
    std::size_t at = 0;
    while (at <= text.size()) {
        const std::size_t nl = text.find('\n', at);
        lines.push_back(text.substr(
            at, nl == std::string::npos ? std::string::npos : nl - at));
        if (nl == std::string::npos) {
            break;
        }
        at = nl + 1;
    }
    return lines;
}

std::string join_directive_lines(const std::vector<std::string>& lines) {
    // The matching half: newline as a SEPARATOR, never a terminator -
    // a final empty line keeps its newline ({"a", ""} is "a\n"), a
    // final non-empty line adds none ({"a"} stays "a"), and the
    // round-trip preserves every row.
    std::string joined;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i != 0) {
            joined += '\n';
        }
        joined += lines[i];
    }
    return joined;
}

bool remove_selected_analysis(Editor& ed) {
    if (ed.doc == nullptr || ed.schem() == nullptr) {
        return false;
    }
    // The guard the button owes its caller: NO selection (index < 0)
    // removes nothing - the old path cast -1 to size_t (SIZE_MAX) and
    // let std::min clamp it to the last entry, so "remove selected"
    // with nothing selected deleted the LAST analysis. A stale index
    // past the end (the list shrank under it) is equally not a
    // selection.
    if (ed.analysis_index < 0) {
        return false;
    }
    const std::size_t at = static_cast<std::size_t>(ed.analysis_index);
    if (at >= ed.schem()->analysis.entries.size()) {
        return false;
    }
    push_remove_analysis(*ed.doc, at);
    ed.analysis_index = -1;
    ed.bump_edit_epoch();
    return true;
}

// --- the problems walk (side_panels.hpp's contract) ---------------------------

bool problems_filter_shows(ws::circsim::Severity severity, bool show_error,
                           bool show_warning, bool show_note) {
    switch (severity) {
        case ws::circsim::Severity::Error:
            return show_error;
        case ws::circsim::Severity::Warning:
            return show_warning;
        case ws::circsim::Severity::Note:
            return show_note;
    }
    return false;  // a severity this build does not know stays hidden
}

bool problems_any_visible(const Editor& ed) {
    if (!ed.problems.has) {
        return false;  // no check yet: nothing to walk through
    }
    for (const ws::circsim::Diag& d : ed.problems.diags) {
        if (problems_filter_shows(d.severity, ed.problems_show_error,
                                  ed.problems_show_warning,
                                  ed.problems_show_note)) {
            return true;
        }
    }
    return false;
}

int step_problem_index(int cursor, std::size_t count, bool forward,
                       const std::function<bool(std::size_t)>& shows) {
    if (count == 0) {
        return -1;
    }
    const int n = static_cast<int>(count);
    // Where the first probe lands: off the start when nothing is under
    // the cursor yet (or the cursor went stale past a shrunken list -
    // that reads as none too), one past `cursor` (either way) when
    // something is - the modulo folds a cursor at either end back into
    // the walk, which is the cycling itself.
    const int start =
        cursor < 0 || cursor >= n
            ? (forward ? 0 : n - 1)
            : (forward ? (cursor + 1) % n : (cursor - 1 + n) % n);
    // Every index exactly once, in walk order, wrapping; the first one
    // that passes is the landing.
    for (int step = 0; step < n; ++step) {
        const int at = forward ? (start + step) % n : (start - step + n) % n;
        if (shows(static_cast<std::size_t>(at))) {
            return at;
        }
    }
    return -1;
}

std::string join_deck_lines(const std::vector<std::string>& lines) {
    // The deck IS the rows in order with single newlines between -
    // join_directive_lines' own rule, delegated to rather than
    // rewritten so the two joins can never drift apart.
    return join_directive_lines(lines);
}

// --- the palette --------------------------------------------------------------

namespace {

// The Palette's tile grid: a tile is ~52 px wide (the columns stretch
// to fill the panel), 44 px tall - the symbol's picture fitted inside,
// the name in a tooltip.
constexpr float kTileTargetW = 52.0f;
constexpr float kTileH = 44.0f;
constexpr float kTilePad = 6.0f;

// The symbols armed most recently, newest first, capped at six -
// Editor::palette_recent's own policy (the browse row's whole
// window).
constexpr std::size_t kRecentMax = 6;

// The note itself: a key already in the history moves to the front
// (re-arming is the strongest recency signal there is), a new one
// enters at the front, and the tail falls past the cap.
void note_recent_symbol(Editor& ed, const std::string& key) {
    std::vector<std::string>& r = ed.palette_recent;
    r.erase(std::remove(r.begin(), r.end(), key), r.end());
    r.insert(r.begin(), key);
    if (r.size() > kRecentMax) {
        r.resize(kRecentMax);
    }
}

// One symbol tile; true on a click (or Enter/Space while focused). The
// caller owns the id scope: the tile's id and its tooltip's are both
// "tile" under the scope it pushed.
[[nodiscard]] bool symbol_tile(Editor& ed, gui::GuiContext& gui,
                               gui::LayoutCursor& layout,
                               const gui::ChromeTheme& theme,
                               const ws::circsim::SymbolDef& sym,
                               float width, bool armed) {
    const gui::Rect r = layout.next(width, kTileH);
    const auto id = gui.ids().id("tile");
    const gui::SelectableResult res = gui::selectable_behavior(gui, id, r);

    canvas::Canvas2D& rc = ed.palette_draw_canvas();
    rc.set_view_scale(1.0f);  // the transform below carries the scale
    std::uint32_t face = theme.control_bg;
    if (res.hit.pressed) {
        face = theme.control_pressed;
    } else if (res.hit.hovered) {
        face = theme.control_hovered;
    }
    rc.fill_rounded_rect(r.x, r.y, r.width, r.height, theme.corner_radius,
                         face);
    if (armed) {
        // The armed tile: an accent wash and outline (a selectable's
        // accent bar, grown to fit a tile).
        rc.fill_rounded_rect(r.x, r.y, r.width, r.height,
                             theme.corner_radius,
                             gui::dim(theme.control_on, 0.28f));
        canvas::Paint outline;
        outline.width = 1.5f;
        outline.color = theme.control_on;
        rc.stroke_rounded_rect(r.x + 0.75f, r.y + 0.75f, r.width - 1.5f,
                               r.height - 1.5f, theme.corner_radius, outline);
    } else if (res.focused) {
        canvas::Paint ring;
        ring.width = 1.0f;
        ring.color = gui::dim(theme.control_on, 0.7f);
        rc.stroke_rounded_rect(r.x + 0.5f, r.y + 0.5f, r.width - 1.0f,
                               r.height - 1.0f, theme.corner_radius, ring);
    }

    // The picture: fitted to the tile and centered on the symbol's own
    // bounds (pins included), so an asymmetric symbol sits in the middle
    // instead of hanging off its origin. No font - a tile has no text.
    const Bounds b = ws::circsim::symbol_bounds(sym);
    const double bw = std::max(1.0, b.max_x - b.min_x);
    const double bh = std::max(1.0, b.max_y - b.min_y);
    const double fit = std::min(
        18.0, std::min(static_cast<double>(r.width - 2.0f * kTilePad) / bw,
                       static_cast<double>(r.height - 2.0f * kTilePad) / bh));
    canvas::Paint paint;
    paint.width = 1.5f;
    paint.color = armed ? theme.control_on : gui::dim(theme.text, 0.85f);
    rc.push_transform();
    rc.translate(r.x + r.width * 0.5f, r.y + r.height * 0.5f);
    rc.scale(static_cast<float>(fit), -static_cast<float>(fit));
    rc.translate(static_cast<float>(-(b.min_x + b.max_x) * 0.5),
                 static_cast<float>(-(b.min_y + b.max_y) * 0.5));
    draw_symbol(rc, sym, Pos{0.0, 0.0}, 0, false, paint, 0u, nullptr);
    rc.pop_transform();

    const std::string hint = sym.name + " (" + sym.key + ")";
    gui::tooltip_text(gui, layout, theme, "tile", r, hint.c_str());
    return res.clicked;
}

}  // namespace

void draw_palette_panel(Editor& ed, gui::GuiContext& gui,
                        gui::LayoutCursor& layout,
                        const gui::ChromeTheme& theme) {
    // The section header: the strong face, a step larger than the body
    // (the font hierarchy's loud rung - the tools list reads as a
    // section, not a longer label).
    gui::heading(layout, theme, "Tools");
    if (gui::selectable(gui, layout, theme, "Select",
                        ed.tool == Tool::Select)) {
        ed.tool = Tool::Select;
        ed.wire_draft = Editor::WireDraft{};
    }
    if (gui::selectable(gui, layout, theme, "wire (W)",
                        ed.tool == Tool::Wire)) {
        ed.tool = Tool::Wire;
        ed.wire_draft = Editor::WireDraft{};
    }
    if (gui::selectable(gui, layout, theme, "label (L)",
                        ed.tool == Tool::Label)) {
        ed.tool = Tool::Label;
    }
    if (ed.tool == Tool::Label) {
        // Tracked, so typing a net name never fires the bare panel keys.
        (void)text_input_tracked(ed, gui, layout, theme,
                                 std::string{"Net name##tool.label"},
                                 ed.label_text,
                                 static_cast<int>(sizeof ed.label_text));
    }
    if (ed.tool == Tool::Place) {
        gui::text(layout, theme, ("Placing: " + ed.place_symbol).c_str());
        hint_line(layout, theme,
                  "Click to place - R rotate - M mirror - Esc cancel");
    }
    gui::separator(layout, theme);

    // The search box. The label is its placeholder; Enter arms the
    // first match, so "res" Enter drops straight into placing a resistor.
    // With text in the box the field shrinks to make room for a Clear
    // button (no hunting for the backspace key to get the palette back).
    const bool had_filter = ed.palette_filter[0] != '\0';
    (void)text_input_tracked(
        ed, gui, layout, theme, std::string{"Search symbols"},
        ed.palette_filter, static_cast<int>(sizeof ed.palette_filter),
        had_filter ? std::max(60.0f, layout.remaining_width() - 64.0f)
                   : -1.0f);
    if (had_filter) {
        layout.same_line();
        if (gui::button(gui, layout, theme, "Clear", 56.0f)) {
            ed.palette_filter[0] = '\0';
        }
    }
    const bool filter_focused =
        gui.is_focused(gui.ids().id("Search symbols"));
    const std::string_view filter{ed.palette_filter};
    const bool filtering = !filter.empty();
    const std::vector<PaletteGroup> groups =
        palette_groups(ed.library, filter);

    const auto arm = [&ed](const ws::circsim::SymbolDef& sym) {
        ed.tool = Tool::Place;
        ed.place_symbol = sym.key;
        ed.ghost_rot = 0;
        ed.ghost_mirror = false;
        note_recent_symbol(ed, sym.key);
    };
    if (filter_focused && !groups.empty() && !groups.front().items.empty() &&
        gui.input().key_pressed[ws::input::keys::kEnter] != 0) {
        arm(*groups.front().items.front());
    }
    // Esc while the box holds the keyboard clears the filter - one
    // press back to browsing, no backspace hunt. (The field's own Esc
    // only clears its selection, and the chord loop's app.escape
    // cannot fire while a field is focused - the text rule - so the
    // three never collide.)
    if (filter_focused &&
        gui.input().key_pressed[ws::input::keys::kEscape] != 0) {
        ed.palette_filter[0] = '\0';
    }

    if (groups.empty()) {
        gui::text(layout, theme,
                  ("No symbols match \"" + std::string{filter} + "\"")
                      .c_str());
        hint_line(layout, theme, "Try a shorter name, e.g. res, cap or gnd");
    }

    // The columns stretch the tiles to fill the panel's width.
    const float gap = theme.spacing;
    const float avail = std::max(kTileTargetW, layout.remaining_width());
    const int cols = std::max(
        1, static_cast<int>((avail + gap) / (kTileTargetW + gap)));
    const float tile_w =
        (avail - static_cast<float>(cols - 1) * gap) /
        static_cast<float>(cols);

    int tile_index = 0;
    // One wrapped run of tiles; the id scope is the running tile index.
    const auto tile_run = [&](const auto& items) {
        std::size_t n = 0;
        for (const ws::circsim::SymbolDef* sym : items) {
            if (n % static_cast<std::size_t>(cols) != 0) {
                layout.same_line();
            }
            ++n;
            gui.ids().push(tile_index++);
            const bool armed =
                ed.tool == Tool::Place && ed.place_symbol == sym->key;
            if (symbol_tile(ed, gui, layout, theme, *sym, tile_w, armed)) {
                arm(*sym);
            }
            gui.ids().pop();
        }
    };

    // Recent: the last few symbols armed, one click from re-arming.
    // Browsing only - a search shows its matches alone.
    if (!filtering) {
        std::vector<const ws::circsim::SymbolDef*> recent;
        for (const std::string& key : ed.palette_recent) {
            for (const PaletteGroup& g : groups) {
                const auto it = std::find_if(
                    g.items.begin(), g.items.end(),
                    [&key](const ws::circsim::SymbolDef* s) {
                        return s->key == key;
                    });
                if (it != g.items.end()) {
                    recent.push_back(*it);
                    break;
                }
            }
        }
        if (!recent.empty()) {
            gui::text(layout, theme, "Recent");
            tile_run(recent);
        }
    }

    int group_index = 0;
    for (const PaletteGroup& g : groups) {
        gui.ids().push(group_index++);
        // A search shows every match, open; browsing remembers what the
        // user collapsed. "##name" keeps the header's id steady while
        // the count in its label changes with the filter.
        bool open = filtering || ed.palette_closed.count(g.name) == 0;
        const bool was_open = open;
        const std::string header = g.name + " (" +
                                   std::to_string(g.items.size()) + ")##" +
                                   g.name;
        (void)gui::collapsing_header(gui, layout, theme, header.c_str(),
                                     open);
        if (!filtering && open != was_open) {
            if (open) {
                ed.palette_closed.erase(g.name);
            } else {
                ed.palette_closed.insert(g.name);
            }
        }
        if (open) {
            tile_run(g.items);
        }
        gui.ids().pop();
    }
}

// --- the properties -------------------------------------------------------------

namespace {

// The Properties panel's tab strip labels (index-matched to
// Editor::properties_tab): the selection's rows, the exported deck,
// the schematic's own setup.
constexpr const char* const kPropertyTabs[] = {"Properties", "Netlist",
                                               "Simulation"};

// True when the symbol is a source the waveform editor can drive: a
// V/I Spice letter AND the `wave` param the editor composes into (a
// .csym source without one keeps the generic rows - the editor never
// writes a param the symbol's emit list would drop).
[[nodiscard]] bool source_wave_editable(const ws::circsim::SymbolDef& sym) {
    if (sym.spice_letter != 'V' && sym.spice_letter != 'I') {
        return false;
    }
    for (const ws::circsim::ParamSpec& spec : sym.params) {
        if (spec.key == "wave") {
            return true;
        }
    }
    return false;
}

// The source's stimulus, parsed from its `wave` param - the mode
// dropdown's own truth (a None kind is DC mode; every other kind,
// Raw included, is AC).
[[nodiscard]] SourceWave source_wave_of(const ws::circsim::Part& p) {
    const auto it = p.params.find("wave");
    return it != p.params.end() ? parse_source_wave(it->second)
                                : SourceWave{};
}

// The waveform format combo's items, index-matched to the template a
// switch lands (parse_source_wave's Kind order, Pwl last because its
// body stays the free-form text row). No "DC only" item: the MODE
// dropdown above owns that choice.
constexpr const char* const kWaveFormats[] = {"Sine", "Pulse",
                                              "Exponential", "Piecewise"};

// One slot's label per kind: ngspice's parameter names, spelled for a
// human (the SPICE token stays in the parens - the row teaches the
// deck's word while it speaks the user's).
struct WaveFieldLabel {
    const char* label;
};
constexpr WaveFieldLabel kSinLabels[6] = {
    {"Offset (VO)"},   {"Amplitude (VA)"}, {"Frequency (FREQ)"},
    {"Delay (TD)"},    {"Damping (THETA)"}, {"Phase (PHASE)"}};
constexpr WaveFieldLabel kPulseLabels[7] = {
    {"Initial (V1)"},  {"Pulsed (V2)"},    {"Delay (TD)"},
    {"Rise (TR)"},     {"Fall (TF)"},      {"Width (PW)"},
    {"Period (PER)"}};
constexpr WaveFieldLabel kExpLabels[6] = {
    {"Initial (V1)"},    {"Pulsed (V2)"},      {"Rise delay (TD1)"},
    {"Rise tau (TAU1)"}, {"Fall delay (TD2)"}, {"Fall tau (TAU2)"}};

// --- the waveform preview (the picture of what the fields shape) -------------

// The preview's geometry: a fixed-height flow rect the canvas draws
// into in screen space (the Results plot's own spelling; the scroll
// panel's clip keeps it inside the panel), the labels' margins around
// its frame.
constexpr float kPreviewHeight = 88.0f;
constexpr float kPreviewMarginLeft = 44.0f;    // the y min/max labels
constexpr float kPreviewMarginBottom = 18.0f;  // the window's end label
constexpr float kPreviewMarginTop = 6.0f;
constexpr float kPreviewMarginRight = 8.0f;

// The series, split at non-finite samples (a negative tau explodes;
// the line stops there instead of poisoning the tessellator with NaN
// vertices) - the Results panel's own run rule.
void stroke_preview_series(canvas::Canvas2D& rc,
                           const std::vector<canvas::Point>& pts,
                           std::uint32_t color) {
    canvas::Paint paint;
    paint.width = 2.0f;
    paint.color = color;
    paint.join = canvas::StrokeJoin::round;
    paint.cap = canvas::StrokeCap::round;
    std::size_t run = 0;
    for (std::size_t i = 0; i <= pts.size(); ++i) {
        const bool break_here = i == pts.size() || std::isnan(pts[i].x) ||
                                std::isnan(pts[i].y);
        if (break_here && run >= 2u) {
            rc.stroke_polyline(pts.data() + (i - run),
                               static_cast<int>(run), paint);
        }
        if (i == pts.size()) {
            break;
        }
        run = break_here ? 0u : run + 1u;
    }
}

// The preview: the waveform's own window mapped into a shallow well -
// the frame, the zero line when zero is in range, the series in the
// accent the selected tab's own underline wears, and the labels the
// font can carry (a fontless theme draws the shape alone). No
// interaction: it is a picture of the model, and the fields below it
// are the hands.
void draw_wave_preview(Editor& ed, gui::LayoutCursor& layout,
                       const gui::ChromeTheme& theme,
                       const WavePreview& pv) {
    const gui::Rect box =
        layout.next(layout.remaining_width(), kPreviewHeight);
    const float iw =
        box.width - kPreviewMarginLeft - kPreviewMarginRight;
    const float ih =
        box.height - kPreviewMarginTop - kPreviewMarginBottom;
    if (iw < 24.0f || ih < 16.0f) {
        return;  // too narrow to be worth a frame
    }
    const float fx = box.x + kPreviewMarginLeft;
    const float fy = box.y + kPreviewMarginTop;

    canvas::Canvas2D& rc = ed.properties_draw_canvas();
    rc.set_view_scale(1.0f);  // identity: local == screen

    // The well: the preview sits in a shallow control face so the rows
    // around it cannot read as its labels.
    rc.fill_rounded_rect(box.x, box.y, box.width, box.height,
                         theme.corner_radius, theme.control_bg);
    // The frame.
    canvas::Paint frame;
    frame.width = 1.0f;
    frame.color = gui::dim(theme.text, 0.35f);
    const canvas::Point quad[4] = {
        {fx, fy}, {fx + iw, fy}, {fx + iw, fy + ih}, {fx, fy + ih}};
    rc.stroke_polyline(quad, 4, frame, true);

    // The y fit: the finite samples, padded by plot's expand_range.
    double lo = std::numeric_limits<double>::infinity();
    double hi = -std::numeric_limits<double>::infinity();
    for (const double v : pv.vs) {
        if (std::isfinite(v)) {
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
    }
    if (lo > hi) {
        return;  // nothing finite (the sampler's own guard, again)
    }
    const ws::circsim::PlotRange yr = ws::circsim::expand_range(lo, hi);
    const auto sx = [&](double t) {
        return static_cast<float>(fx + t / pv.t_end * iw);
    };
    const auto sy = [&](double v) {
        return static_cast<float>(fy + (yr.hi - v) / (yr.hi - yr.lo) * ih);
    };

    // The zero line, when zero is in range - the reference a
    // waveform reads against.
    if (yr.lo < 0.0 && yr.hi > 0.0) {
        canvas::Paint zero;
        zero.width = 1.0f;
        zero.color = gui::dim(theme.text, 0.2f);
        rc.stroke_line({fx, sy(0.0)}, {fx + iw, sy(0.0)}, zero);
    }

    // The series.
    std::vector<canvas::Point> pts;
    pts.reserve(pv.ts.size());
    for (std::size_t i = 0; i < pv.ts.size(); ++i) {
        const double v = pv.vs[i];
        if (!std::isfinite(v)) {
            pts.push_back({std::numeric_limits<float>::quiet_NaN(),
                           std::numeric_limits<float>::quiet_NaN()});
            continue;
        }
        pts.push_back({sx(pv.ts[i]), sy(v)});
    }
    stroke_preview_series(rc, pts, theme.control_on);

    // The labels: the face the panel's own rows wear.
    const ws::ui::r2d::Font* font =
        theme.font != nullptr ? theme.font : ed.font;
    if (font == nullptr || font->line_height() <= 0.0f) {
        return;  // the shape alone still says the shape
    }
    const std::uint32_t label = gui::dim(theme.text, 0.55f);
    if (pv.flat) {
        // One line, one label: the level itself, just above the line
        // (a flat form sits at the frame's vertical center by
        // construction - expand_range pads it symmetrically).
        const std::string text =
            ws::circsim::format_eng(pv.vs.front());
        const float w = rc.measure_text(text.c_str(), *font);
        (void)rc.draw_text(fx + (iw - w) * 0.5f,
                           sy(pv.vs.front()) - font->line_height() - 2.0f,
                           text.c_str(), *font, label);
        return;
    }
    const float lh = font->line_height();
    // y max / y min, right-aligned into the left margin.
    const std::string ymax = ws::circsim::format_eng(yr.hi);
    const std::string ymin = ws::circsim::format_eng(yr.lo);
    const float wmax = rc.measure_text(ymax.c_str(), *font);
    const float wmin = rc.measure_text(ymin.c_str(), *font);
    (void)rc.draw_text(fx - 5.0f - wmax, fy, ymax.c_str(), *font, label);
    (void)rc.draw_text(fx - 5.0f - wmin, fy + ih - lh, ymin.c_str(), *font,
                       label);
    // The window's end, under the frame's right corner (its start is
    // 0 - the frame's own left edge says it).
    const std::string end = ws::circsim::format_eng(pv.t_end) + "s";
    const float wend = rc.measure_text(end.c_str(), *font);
    (void)rc.draw_text(fx + iw - wend, fy + ih + 2.0f, end.c_str(), *font,
                       label);
}

// The source editor: ONE dropdown picks the stimulus mode, then only
// that mode's fields exist. DC mode is the DC value row (with its
// flat-level picture) and nothing else - no AC rows, no waveform
// section. AC mode is the waveform - a format picker and one number
// field per parameter, the whole `wave` param text
// composed/decomposed through parse/compose so the deck line is never
// hand-typed - and the form's own first number IS the DC offset (the
// engine solves the bias point from waveform(0) when the card spells
// no value), so no DC row appears beside it either: never two places
// for one number. Every edit lands through push_part_text or
// push_source_mode, so the undo stack and the coalescing are the rows'
// own story everywhere else in this panel.
void source_wave_editor(Editor& ed, gui::GuiContext& gui,
                        gui::LayoutCursor& layout,
                        const gui::ChromeTheme& theme,
                        const ws::circsim::Part& p) {
    panel_section(layout, theme, "Source setup");

    // The waveform (the `wave` param) - the mode's own truth.
    const auto wave_at = p.params.find("wave");
    const std::string wave_text =
        wave_at != p.params.end() ? wave_at->second : "";
    const SourceWave wave = parse_source_wave(wave_text);
    const bool ac_mode = wave.kind != SourceWave::Kind::None;

    // The mode selector: selected re-derives from the model every
    // frame (the view-of-model rule every combo in this panel keeps);
    // a REAL move lands the flip as ONE undo step through
    // push_source_mode - both halves travel together, so one Ctrl+Z
    // reverts the whole switch.
    static const char* const kModes[] = {"DC (constant)",
                                         "AC (waveform)"};
    int selected = ac_mode ? 1 : 0;
    const std::string mode_key = part_field_key(p.id, 2, "wave") + ".mode";
    FieldBuf& mode_f = field_of(ed, mode_key);
    if (mode_f.stamp != ed.field_stamp) {
        mode_f.stamp = ed.field_stamp;
        mode_f.combo_open = false;
    }
    if (gui::combo(gui, layout, theme, ("Mode##" + mode_key).c_str(),
                   kModes, 2, selected, mode_f.combo_open)) {
        if (selected == 1 && !ac_mode) {
            // DC -> AC: a Sine template carrying the current level as
            // its offset (nothing the user shaped is lost), and the DC
            // value cleared - the waveform is the whole stimulus now.
            SourceWave next;
            next.kind = SourceWave::Kind::Sin;
            next.p[0] = ws::spice::parse_number(p.value).value_or(0.0);
            next.p[1] = 1.0;
            next.p[2] = 1000.0;
            push_source_mode(*ed.doc, p.id, "",
                             compose_source_wave(next));
        } else if (selected == 0 && ac_mode) {
            // AC -> DC: the wave's own baseline becomes the DC value;
            // the wave param itself clears.
            push_source_mode(*ed.doc, p.id, source_wave_baseline(wave),
                             "");
        }
        ed.bump_edit_epoch();  // the rows reload for the new mode
        return;  // the model moved: the next frame draws the new mode
    }

    if (!ac_mode) {
        // DC mode: the value row with its flat-level picture is the
        // whole editor. A stale AC magnitude (set before a switch, or
        // ridden in from an older file) stays editable - out of the
        // way, never silently dropped.
        part_text_row(ed, gui, layout, theme, "DC value", p.id, 1, "",
                      p.value);
        const auto level = ws::spice::parse_number(p.value);
        if (level.has_value()) {
            const WavePreview flat = sample_source_wave(wave, *level);
            if (flat.drawable) {
                draw_wave_preview(ed, layout, theme, flat);
            }
        }
        hint_line(layout, theme,
                  "A steady level - .op solves it, .dc sweeps it");
        const auto ac_at = p.params.find("ac");
        if (ac_at != p.params.end() && !ac_at->second.empty()) {
            part_text_row(ed, gui, layout, theme, "AC magnitude", p.id,
                          2, "ac", ac_at->second);
            hint_line(layout, theme,
                      "Small-signal level for .ac sweeps - blank means "
                      "none");
        }
        return;
    }

    // A stale DC value (an older file's "5" beside the wave, or a deck
    // that spelled both): the engine reads the explicit DC FIRST for
    // the bias point, so it would pin .op over the waveform's offset -
    // named and clearable here, never silently kept.
    if (!p.value.empty()) {
        hint_line(layout, theme,
                  "DC value '" + p.value +
                      "' still pins the .op bias over the waveform's "
                      "offset");
        if (gui::button(gui, layout, theme, "Clear DC value")) {
            push_source_mode(*ed.doc, p.id, "", wave_text);
            ed.bump_edit_epoch();
        }
    }

    // The format picker: selected re-derives from the model every
    // frame (the view-of-model rule the analysis combo keeps); a REAL
    // move lands the picked format's editable template - sane numbers
    // the user then shapes, never a bare "SIN(".
    selected = wave.kind == SourceWave::Kind::Sin   ? 0
               : wave.kind == SourceWave::Kind::Pulse ? 1
               : wave.kind == SourceWave::Kind::Exp   ? 2
               : wave.kind == SourceWave::Kind::Pwl   ? 3
                                                      : -1;
    const std::string fmt_key = part_field_key(p.id, 2, "wave") + ".fmt";
    FieldBuf& fmt = field_of(ed, fmt_key);
    if (fmt.stamp != ed.field_stamp) {
        fmt.stamp = ed.field_stamp;
        fmt.combo_open = false;
    }
    if (gui::combo(gui, layout, theme,
                   ("Waveform##" + fmt_key).c_str(), kWaveFormats, 4,
                   selected, fmt.combo_open) &&
        selected >= 0) {
        SourceWave next;  // the editable templates, one per format
        switch (selected) {
            case 0:
                next.kind = SourceWave::Kind::Sin;
                next.p[0] = 0.0; next.p[1] = 1.0; next.p[2] = 1000.0;
                break;  // SIN(0 1 1k): a 1V 1kHz sine
            case 1:
                next.kind = SourceWave::Kind::Pulse;
                next.p[0] = 0.0; next.p[1] = 5.0;
                next.p[3] = 1e-6; next.p[4] = 1e-6;
                next.p[5] = 1e-3; next.p[6] = 2e-3;
                break;  // PULSE(0 5 0 1u 1u 1m 2m): a 5V square wave
            case 2:
                next.kind = SourceWave::Kind::Exp;
                next.p[0] = 0.0; next.p[1] = 5.0;
                next.p[3] = 1e-5; next.p[4] = 1e-3; next.p[5] = 1e-4;
                break;  // EXP(0 5 0 10u 1m 100u): a rise-and-settle step
            case 3:
                next.kind = SourceWave::Kind::Pwl;
                next.raw = "PWL(0 0 1m 5)";
                break;  // corners: time/value pairs, free-form below
        }
        push_part_text(*ed.doc, p.id, 2, "wave",
                       compose_source_wave(next), "Set waveform");
        ed.bump_edit_epoch();  // the field rows reload from the template
    }

    // The preview: the picture of what the fields below are shaping,
    // re-derived from the model every frame (the fields' own
    // view-of-model rule - a field edit lands and the very next draw
    // shows the new shape). A form the sampler cannot picture draws
    // nothing, never a guess: Raw, an unreadable PWL.
    const double dc_level = [&] {
        const auto parsed = ws::spice::parse_number(p.value);
        return parsed.has_value()
                   ? *parsed
                   : std::numeric_limits<double>::quiet_NaN();
    }();
    const WavePreview preview = sample_source_wave(wave, dc_level);
    if (preview.drawable) {
        draw_wave_preview(ed, layout, theme, preview);
        if (!preview.flat) {
            // The window's span: what the x axis is showing (a flat
            // line's window is arbitrary and says nothing).
            hint_line(layout, theme,
                      "preview: 0 to " +
                          ws::circsim::format_eng(preview.t_end) + "s");
        }
    }

    if (wave.kind == SourceWave::Kind::Pwl ||
        wave.kind == SourceWave::Kind::Raw) {
        // Free-form corners (or a text the editor could not parse):
        // the plain text row, verbatim - never a silent rewrite.
        part_text_row(ed, gui, layout, theme, "Waveform", p.id, 2, "wave",
                      wave_text);
        hint_line(layout, theme,
                  wave.kind == SourceWave::Kind::Pwl
                      ? "Piecewise corners: PWL(t1 v1 t2 v2 ...)"
                      : "Not a form the editor reads - kept verbatim");
    } else {
        // One number row per parameter: parse, re-compose the whole
        // form, push; a bad value keeps the old and says so on the
        // status line (the analysis rows' own contract).
        const WaveFieldLabel* labels =
            wave.kind == SourceWave::Kind::Sin
                ? kSinLabels
                : (wave.kind == SourceWave::Kind::Pulse ? kPulseLabels
                                                        : kExpLabels);
        const int slots = wave.kind == SourceWave::Kind::Pulse ? 7 : 6;
        const std::string base = part_field_key(p.id, 2, "wave");
        for (int slot = 0; slot < slots; ++slot) {
            const char* label = labels[slot].label;
            char current_text[64];
            std::snprintf(current_text, sizeof current_text, "%.12g",
                          wave.p[slot]);
            const std::string key = base + "." + label;
            char* buf = field_buf(ed, key, current_text);
            if (text_input_tracked(ed, gui, layout, theme,
                                   std::string{label} + "##" + key, buf,
                                   128)) {
                const auto parsed = ws::spice::parse_number(buf);
                if (parsed.has_value()) {
                    SourceWave next = wave;
                    next.p[slot] = *parsed;
                    push_part_text(*ed.doc, p.id, 2, "wave",
                                   compose_source_wave(next),
                                   "Edit waveform");
                } else {
                    ed.status(std::string{"'"} + buf +
                              "' is not a SPICE number - kept the old value");
                }
            }
        }
        hint_line(layout, theme,
                  "Times accept SPICE units - 1u, 2.5n, 1meg");
    }

    // The AC magnitude (the `ac` param): the small-signal level a .ac
    // sweep reads; blank = none. Last on purpose - it belongs to the
    // frequency-domain analysis, not to the waveform the rows above
    // shape, and in DC mode it only shows when something is already
    // set.
    const auto ac_at = p.params.find("ac");
    part_text_row(ed, gui, layout, theme, "AC magnitude", p.id, 2, "ac",
                  ac_at != p.params.end() ? ac_at->second : "");
    hint_line(layout, theme,
              "Small-signal level for .ac sweeps - blank means none");
}

}  // namespace

void draw_properties_panel(Editor& ed, gui::GuiContext& gui,
                           gui::LayoutCursor& layout,
                           const gui::ChromeTheme& theme) {
    watch_selection(ed);
    ed.text_field_focused = false;  // this frame's draw sets it back
    if (ed.doc == nullptr || ed.schem() == nullptr) {
        gui::text(layout, theme, "No document open");
        return;
    }

    // The tabs instead of the stack: the right panel's three concerns
    // (the selection, the deck, the schematic's setup) each get the
    // full height, so nothing scrolls off under the others. The strip
    // divides the panel's width; the caller's int IS the state.
    const float tab_w =
        std::max(60.0f, layout.remaining_width() / 3.0f);
    (void)gui::tab_bar(gui, layout, theme, "props tabs", kPropertyTabs, 3,
                       ed.properties_tab, tab_w);

    if (ed.properties_tab == 1) {
        // The Netlist tab: the bottom panel's own view, beside the
        // selection (the same cache, the same re-export button - the
        // ids scope under this panel keeps them apart).
        draw_netlist_panel(ed, gui, layout, theme);
        return;
    }
    if (ed.properties_tab == 2) {
        draw_simulation_setup(ed, gui, layout, theme);
        return;
    }

    // --- the Properties tab: the selection's rows
    if (ed.sel.kind == Selection::Kind::None) {
        gui::text(layout, theme, "Nothing selected");
        hint_line(layout, theme, "Click a part, wire or label to edit it");
        return;
    }

    if (ed.sel.kind == Selection::Kind::Part) {
        const ws::circsim::Part* p = ed.schem()->find_part(ed.sel.id);
        if (p == nullptr) {
            ed.sel.clear();
            gui::text(layout, theme, "(the part is gone)");
            return;
        }
        const ws::circsim::SymbolDef* sym = ed.sym_of(*p);
        label_line(layout, theme, "Symbol",
                   sym != nullptr
                       ? sym->name + " (" + sym->key + ")"
                       : p->symbol + " (unknown)");
        part_text_row(ed, gui, layout, theme, "Reference", p->id, 0, "",
                      p->ref);
        // A source's value row lives INSIDE its mode editor, under the
        // Mode dropdown that decides it exists (DC mode shows it, AC
        // mode shows the waveform's own offset instead - never the
        // same number in two rows). Every other part keeps the
        // built-in row right here.
        const bool source_editor =
            sym != nullptr && source_wave_editable(*sym);
        if (sym == nullptr ||
            (!source_editor && (sym->needs_value || !p->value.empty()))) {
            part_text_row(ed, gui, layout, theme, "Value", p->id, 1, "",
                          p->value);
        }
        if (sym != nullptr && !sym->params.empty() && !source_editor) {
            panel_section(layout, theme, "Parameters");
        }
        if (sym != nullptr) {
            for (const ws::circsim::ParamSpec& spec : sym->params) {
                // The source editor owns the ac/wave rows (the picker
                // and its fields); the generic loop keeps every other
                // param a .csym source might carry.
                if (source_editor &&
                    (spec.key == "ac" || spec.key == "wave")) {
                    continue;
                }
                const auto it = p->params.find(spec.key);
                const std::string current =
                    it != p->params.end() ? it->second : "";
                const std::string shown =
                    spec.label.empty() ? spec.key : spec.label;
                if (spec.type == ws::circsim::ParamSpec::Type::Choice &&
                    !spec.choices.empty()) {
                    // The combo row: selected tracks the current value
                    // (ci), the push lands the picked choice.
                    std::vector<const char*> items;
                    int selected = -1;
                    for (std::size_t i = 0; i < spec.choices.size(); ++i) {
                        items.push_back(spec.choices[i].c_str());
                        if (ws::circsim::CiLess{}(spec.choices[i],
                                                  current) == false &&
                            ws::circsim::CiLess{}(current,
                                                  spec.choices[i]) == false) {
                            selected = static_cast<int>(i);
                        }
                    }
                    FieldBuf& field =
                        field_of(ed, part_field_key(p->id, 2, spec.key));
                    if (field.stamp != ed.field_stamp) {
                        field.stamp = ed.field_stamp;
                        field.combo_open = false;
                    }
                    // The "##" rule again (text_input_tracked's note):
                    // two Choice params sharing a display label stay
                    // two combos.
                    if (gui::combo(gui, layout, theme,
                                   (std::string{shown} + "##" +
                                    part_field_key(p->id, 2, spec.key))
                                       .c_str(),
                                   items.data(),
                                   static_cast<int>(items.size()), selected,
                                   field.combo_open) &&
                        selected >= 0) {
                        push_part_text(*ed.doc, p->id, 2, spec.key,
                                       spec.choices[static_cast<std::size_t>(
                                           selected)],
                                       "Set " + shown);
                    }
                } else {
                    part_text_row(ed, gui, layout, theme, shown.c_str(),
                                  p->id, 2, spec.key, current);
                }
            }
        }
        if (source_editor) {
            // The AC magnitude + waveform picker: the netlist's source
            // line without the hand-typed "SIN(0 1 1k)".
            source_wave_editor(ed, gui, layout, theme, *p);
        }
        char where[64];
        std::snprintf(where, sizeof where, "Position: (%.12g, %.12g)",
                      p->pos.x, p->pos.y);
        gui::separator(layout, theme);
        hint_line(layout, theme, where);
        if (gui::button(gui, layout, theme, "Delete part")) {
            push_delete_part(*ed.doc, p->id);
            ed.sel.clear();
        }
        return;
    }

    if (ed.sel.kind == Selection::Kind::Wire) {
        const ws::circsim::Wire* w = ed.schem()->find_wire(ed.sel.id);
        if (w == nullptr) {
            ed.sel.clear();
            return;
        }
        label_line(layout, theme, "Wire " + std::to_string(w->id),
                   endpoint_label(ed, w->a) + " to " + endpoint_label(ed, w->b));
        if (gui::button(gui, layout, theme, "Delete wire")) {
            push_delete_wire(*ed.doc, w->id);
            ed.sel.clear();
        }
        return;
    }

    if (ed.sel.kind == Selection::Kind::Junction) {
        const ws::circsim::Junction* j =
            ed.schem()->find_junction(ed.sel.id);
        if (j == nullptr) {
            ed.sel.clear();
            return;
        }
        char where[64];
        std::snprintf(where, sizeof where, "Junction at (%.12g, %.12g)",
                      j->pos.x, j->pos.y);
        gui::text(layout, theme, where);
        if (gui::button(gui, layout, theme, "Delete junction")) {
            // A junction delete is the first wire through it; find the
            // id BEFORE the command runs (it mutates the deque this
            // walk would still hold).
            std::optional<std::uint32_t> through;
            for (const ws::circsim::Wire& w : ed.schem()->wires()) {
                const ws::circsim::JunctionRef* a =
                    std::get_if<ws::circsim::JunctionRef>(&w.a);
                const ws::circsim::JunctionRef* b =
                    std::get_if<ws::circsim::JunctionRef>(&w.b);
                if ((a != nullptr && a->junction == j->id) ||
                    (b != nullptr && b->junction == j->id)) {
                    through = w.id;
                    break;
                }
            }
            if (through.has_value()) {
                push_delete_wire(*ed.doc, *through);
            }
            ed.sel.clear();
        }
        return;
    }

    if (ed.sel.kind == Selection::Kind::Label) {
        const ws::circsim::Label* l = ed.schem()->find_label(ed.sel.id);
        if (l == nullptr) {
            ed.sel.clear();
            return;
        }
        char* buf = field_buf(ed, "l" + std::to_string(l->id) + ".name",
                              l->name);
        if (text_input_tracked(
                ed, gui, layout, theme,
                "Net name##l" + std::to_string(l->id) + ".name", buf,
                128)) {
            push_label_text(*ed.doc, l->id, buf);
        }
        if (gui::button(gui, layout, theme, "Delete label")) {
            push_delete_label(*ed.doc, l->id);
            ed.sel.clear();
        }
        return;
    }

    // A selection kind the rows above did not cover (None is the
    // tab branch's own early return; a future kind lands here).
    gui::text(layout, theme, "(nothing to edit)");
}

// The Simulation tab: the schematic's own setup - the grid readout,
// the analysis list (add/remove/edit) and the directives block. The
// stack's bottom half, always reachable now that the tabs hold it
// open instead of the selection hiding it.
void draw_simulation_setup(Editor& ed, gui::GuiContext& gui,
                           gui::LayoutCursor& layout,
                           const gui::ChromeTheme& theme) {
    if (ed.doc == nullptr || ed.schem() == nullptr) {
        gui::text(layout, theme, "No document open");
        return;
    }
    gui::heading(layout, theme, "Schematic");
    char grid[64];
    std::snprintf(grid, sizeof grid, "%.12g", ed.schem()->grid);
    label_line(layout, theme, "Grid", grid);
    gui::separator(layout, theme);

    // --- the analysis list
    gui::heading(layout, theme, "Analyses (in deck order)");
    hint_line(layout, theme,
              "Run executes these top to bottom - click one to edit "
              "its parameters");
    if (ed.analysis_index >=
        static_cast<int>(ed.schem()->analysis.entries.size())) {
        ed.analysis_index = -1;  // the list shrank under the selection
    }
    if (ed.schem()->analysis.entries.empty()) {
        hint_line(layout, theme, "None yet - add one below");
    }
    int index = 0;
    for (const ws::circsim::AnalysisSpec& spec :
         ed.schem()->analysis.entries) {
        gui.ids().push(index);
        // The readable face: WHAT kind of simulation this is (the
        // plain word with the SPICE token beside it) on the selectable
        // row, then its own parameters in engineering units under it.
        // The raw card stays in the Netlist tab, where the deck's
        // vocabulary lives.
        const std::string title =
            std::to_string(index + 1) + ". " + analysis_brief(spec);
        const bool selected = ed.analysis_index == index;
        if (gui::selectable(gui, layout, theme, title.c_str(), selected)) {
            ed.analysis_index = index;
            ed.bump_edit_epoch();  // the field rows below reload
        }
        hint_line(layout, theme, analysis_params_brief(spec));
        gui.ids().pop();
        ++index;
    }

    // One button per kind. (The old "add" combo answered only on a real
    // move off its first item, so ".op" - item 0 - could never be added.)
    static const char* const kKinds[] = {"+ op", "+ dc", "+ tran", "+ ac"};
    const float add_w = std::max(
        40.0f, (layout.remaining_width() - 3.0f * theme.spacing) / 4.0f);
    int add_kind = -1;
    for (int k = 0; k < 4; ++k) {
        if (k != 0) {
            layout.same_line();
        }
        if (gui::button(gui, layout, theme, kKinds[k], add_w)) {
            add_kind = k;
        }
    }
    if (add_kind >= 0) {
        ws::circsim::AnalysisSpec spec;
        switch (add_kind) {
            case 1: spec = ws::circsim::DcSpec{}; break;
            // A fresh .tran runs out of the box: tstop 1ms at a 1us
            // step (the classic 1000-point sweep), both editable the
            // moment the entry lands.
            case 2:
                spec = ws::circsim::TranSpec{.tstep = 1e-6,
                                             .tstop = 1e-3};
                break;
            case 3: spec = ws::circsim::AcSpec{}; break;
            default: spec = ws::circsim::OpSpec{}; break;
        }
        push_add_analysis(*ed.doc, spec);
        ed.analysis_index = static_cast<int>(
            ed.schem()->analysis.entries.size() - 1u);
        ed.bump_edit_epoch();
    }

    if (!ed.schem()->analysis.entries.empty()) {
        // Dead until something is selected (the disabled face), and the
        // act itself re-checks.
        if (gui::button(gui, layout, theme, "Remove selected", 120.0f, {},
                        ed.analysis_index < 0)) {
            (void)remove_selected_analysis(ed);
        }
        // A stale index is NOT clamped onto the last entry any more (that
        // edited the wrong analysis); the check above already reset it.
        if (ed.analysis_index >= 0) {
            const std::string editing =
                "Edit analysis: " +
                analysis_brief(
                    ed.schem()->analysis.entries[static_cast<std::size_t>(
                        ed.analysis_index)]);
            panel_section(layout, theme, editing.c_str());
            analysis_fields(ed, gui, layout, theme,
                            static_cast<std::size_t>(ed.analysis_index));
        } else {
            hint_line(layout, theme, "Select an analysis to edit it");
        }
    }

    // --- the directives block: one row per line
    panel_section(layout, theme, "Directives (.model / .include, verbatim)");
    hint_line(layout, theme, "One line per row, e.g. .include models.lib");
    const std::vector<std::string> lines = split_directive_lines(
        ed.schem()->directives);
    constexpr float kRowButtonW = 24.0f;
    int line_at = 0;
    int remove_at = -1;
    for (const std::string& line : lines) {
        gui.ids().push(line_at);
        char* buf = field_buf(ed, "directives." + std::to_string(line_at),
                              line);
        const float field_w = std::max(
            60.0f, layout.remaining_width() - kRowButtonW - theme.spacing);
        if (text_input_tracked(ed, gui, layout, theme,
                               std::string{"line##directives."} +
                                   std::to_string(line_at),
                               buf, 128, field_w)) {
            // The row edited: re-join with the (possibly cleared to
            // empty) row kept in place - the split/join pair's
            // round-trip is what preserves an emptied last line.
            std::vector<std::string> next = lines;
            next[static_cast<std::size_t>(line_at)] = buf;
            push_directives(*ed.doc, join_directive_lines(next));
        }
        layout.same_line();
        // Per-row delete; dead on the lone empty row (nothing to delete).
        if (gui::button(gui, layout, theme, "x", kRowButtonW, {},
                        lines.size() == 1 && line.empty())) {
            remove_at = line_at;
        }
        gui.ids().pop();
        ++line_at;
    }
    if (remove_at >= 0) {
        std::vector<std::string> next = lines;
        next.erase(next.begin() + remove_at);
        push_directives(*ed.doc, join_directive_lines(next));
        ed.bump_edit_epoch();  // the rows' buffers are keyed by position
    }
    if (gui::button(gui, layout, theme, "Add line")) {
        // Through the join, so a new row is a truly empty row (the old
        // path appended a lone space, and on an empty block it produced
        // a single " " row instead of a second one).
        std::vector<std::string> next = lines;
        next.emplace_back();
        push_directives(*ed.doc, join_directive_lines(next));
        ed.bump_edit_epoch();
    }
}

// --- the problems ----------------------------------------------------------------

namespace {

// The one jump every Problems-row act rides - the row click, Prev and
// Next, the F8 pair the app binds: locate through the back-maps (the
// check's nets, the run's export map - whichever halves that check
// produced), then select-and-zoom in one act, with the cursor kept in
// step so the row highlight follows the keyboard onto the same
// finding.
void goto_problem(Editor& ed, std::size_t at) {
    if (at >= ed.problems.diags.size()) {
        return;
    }
    ed.problems_cursor = static_cast<int>(at);
    const ws::circsim::Diag& d = ed.problems.diags[at];
    const ws::circsim::DiagTarget t = ws::circsim::locate_diag(
        d, *ed.schem(),
        ed.problems.nets_valid ? &ed.problems.nets : nullptr,
        ed.problems.map_valid ? &ed.problems.map : nullptr,
        &ed.library);
    ed.jump_to(t);
}

}  // namespace

bool step_problem(Editor& ed, bool forward) {
    // The walk needs a check to walk through; the load report a fresh
    // document shows is the document's own, not the cursor's.
    if (!ed.problems.has || ed.problems.diags.empty()) {
        return false;
    }
    const int next = step_problem_index(
        ed.problems_cursor, ed.problems.diags.size(), forward,
        [&ed](std::size_t i) {
            return problems_filter_shows(
                ed.problems.diags[i].severity, ed.problems_show_error,
                ed.problems_show_warning, ed.problems_show_note);
        });
    if (next < 0) {
        return false;  // the filter hides everything: nowhere to land
    }
    goto_problem(ed, static_cast<std::size_t>(next));
    return true;
}

void draw_problems_panel(Editor& ed, gui::GuiContext& gui,
                         gui::LayoutCursor& layout,
                         const gui::ChromeTheme& theme) {
    if (ed.doc == nullptr || ed.schem() == nullptr) {
        gui::text(layout, theme, "No document open");
        return;
    }

    // The data rows wear the mono face: a finding line is code-ish
    // text (refdeses, net names, [E]/[W]/[N] tags) that reads easier
    // with honest columns, the typography brief's own class. The
    // swapped-theme idiom (the palette fade's own move): every face
    // and metric stays the panel's, only the label font swaps - and a
    // missing mono face keeps the body font, readable either way.
    gui::ChromeTheme rows_theme = theme;
    if (ed.font_mono != nullptr && !ed.font_mono->empty()) {
        rows_theme.font = ed.font_mono;
    }

    const bool stale =
        ed.problems.has && ed.problems.version != ed.schem()->version();

    // The header's severities - ALL of them, filter or no filter (the
    // toggles hide rows, never findings) - beside the rows the filter
    // leaves standing (the walk steps through these and nothing else,
    // so the button pair is dead exactly when there is nothing to
    // land on). One walk feeds the labels, the hint and the buttons.
    std::size_t errors = 0;
    std::size_t warnings = 0;
    std::size_t notes = 0;
    std::size_t visible = 0;
    for (const ws::circsim::Diag& d : ed.problems.diags) {
        if (d.severity == ws::circsim::Severity::Error) {
            ++errors;
        } else if (d.severity == ws::circsim::Severity::Warning) {
            ++warnings;
        } else {
            ++notes;
        }
        if (problems_filter_shows(d.severity, ed.problems_show_error,
                                  ed.problems_show_warning,
                                  ed.problems_show_note)) {
            ++visible;
        }
    }
    const bool can_step = problems_any_visible(ed);

    if (gui::button(gui, layout, theme, "Re-check")) {
        ed.run_erc();
    }
    // The cursor pair beside Re-check: the same walk F8/Shift+F8 ride
    // (step_problem - the buttons and the chords cannot drift apart),
    // dead until a check has something to show.
    layout.same_line();
    if (gui::button(gui, layout, theme, "Prev", 56.0f, {}, !can_step)) {
        (void)step_problem(ed, false);
    }
    layout.same_line();
    if (gui::button(gui, layout, theme, "Next", 56.0f, {}, !can_step)) {
        (void)step_problem(ed, true);
    }
    layout.same_line();
    if (!ed.problems.has) {
        // No check yet: the load report stands in (the tolerant loader's
        // repairs are problems too).
        const std::size_t count = ed.doc->report.diags.size();
        gui::text(layout, theme,
                  ("Load report: " + counted(count, "note")).c_str());
        if (count == 0) {
            hint_line(layout, theme,
                      "Nothing to report - press Re-check to run ERC");
        }
        int index = 0;
        for (const ws::circsim::Diag& d : ed.doc->report.diags) {
            gui.ids().push(index);
            (void)gui::selectable(gui, layout, rows_theme, d.render().c_str(),
                                  false);
            gui.ids().pop();
            ++index;
        }
        return;
    }

    std::string header = ed.problems.header;
    if (stale) {
        header += " - stale, press Re-check";
    }
    gui::text(layout, theme, header.c_str());

    // The filter row: one checkbox per severity, its label the LIVE
    // count - the FULL count, the same number the header said, because
    // hiding rows never rewrites the findings. The "###" rule keeps
    // the widget's identity steady while its count relabels (a focused
    // checkbox survives the relabel; a "##" id would not).
    if (!ed.problems.diags.empty()) {
        const std::string err =
            "Errors " + std::to_string(errors) + "###problems.error";
        const std::string warn =
            "Warnings " + std::to_string(warnings) + "###problems.warning";
        const std::string note =
            "Notes " + std::to_string(notes) + "###problems.note";
        // (void) flips: the checkbox writes the flag in place, and the
        // next frame's draw already answers to it - nothing else to do
        // on the toggle frame.
        (void)gui::checkbox(gui, layout, theme, err.c_str(),
                            ed.problems_show_error);
        layout.same_line();
        (void)gui::checkbox(gui, layout, theme, warn.c_str(),
                            ed.problems_show_warning);
        layout.same_line();
        (void)gui::checkbox(gui, layout, theme, note.c_str(),
                            ed.problems_show_note);
        hint_line(layout, theme,
                  "click a row, or F8 / Shift+F8 to step");
    }

    int index = 0;
    for (const ws::circsim::Diag& d : ed.problems.diags) {
        // The ORIGINAL index rides on: the row's id scope and its line
        // lookup both key on the diag's own slot in ed.problems, so a
        // filter that hides rows must skip them, never renumber them.
        const int at = index++;
        if (!problems_filter_shows(d.severity, ed.problems_show_error,
                                   ed.problems_show_warning,
                                   ed.problems_show_note)) {
            continue;
        }
        gui.ids().push(at);
        const std::string prefix =
            d.severity == ws::circsim::Severity::Error
                ? "[E] "
                : (d.severity == ws::circsim::Severity::Warning ? "[W] "
                                                                : "[N] ");
        // The rendered lines run parallel to the diags; fall back to the
        // diag's own rendering rather than index past a short list.
        const std::size_t line_at = static_cast<std::size_t>(at);
        const std::string text =
            line_at < ed.problems.lines.size()
                ? ed.problems.lines[line_at]
                : d.render();
        // The cursor's row wears the selected face - the keyboard's
        // landing, and the click's (both set the cursor through the
        // same goto_problem path).
        if (gui::selectable(gui, layout, rows_theme, (prefix + text).c_str(),
                            at == ed.problems_cursor)) {
            goto_problem(ed, line_at);
        }
        gui.ids().pop();
    }
    if (ed.problems.diags.empty()) {
        gui::text(layout, theme,
                  stale ? "No findings at the last check"
                        : "No findings - all clear");
    } else if (visible == 0) {
        // Everything hidden: say so with the truth the filter is
        // hiding (the full count), and offer the one-click way back.
        if (gui::button(gui, layout, theme, "Show all", 80.0f)) {
            ed.problems_show_error = true;
            ed.problems_show_warning = true;
            ed.problems_show_note = true;
        }
        layout.same_line();
        gui::text(layout, theme,
                  (counted(ed.problems.diags.size(), "finding") +
                   " hidden by the filter")
                      .c_str());
    }
}

// --- the netlist -------------------------------------------------------------------

void draw_netlist_panel(Editor& ed, gui::GuiContext& gui,
                        gui::LayoutCursor& layout,
                        const gui::ChromeTheme& theme) {
    if (ed.doc == nullptr || ed.schem() == nullptr) {
        gui::text(layout, theme, "No document open");
        return;
    }
    const bool stale =
        ed.netlist.has && ed.netlist.version != ed.schem()->version();
    if (gui::button(gui, layout, theme, "Re-export")) {
        ed.refresh_netlist();
    }
    // Copy: the deck AS SHOWN onto the system clipboard - the stale
    // one too (the status line says so), because "what is on screen"
    // is the honest answer until Re-export runs. The clipboard is the
    // gui's own seam (the windowing layer's - the app wired it to the
    // OS window at setup); an unset seam is Android's shape, and the
    // button's status line answers the honest "unavailable" instead
    // of a silent drop.
    layout.same_line();
    if (gui::button(gui, layout, theme, "Copy", 64.0f, {},
                    !ed.netlist.has)) {
        if (gui.has_clipboard()) {
            gui.set_clipboard_text(join_deck_lines(ed.netlist.lines).c_str());
            ed.status("Copied " + counted(ed.netlist.lines.size(), "line") +
                      (stale ? " (stale)" : ""));
        } else {
            ed.status("copy failed - the clipboard is unavailable");
        }
    }
    layout.same_line();
    if (!ed.netlist.has) {
        gui::text(layout, theme, "Not exported yet - press Re-export");
        return;
    }
    const std::string head =
        std::string{stale ? "Stale - edited since the export" : "The deck"} +
        " (" + counted(ed.netlist.lines.size(), "line") + ")";
    gui::text(layout, theme, head.c_str());

    // The lines, in the mono face when there is one (the theme's face
    // otherwise - readable, just not column-aligned). A fontless
    // theme with no mono face falls back to plain text rows, the
    // value rows' own ladder - never a null deref on the way down.
    // A dim line-number gutter on the left; comments recede and
    // dot-commands (.tran, .model ...) take the accent.
    const ws::ui::r2d::Font* font =
        ed.font_mono != nullptr && !ed.font_mono->empty() ? ed.font_mono
                                                          : theme.font;
    if (font == nullptr || font->line_height() <= 0.0f) {
        for (const std::string& line : ed.netlist.lines) {
            gui::text(layout, theme, line.c_str());
        }
        return;
    }
    const float line_h = font->line_height() + 2.0f;
    // The gutter is a real measurement, not an estimate: the widest
    // line number is the last one (the count - the numbers only grow),
    // and the font knows its own advances: measure_text answers exactly
    // what draw_text will advance for the same string, so the gutter
    // always fits the numbers and the deck's column never crowds the
    // way the old digits * line_height * 0.55 guess could.
    const float gutter =
        font->measure_text(std::to_string(ed.netlist.lines.size()).c_str()) +
        10.0f;
    int index = 0;
    for (const std::string& line : ed.netlist.lines) {
        gui.ids().push(index);
        const gui::Rect row = layout.next(layout.remaining_width(), line_h);
        char number[16];
        std::snprintf(number, sizeof number, "%d", index + 1);
        (void)ws::ui::r2d::draw_text(row.x, row.y, number, *font,
                                     gui::dim(theme.text, 0.4f));
        std::uint32_t color = gui::dim(theme.text, 0.85f);
        if (!line.empty() && line[0] == '*') {
            color = gui::dim(theme.text, 0.5f);
        } else if (!line.empty() && line[0] == '.') {
            color = theme.control_on;
        }
        (void)ws::ui::r2d::draw_text(row.x + gutter, row.y, line.c_str(),
                                     *font, color);
        gui.ids().pop();
        ++index;
    }
}

}  // namespace editor
