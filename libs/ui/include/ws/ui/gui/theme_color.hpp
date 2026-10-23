#pragma once

// Theme color math (nextplan stage 10, phase 1): the perceptual
// (OKLCH) conversions an accent derivation walks in, the WCAG
// contrast ratio the checks are written against, and the two
// operations that make a theme answerable data - ask it which of its
// face pairs fail their check, and derive a checked face set from one
// accent color. Pure std on top of theme.hpp, no renderer and no
// platform: themes are data first (a file on disk next phase, a unit
// test's fixture today), so the math has to run headless. The stock
// themes ship contrast-passing; apply_accent keeps a caller's accent
// just as honest by walking OKLCH lightness until every checked pair
// clears its ratio.

#include <ws/ui/gui/theme.hpp>

#include <cstdint>
#include <vector>

namespace ws::ui::gui {

// A color in OKLCH - Bjorn Ottosson's Oklab in cylindrical form:
// perceptual lightness L in [0, 1], chroma C >= 0, hue H in degrees
// [0, 360). Chroma is what a derivation wants to hold steady while it
// walks L; H is what it must not lose. C ~ 0 makes H meaningless (a
// gray has no hue to preserve) - the round trip keeps whatever H the
// atan2 happened to answer and that is fine.
struct Oklch {
    double L = 0.0;
    double C = 0.0;
    double H = 0.0;
};

// The perceptual round trip. Both sides take the color's RGB half
// and leave alpha out of the math: oklch_from_rgb ignores it,
// rgb_from_oklch takes the alpha byte to emit (opaque by default).
// The conversions are color math, not face surgery - no derived face
// in this library is translucent. Out-of-gamut inputs (a caller may
// build any L/C/H) clamp per channel on the way back; an in-gamut
// sRGB color round-trips within one step of 8-bit quantization.
[[nodiscard]] Oklch oklch_from_rgb(std::uint32_t color);
[[nodiscard]] std::uint32_t rgb_from_oklch(
    const Oklch& oklch, std::uint32_t alpha = 0xFFu);

// The WCAG 2.x relative-luminance contrast ratio, in [1, 21]: the
// one number the checks below are written against. Alpha is ignored -
// contrast is an opaque-color question. Order-independent.
[[nodiscard]] double contrast_ratio(std::uint32_t a, std::uint32_t b);

// One pair that failed its check: the pair's name (a stable string
// the gallery can show next to a swatch), what it measured, and the
// ratio the check required.
struct ContrastFailure {
    const char* pair = "";
    double ratio = 0.0;
    double required = 0.0;
};

// The theme's checked pairs, failures only - empty means the theme
// passes. Text clears 4.5:1 against both backgrounds (WCAG AA body
// text); control_on and control_pressed - the faces that CARRY
// state, checked and pressed, the "identify the component" minimum
// of WCAG 1.4.11 - clear 3:1 against both. control_hovered is
// deliberately not a WCAG pair: the pointer itself is the indicator
// (1.4.11's own reading, and a 3:1 hover demand would force absurd
// hover faces); its contract is perceptual instead - a shift of at
// least 0.05 Oklab lightness off control_bg, which apply_accent
// derives by construction and the tests pin on the stock themes.
// Veil, border and the toast severities are exempt by design
// (overlay-on-arbitrary-content and decorative faces), and disabled
// faces are exempt per WCAG's inactive-component exception.
[[nodiscard]] std::vector<ContrastFailure> contrast_failures(
    const ChromeTheme& theme);

// Derives the accent-driven faces from one accent color:
// control_on (the accent itself, walked to its ratios),
// control_pressed (the same hue a step heavier - less chroma,
// lightness pulled toward the control face), control_hovered (the
// control face shifted 0.07 lightness toward the background's
// polarity), and control_disabled (the control face desaturated and
// pulled toward the panel - exempt from checks, derived for look).
// Each checked face walks OKLCH lightness AWAY from the
// backgrounds' polarity until its pairs clear 3:1; an accent that
// already clears passes through unmoved, and an unwinnable accent
// lands as far from its background as the gamut allows - the
// shortfall stays visible through contrast_failures(). Semantic
// faces (text, the backgrounds, veil, border, the toast severities)
// are never touched. Idempotent per accent: deriving the same accent
// onto the same theme yields the same faces.
void apply_accent(ChromeTheme& theme, std::uint32_t accent);

}  // namespace ws::ui::gui
