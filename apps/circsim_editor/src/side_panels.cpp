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
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <ws/core/assert.hpp>
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
        const gui::Rect row =
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
// invalid value keeps the old and notes the status line.
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
    // own across analysis switches.
    const auto number_row = [&](const char* label, const std::string& skey,
                                double current, const auto& land) {
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
        if (!ws::spice::parse_number(buf).has_value()) {
            hint_line(layout, theme,
                      "Not a SPICE number - try 1u, 4.7k or 1meg");
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
        if (text_input_tracked(ed, gui, layout, theme, "source##" + skey +
                                                              ".source",
                               src, 128)) {
            ws::circsim::DcSpec next = *dc;
            next.source = src;
            push_set_analysis(*ed.doc, index, next);
        }
        number_row("start", skey, dc->start, [&](double v) {
            ws::circsim::DcSpec next = *dc;
            next.start = v;
            push_set_analysis(*ed.doc, index, next);
        });
        number_row("stop", skey, dc->stop, [&](double v) {
            ws::circsim::DcSpec next = *dc;
            next.stop = v;
            push_set_analysis(*ed.doc, index, next);
        });
        number_row("step", skey, dc->step, [&](double v) {
            ws::circsim::DcSpec next = *dc;
            next.step = v;
            push_set_analysis(*ed.doc, index, next);
        });
        return;
    }
    if (const ws::circsim::TranSpec* tran =
            std::get_if<ws::circsim::TranSpec>(&spec)) {
        const std::string skey = key + ".tran";
        // 0 spells "unset" for the optional pair (tstart/tmax), which
        // the card layer treats exactly like nullopt's defaults.
        number_row("tstep", skey, tran->tstep, [&](double v) {
            ws::circsim::TranSpec next = *tran;
            next.tstep = v;
            push_set_analysis(*ed.doc, index, next);
        });
        number_row("tstop", skey, tran->tstop, [&](double v) {
            ws::circsim::TranSpec next = *tran;
            next.tstop = v;
            push_set_analysis(*ed.doc, index, next);
        });
        number_row("tstart", skey, tran->tstart.value_or(0.0),
                   [&](double v) {
                       ws::circsim::TranSpec next = *tran;
                       next.tstart = v > 0.0 ? std::optional<double>{v}
                                             : std::nullopt;
                       push_set_analysis(*ed.doc, index, next);
                   });
        number_row("tmax", skey, tran->tmax.value_or(0.0), [&](double v) {
            ws::circsim::TranSpec next = *tran;
            next.tmax = v > 0.0 ? std::optional<double>{v} : std::nullopt;
            push_set_analysis(*ed.doc, index, next);
        });
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
        if (text_input_tracked(ed, gui, layout, theme, "points##" + skey +
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
        number_row("fstart", skey, ac->fstart, [&](double v) {
            ws::circsim::AcSpec next = *ac;
            next.fstart = v;
            push_set_analysis(*ed.doc, index, next);
        });
        number_row("fstop", skey, ac->fstop, [&](double v) {
            ws::circsim::AcSpec next = *ac;
            next.fstop = v;
            push_set_analysis(*ed.doc, index, next);
        });
        return;
    }
}

}  // namespace

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

// --- the palette --------------------------------------------------------------

namespace {

// The Palette's tile grid: a tile is ~52 px wide (the columns stretch
// to fill the panel), 44 px tall - the symbol's picture fitted inside,
// the name in a tooltip.
constexpr float kTileTargetW = 52.0f;
constexpr float kTileH = 44.0f;
constexpr float kTilePad = 6.0f;

// The symbols armed most recently, newest first. Process-wide on purpose:
// it is a UI convenience, not document state, so it survives a document
// switch. (Move it onto Editor if a second editor ever needs its own.)
constexpr std::size_t kRecentMax = 6;

[[nodiscard]] std::vector<std::string>& recent_symbols() {
    static std::vector<std::string> keys;
    return keys;
}

void note_recent_symbol(const std::string& key) {
    std::vector<std::string>& r = recent_symbols();
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
        note_recent_symbol(sym.key);
    };
    if (filter_focused && !groups.empty() && !groups.front().items.empty() &&
        gui.input().key_pressed[ws::input::keys::kEnter] != 0) {
        arm(*groups.front().items.front());
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
        for (const std::string& key : recent_symbols()) {
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
        if (sym == nullptr || sym->needs_value || !p->value.empty()) {
            part_text_row(ed, gui, layout, theme, "Value", p->id, 1, "",
                          p->value);
        }
        if (sym != nullptr && !sym->params.empty()) {
            panel_section(layout, theme, "Parameters");
        }
        if (sym != nullptr) {
            for (const ws::circsim::ParamSpec& spec : sym->params) {
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
    gui::heading(layout, theme, "Schematic")
    char grid[64];
    std::snprintf(grid, sizeof grid, "%.12g", ed.schem()->grid);
    label_line(layout, theme, "Grid", grid);
    gui::separator(layout, theme);

    // --- the analysis list
    gui::heading(layout, theme, "Analyses (in deck order)");
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
        const std::string card = ws::circsim::analysis_card(spec);
        const bool selected = ed.analysis_index == index;
        if (gui::selectable(gui, layout, theme, card.c_str(), selected)) {
            ed.analysis_index = index;
            ed.bump_edit_epoch();  // the field rows below reload
        }
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
            case 2: spec = ws::circsim::TranSpec{.tstop = 1.0e-3}; break;
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
            panel_section(layout, theme, "Edit analysis");
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
    if (gui::button(gui, layout, theme, "Re-check")) {
        ed.run_erc();
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

    std::size_t errors = 0;
    std::size_t warnings = 0;
    std::size_t notes = 0;
    for (const ws::circsim::Diag& d : ed.problems.diags) {
        if (d.severity == ws::circsim::Severity::Error) {
            ++errors;
        } else if (d.severity == ws::circsim::Severity::Warning) {
            ++warnings;
        } else {
            ++notes;
        }
    }
    if (!ed.problems.diags.empty()) {
        hint_line(layout, theme,
                  counted(errors, "error") + ", " +
                      counted(warnings, "warning") + ", " +
                      counted(notes, "note") + " - click a row to jump");
    }

    int index = 0;
    for (const ws::circsim::Diag& d : ed.problems.diags) {
        gui.ids().push(index);
        const std::string prefix =
            d.severity == ws::circsim::Severity::Error
                ? "[E] "
                : (d.severity == ws::circsim::Severity::Warning ? "[W] "
                                                                : "[N] ");
        // The rendered lines run parallel to the diags; fall back to the
        // diag's own rendering rather than index past a short list.
        const std::size_t at = static_cast<std::size_t>(index);
        const std::string text = at < ed.problems.lines.size()
                                     ? ed.problems.lines[at]
                                     : d.render();
        if (gui::selectable(gui, layout, rows_theme, (prefix + text).c_str(),
                            false)) {
            // The jump: locate through the back-maps, then select and
            // zoom in one act.
            const ws::circsim::DiagTarget t = ws::circsim::locate_diag(
                d, *ed.schem(),
                ed.problems.nets_valid ? &ed.problems.nets : nullptr,
                ed.problems.map_valid ? &ed.problems.map : nullptr,
                &ed.library);
            ed.jump_to(t);
        }
        gui.ids().pop();
        ++index;
    }
    if (ed.problems.diags.empty()) {
        gui::text(layout, theme,
                  stale ? "No findings at the last check"
                        : "No findings - all clear");
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
    const std::size_t digits =
        std::to_string(ed.netlist.lines.size()).size();
    const float gutter =
        static_cast<float>(digits) * font->line_height() * 0.55f + 10.0f;
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
