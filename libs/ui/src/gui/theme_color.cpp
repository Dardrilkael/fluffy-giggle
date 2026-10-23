#include <ws/ui/gui/theme_color.hpp>

#include <algorithm>
#include <cmath>

namespace ws::ui::gui {
namespace {

constexpr double kPi = 3.14159265358979323846;

// sRGB channel <-> linear light, the same 2.4-power piecewise both
// the WCAG luminance and Oklab define "linear" by. Only what they DO
// with linear differs - weight it for luminance, cube-root it for
// perceptual lightness. Not constexpr: pow/cbrt are runtime math in
// C++20, and nothing needs these at compile time.
inline double to_linear(double channel) {
    return channel <= 0.04045
               ? channel / 12.92
               : std::pow((channel + 0.055) / 1.055, 2.4);
}

inline double from_linear(double light) {
    return light <= 0.0031308
               ? light * 12.92
               : 1.055 * std::pow(light, 1.0 / 2.4) - 0.055;
}

inline double clamp01(double value) {
    if (value < 0.0) {
        return 0.0;
    }
    if (value > 1.0) {
        return 1.0;
    }
    return value;
}

// One color as linear-channel triples, the shape the Oklab matrices
// consume and produce.
struct Rgb {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
};

// 0xRRGGBBAA (r2d::rgba order: R is the top byte, alpha the LOW
// one) -> [0, 1] channels. Alpha stays behind - the conversions are
// color math, not face surgery.
[[nodiscard]] Rgb unpack(std::uint32_t color) {
    Rgb out;
    out.r = static_cast<double>((color >> 24) & 0xFFu) / 255.0;
    out.g = static_cast<double>((color >> 16) & 0xFFu) / 255.0;
    out.b = static_cast<double>((color >> 8) & 0xFFu) / 255.0;
    return out;
}

// [0, 1] channels (clamped) + the alpha byte -> 0xRRGGBBAA. The only
// rounding in the library: an Oklab value lands on the nearest
// representable color, which is what "derive a face" means.
[[nodiscard]] std::uint32_t pack(const Rgb& rgb, std::uint32_t alpha) {
    const auto byte = [](double value) -> int {
        if (value <= 0.0) {
            return 0;
        }
        if (value >= 1.0) {
            return 255;
        }
        return static_cast<int>(std::lround(value * 255.0));
    };
    const std::uint32_t r = static_cast<std::uint32_t>(byte(rgb.r));
    const std::uint32_t g = static_cast<std::uint32_t>(byte(rgb.g));
    const std::uint32_t b = static_cast<std::uint32_t>(byte(rgb.b));
    return (r << 24) | (g << 16) | (b << 8) | (alpha & 0xFFu);
}

// The WCAG relative luminance of the color's RGB half.
[[nodiscard]] double relative_luminance(std::uint32_t color) {
    const Rgb srgb = unpack(color);
    return 0.2126 * to_linear(srgb.r) + 0.7152 * to_linear(srgb.g) +
           0.0722 * to_linear(srgb.b);
}

}  // namespace

Oklch oklch_from_rgb(std::uint32_t color) {
    const Rgb srgb = unpack(color);
    const double r = to_linear(srgb.r);
    const double g = to_linear(srgb.g);
    const double b = to_linear(srgb.b);

    // Oklab via the LMS cube roots (Bjorn Ottosson's basis). The
    // long names are the axes: lightness, then the two opponent
    // axes Oklab's a/b really are.
    const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g +
                               0.0514459929 * b);
    const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g +
                              0.1073969566 * b);
    const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g +
                               0.6299787005 * b);

    const double lightness =
        0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
    const double axis_a =
        1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
    const double axis_b =
        0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;

    Oklch out;
    out.L = lightness;
    out.C = std::sqrt(axis_a * axis_a + axis_b * axis_b);
    out.H = std::atan2(axis_b, axis_a) * (180.0 / kPi);
    if (out.H < 0.0) {
        out.H += 360.0;
    }
    return out;
}

std::uint32_t rgb_from_oklch(const Oklch& oklch, std::uint32_t alpha) {
    const double hue = oklch.H * (kPi / 180.0);
    const double a = oklch.C * std::cos(hue);
    const double b = oklch.C * std::sin(hue);

    // The Oklab inverse: back through the opponent axes, cube the
    // LMS channels (cube is cbrt's own inverse, negatives included),
    // then the linear-RGB matrix. Out-of-gamut results clamp per
    // channel on the way out.
    const double l_ = oklch.L + 0.3963377774 * a + 0.2158037573 * b;
    const double m_ = oklch.L - 0.1055613458 * a - 0.0638541728 * b;
    const double s_ = oklch.L - 0.0894841775 * a - 1.2914855480 * b;

    const double l3 = l_ * l_ * l_;
    const double m3 = m_ * m_ * m_;
    const double s3 = s_ * s_ * s_;

    Rgb linear;
    linear.r = 4.0767416621 * l3 - 3.3077115913 * m3 + 0.2309699292 * s3;
    linear.g = -1.2684380046 * l3 + 2.6097574011 * m3 - 0.3413193965 * s3;
    linear.b = -0.0041960863 * l3 - 0.7034186147 * m3 + 1.7076147010 * s3;

    Rgb out;
    out.r = from_linear(clamp01(linear.r));
    out.g = from_linear(clamp01(linear.g));
    out.b = from_linear(clamp01(linear.b));
    return pack(out, alpha);
}

double contrast_ratio(std::uint32_t a, std::uint32_t b) {
    const double first = relative_luminance(a);
    const double second = relative_luminance(b);
    const double lighter = first > second ? first : second;
    const double darker = first > second ? second : first;
    return (lighter + 0.05) / (darker + 0.05);
}

std::vector<ContrastFailure> contrast_failures(const ChromeTheme& theme) {
    struct Pair {
        const char* name;
        std::uint32_t face;
        std::uint32_t against;
        double required;
    };
    const Pair pairs[] = {
        {"text / panel_bg", theme.text, theme.panel_bg, 4.5},
        {"text / control_bg", theme.text, theme.control_bg, 4.5},
        {"control_on / panel_bg", theme.control_on, theme.panel_bg, 3.0},
        {"control_on / control_bg", theme.control_on, theme.control_bg, 3.0},
        {"control_pressed / panel_bg", theme.control_pressed,
         theme.panel_bg, 3.0},
        {"control_pressed / control_bg", theme.control_pressed,
         theme.control_bg, 3.0},
    };
    std::vector<ContrastFailure> failures;
    for (const Pair& pair : pairs) {
        const double ratio = contrast_ratio(pair.face, pair.against);
        if (ratio < pair.required - 1e-9) {
            failures.push_back({pair.name, ratio, pair.required});
        }
    }
    return failures;
}

namespace {

// Walks `face`'s OKLCH lightness in `direction` (+1 lightens, the way
// away from a dark background; -1 darkens, away from a light one)
// until it clears `required` against both backgrounds, or lightness
// clamps at a gamut end. A face that already clears returns as it
// came - accents are never drifted for nothing - and an unwinnable
// walk lands on the best candidate it saw, as far from its
// backgrounds as the space allowed, so the shortfall stays honest
// through contrast_failures().
[[nodiscard]] std::uint32_t ensure_contrast(std::uint32_t face,
                                            std::uint32_t bg_a,
                                            std::uint32_t bg_b,
                                            double required,
                                            double direction) {
    const auto clears = [&](std::uint32_t color) {
        return contrast_ratio(color, bg_a) >= required &&
               contrast_ratio(color, bg_b) >= required;
    };
    if (clears(face)) {
        return face;
    }

    Oklch ok = oklch_from_rgb(face);
    std::uint32_t best = face;
    double best_gap =
        std::min(contrast_ratio(face, bg_a), contrast_ratio(face, bg_b));
    for (int step = 0; step < 100; ++step) {
        ok.L += 0.02 * direction;
        if (ok.L < 0.0) {
            ok.L = 0.0;
        } else if (ok.L > 1.0) {
            ok.L = 1.0;
        }
        const std::uint32_t candidate = rgb_from_oklch(ok);
        const double gap = std::min(contrast_ratio(candidate, bg_a),
                                    contrast_ratio(candidate, bg_b));
        if (gap > best_gap) {
            best_gap = gap;
            best = candidate;
        }
        if (clears(candidate)) {
            return candidate;
        }
        if (ok.L <= 0.0 || ok.L >= 1.0) {
            break;
        }
    }
    return best;
}

}  // namespace

void apply_accent(ChromeTheme& theme, std::uint32_t accent) {
    // Alpha never enters the math - a derived face is opaque, so the
    // accent's own alpha is forced on here and preserved in what
    // ThemeData::apply_accent remembers.
    const std::uint32_t opaque = accent | 0x000000FFu;
    // Which way lightness must go to gain contrast: away from the
    // panel's polarity.
    const bool dark = oklch_from_rgb(theme.panel_bg).L < 0.5;
    const double away = dark ? 1.0 : -1.0;

    // control_on: the accent itself, walked to its interactive
    // ratios - the face "checked" and the focus ring draw.
    theme.control_on = ensure_contrast(opaque, theme.panel_bg,
                                       theme.control_bg, 3.0, away);

    // control_pressed: the same hue a step heavier - chroma down,
    // lightness pulled a quarter of the way to the control face -
    // then walked to the same ratios, so "pressed" reads as the same
    // color working harder, not a different one.
    Oklch press = oklch_from_rgb(theme.control_on);
    press.C *= 0.7;
    press.L += (oklch_from_rgb(theme.control_bg).L - press.L) * 0.25;
    theme.control_pressed = ensure_contrast(rgb_from_oklch(press),
                                            theme.panel_bg,
                                            theme.control_bg, 3.0, away);

    // control_hovered: no WCAG pair (the pointer is the indicator) -
    // the contract is perceptual, a 0.07 lightness shift toward the
    // background's polarity off the control face.
    Oklch hover = oklch_from_rgb(theme.control_bg);
    hover.L += away * 0.07;
    theme.control_hovered = rgb_from_oklch(hover);

    // control_disabled: desaturated and pulled toward the panel, the
    // "less there" look. Exempt from checks (WCAG's inactive
    // exception) - aesthetic derivation only.
    Oklch dead = oklch_from_rgb(theme.control_bg);
    dead.C *= 0.5;
    dead.L += (oklch_from_rgb(theme.panel_bg).L - dead.L) * 0.4;
    theme.control_disabled = rgb_from_oklch(dead);
}

}  // namespace ws::ui::gui
