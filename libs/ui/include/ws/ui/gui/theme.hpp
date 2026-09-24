#pragma once

#include <ws/ui/renderer2d.hpp>

#include <cstdint>

namespace ws::ui::gui {

// The look of the gui: one struct of plain values, no registries and no
// inheritance. Widgets take a const ChromeTheme& and read it inline, so
// a panel can carry its own variant without touching anyone else's
// look. Phase 6 wires colors and metrics; corner_radius is carried but
// only honored once r2d can draw rounded quads (Phase 8).
struct ChromeTheme {
    // Faces, packed 0xRRGGBBAA (r2d::rgba order).
    std::uint32_t panel_bg = r2d::rgba(38, 40, 46);
    std::uint32_t control_bg = r2d::rgba(70, 73, 78);
    std::uint32_t control_hovered = r2d::rgba(92, 96, 102);
    std::uint32_t control_pressed = r2d::rgba(72, 132, 192);
    std::uint32_t control_on = r2d::rgba(90, 168, 110);  // toggled/checked accent
    std::uint32_t text = r2d::rgba(235, 238, 242);
    std::uint32_t border = r2d::rgba(255, 255, 255, 36);

    // Metrics, gui-space pixels.
    float control_height = 24.0f;
    float corner_radius = 4.0f;  // drawn as real corners in Phase 8
    float padding = 12.0f;       // panel content inset
    float spacing = 8.0f;        // reserved for widget gaps (Phase 7+)
};

// The stock look. constexpr, so a custom theme starts from it with
// designated member overrides and stays a compile-time value.
inline constexpr ChromeTheme kDefaultChrome{};

}  // namespace ws::ui::gui
