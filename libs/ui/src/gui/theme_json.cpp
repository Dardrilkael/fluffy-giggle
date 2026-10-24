#include <ws/ui/gui/theme_json.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ws::ui::gui {

// The settings json facility, under its own short name for this TU.
namespace json = ::ws::settings::json;

namespace {

// The fold tables: every serialized member as a (name, member pointer)
// pair, in the struct's own declaration order - the same order
// write_theme emits, so a written file reads like the header.

struct FaceEntry {
    const char* name;
    std::uint32_t ChromeTheme::*member;
};
constexpr FaceEntry kFaceEntries[] = {
    {"panel_bg", &ChromeTheme::panel_bg},
    {"control_bg", &ChromeTheme::control_bg},
    {"control_hovered", &ChromeTheme::control_hovered},
    {"control_pressed", &ChromeTheme::control_pressed},
    {"control_disabled", &ChromeTheme::control_disabled},
    {"control_on", &ChromeTheme::control_on},
    {"text", &ChromeTheme::text},
    {"border", &ChromeTheme::border},
    {"veil", &ChromeTheme::veil},
    {"toast_info", &ChromeTheme::toast_info},
    {"toast_warning", &ChromeTheme::toast_warning},
    {"toast_error", &ChromeTheme::toast_error},
};

struct MetricEntry {
    const char* name;
    float ChromeTheme::*member;
};
constexpr MetricEntry kMetricEntries[] = {
    {"control_height", &ChromeTheme::control_height},
    {"corner_radius", &ChromeTheme::corner_radius},
    {"padding", &ChromeTheme::padding},
    {"spacing", &ChromeTheme::spacing},
    {"scroll_speed", &ChromeTheme::scroll_speed},
    {"drag_threshold", &ChromeTheme::drag_threshold},
    {"tooltip_delay", &ChromeTheme::tooltip_delay},
    {"long_press_time", &ChromeTheme::long_press_time},
    {"menu_open_delay", &ChromeTheme::menu_open_delay},
    {"label_char_width", &ChromeTheme::label_char_width},
    {"label_inset", &ChromeTheme::label_inset},
    {"check_inset", &ChromeTheme::check_inset},
    {"slider_handle_width", &ChromeTheme::slider_handle_width},
    {"scrollbar_thumb_width", &ChromeTheme::scrollbar_thumb_width},
    {"scrollbar_band_width", &ChromeTheme::scrollbar_band_width},
    {"scrollbar_min_thumb", &ChromeTheme::scrollbar_min_thumb},
    {"disabled_alpha", &ChromeTheme::disabled_alpha},
};

struct FontEntry {
    const char* name;
    FontRole ThemeData::*member;
};
constexpr FontEntry kFontEntries[] = {
    {"regular", &ThemeData::regular},
    {"bold", &ThemeData::bold},
    {"italic", &ThemeData::italic},
    {"mono", &ThemeData::mono},
};

struct ShadowEntry {
    const char* name;
    ShadowTokens ThemeData::*member;
};
constexpr ShadowEntry kShadowEntries[] = {
    {"panel", &ThemeData::panel_shadow},
    {"control", &ThemeData::control_shadow},
};

// One hex digit, case-insensitive, or -1.
[[nodiscard]] int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// "#RRGGBB" (alpha FF implied) or "#RRGGBBAA", case-insensitive ->
// 0xRRGGBBAA (r2d::rgba order). `key` names the member in the refusal.
[[nodiscard]] ws::core::Result<std::uint32_t> color_from_hex(
    std::string_view text, std::string_view key) {
    const auto refused = [&] {
        return fail(ThemeError::bad_value,
                    std::string{key} + ": '" + std::string{text} +
                        "' is not a #RRGGBBAA color");
    };
    if (text.size() != 7 && text.size() != 9) {
        return refused();
    }
    if (text[0] != '#') {
        return refused();
    }
    std::uint32_t out = 0;
    for (std::size_t i = 1; i < text.size(); ++i) {
        const int digit = hex_digit(text[i]);
        if (digit < 0) {
            return refused();
        }
        out = (out << 4) | static_cast<std::uint32_t>(digit);
    }
    if (text.size() == 7) {
        out = (out << 8) | 0xFFu;
    }
    return out;
}

// Always 8-digit uppercase - the canonical written form.
[[nodiscard]] std::string to_hex(std::uint32_t color) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(9);
    out.push_back('#');
    for (int shift = 28; shift >= 0; shift -= 4) {
        out.push_back(digits[(color >> shift) & 0xFu]);
    }
    return out;
}

// A metric or size: an integer OR a real both fold (the writer emits
// reals, a hand edit may write "24" for 24.0).
[[nodiscard]] ws::core::Result<double> number_from(
    const json::Value& value, std::string_view key) {
    if (value.is_integer()) {
        return static_cast<double>(value.get_integer().value());
    }
    if (value.is_real()) {
        return value.get_real().value();
    }
    return fail(ThemeError::bad_value,
                std::string{key} + ": expected a number");
}

constexpr std::string_view kKnownMembers[] = {
    "version", "accent", "chrome", "fonts", "icon_set", "shadows",
};

[[nodiscard]] bool is_known_member(std::string_view key) {
    for (const std::string_view known : kKnownMembers) {
        if (key == known) {
            return true;
        }
    }
    return false;
}

// The top-level members the theme does not know, in file order - what
// ThemeFile remembers and save() re-emits verbatim.
[[nodiscard]] std::vector<json::Object::Entry> unknown_members(
    const json::Object& root) {
    std::vector<json::Object::Entry> out;
    for (const json::Object::Entry& entry : root) {
        if (!is_known_member(entry.first)) {
            out.push_back(entry);
        }
    }
    return out;
}

}  // namespace

namespace {

// Fold the document's known members into `out` (which starts as the
// base). All-or-nothing: the caller folds into a local and adopts it
// only on success. A member the text leaves out keeps the base value;
// a member with the wrong type fails the whole fold naming the key.
[[nodiscard]] ws::core::Status fold_theme(const json::Value& root,
                                          ThemeData& out) {
    const json::Object* obj = root.as_object();
    if (obj == nullptr) {
        return fail(ThemeError::bad_value,
                    "the theme document root must be an object");
    }

    if (const json::Value* accent = obj->find("accent")) {
        const std::string* text = accent->as_text();
        if (text == nullptr) {
            return fail(ThemeError::bad_value,
                        "accent: expected a #RRGGBBAA color");
        }
        WS_TRY_ASSIGN(out.accent, color_from_hex(*text, "accent"));
    }

    if (const json::Value* chrome = obj->find("chrome")) {
        const json::Object* chrome_obj = chrome->as_object();
        if (chrome_obj == nullptr) {
            return fail(ThemeError::bad_value,
                        "chrome: expected an object");
        }
        for (const FaceEntry& entry : kFaceEntries) {
            const json::Value* value = chrome_obj->find(entry.name);
            if (value == nullptr) {
                continue;
            }
            const std::string* text = value->as_text();
            if (text == nullptr) {
                return fail(ThemeError::bad_value,
                            std::string{"chrome."} + entry.name +
                                ": expected a #RRGGBBAA color");
            }
            WS_TRY_ASSIGN(out.chrome.*entry.member,
                          color_from_hex(
                              *text, std::string{"chrome."} + entry.name));
        }
        for (const MetricEntry& entry : kMetricEntries) {
            const json::Value* value = chrome_obj->find(entry.name);
            if (value == nullptr) {
                continue;
            }
            WS_TRY_ASSIGN(double number,
                          number_from(*value,
                                      std::string{"chrome."} + entry.name));
            out.chrome.*entry.member = static_cast<float>(number);
        }
    }

    if (const json::Value* fonts = obj->find("fonts")) {
        const json::Object* fonts_obj = fonts->as_object();
        if (fonts_obj == nullptr) {
            return fail(ThemeError::bad_value, "fonts: expected an object");
        }
        for (const FontEntry& entry : kFontEntries) {
            const json::Value* value = fonts_obj->find(entry.name);
            if (value == nullptr) {
                continue;
            }
            const json::Object* role = value->as_object();
            if (role == nullptr) {
                return fail(ThemeError::bad_value,
                            std::string{"fonts."} + entry.name +
                                ": expected an object");
            }
            const std::string key = std::string{"fonts."} + entry.name;
            FontRole& target = out.*entry.member;
            if (const json::Value* family = role->find("family")) {
                const std::string* text = family->as_text();
                if (text == nullptr) {
                    return fail(ThemeError::bad_value,
                                key + ".family: expected a string");
                }
                target.family = *text;
            }
            if (const json::Value* size = role->find("size")) {
                WS_TRY_ASSIGN(double number,
                              number_from(*size, key + ".size"));
                target.size = static_cast<float>(number);
            }
        }
    }

    if (const json::Value* icon_set = obj->find("icon_set")) {
        const std::string* text = icon_set->as_text();
        if (text == nullptr) {
            return fail(ThemeError::bad_value,
                        "icon_set: expected a string");
        }
        out.icon_set = *text;
    }

    if (const json::Value* shadows = obj->find("shadows")) {
        const json::Object* shadows_obj = shadows->as_object();
        if (shadows_obj == nullptr) {
            return fail(ThemeError::bad_value,
                        "shadows: expected an object");
        }
        for (const ShadowEntry& entry : kShadowEntries) {
            const json::Value* value = shadows_obj->find(entry.name);
            if (value == nullptr) {
                continue;
            }
            const json::Object* token = value->as_object();
            if (token == nullptr) {
                return fail(ThemeError::bad_value,
                            std::string{"shadows."} + entry.name +
                                ": expected an object");
            }
            const std::string key = std::string{"shadows."} + entry.name;
            ShadowTokens& target = out.*entry.member;
            if (const json::Value* color = token->find("color")) {
                const std::string* text = color->as_text();
                if (text == nullptr) {
                    return fail(ThemeError::bad_value,
                                key + ".color: expected a #RRGGBBAA color");
                }
                WS_TRY_ASSIGN(target.color,
                              color_from_hex(*text, key + ".color"));
            }
            if (const json::Value* blur = token->find("blur")) {
                WS_TRY_ASSIGN(double number,
                              number_from(*blur, key + ".blur"));
                target.blur = static_cast<float>(number);
            }
            if (const json::Value* offset_y = token->find("offset_y")) {
                WS_TRY_ASSIGN(double number,
                              number_from(*offset_y, key + ".offset_y"));
                target.offset_y = static_cast<float>(number);
            }
        }
    }
    return {};
}

// --- The write side ---------------------------------------------------------

[[nodiscard]] json::Value chrome_object(const ChromeTheme& chrome) {
    json::Object out;
    for (const FaceEntry& entry : kFaceEntries) {
        out.set(entry.name, json::Value::text(to_hex(chrome.*entry.member)));
    }
    for (const MetricEntry& entry : kMetricEntries) {
        out.set(entry.name,
                json::Value::real(static_cast<double>(chrome.*entry.member)));
    }
    return json::Value::object(std::move(out));
}

[[nodiscard]] json::Value font_role_object(const FontRole& role) {
    json::Object out;
    out.set("family", json::Value::text(role.family));
    out.set("size", json::Value::real(static_cast<double>(role.size)));
    return json::Value::object(std::move(out));
}

[[nodiscard]] json::Value fonts_object(const ThemeData& theme) {
    json::Object out;
    for (const FontEntry& entry : kFontEntries) {
        out.set(entry.name, font_role_object(theme.*entry.member));
    }
    return json::Value::object(std::move(out));
}

[[nodiscard]] json::Value shadow_object(const ShadowTokens& token) {
    json::Object out;
    out.set("color", json::Value::text(to_hex(token.color)));
    out.set("blur", json::Value::real(static_cast<double>(token.blur)));
    out.set("offset_y",
            json::Value::real(static_cast<double>(token.offset_y)));
    return json::Value::object(std::move(out));
}

[[nodiscard]] json::Value shadows_object(const ThemeData& theme) {
    json::Object out;
    for (const ShadowEntry& entry : kShadowEntries) {
        out.set(entry.name, shadow_object(theme.*entry.member));
    }
    return json::Value::object(std::move(out));
}

// The whole document: knowns in canonical order, then the remembered
// unknowns in their file order. Unknown names cannot collide with the
// knowns (they were collected as "not one of these"), so set()
// appends.
[[nodiscard]] json::Value theme_document(
    const ThemeData& theme,
    const std::vector<json::Object::Entry>& unknowns) {
    json::Object root;
    root.set("version", json::Value::integer(kThemeShapeVersion));
    root.set("accent", json::Value::text(to_hex(theme.accent)));
    root.set("chrome", chrome_object(theme.chrome));
    root.set("fonts", fonts_object(theme));
    root.set("icon_set", json::Value::text(theme.icon_set));
    root.set("shadows", shadows_object(theme));
    for (const json::Object::Entry& entry : unknowns) {
        root.set(entry.first, entry.second);
    }
    return json::Value::object(std::move(root));
}

// Parse + the version walk - the shared front half of read_theme and
// every ThemeFile read. The inner settings error's reason rides the
// message; the theme's own code and category replace its own.
[[nodiscard]] ws::core::Result<json::Value> parse_theme_document(
    std::string_view text, const ws::settings::Migrations& migrations) {
    auto parsed = json::parse(text);
    if (!parsed) {
        return fail(ThemeError::parse_failed,
                    "parse: " + parsed.error().message);
    }
    if (!parsed->is_object()) {
        return fail(ThemeError::bad_value,
                    "the theme document root must be an object");
    }
    const ws::core::Status walked =
        migrations.run(*parsed, kThemeShapeVersion);
    if (!walked) {
        return fail(ThemeError::migrate_failed,
                    "migrate: " + walked.error().message);
    }
    return parsed;
}

}  // namespace

// --- The text API ----------------------------------------------------------

ws::core::Result<ThemeData> read_theme(
    std::string_view text, const ThemeData& base,
    const ws::settings::Migrations& migrations) {
    WS_TRY_ASSIGN(auto document, parse_theme_document(text, migrations));
    ThemeData folded = base;
    WS_TRY(fold_theme(document, folded));
    return folded;
}

std::string write_theme(const ThemeData& theme) {
    return json::write(theme_document(theme, {}));
}

// --- The file layer --------------------------------------------------------

ThemeFile::ThemeFile(std::string path, ws::settings::FileSystem& fs,
                     const ws::settings::Migrations& migrations,
                     ThemeData base, std::int64_t poll_ms, Clock clock)
    : path_(std::move(path)),
      fs_(fs),
      migrations_(migrations),
      base_(std::move(base)),
      theme_(base_),
      poll_ms_(poll_ms < 0 ? 0 : poll_ms) {
    if (clock) {
        clock_ = std::move(clock);
    } else {
        clock_ = [] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now()
                           .time_since_epoch())
                .count();
        };
    }
}

ThemeLoad ThemeFile::load() {
    ThemeLoad out;
    out.theme = base_;
    load_ran_ = true;
    // The poll cadence starts here: load() just read the file, so the
    // first poll() waits a full poll_ms before reading again.
    last_poll_ = clock_();

    auto read = fs_.read(path_);
    if (!read) {
        if (!fs_.exists(path_)) {
            out.missing = true;  // a first run, not a failure
            return out;
        }
        out.failed = true;
        out.error = read.error().message;
        return out;
    }

    auto document = parse_theme_document(*read, migrations_);
    if (!document) {
        // Remember the bad bytes so the poller stays quiet until the
        // file changes - the load already reported this failure.
        bytes_ = std::move(*read);
        out.failed = true;
        out.error = document.error().message;
        return out;
    }
    ThemeData folded = base_;
    const ws::core::Status folded_status = fold_theme(*document, folded);
    if (!folded_status) {
        bytes_ = std::move(*read);
        out.failed = true;
        out.error = folded_status.error().message;
        return out;
    }
    unknowns_ = unknown_members(*document->as_object());
    theme_ = std::move(folded);
    bytes_ = std::move(*read);
    out.theme = theme_;
    return out;
}

ws::core::Status ThemeFile::save() {
    const std::string text = json::write(theme_document(theme_, unknowns_));
    const ws::core::Status written = fs_.write(path_, text);
    if (!written) {
        return fail(ThemeError::io_failed, written.error().message);
    }
    bytes_ = text;  // the poller must not read our own save as a reload
    return {};
}

ThemePoll ThemeFile::poll() {
    ThemePoll out;
    if (!load_ran_) {
        return out;  // nothing loaded yet - the poll owes nothing
    }
    const std::int64_t now = clock_();
    if (now - last_poll_ < poll_ms_) {
        return out;
    }
    last_poll_ = now;

    auto read = fs_.read(path_);
    if (!read) {
        if (!fs_.exists(path_)) {
            if (bytes_.empty()) {
                return out;  // never there: nothing to watch
            }
            // Deleted out from under a good load: report once, keep
            // the loaded theme, then quiet until a file reappears.
            bytes_.clear();
            out.failed = true;
            out.error = "the theme file was removed; keeping the loaded theme";
            return out;
        }
        out.failed = true;
        out.error = read.error().message;
        return out;
    }
    if (*read == bytes_) {
        return out;  // unchanged
    }

    auto document = parse_theme_document(*read, migrations_);
    if (!document) {
        bytes_ = std::move(*read);
        out.failed = true;
        out.error = document.error().message;
        return out;
    }
    ThemeData folded = base_;
    const ws::core::Status folded_status = fold_theme(*document, folded);
    if (!folded_status) {
        bytes_ = std::move(*read);
        out.failed = true;
        out.error = folded_status.error().message;
        return out;
    }
    unknowns_ = unknown_members(*document->as_object());
    theme_ = std::move(folded);
    bytes_ = std::move(*read);
    out.reloaded = true;
    return out;
}

void ThemeFile::set_theme(ThemeData theme) {
    theme_ = std::move(theme);
}

}  // namespace ws::ui::gui
