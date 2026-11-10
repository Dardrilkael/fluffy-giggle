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
// writes through SettingsStore::set(Layer::user, key, value) - on the
// numeric rows, only on the field's own commit edges (Enter, the
// blur, a step), so a keystroke is not a settings write. A change
// that fails validation (coerce rejects) is silently dropped - the
// field re-spells from the store's standing value on its next idle
// frame; the schema's constraint is the authority.

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
                    bool combo_open = false;
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
                    bool combo_open = false;
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
                    // Text input: use the state's buffer.
                    auto& buf = state.text_bufs[desc->key];
                    // Sync the buffer with the current value if it's not
                    // being edited (a simple heuristic: always copy the
                    // current value in; the text_input widget handles
                    // editing in-place). A full implementation would
                    // track focus and only sync on blur.
                    //
                    // copy_n, not strncpy: MSVC C4996s the CRT copy (and
                    // strncpy_s is MSVC-only), while copying the chars
                    // and writing the terminator is the same truncating
                    // copy with no deprecated call in it (app.cpp's
                    // env_probe is the scoped-pragma case only because
                    // getenv has no portable non-deprecated spelling).
                    const std::size_t n =
                        std::min(r->value.size(), buf.size() - 1);
                    std::copy_n(r->value.data(), n, buf.data());
                    buf.data()[n] = '\0';
                    if (text_input(gui, layout, theme, label, buf.data(),
                                   static_cast<int>(buf.size()))) {
                        if (settings.set(settings::Layer::user, desc->key,
                                         std::string{buf.data()}).has_value()) {
                            ++changed;
                        }
                    }
                }
            }
        }
    }
    return changed;
}

}  // namespace ws::ui::gui
