#pragma once

// The theme as DATA (nextplan stage 10, phase 1). ChromeTheme stays
// the widget contract - constexpr, no strings, taken by const& per
// call - and this is the serializable document AROUND it: the font
// roles an app loads, the icon set it picks, the shadow tokens the
// renderer will draw, and the accent the derived faces came from.
// Theme JSON (phase 2) reads and writes this whole struct through
// ws::settings; widgets never see it.

#include <ws/ui/gui/theme.hpp>

#include <cstdint>
#include <string>

namespace ws::ui::gui {

// One font role: the family NAME (empty = the caller's own pick -
// the library resolves nothing) and the suggested pixel size the app
// bakes at. Roles are data v1: draw_text takes one font, so an app
// that wants bold or italic today loads the face it wants and points
// theme.chrome.font at it; the roles make the choice persistable and
// a settings-driven app able to honor it.
struct FontRole {
    std::string family;
    float size = 16.0f;
};

// Shadow tokens: data now, rendering later (the stage list names
// them; r2d draws no shadows yet). blur is px of softness, offset_y
// the px drop, color carries its own alpha.
struct ShadowTokens {
    std::uint32_t color = 0x00000066u;  // black, 40% alpha
    float blur = 8.0f;
    float offset_y = 2.0f;
};

// The full theme document: the widget-facing chrome plus everything
// app-facing a file should remember.
struct ThemeData {
    ChromeTheme chrome;
    // The accent apply_accent() last derived the faces from, forced
    // opaque - 0 means they were never derived (the stock
    // hand-picked palette).
    std::uint32_t accent = 0u;
    FontRole regular;
    FontRole bold;
    FontRole italic;
    FontRole mono;
    // The icon set id - an app-level string the app resolves to the
    // atlas it loaded. The library never opens it.
    std::string icon_set;
    ShadowTokens panel_shadow;
    ShadowTokens control_shadow;
};

// The stock documents: the dark and light chromes with role, icon
// and shadow defaults an app can start from. Out of line because
// std::string members keep them from being constexpr - functions,
// not constants, unlike kDefaultChrome/kLightChrome.
[[nodiscard]] ThemeData default_dark();
[[nodiscard]] ThemeData default_light();

// Derives the accent-driven faces into the document's chrome
// (ChromeTheme apply_accent, same rules) and remembers the accent -
// so a saved theme can say which color its faces came from and a
// gallery round-trip can re-derive them.
void apply_accent(ThemeData& theme, std::uint32_t accent);

}  // namespace ws::ui::gui
