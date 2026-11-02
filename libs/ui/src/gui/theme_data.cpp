#include <ws/ui/gui/theme_data.hpp>

#include <ws/ui/gui/theme_color.hpp>

namespace ws::ui::gui {

ThemeData default_dark() {
    ThemeData out;
    out.chrome = kDefaultChrome;
    // Everything else stays default-constructed except the families:
    // the stock documents NAME the bundled face (P1's Inter) so a
    // saved theme records what the stock look ran - the app still
    // resolves the name (r2d::load_default_font answers it), the
    // library never opens a file. Role sizes and the icon set stay
    // the caller's pick; the documents are starting points, not
    // prescriptions.
    out.regular.family = "Inter";
    out.bold.family = "Inter SemiBold";
    return out;
}

ThemeData default_light() {
    ThemeData out;
    out.chrome = kLightChrome;
    out.regular.family = "Inter";
    out.bold.family = "Inter SemiBold";
    return out;
}

void apply_accent(ThemeData& theme, std::uint32_t accent) {
    apply_accent(theme.chrome, accent);
    theme.accent = accent | 0x000000FFu;
}

}  // namespace ws::ui::gui
