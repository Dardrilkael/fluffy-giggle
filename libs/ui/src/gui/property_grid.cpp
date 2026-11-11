// property_grid.hpp's implementation. Renders one widget row per
// schema setting, grouped by category. The row's widget depends on the
// setting's type and constraints:
//   boolean            -> checkbox
//   integer + range    -> input_int (typed, arrows step 1, clamped)
//   integer + enum     -> combo
//   integer (neither)  -> input_int (unbounded - the honest row, where
//                         the old 0..100 slider fallback invented a
//                         range the schema never declared)
//   real + range       -> input_float (typed, clamped)
//   real + enum        -> combo
//   real (neither)     -> input_float (unbounded)
//   text + enum        -> combo
//   text + regex/plain -> text_input
//
// The grid reads the current value through SettingsStore::get<T> and
// writes through SettingsStore::set(Layer::user, key, value) - only on
// the rows' own commit edges: Enter, a step or the blur on the numeric
// rows, and the blur on the text rows (the one edge a bare text field
// has; a single line of text owns no Enter), so a keystroke is never a
// settings write. A change that fails validation (coerce rejects) is
// silently dropped - the field re-spells from the store's standing
// value on its next idle frame; the schema's constraint is the
// authority.
//
// The text rows keep the numeric field's own discipline around that
// edge: the focused field owns the spelling (the buffer is not
// re-synced while the edit stands, so an external change - another
// panel bound to the store, a settings sync - waits for the blur
// instead of stomping the typing), and the edit ends at the first
// unfocused frame with one commit; a refused write reverts the buffer
// to the standing value on that same frame, never mid-typing.
//
// The enum rows ride the combo's own contract for their dropdown: the
// open flag is the caller's to keep, so it lives in the grid's state
// keyed by the setting's key (a frame-local bool would lay the list
// for exactly the frame of the click that opened it - the next frame
// re-arms false, the list vanishes, and the pick, a press on an item
// one frame or ten later, falls through to whatever sat beneath).

#include <ws/ui/gui/property_grid.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include <ws/ui/gui/widgets.hpp>

namespace ws::ui::gui {

namespace {

using settings::SettingDesc;
using settings::SettingType;
using settings::SettingValue;
using settings::SettingsStore;
using settings::Schema;

// Finds the range constraint on a setting, or nullptr.
const settings::RangeConstraint* find_range(const SettingDesc& desc) {
    for (const auto& c : desc.constraints) {
        if (std::holds_alternative<settings::RangeConstraint>(c)) {
            return &std::get<settings::RangeConstraint>(c);
        }
    }
    return nullptr;
}

// Finds the enum constraint on a setting, or nullptr.
const settings::EnumConstraint* find_enum(const SettingDesc& desc) {
    for (const auto& c : desc.constraints) {
        if (std::holds_alternative<settings::EnumConstraint>(c)) {
            return &std::get<settings::EnumConstraint>(c);
        }
    }
    return nullptr;
}

// Builds a string list from an enum constraint's allowed values (for
// the combo). Integer enums stringify as their value; text enums use
// the string directly.
std::vector<std::string> enum_labels(const settings::EnumConstraint& e,
                                     SettingType type) {
    std::vector<std::string> out;
    out.reserve(e.allowed.size());
    for (const SettingValue& v : e.allowed) {
        if (type == SettingType::integer) {
            auto val = v.as<std::int64_t>();
            if (val) {
                out.push_back(std::to_string(*val));
            }
        } else if (type == SettingType::text) {
            auto val = v.as<std::string>();
            if (val) {
                out.push_back(*val);
            }
        } else if (type == SettingType::real) {
            auto val = v.as<double>();
            if (val) {
                out.push_back(std::to_string(*val));
            }
        }
    }
    return out;
}

}  // namespace

std::size_t property_grid(GuiContext& gui, LayoutCursor& layout,
                          const ChromeTheme& theme,
                          const Schema& schema,
                          SettingsStore& settings,
                          PropertyGridState& state) {
    std::size_t changed = 0;
    const auto categories = schema.categories();
    for (const auto& cat : categories) {
        // Ensure the category has an open-state entry - defaulting OPEN,
        // the documented intent. (The entry used to come from
        // operator[]'s zero-init, which said CLOSED: every grid laid
        // its categories collapsed until clicked, and the smoke test
        // never noticed - a closed header still draws its quads. The
        // typed rows made it visible: a collapsed category renders no
        // rows at all.)
        auto& open = state.category_open.try_emplace(cat, true).first->second;
        if (!collapsing_header(gui, layout, theme, cat.c_str(), open)) {
            continue;
        }
        const auto descs = schema.in_category(cat);
        for (const SettingDesc* desc : descs) {
            if (desc == nullptr) continue;
            const char* label = desc->key.c_str();

            if (desc->type == SettingType::boolean) {
                auto r = settings.get<bool>(desc->key);
                if (!r) continue;
                bool val = r->value;
                if (checkbox(gui, layout, theme, label, val)) {
                    if (settings.set(settings::Layer::user, desc->key, val).has_value()) {
                        ++changed;
                    }
                }
            } else if (desc->type == SettingType::integer) {
                const auto* range = find_range(*desc);
                const auto* en = find_enum(*desc);
                auto r = settings.get<std::int64_t>(desc->key);
                if (!r) continue;
                if (en != nullptr && !en->allowed.empty()) {
                    // Combo: build the label list, find the current index.
                    auto labels = enum_labels(*en, SettingType::integer);
                    std::vector<const char*> ptrs;
                    ptrs.reserve(labels.size());
                    for (const auto& s : labels) ptrs.push_back(s.c_str());
                    int sel = 0;
                    // Find the current value's index.
                    for (std::size_t i = 0; i < en->allowed.size(); ++i) {
                        auto v = en->allowed[i].as<std::int64_t>();
                        if (v && *v == r->value) { sel = static_cast<int>(i); break; }
                    }
                    // The dropdown's open flag is the combo contract's
                    // caller-owned state (see the file header): kept
                    // in the grid's state keyed by the setting, so the
                    // list the row's click or Enter opened still hangs
                    // when the pick lands, a frame or ten later.
                    auto& combo_open = state.combo_open[desc->key];
                    if (combo(gui, layout, theme, label, ptrs.data(),
                              static_cast<int>(ptrs.size()), sel, combo_open)) {
                        if (sel >= 0 && sel < static_cast<int>(en->allowed.size())) {
                            auto v = en->allowed[sel].as<std::int64_t>();
                            if (v && settings.set(settings::Layer::user, desc->key, *v).has_value()) {
                                ++changed;
                            }
                        }
                    }
                } else {
                    int val = static_cast<int>(r->value);
                    const int lo = range ? static_cast<int>(range->min) : 0;
                    const int hi = range ? static_cast<int>(range->max) : 0;
                    // min == max (no schema range) lays the field
                    // unbounded; a schema range bounds the commits (a
                    // typed value clamps into it, the family's rule).
                    if (input_int(gui, layout, theme, label, val, lo, hi)) {
                        if (settings.set(settings::Layer::user, desc->key,
                                         static_cast<std::int64_t>(val)).has_value()) {
                            ++changed;
                        }
                    }
                }
            } else if (desc->type == SettingType::real) {
                const auto* range = find_range(*desc);
                auto r = settings.get<double>(desc->key);
                if (!r) continue;
                float val = static_cast<float>(r->value);
                const float lo = range ? static_cast<float>(range->min) : 0.0f;
                const float hi = range ? static_cast<float>(range->max) : 0.0f;
                // The typed row: exact values like 0.0001 land as
                // typed (a slider could only scrub at its own
                // resolution); min == max lays the field unbounded.
                if (input_float(gui, layout, theme, label, val, lo, hi)) {
                    if (settings.set(settings::Layer::user, desc->key,
                                     static_cast<double>(val)).has_value()) {
                        ++changed;
                    }
                }
            } else if (desc->type == SettingType::text) {
                const auto* en = find_enum(*desc);
                auto r = settings.get<std::string>(desc->key);
                if (!r) continue;
                if (en != nullptr && !en->allowed.empty()) {
                    auto labels = enum_labels(*en, SettingType::text);
                    std::vector<const char*> ptrs;
                    ptrs.reserve(labels.size());
                    for (const auto& s : labels) ptrs.push_back(s.c_str());
                    int sel = 0;
                    for (std::size_t i = 0; i < en->allowed.size(); ++i) {
                        auto v = en->allowed[i].as<std::string>();
                        if (v && *v == r->value) { sel = static_cast<int>(i); break; }
                    }
                    // The same caller-owned flag as the integer enums
                    // above: the text enums' dropdowns hang on the same
                    // contract, not on a bool that dies with the frame.
                    auto& combo_open = state.combo_open[desc->key];
                    if (combo(gui, layout, theme, label, ptrs.data(),
                              static_cast<int>(ptrs.size()), sel, combo_open)) {
                        if (sel >= 0 && sel < static_cast<int>(en->allowed.size())) {
                            auto v = en->allowed[sel].as<std::string>();
                            if (v && settings.set(settings::Layer::user, desc->key, *v).has_value()) {
                                ++changed;
                            }
                        }
                    }
                } else {
                    // Text input: the state's buffer, with the numeric
                    // rows' own discipline (see the file header) - the
                    // focused field owns the spelling, the blur commits.
                    auto& buf = state.text_bufs[desc->key];
                    auto& editing = state.text_editing[desc->key];
                    // The row's own id: text_input derives the same one
                    // from the same scope (begin_widget_row's
                    // ids().id(label)), so the focus read here is the
                    // focus the field itself will see.
                    const WidgetId row_id = gui.ids().id(label);
                    if (gui.is_focused(row_id)) {
                        // The edit stands: the field owns the spelling -
                        // no store write, no re-sync, whatever else the
                        // frame does to the setting's value.
                        editing = true;
                    } else {
                        // The row the typing landed in reads as blurred
                        // only once: the first unfocused frame after the
                        // edit is THE blur, its commit edge.
                        const std::string typed{buf.data()};
                        const std::string* standing = &r->value;
                        if (editing) {
                            editing = false;
                            // Write only on a real move (the radio's
                            // idempotence: re-typing the standing value
                            // is a no-op). A refused write keeps the
                            // standing value and shows it, this frame.
                            if (typed != r->value &&
                                settings.set(settings::Layer::user,
                                              desc->key, typed)
                                    .has_value()) {
                                ++changed;
                                standing = &typed;
                            }
                        }
                        // The idle re-sync (and the blur's own
                        // re-spell): the store's standing value lands
                        // in the buffer, so an external change (a
                        // caller's write, a settings sync) shows the
                        // moment it lands - while no edit stands.
                        //
                        // copy_n, not strncpy: MSVC C4996s the CRT copy
                        // (and strncpy_s is MSVC-only), while copying the
                        // chars and writing the terminator is the same
                        // truncating copy with no deprecated call in it
                        // (app.cpp's env_probe is the scoped-pragma case
                        // only because getenv has no portable
                        // non-deprecated spelling).
                        const std::size_t n =
                            std::min(standing->size(), buf.size() - 1);
                        std::copy_n(standing->data(), n, buf.data());
                        buf.data()[n] = '\0';
                    }
                    (void)text_input(gui, layout, theme, label, buf.data(),
                                     static_cast<int>(buf.size()));
                }
            }
        }
    }
    return changed;
}

}  // namespace ws::ui::gui
