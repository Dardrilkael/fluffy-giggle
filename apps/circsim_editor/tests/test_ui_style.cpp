// apps/circsim_editor - the visual identity's contract (ui_style.hpp).
//
// Pure data under test: no canvas, no frame. The editor's two chromes
// must pass the same WCAG pair checks the library's stock themes do,
// and every device ink must stay legible on the canvas it is drawn on.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "../src/ui_style.hpp"

#include <ws/ui/gui/theme_color.hpp>

#include <string>

namespace {

using editor::style::device_color;
using editor::style::kDaylightChrome;
using editor::style::kNightChrome;
using ws::ui::gui::contrast_failures;
using ws::ui::gui::contrast_ratio;
using ws::ui::gui::ContrastFailure;

// Every letter device_color() maps, so a new family cannot ship
// without joining the contrast sweep below.
constexpr char kFamilyLetters[] = {'R', 'C', 'L', 'K', 'V', 'I', 'D',
                                   'Q', 'M', 'J', 'E', 'F', 'G', 'H',
                                   'B', 'S', 'W', 'X', 'T'};

std::string describe(const std::vector<ContrastFailure>& failures) {
    std::string out;
    for (const ContrastFailure& f : failures) {
        out += std::string(f.pair) + " (" + std::to_string(f.ratio) +
               " < " + std::to_string(f.required) + ") ";
    }
    return out;
}

}  // namespace

TEST_CASE("the night chrome passes the library's contrast checks") {
    const auto failures = contrast_failures(kNightChrome);
    CHECK_MESSAGE(failures.empty(), describe(failures));
}

TEST_CASE("the daylight chrome passes the library's contrast checks") {
    const auto failures = contrast_failures(kDaylightChrome);
    CHECK_MESSAGE(failures.empty(), describe(failures));
}

TEST_CASE("every accent preset passes the contrast checks on both chromes") {
    for (const editor::style::AccentPreset& preset :
         editor::style::kAccentPresets) {
        const auto night = contrast_failures(editor::style::accent_chrome(
            ws::ui::gui::kDefaultChrome, preset.key));
        CHECK_MESSAGE(night.empty(),
                      preset.key << " on night: " << describe(night));
        const auto day = contrast_failures(editor::style::accent_chrome(
            ws::ui::gui::kLightChrome, preset.key));
        CHECK_MESSAGE(day.empty(),
                      preset.key << " on day: " << describe(day));
    }
}

TEST_CASE("accent_preset: unknown keys fall back to the default") {
    CHECK(&editor::style::accent_preset("") ==
          &editor::style::kAccentPresets.front());
    CHECK(&editor::style::accent_preset("chartreuse") ==
          &editor::style::kAccentPresets.front());
    CHECK(editor::style::accent_preset("teal").key == "teal");
    // The default preset leaves the tuned chrome byte-for-byte alone.
    const ws::ui::gui::ChromeTheme def = editor::style::accent_chrome(
        ws::ui::gui::kDefaultChrome, "indigo");
    CHECK(def.control_on == kNightChrome.control_on);
    CHECK(def.control_pressed == kNightChrome.control_pressed);
}

TEST_CASE("chrome_for follows the stock theme's brightness") {
    CHECK(&editor::style::chrome_for(ws::ui::gui::kDefaultChrome) ==
          &kNightChrome);
    CHECK(&editor::style::chrome_for(ws::ui::gui::kLightChrome) ==
          &kDaylightChrome);
    // A custom dark theme still lands on the night side.
    ws::ui::gui::ChromeTheme custom = ws::ui::gui::kDefaultChrome;
    custom.panel_bg = ws::ui::r2d::rgba(10, 10, 10);
    CHECK(&editor::style::chrome_for(custom) == &kNightChrome);
}

TEST_CASE("the canvas sits a step away from the panels") {
    // Dark: the drawing is the darkest surface after the bars.
    CHECK(contrast_ratio(kNightChrome.canvas_bg, kNightChrome.panel_bg) >
          1.05);
    // Light: the canvas is the brightest surface.
    CHECK(contrast_ratio(kDaylightChrome.canvas_bg,
                         kDaylightChrome.panel_bg) > 1.0);
}

TEST_CASE("every device ink clears 3:1 on its own canvas") {
    for (const char letter : kFamilyLetters) {
        const std::uint32_t dark_ink = device_color(letter, true, 0u);
        const std::uint32_t light_ink = device_color(letter, false, 0u);
        REQUIRE_MESSAGE(dark_ink != 0u,
                        "no dark ink for family " << letter);
        REQUIRE_MESSAGE(light_ink != 0u,
                        "no light ink for family " << letter);
        CHECK_MESSAGE(contrast_ratio(dark_ink, kNightChrome.canvas_bg) >= 3.0,
                      "dark ink for " << letter << " is too dim");
        CHECK_MESSAGE(
            contrast_ratio(light_ink, kDaylightChrome.canvas_bg) >= 3.0,
            "light ink for " << letter << " is too pale");
    }
}

TEST_CASE("device_color falls back for power, ground and unknown letters") {
    constexpr std::uint32_t kFallback = 0x123456FFu;
    CHECK(device_color(0, true, kFallback) == kFallback);
    CHECK(device_color('?', false, kFallback) == kFallback);
}

TEST_CASE("device_color is case-insensitive and groups families") {
    CHECK(device_color('r', true, 0u) == device_color('R', true, 0u));
    // V and I share the source ink; L and K share the magnetic ink;
    // the three transistor letters share one ink.
    CHECK(device_color('V', true, 0u) == device_color('I', true, 0u));
    CHECK(device_color('L', true, 0u) == device_color('K', true, 0u));
    CHECK(device_color('Q', true, 0u) == device_color('M', true, 0u));
    CHECK(device_color('M', true, 0u) == device_color('J', true, 0u));
    // ...and distinct families stay distinct.
    CHECK(device_color('R', true, 0u) != device_color('C', true, 0u));
    CHECK(device_color('D', true, 0u) != device_color('Q', true, 0u));
}
