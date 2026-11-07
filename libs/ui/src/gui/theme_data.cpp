#include <ws/ui/gui/theme_data.hpp>

#include <ws/ui/gui/theme_color.hpp>

namespace ws::ui::gui {

ThemeData default_dark() {
    ThemeData out;
    out.chrome = kDefaultChrome;
    // Everything else stays default-constructed except the families:
    // the stock documents NAME the bundled faces (P1's Inter, the
    // typography pass's JetBrains Mono) so a saved theme records what
    // the stock look ran - the app still resolves the name
    // (r2d::load_default_font / load_default_mono_font answer them),
    // the library never opens a file. Role sizes and the icon set stay
    // the caller's pick; the documents are starting points, not
    // prescriptions.
    out.regular.family = "Inter";
    out.bold.family = "Inter SemiBold";
    out.mono.family = "JetBrains Mono";
    return out;
}

ThemeData default_light() {
    ThemeData out;
    out.chrome = kLightChrome;
    out.regular.family = "Inter";
    out.bold.family = "Inter SemiBold";
    out.mono.family = "JetBrains Mono";
    return out;
}

void apply_accent(ThemeData& theme, std::uint32_t accent) {
    apply_accent(theme.chrome, accent);
    theme.accent = accent | 0x000000FFu;
}

}  // namespace ws::ui::gui
