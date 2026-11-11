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
// The real enums' labels spell through input_format_float - the
// input_float row's own %g pen - so the list reads like the field it
// replaces: "0.5", not "0.500000".
//
// The state follows the schema bound each frame: the keyed maps are
// pruned to the schema's live keys and categories before the walk, so
// a state re-bound to another schema - or a schema re-declared at
// runtime - starts clean. Nothing else would ever retire an entry
// (the state is the caller's and the keys are the schema's own
// strings), and the half of it that is not a leak is worse: a key that
// left mid-edit and returned would cash its surviving buffer and
// editing flag as one phantom blur commit, and a surviving open flag
// would hand the returned row a dropdown it never opened.
//
// The walk itself is cached in the state, lifetime-sound on two
// halves: the ROW pointers are re-bucketed from the live descriptors
// every frame (a walk of the contiguous descriptor vector and a
// per-category bucket scan - no allocation, the buckets' capacity
// rides), so they always point into THIS frame's binding and a schema
// whose lifetime ends with the frame (a per-frame copy, a temporary in
// the caller's stack slot) reads through its own bytes, never through
// storage an earlier binding has freed. The cached half is the part
// that is values, not storage: the category names - the Schema
// introspection returns its categories and per-category rows BY VALUE
// (a copy of every name, a vector per category), and the enum rows'
// label lists allocate the same way - the one allocating path an
// otherwise allocation-free walk had left (fit_text's own comment
// states the mid-frame no-allocation rule; a settings panel open at
// 60 fps was allocating per category, per enum row, every frame).
// Reusing the cached half is validated per frame by the binding's
// address and descriptor count - and by a STAMP, an FNV over every
// descriptor's identity as the grid consumes it (key, type, category,
// the constraints' shapes): the address and the count alone cannot
// name the object, since two different schemas can share an address
// and a count. The enum rows' choice lists ride the same wipe; each
// row rebuilds its list once on first use after it.

#include <ws/ui/gui/property_grid.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
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
                // The input_float row's own pen (input_format_float,
                // %g at the field's default 6 significant digits), so
                // the list reads like the field it replaces: "0.5",
                // "2.5" - not the C++ default's fixed six decimals
                // ("0.500000"), a spelling no row in this family ever
                // showed. The float cast is the field's own: a label is
                // a display, and the pick below writes the allowed
                // double back, never the spelled float.
                char spelled[24];
                (void)input_format_float(
                    spelled, static_cast<int>(sizeof(spelled)),
                    static_cast<float>(*val), 6);
                out.emplace_back(spelled);
            }
        }
    }
    return out;
}

// The walk stamp: FNV-1a over the binding's own identity as the grid
// consumes it. One step per byte, one delimiter per field, so two
// descriptors' fields cannot concatenate into a third's stamp.
constexpr std::uint64_t kStampBasis = 14695981039346656037ULL;
constexpr std::uint64_t kStampPrime = 1099511628211ULL;

std::uint64_t stamp_byte(std::uint64_t hash, std::uint64_t byte) {
    return (hash ^ (byte & 0xFFu)) * kStampPrime;
}

// A string field: its bytes, then the delimiter.
std::uint64_t stamp_field(std::uint64_t hash, std::string_view bytes) {
    for (const char c : bytes) {
        hash = stamp_byte(hash, static_cast<unsigned char>(c));
    }
    return stamp_byte(hash, ';');
}

// A whole-number field (a type, a count, a variant tag), low byte
// first, then the delimiter.
std::uint64_t stamp_bits(std::uint64_t hash, std::uint64_t bits) {
    for (int shift = 0; shift < 64; shift += 8) {
        hash = stamp_byte(hash, (bits >> shift) & 0xFFu);
    }
    return stamp_byte(hash, ';');
}

// A double field as its own bit pattern - the raw bytes the range
// arithmetic reads, not a re-rounded spelling of them.
std::uint64_t stamp_double(std::uint64_t hash, double v) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof bits);
    return stamp_bits(hash, bits);
}

// One descriptor's own contribution: the key, the type, the category,
// and the constraints' shapes - the constraint count first, then each
// constraint's variant tag with the parts the grid reads (a range's
// bounds and action, an enum's entry count; a regex only tags its
// kind, the grid never reads its pattern). What the stamp cannot see
// is an enum's VALUE edit behind an identical shape - the struct's own
// comment names the residual and why nothing reachable can produce it.
std::uint64_t stamp_desc(std::uint64_t hash, const SettingDesc& d) {
    hash = stamp_field(hash, d.key);
    hash = stamp_bits(hash, static_cast<std::uint64_t>(d.type));
    hash = stamp_field(hash, d.category);
    hash = stamp_bits(hash, d.constraints.size());
    for (const auto& c : d.constraints) {
        hash = stamp_bits(hash, static_cast<std::uint64_t>(c.index()));
        if (const auto* range = std::get_if<settings::RangeConstraint>(&c)) {
            hash = stamp_double(hash, range->min);
            hash = stamp_double(hash, range->max);
            hash = stamp_bits(hash, static_cast<std::uint64_t>(range->action));
        } else if (const auto* en = std::get_if<settings::EnumConstraint>(&c)) {
            hash = stamp_bits(hash, en->allowed.size());
        }
    }
    return hash;
}

}  // namespace

std::size_t property_grid(GuiContext& gui, LayoutCursor& layout,
                          const ChromeTheme& theme,
                          const Schema& schema,
                          SettingsStore& settings,
                          PropertyGridState& state) {
    std::size_t changed = 0;

    // The schema walk, validated then re-bucketed every frame (the
    // state's own comment spells the whole contract): the row pointers
    // always point into THIS frame's live descriptors - a schema whose
    // lifetime ends with the frame reads through its own bytes, never
    // through storage an earlier binding has freed - and the cached
    // category list is served only when the binding's address, its
    // descriptor count AND its content stamp all say this is the walk
    // that built them. The address and the count alone cannot say that:
    // two different schemas can share an address (a per-frame copy in
    // the caller's stack slot) and a count.
    const auto& descs = schema.descriptors();
    bool valid = state.walk_from == &schema &&
                 state.walk_size == descs.size() &&
                 !state.walk_categories.empty();
    if (valid) {
        // The fast path: clear the buckets (their capacity rides, the
        // steady frame allocates nothing), re-stamp and re-bucket in
        // one walk over the contiguous descriptors. A category the
        // cached list does not know - a different binding's content,
        // which the stamp below catches in every reachable spelling -
        // or a stamp that came out different invalidates; the partial
        // bucketing of that walk is discarded by the rebuild.
        for (auto& bucket : state.walk_rows) {
            bucket.clear();
        }
        std::uint64_t stamp = kStampBasis;
        for (const SettingDesc& d : descs) {
            stamp = stamp_desc(stamp, d);
            int slot = -1;
            for (std::size_t ci = 0; ci < state.walk_categories.size(); ++ci) {
                if (d.category == state.walk_categories[ci]) {
                    slot = static_cast<int>(ci);
                    break;
                }
            }
            if (slot < 0) {
                valid = false;
                break;
            }
            state.walk_rows[static_cast<std::size_t>(slot)].push_back(&d);
        }
        if (valid && stamp != state.walk_stamp) {
            valid = false;
        }
    }
    if (!valid) {
        // The rebuild: the categories re-derive from the descriptors
        // themselves (the one allocating walk, paid once per binding
        // change), the buckets refill fresh, and the enum rows' choice
        // lists wipe with it - the new binding's enums re-spell.
        state.walk_from = &schema;
        state.walk_size = descs.size();
        state.walk_categories = schema.categories();
        state.walk_rows.resize(state.walk_categories.size());
        for (auto& bucket : state.walk_rows) {
            bucket.clear();
        }
        std::uint64_t stamp = kStampBasis;
        for (const SettingDesc& d : descs) {
            stamp = stamp_desc(stamp, d);
            // categories() derives from the descriptors themselves, so
            // every category IS in the list; a miss is unreachable for
            // a well-formed schema and drops the row rather than
            // crashing a hand-rolled one.
            int slot = -1;
            for (std::size_t ci = 0; ci < state.walk_categories.size(); ++ci) {
                if (d.category == state.walk_categories[ci]) {
                    slot = static_cast<int>(ci);
                    break;
                }
            }
            if (slot >= 0) {
                state.walk_rows[static_cast<std::size_t>(slot)].push_back(&d);
            }
        }
        state.walk_stamp = stamp;
        state.enum_choices.clear();
    }
    const std::vector<std::string>& categories = state.walk_categories;

    // The state follows the schema bound THIS frame: an entry whose key
    // or category left the schema leaves with it, before the walk can
    // read it back. The state is the caller's and the maps are keyed by
    // the schema's own strings, so nothing else would ever retire an
    // entry - a state re-bound to another schema (another document's
    // settings) or a schema re-declared at runtime would carry the old
    // keys for as long as the app ran. The leak is the small half: a
    // text row that leaves mid-edit leaves its editing flag AND its
    // buffer behind, and a key that RETURNS would cash them on its
    // first unfocused frame - one phantom commit of typing nobody typed
    // in this binding, and a dropdown that arrives pre-opened. The
    // prune is row-aware (a text row's state lives only while the key
    // lays a text row - a re-declared type takes the old row's state
    // with it) and costs one schema find per entry, the same order as
    // the walk's own per-row hashing.
    const auto lays_text_row = [&](std::string_view key) {
        const SettingDesc* d = schema.find(key);
        if (d == nullptr || d->type != SettingType::text) {
            return false;
        }
        const auto* en = find_enum(*d);
        return en == nullptr || en->allowed.empty();
    };
    const auto lays_enum_row = [&](std::string_view key) {
        const SettingDesc* d = schema.find(key);
        if (d == nullptr || d->type == SettingType::boolean) {
            return false;
        }
        const auto* en = find_enum(*d);
        return en != nullptr && !en->allowed.empty();
    };
    std::erase_if(state.text_bufs, [&](const auto& kv) {
        return !lays_text_row(kv.first);
    });
    std::erase_if(state.text_editing, [&](const auto& kv) {
        return !lays_text_row(kv.first);
    });
    std::erase_if(state.combo_open, [&](const auto& kv) {
        return !lays_enum_row(kv.first);
    });
    std::erase_if(state.category_open, [&](const auto& kv) {
        return std::find(categories.begin(), categories.end(), kv.first) ==
               categories.end();
    });

    for (std::size_t ci = 0; ci < categories.size(); ++ci) {
        // Ensure the category has an open-state entry - defaulting OPEN,
        // the documented intent. (The entry used to come from
        // operator[]'s zero-init, which said CLOSED: every grid laid
        // its categories collapsed until clicked, and the smoke test
        // never noticed - a closed header still draws its quads. The
        // typed rows made it visible: a collapsed category renders no
        // rows at all.)
        auto& open =
            state.category_open.try_emplace(categories[ci], true).first->second;
        if (!collapsing_header(gui, layout, theme, categories[ci].c_str(),
                               open)) {
            continue;
        }
        for (const SettingDesc* desc : state.walk_rows[ci]) {
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
                    // The list is the state's cache (see the file header):
                    // built once per binding, not once per frame.
                    auto [choice_it, fresh] =
                        state.enum_choices.try_emplace(desc->key);
                    if (fresh) {
                        PropertyGridState::EnumChoices& choice = choice_it->second;
                        choice.labels = enum_labels(*en, SettingType::integer);
                        choice.ptrs.reserve(choice.labels.size());
                        for (const std::string& s : choice.labels) {
                            choice.ptrs.push_back(s.c_str());
                        }
                    }
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
                    if (combo(gui, layout, theme, label, choice_it->second.ptrs.data(),
                              static_cast<int>(choice_it->second.ptrs.size()), sel, combo_open)) {
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
                const auto* en = find_enum(*desc);
                auto r = settings.get<double>(desc->key);
                if (!r) continue;
                if (en != nullptr && !en->allowed.empty()) {
                    // The enum row the header's mapping table has
                    // promised all along ("real + enum -> combo"): the
                    // integer and text enums' own path, with the
                    // double's own read and write. (The branch used to
                    // be missing: a real+enum setting laid the UNBOUNDED
                    // input_float, so the allowed list the schema
                    // declared never showed - the only way to move the
                    // setting was to type a value the enum silently
                    // refused at the blur, and enum_labels' real arm sat
                    // dead above.) The choice list rides the state's
                    // cache like the other enum rows'.
                    auto [choice_it, fresh] =
                        state.enum_choices.try_emplace(desc->key);
                    if (fresh) {
                        PropertyGridState::EnumChoices& choice = choice_it->second;
                        choice.labels = enum_labels(*en, SettingType::real);
                        choice.ptrs.reserve(choice.labels.size());
                        for (const std::string& s : choice.labels) {
                            choice.ptrs.push_back(s.c_str());
                        }
                    }
                    int sel = 0;
                    for (std::size_t i = 0; i < en->allowed.size(); ++i) {
                        auto v = en->allowed[i].as<double>();
                        if (v && *v == r->value) { sel = static_cast<int>(i); break; }
                    }
                    // The caller-owned open flag, the same combo
                    // contract as the integer and text enums above.
                    auto& combo_open = state.combo_open[desc->key];
                    if (combo(gui, layout, theme, label, choice_it->second.ptrs.data(),
                              static_cast<int>(choice_it->second.ptrs.size()), sel, combo_open)) {
                        if (sel >= 0 && sel < static_cast<int>(en->allowed.size())) {
                            auto v = en->allowed[sel].as<double>();
                            if (v && settings.set(settings::Layer::user, desc->key,
                                                  *v).has_value()) {
                                ++changed;
                            }
                        }
                    }
                } else {
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
                }
            } else if (desc->type == SettingType::text) {
                const auto* en = find_enum(*desc);
                auto r = settings.get<std::string>(desc->key);
                if (!r) continue;
                if (en != nullptr && !en->allowed.empty()) {
                    // The choice list rides the state's cache like the
                    // other enum rows' (see the file header).
                    auto [choice_it, fresh] =
                        state.enum_choices.try_emplace(desc->key);
                    if (fresh) {
                        PropertyGridState::EnumChoices& choice = choice_it->second;
                        choice.labels = enum_labels(*en, SettingType::text);
                        choice.ptrs.reserve(choice.labels.size());
                        for (const std::string& s : choice.labels) {
                            choice.ptrs.push_back(s.c_str());
                        }
                    }
                    int sel = 0;
                    for (std::size_t i = 0; i < en->allowed.size(); ++i) {
                        auto v = en->allowed[i].as<std::string>();
                        if (v && *v == r->value) { sel = static_cast<int>(i); break; }
                    }
                    // The same caller-owned flag as the integer enums
                    // above: the text enums' dropdowns hang on the same
                    // contract, not on a bool that dies with the frame.
                    auto& combo_open = state.combo_open[desc->key];
                    if (combo(gui, layout, theme, label, choice_it->second.ptrs.data(),
                              static_cast<int>(choice_it->second.ptrs.size()), sel, combo_open)) {
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
                        //
                        // string_view, not string: the idle re-sync
                        // branch runs EVERY unfocused frame, and a
                        // value past SSO's 15 bytes would turn that
                        // comparison into a per-frame allocation - the
                        // one spelling this walk just dug out. The view
                        // reads the caller-owned buffer directly; the
                        // commit's own std::string materializes only
                        // when a real write is about to happen.
                        const std::string_view typed{buf.data()};
                        const std::string* standing = &r->value;
                        std::string moved;
                        if (editing) {
                            editing = false;
                            // Write only on a real move (the radio's
                            // idempotence: re-typing the standing value
                            // is a no-op). A refused write keeps the
                            // standing value and shows it, this frame.
                            if (typed != r->value) {
                                moved.assign(typed.data(), typed.size());
                                if (settings.set(settings::Layer::user,
                                                  desc->key, moved)
                                        .has_value()) {
                                    ++changed;
                                    standing = &moved;
                                }
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
