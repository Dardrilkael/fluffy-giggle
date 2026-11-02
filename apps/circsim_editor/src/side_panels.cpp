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

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

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
[[nodiscard]] bool text_input_tracked(Editor& ed, gui::GuiContext& gui,
                                     gui::LayoutCursor& layout,
                                     const gui::ChromeTheme& theme,
                                     const char* label, char* buf,
                                     int capacity, float width = -1.0f) {
    const bool changed =
        gui::text_input(gui, layout, theme, label, buf, capacity, width);
    if (gui.is_focused(gui.ids().id(label))) {
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
void watch_selection(Editor& ed) {
    if (ed.fields_sel_kind != static_cast<int>(ed.sel.kind) ||
        ed.fields_sel_id != ed.sel.id) {
        ed.fields_sel_kind = static_cast<int>(ed.sel.kind);
        ed.fields_sel_id = ed.sel.id;
        ++ed.field_stamp;
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
// coalescer makes this one undo step per run).
void part_text_row(Editor& ed, gui::GuiContext& gui,
                   gui::LayoutCursor& layout, const gui::ChromeTheme& theme,
                   const char* label, std::uint32_t part_id, int kind,
                   const std::string& param, const std::string& current) {
    const std::string key = part_field_key(part_id, kind, param);
    if (text_input_tracked(ed, gui, layout, theme, label,
                           field_buf(ed, key, current), 128)) {
        push_part_text(*ed.doc, part_id, kind, param,
                       std::string{field_of(ed, key).buf.data()},
                       std::string{"Edit "} + label);
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
    // value keeps the old and says so on the status line.
    const auto number_row = [&](const char* label, const std::string& skey,
                                double current, const auto& land) {
        char current_text[64];
        std::snprintf(current_text, sizeof current_text, "%.12g", current);
        char* buf = field_buf(ed, skey + "." + label, current_text);
        if (text_input_tracked(ed, gui, layout, theme, label, buf, 128)) {
            const auto parsed = ws::spice::parse_number(buf);
            if (parsed.has_value()) {
                land(*parsed);
            } else {
                ed.status(std::string{"'"} + buf +
                          "' is not a SPICE number - kept the old value");
            }
        }
    };

    if (std::get_if<ws::circsim::OpSpec>(&spec) != nullptr) {
        gui::text(layout, theme, ".op - no fields");
        return;
    }
    if (const ws::circsim::DcSpec* dc =
            std::get_if<ws::circsim::DcSpec>(&spec)) {
        const std::string skey = key + ".dc";
        char* src = field_buf(ed, skey + ".source", dc->source);
        if (text_input_tracked(ed, gui, layout, theme, "source", src, 128)) {
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
        if (gui::combo(gui, layout, theme, "sweep", kSweeps, 3, selected,
                       sweep.combo_open)) {
            ws::circsim::AcSpec next = *ac;
            next.sweep = static_cast<ws::circsim::AcSpec::Sweep>(selected);
            push_set_analysis(*ed.doc, index, next);
        }
        char n_text[32];
        std::snprintf(n_text, sizeof n_text, "%d", ac->n);
        char* n_buf = field_buf(ed, skey + ".n", n_text);
        if (text_input_tracked(ed, gui, layout, theme, "points", n_buf, 128)) {
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

// --- the palette --------------------------------------------------------------

void draw_palette_panel(Editor& ed, gui::GuiContext& gui,
                        gui::LayoutCursor& layout,
                        const gui::ChromeTheme& theme) {
    gui::text(layout, theme, "tools");
    if (gui::selectable(gui, layout, theme, "select",
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
        (void)gui::text_input(gui, layout, theme, "name", ed.label_text,
                              static_cast<int>(sizeof ed.label_text));
    }
    gui::separator(layout, theme);
    gui::text(layout, theme, "symbols (click to place)");

    int index = 0;
    for (const ws::circsim::SymbolDef& sym : ed.library.symbols()) {
        gui.ids().push(index++);
        // The preview strip, then the row beside it.
        const float preview_w = 46.0f;
        const float preview_h = 30.0f;
        const gui::Rect preview = layout.next(preview_w, preview_h);
        const float row_w = std::max(60.0f, layout.remaining_width());
        char label[160];
        std::snprintf(label, sizeof label, "%s (%s)", sym.name.c_str(),
                      sym.key.c_str());
        const bool armed =
            ed.tool == Tool::Place && ed.place_symbol == sym.key;
        if (gui::selectable(gui, layout, theme, label, armed, row_w)) {
            ed.tool = Tool::Place;
            ed.place_symbol = sym.key;
            ed.ghost_rot = 0;
            ed.ghost_mirror = false;
        }
        gui.ids().pop();

        // The mini preview: the same world renderer, fitted and flipped
        // into the strip (no font - a 30px preview has no text to show).
        const Bounds b = ws::circsim::symbol_bounds(sym);
        const double bw = std::max(1.0, b.max_x - b.min_x);
        const double bh = std::max(1.0, b.max_y - b.min_y);
        const double fit =
            std::min(static_cast<double>(preview_w - 8) / bw,
                     static_cast<double>(preview_h - 8) / bh);
        canvas::Paint preview_paint;
        preview_paint.width = 1.5f;
        preview_paint.color =
            armed ? theme.control_on : gui::dim(theme.text, 0.8f);
        ed.renderer.push_transform();
        ed.renderer.translate(preview.x + preview.width * 0.5f,
                              preview.y + preview.height * 0.5f);
        ed.renderer.scale(static_cast<float>(fit), -static_cast<float>(fit));
        draw_symbol(ed.renderer, sym, Pos{0.0, 0.0}, 0, false, preview_paint,
                    0u, nullptr);
        ed.renderer.pop_transform();
    }
    gui::text(layout, theme,
              "R rotate - M mirror - Esc cancel");
}

// --- the properties -------------------------------------------------------------

void draw_properties_panel(Editor& ed, gui::GuiContext& gui,
                           gui::LayoutCursor& layout,
                           const gui::ChromeTheme& theme) {
    watch_selection(ed);
    ed.text_field_focused = false;  // this frame's draw sets it back
    if (ed.doc == nullptr || ed.schem() == nullptr) {
        gui::text(layout, theme, "no document");
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
        label_line(layout, theme, "symbol",
                   sym != nullptr
                       ? sym->name + " (" + sym->key + ")"
                       : p->symbol + " (unknown)");
        part_text_row(ed, gui, layout, theme, "ref", p->id, 0, "", p->ref);
        if (sym == nullptr || sym->needs_value || !p->value.empty()) {
            part_text_row(ed, gui, layout, theme, "value", p->id, 1, "",
                          p->value);
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
                    if (gui::combo(gui, layout, theme, shown.c_str(),
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
        std::snprintf(where, sizeof where, "at (%.12g, %.12g)", p->pos.x,
                      p->pos.y);
        gui::text(layout, theme, where);
        if (gui::button(gui, layout, theme, "delete")) {
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
        label_line(layout, theme, "wire " + std::to_string(w->id),
                   endpoint_label(ed, w->a) + " to " + endpoint_label(ed, w->b));
        if (gui::button(gui, layout, theme, "delete")) {
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
        std::snprintf(where, sizeof where, "junction at (%.12g, %.12g)",
                      j->pos.x, j->pos.y);
        gui::text(layout, theme, where);
        if (gui::button(gui, layout, theme, "delete")) {
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
        if (text_input_tracked(ed, gui, layout, theme, "net name", buf, 128)) {
            push_label_text(*ed.doc, l->id, buf);
        }
        if (gui::button(gui, layout, theme, "delete")) {
            push_delete_label(*ed.doc, l->id);
            ed.sel.clear();
        }
        return;
    }

    // Nothing selected: the schematic's own setup.
    gui::text(layout, theme, "schematic");
    char grid_line[64];
    std::snprintf(grid_line, sizeof grid_line, "grid: %.12g",
                  ed.schem()->grid);
    gui::text(layout, theme, grid_line);
    gui::separator(layout, theme);

    // --- the analysis list
    gui::text(layout, theme, "analyses (deck order)");
    int index = 0;
    for (const ws::circsim::AnalysisSpec& spec :
         ed.schem()->analysis.entries) {
        gui.ids().push(index);
        const std::string card = ws::circsim::analysis_card(spec);
        const bool selected = ed.analysis_index == index;
        if (gui::selectable(gui, layout, theme, card.c_str(), selected)) {
            ed.analysis_index = index;
            ++ed.field_stamp;  // the field rows below reload
        }
        gui.ids().pop();
        ++index;
    }
    static const char* const kKinds[] = {"op", "dc", "tran", "ac"};
    FieldBuf& add = field_of(ed, "analysis.add");
    if (add.stamp != ed.field_stamp) {
        add.stamp = ed.field_stamp;
        add.combo_open = false;
    }
    int picked = 0;
    const float add_w = std::max(60.0f, layout.remaining_width() - 80.0f);
    if (gui::combo(gui, layout, theme, "add", kKinds, 4, picked,
                   add.combo_open, add_w)) {
        ws::circsim::AnalysisSpec spec;
        switch (picked) {
            case 1: spec = ws::circsim::DcSpec{}; break;
            case 2: spec = ws::circsim::TranSpec{.tstop = 1.0e-3}; break;
            case 3: spec = ws::circsim::AcSpec{}; break;
            default: spec = ws::circsim::OpSpec{}; break;
        }
        push_add_analysis(*ed.doc, spec);
        ed.analysis_index = static_cast<int>(
            ed.schem()->analysis.entries.size() - 1u);
        ++ed.field_stamp;
    }
    if (!ed.schem()->analysis.entries.empty()) {
        if (gui::button(gui, layout, theme, "remove selected")) {
            const std::size_t at = std::min<std::size_t>(
                static_cast<std::size_t>(ed.analysis_index),
                ed.schem()->analysis.entries.size() - 1u);
            push_remove_analysis(*ed.doc, at);
            ed.analysis_index = -1;
            ++ed.field_stamp;
        }
        const std::size_t at = std::min<std::size_t>(
            static_cast<std::size_t>(ed.analysis_index),
            ed.schem()->analysis.entries.size() - 1u);
        if (ed.analysis_index >= 0) {
            gui::separator(layout, theme);
            gui::text(layout, theme, "edit analysis");
            analysis_fields(ed, gui, layout, theme, at);
        }
    }

    // --- the directives block: one row per line
    gui::separator(layout, theme);
    gui::text(layout, theme, "directives (.model/.include, verbatim)");
    std::vector<std::string> lines;
    {
        const std::string& text = ed.schem()->directives;
        std::size_t at = 0;
        if (text.empty()) {
            lines.push_back("");
        }
        while (at < text.size()) {
            const std::size_t nl = text.find('\n', at);
            lines.push_back(text.substr(
                at, nl == std::string::npos ? std::string::npos : nl - at));
            if (nl == std::string::npos) {
                break;
            }
            at = nl + 1;
        }
    }
    int line_at = 0;
    for (const std::string& line : lines) {
        gui.ids().push(line_at);
        char* buf = field_buf(ed, "directives." + std::to_string(line_at),
                              line);
        if (text_input_tracked(ed, gui, layout, theme, "line", buf, 128)) {
            lines[static_cast<std::size_t>(line_at)] = buf;
            std::string joined;
            for (const std::string& l : lines) {
                joined += l;
                joined += '\n';
            }
            if (!joined.empty()) {
                joined.pop_back();
            }
            push_directives(*ed.doc, joined);
        }
        gui.ids().pop();
        ++line_at;
    }
    if (gui::button(gui, layout, theme, "add line")) {
        std::string joined = ed.schem()->directives;
        if (!joined.empty()) {
            joined += '\n';
        }
        joined += ' ';
        push_directives(*ed.doc, joined);
        ++ed.field_stamp;
    }
}

// --- the problems ----------------------------------------------------------------

void draw_problems_panel(Editor& ed, gui::GuiContext& gui,
                         gui::LayoutCursor& layout,
                         const gui::ChromeTheme& theme) {
    if (ed.doc == nullptr || ed.schem() == nullptr) {
        gui::text(layout, theme, "no document");
        return;
    }

    const bool stale =
        ed.problems.has && ed.problems.version != ed.schem()->version();
    if (gui::button(gui, layout, theme, "re-check")) {
        ed.run_erc();
    }
    layout.same_line();
    if (ed.problems.has) {
        gui::text(layout, theme,
                  stale ? std::string(ed.problems.header + " - stale")
                              .c_str()
                        : ed.problems.header.c_str());
    } else {
        // No check yet: the load report stands in (the tolerant loader's
        // repairs are problems too).
        const std::size_t count = ed.doc->report.diags.size();
        char header[128];
        std::snprintf(header, sizeof header, "load: %zu note%s", count,
                      count == 1 ? "" : "s");
        gui::text(layout, theme, header);
        int index = 0;
        for (const ws::circsim::Diag& d : ed.doc->report.diags) {
            gui.ids().push(index);
            (void)gui::selectable(gui, layout, theme, d.render().c_str(),
                                  false);
            gui.ids().pop();
            ++index;
        }
        return;
    }

    int index = 0;
    for (const ws::circsim::Diag& d : ed.problems.diags) {
        gui.ids().push(index);
        const std::string prefix =
            d.severity == ws::circsim::Severity::Error
                ? "[E] "
                : (d.severity == ws::circsim::Severity::Warning ? "[W] "
                                                                : "[N] ");
        if (gui::selectable(gui, layout, theme,
                            (prefix + ed.problems.lines[static_cast<std::size_t>(
                                           index)])
                                .c_str(),
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
        gui::text(layout, theme, "no findings");
    }
}

// --- the netlist -------------------------------------------------------------------

void draw_netlist_panel(Editor& ed, gui::GuiContext& gui,
                        gui::LayoutCursor& layout,
                        const gui::ChromeTheme& theme) {
    if (ed.doc == nullptr || ed.schem() == nullptr) {
        gui::text(layout, theme, "no document");
        return;
    }
    const bool stale =
        ed.netlist.has && ed.netlist.version != ed.schem()->version();
    if (gui::button(gui, layout, theme, "re-export")) {
        ed.refresh_netlist();
    }
    layout.same_line();
    if (!ed.netlist.has) {
        gui::text(layout, theme, "not exported yet");
        return;
    }
    gui::text(layout, theme,
              stale ? "stale - edited since the export" : "the deck");

    // The lines, in the mono face when there is one (the theme's face
    // otherwise - readable, just not column-aligned).
    const ws::ui::r2d::Font& font =
        ed.font_mono != nullptr && !ed.font_mono->empty() ? *ed.font_mono
                                                          : *theme.font;
    const float line_h = font.line_height() + 2.0f;
    int index = 0;
    for (const std::string& line : ed.netlist.lines) {
        gui.ids().push(index);
        const gui::Rect row = layout.next(layout.remaining_width(), line_h);
        (void)ws::ui::r2d::draw_text(row.x, row.y, line.c_str(), font,
                                     gui::dim(theme.text, 0.85f));
        gui.ids().pop();
        ++index;
    }
}

}  // namespace editor
