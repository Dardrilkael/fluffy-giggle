// The P2 token-pass acceptance (the ui-skin plan): every new
// ChromeTheme field is real data - defaulted, overridable, scaled where
// px, carried where timing, serialized where serializable - and the
// stock themes still answer the (now larger) contrast table. The JSON
// half rides the same read/write fold tables the stage-10 suite pinned.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <ws/ui/gui/theme.hpp>
#include <ws/ui/gui/theme_color.hpp>
#include <ws/ui/gui/theme_data.hpp>
#include <ws/ui/gui/theme_json.hpp>

#include <string>

namespace {

using ws::ui::gui::ChromeTheme;
using ws::ui::gui::contrast_failures;
using ws::ui::gui::contrast_ratio;
using ws::ui::gui::default_dark;
using ws::ui::gui::default_light;
using ws::ui::gui::kDefaultChrome;
using ws::ui::gui::kLightChrome;
using ws::ui::gui::read_theme;
using ws::ui::gui::write_theme;

}  // namespace

TEST_CASE("the radius roles inherit corner_radius until overridden") {
    // The back-compat rule: one number restyles everything.
    CHECK(kDefaultChrome.control_radius() == kDefaultChrome.corner_radius);
    CHECK(kDefaultChrome.panel_radius() == kDefaultChrome.corner_radius);
    CHECK(kDefaultChrome.popup_radius() == kDefaultChrome.corner_radius);
    CHECK(kDefaultChrome.pill_radius() == kDefaultChrome.corner_radius);

    // Four numbers restyle one role each.
    ChromeTheme square = kDefaultChrome;
    square.corner_radius = 7.0f;
    square.radius_control = 0.0f;
    square.radius_popup = 2.0f;
    CHECK(square.control_radius() == 0.0f);
    CHECK(square.popup_radius() == 2.0f);
    CHECK(square.panel_radius() == 7.0f);   // the inherited default
    CHECK(square.pill_radius() == 7.0f);

    // scaled() keeps the sentinel a sentinel and scales the resolved
    // answer with it.
    const ChromeTheme big = square.scaled(2.0f);
    CHECK(big.radius_control == 0.0f);
    CHECK(big.radius_popup == 4.0f);
    CHECK(big.panel_radius() == 14.0f);  // corner_radius scaled
    CHECK(big.control_radius() == 0.0f);
    CHECK(big.panel_radius() == 14.0f);
}

TEST_CASE("the new metrics scale like px and the motion tokens do not") {
    ChromeTheme t = kDefaultChrome;
    t.border_width = 2.0f;
    t.focus_ring_width = 1.5f;
    t.shadow_blur = 9.0f;
    t.shadow_offset_y = 3.0f;
    t.anim_duration = 0.25f;
    t.anim_enabled = false;

    const ChromeTheme big = t.scaled(2.0f);
    CHECK(big.control_height == t.control_height * 2.0f);
    CHECK(big.border_width == 4.0f);
    CHECK(big.focus_ring_width == 3.0f);
    CHECK(big.shadow_blur == 18.0f);
    CHECK(big.shadow_offset_y == 6.0f);
    // Timings and switches carry as-is: a denser screen does not make a
    // user faster.
    CHECK(big.anim_duration == 0.25f);
    CHECK(big.anim_enabled == false);
    CHECK(big.tooltip_delay == t.tooltip_delay);

    // The one deliberate stock bump, pinned.
    CHECK(kDefaultChrome.control_height == 28.0f);
    // The shadow tokens wait for P5's render: the stock default is none.
    CHECK(kDefaultChrome.shadow_blur == 0.0f);
    CHECK(kDefaultChrome.shadow_offset_y == 0.0f);
}

TEST_CASE("both stock themes answer the grown contrast table") {
    for (const ChromeTheme* chrome : {&kDefaultChrome, &kLightChrome}) {
        const auto failures = contrast_failures(*chrome);
        for (const ws::ui::gui::ContrastFailure& failure : failures) {
            MESSAGE("unpinned pair: ", failure.pair, " ratio ",
                    failure.ratio, " required ", failure.required);
        }
        CHECK(failures.empty());
    }

    // The new pairs specifically: the quiet rung is real text (4.5),
    // the muted rung a hint at the component ratio (3.0) on every face
    // it can land on, and the on-accent label clears the component
    // ratio (3.0) on both accent faces.
    for (const ChromeTheme* chrome : {&kDefaultChrome, &kLightChrome}) {
        CHECK(contrast_ratio(chrome->text_secondary, chrome->panel_bg) >=
              4.5);
        CHECK(contrast_ratio(chrome->text_secondary, chrome->control_bg) >=
              4.5);
        CHECK(contrast_ratio(chrome->text_muted, chrome->panel_bg) >= 3.0);
        CHECK(contrast_ratio(chrome->text_muted, chrome->control_bg) >= 3.0);
        CHECK(contrast_ratio(chrome->text_muted, chrome->bar_bg) >= 3.0);
        CHECK(contrast_ratio(chrome->text_on_accent, chrome->control_on) >=
              3.0);
        CHECK(contrast_ratio(chrome->text_on_accent,
                             chrome->control_pressed) >= 3.0);
    }

    // A theme that breaks a new pair is caught: the quiet rung drowned
    // into the background fails by name.
    ChromeTheme drowned = kDefaultChrome;
    drowned.text_secondary = drowned.panel_bg;
    const auto failures = contrast_failures(drowned);
    REQUIRE(failures.size() >= 2);
    CHECK(std::string{failures.front().pair} ==
          std::string{"text_secondary / panel_bg"});
}

TEST_CASE("apply_accent derives the accent's hover rung and the on-accent label") {
    using ws::ui::gui::apply_accent;

    for (const ChromeTheme* stock : {&kDefaultChrome, &kLightChrome}) {
        ChromeTheme theme = *stock;
        apply_accent(theme, 0x54A054FFu);  // a mid green

        // The on-accent label clears 3:1 against BOTH derived accent
        // faces (or best-effort lands as far as it could - a mid
        // lightness accent can be unwinnable, the contract says; green
        // at L .62 on dark clears with white after control_on walks).
        CHECK(contrast_ratio(theme.text_on_accent, theme.control_on) >=
              3.0 - 1e-9);
        // The hover rung exists and differs from the resting accent.
        CHECK(theme.accent_hover != theme.control_on);
        // Idempotence: deriving the same accent twice answers the same
        // faces.
        const ChromeTheme again = theme;
        apply_accent(theme, 0x54A054FFu);
        CHECK(theme.control_on == again.control_on);
        CHECK(theme.accent_hover == again.accent_hover);
        CHECK(theme.text_on_accent == again.text_on_accent);
    }

    // A pale accent on the dark stock walks control_on darker until it
    // clears its pairs; the on-accent label follows (white clears on a
    // walked-dark accent, black on a bright one - either way it is
    // DERIVED, never hand-picked).
    ChromeTheme pale = kDefaultChrome;
    apply_accent(pale, 0xE8E8E8FFu);
    CHECK(contrast_ratio(pale.text_on_accent, pale.control_on) >= 3.0);
}

TEST_CASE("the muted rung answers the typography brief") {
    // The brief's own gray, pinned: #858585 on the dark stock - the
    // placeholder/inactive-tab gray that does not compete with
    // content.
    CHECK(kDefaultChrome.text_muted == ws::ui::r2d::rgba(133, 133, 133));

    // The not-competing rule, made measurable: the muted rung sits a
    // full contrast STEP under the body text on its own background,
    // on both stocks (a dark stock dims toward the panel, a light one
    // lightens toward it - the polarity flips, the gap does not).
    for (const ChromeTheme* chrome : {&kDefaultChrome, &kLightChrome}) {
        CHECK(contrast_ratio(chrome->text_muted, chrome->panel_bg) <
              contrast_ratio(chrome->text, chrome->panel_bg));
        CHECK(contrast_ratio(chrome->text_muted, chrome->control_bg) <
              contrast_ratio(chrome->text, chrome->control_bg));
    }

    // And it is its OWN face, not a derived one: dim(text_muted, x)
    // can never equal text again, but more to the point the stocks
    // pick different grays for their polarity - the dark stock's
    // #858585 would fail the light faces, so the light stock picks
    // its own (the same rule text_secondary follows).
    CHECK(kLightChrome.text_muted != kDefaultChrome.text_muted);
}

TEST_CASE("every new token round-trips through the theme json") {
    ws::ui::gui::ThemeData styled = default_dark();
    styled.chrome.text_on_accent = 0x101820FFu;
    styled.chrome.accent_hover = 0x70B0E0FFu;
    styled.chrome.text_secondary = 0x99A0AAFFu;
    styled.chrome.text_muted = 0x858585FFu;
    styled.chrome.border_strong = 0xFFFFFF66u;
    styled.chrome.shadow = 0x00000077u;
    styled.chrome.radius_control = 0.0f;
    styled.chrome.radius_panel = 8.0f;
    styled.chrome.radius_popup = 10.0f;
    styled.chrome.radius_pill = 999.0f;
    styled.chrome.border_width = 2.0f;
    styled.chrome.focus_ring_width = 1.5f;
    styled.chrome.shadow_blur = 12.0f;
    styled.chrome.shadow_offset_y = 4.0f;
    styled.chrome.anim_duration = 0.2f;
    styled.chrome.anim_enabled = false;
    styled.chrome.canvas_bg = 0x1C1E24FFu;
    styled.chrome.grid_minor = 0xFFFFFF2Cu;
    styled.chrome.grid_major = 0xFFFFFF60u;
    styled.chrome.grid_axis = 0x5490D038u;

    const std::string text = write_theme(styled);

    // The canonical shape names them all.
    CHECK(text.find("\"text_on_accent\": \"#101820FF\"") != std::string::npos);
    CHECK(text.find("\"accent_hover\": \"#70B0E0FF\"") != std::string::npos);
    CHECK(text.find("\"text_secondary\": \"#99A0AAFF\"") != std::string::npos);
    CHECK(text.find("\"text_muted\": \"#858585FF\"") != std::string::npos);
    CHECK(text.find("\"border_strong\": \"#FFFFFF66\"") != std::string::npos);
    CHECK(text.find("\"shadow\": \"#00000077\"") != std::string::npos);
    CHECK(text.find("\"radius_control\": 0.0") != std::string::npos);
    CHECK(text.find("\"radius_pill\": 999.0") != std::string::npos);
    CHECK(text.find("\"border_width\": 2.0") != std::string::npos);
    CHECK(text.find("\"focus_ring_width\": 1.5") != std::string::npos);
    CHECK(text.find("\"shadow_blur\": 12.0") != std::string::npos);
    CHECK(text.find("\"shadow_offset_y\": 4.0") != std::string::npos);
    CHECK(text.find("\"anim_duration\": 0.2") != std::string::npos);
    CHECK(text.find("\"anim_enabled\": false") != std::string::npos);
    CHECK(text.find("\"canvas_bg\": \"#1C1E24FF\"") != std::string::npos);
    CHECK(text.find("\"grid_minor\": \"#FFFFFF2C\"") != std::string::npos);
    CHECK(text.find("\"grid_major\": \"#FFFFFF60\"") != std::string::npos);
    CHECK(text.find("\"grid_axis\": \"#5490D038\"") != std::string::npos);

    // Reading over the OTHER stock recovers every token - and the
    // write(read(.)) pair is byte-stable.
    const auto over_light = read_theme(text, default_light());
    REQUIRE(over_light);
    CHECK(over_light->chrome.text_on_accent == styled.chrome.text_on_accent);
    CHECK(over_light->chrome.accent_hover == styled.chrome.accent_hover);
    CHECK(over_light->chrome.text_secondary == styled.chrome.text_secondary);
    CHECK(over_light->chrome.text_muted == styled.chrome.text_muted);
    CHECK(over_light->chrome.border_strong == styled.chrome.border_strong);
    CHECK(over_light->chrome.shadow == styled.chrome.shadow);
    CHECK(over_light->chrome.radius_control == styled.chrome.radius_control);
    CHECK(over_light->chrome.radius_panel == styled.chrome.radius_panel);
    CHECK(over_light->chrome.radius_popup == styled.chrome.radius_popup);
    CHECK(over_light->chrome.radius_pill == styled.chrome.radius_pill);
    CHECK(over_light->chrome.border_width == styled.chrome.border_width);
    CHECK(over_light->chrome.focus_ring_width ==
          styled.chrome.focus_ring_width);
    CHECK(over_light->chrome.shadow_blur == styled.chrome.shadow_blur);
    CHECK(over_light->chrome.shadow_offset_y ==
          styled.chrome.shadow_offset_y);
    CHECK(over_light->chrome.anim_duration == styled.chrome.anim_duration);
    CHECK(over_light->chrome.anim_enabled == styled.chrome.anim_enabled);
    CHECK(over_light->chrome.canvas_bg == styled.chrome.canvas_bg);
    CHECK(over_light->chrome.grid_minor == styled.chrome.grid_minor);
    CHECK(over_light->chrome.grid_major == styled.chrome.grid_major);
    CHECK(over_light->chrome.grid_axis == styled.chrome.grid_axis);
    CHECK(write_theme(*over_light) == text);
}

TEST_CASE("an old theme file loads with every new token at its default") {
    // The migration is "absent = default": a pre-P2 file (no new
    // members) reads clean, and only its old members fold.
    const auto loaded = read_theme(
        R"json({
            "version": 1,
            "chrome": {"control_height": 30, "corner_radius": 6.5}
        })json",
        default_dark());
    REQUIRE(loaded);
    CHECK(loaded->chrome.control_height == 30.0f);
    CHECK(loaded->chrome.corner_radius == 6.5f);
    // The new family, all at the stock defaults.
    const ChromeTheme& stock = kDefaultChrome;
    CHECK(loaded->chrome.radius_control == stock.radius_control);
    CHECK(loaded->chrome.control_radius() == 6.5f);  // inherits the folded one
    CHECK(loaded->chrome.text_secondary == stock.text_secondary);
    CHECK(loaded->chrome.text_muted == stock.text_muted);
    CHECK(loaded->chrome.text_on_accent == stock.text_on_accent);
    CHECK(loaded->chrome.border_width == stock.border_width);
    CHECK(loaded->chrome.shadow_blur == stock.shadow_blur);
    CHECK(loaded->chrome.anim_duration == stock.anim_duration);
    CHECK(loaded->chrome.anim_enabled == stock.anim_enabled);
    CHECK(loaded->chrome.canvas_bg == stock.canvas_bg);
    CHECK(loaded->chrome.grid_minor == stock.grid_minor);
    CHECK(loaded->chrome.grid_major == stock.grid_major);
    CHECK(loaded->chrome.grid_axis == stock.grid_axis);

    // A wrong-typed flag is refused like any wrong member: the fold is
    // all-or-nothing.
    const auto refused = read_theme(
        R"json({
            "version": 1,
            "chrome": {"anim_enabled": 1}
        })json",
        default_dark());
    CHECK(!refused);
}

TEST_CASE("the canvas grid tokens answer the brief on both stocks") {
    // The stage-16 faces: the canvas floor is its own step away from
    // panel_bg (never black), the grid alphas sit in the brief's bands
    // (minor 6-8%, major 16-20% - the contrast answer for decorative
    // marks on the canvas background), and the axis face is an
    // accent-family color at a modest alpha. Decorative like border:
    // no WCAG pair is checked.
    for (const ChromeTheme* chrome : {&kDefaultChrome, &kLightChrome}) {
        CHECK((chrome->canvas_bg & 0xFFu) == 0xFFu);  // opaque floor
        CHECK(chrome->canvas_bg != chrome->panel_bg);  // its own step
        const int minor = chrome->grid_minor & 0xFFu;
        const int major = chrome->grid_major & 0xFFu;
        CHECK(minor >= 40);   // ~16%
        CHECK(minor <= 44);   // ~17%
        CHECK(major >= 92);   // ~36%
        CHECK(major <= 96);   // ~38%
        CHECK(major > minor * 2);  // structure carries, texture fades
        const int axis = chrome->grid_axis & 0xFFu;
        CHECK(axis < major);   // axis is a subtle reading, major carries structure
        CHECK(axis >= 56);     // ~22% - visible but not loud
        CHECK(axis <= 60);     // ~24%
    }
}

TEST_CASE("the stock documents name the bundled faces") {
    // P1's Inter for the UI rungs, the typography pass's JetBrains Mono
    // for the data rung: the families record what the stock look runs;
    // the app still resolves them (load_default_font /
    // load_default_mono_font), the library opens nothing.
    CHECK(default_dark().regular.family == "Inter");
    CHECK(default_dark().bold.family == "Inter SemiBold");
    CHECK(default_dark().mono.family == "JetBrains Mono");
    CHECK(default_light().regular.family == "Inter");
    CHECK(default_light().mono.family == "JetBrains Mono");
}

TEST_CASE("is_dark reads the chrome's brightness off the panel face") {
    // The P1b pick the bake pair follows: the stock dark chrome's
    // panel is a dark face, the light chrome's a pale one - and a
    // custom theme that flips the panel flips the answer.
    CHECK(ws::ui::gui::is_dark(kDefaultChrome));
    CHECK(!ws::ui::gui::is_dark(kLightChrome));
    CHECK(ws::ui::gui::is_dark(kDefaultChrome.panel_bg));
    CHECK(!ws::ui::gui::is_dark(kLightChrome.panel_bg));

    // The packed-face order: 0xRRGGBBAA, so a white face with any
    // alpha reads light and a black face reads dark.
    CHECK(!ws::ui::gui::is_dark(ws::ui::r2d::rgba(255, 255, 255)));
    CHECK(ws::ui::gui::is_dark(ws::ui::r2d::rgba(0, 0, 0)));
    // The midpoint guard: exactly luma 128 is light (the pick is
    // "dark means luma below half").
    CHECK(ws::ui::gui::is_dark(ws::ui::r2d::rgba(127, 127, 127)));
    CHECK(!ws::ui::gui::is_dark(ws::ui::r2d::rgba(128, 128, 128)));
}
