#include <ws/ui/gui/theme_data.hpp>

#include <ws/ui/gui/theme_color.hpp>

namespace ws::ui::gui {

ThemeData default_dark() {
    ThemeData out;
    out.chrome = kDefaultChrome;
    // Everything else stays default-constructed: empty families (the
    // caller's own pick), 16px role sizes, no icon set, the stock
    // shadow tokens. The documents are starting points, not
    // prescriptions.
    return out;
}

ThemeData default_light() {
    ThemeData out;
    out.chrome = kLightChrome;
    return out;
}

void apply_accent(ThemeData& theme, std::uint32_t accent) {
    apply_accent(theme.chrome, accent);
    theme.accent = accent | 0x000000FFu;
}

}  // namespace ws::ui::gui
