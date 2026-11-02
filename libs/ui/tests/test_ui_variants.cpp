// The P6 acceptance (the ui-skin plan): the built-in icon set (the
// raster, the stock mapping and its painters override), the button
// variants (faces resolved through the theme's tokens, the default
// byte-matching the pre-variant look) and the pixel snap helpers. The
// stub's counters and last-color captures are the proof; the icon
// raster runs pure CPU (font::bake_icon_glyph).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/icons.hpp>
#include <ws/ui/gui/layout_cursor.hpp>
#include <ws/ui/gui/painters.hpp>
#include <ws/ui/gui/theme.hpp>
#include <ws/ui/gui/theme_data.hpp>
#include <ws/ui/gui/theme_json.hpp>
#include <ws/ui/gui/theme_color.hpp>
#include <ws/ui/gui/widgets.hpp>

// The icon raster and the snap helpers are ui-internal (src/ is on this
// target's include path, the test_ui.cpp precedent).
#include "text/font.h"
#include "gui/widgets_detail.hpp"

#include "ui_gui_stub.hpp"

#include <cstdint>
#include <string>

namespace {

using ws::ui::gui::ChromeTheme;
using ws::ui::gui::GuiContext;
using ws::ui::gui::kDefaultChrome;
using ws::ui::gui::LayoutCursor;
using ws::ui::gui::Rect;

ws::input::InputFrame at(float x, float y, bool down = false,
                         bool pressed = false, bool released = false) {
    ws::input::InputFrame frame;
    frame.mouse_x = x;
    frame.mouse_y = y;
    frame.mouse_present = true;
    frame.mouse_down[0] = down;
    frame.mouse_pressed[0] = pressed;
    frame.mouse_released[0] = released;
    return frame;
}

}  // namespace

// --- the icon raster -----------------------------------------------------------

TEST_CASE("every built-in icon bakes with coverage and stable metrics") {
    for (std::uint32_t icon_idx = 0; icon_idx < ws::ui::r2d::font::kIconCount; ++icon_idx) {
        const std::uint32_t cp = ws::ui::r2d::font::kIconFirst + icon_idx;
        ws::ui::r2d::font::BakedGlyph glyph;
        REQUIRE(ws::ui::r2d::font::bake_icon_glyph(cp, 16.0f, glyph));
        CHECK(glyph.codepoint == cp);
        CHECK(glyph.w > 0);
        CHECK(glyph.h > 0);
        CHECK(glyph.advance == 0.0f);  // never flows through text
        // Real coverage, read off the ALPHA bytes (rgb stay 255 - the
        // baked-mask convention): some pixel draws, some does not.
        std::uint32_t inked = 0;
        for (std::size_t rgba_idx = 3; rgba_idx < glyph.rgba.size(); rgba_idx += 4) {
            if (glyph.rgba[rgba_idx] != 0u) {
                ++inked;
            }
        }
        // A stroke glyph at 16 px covers ~5-25% of its box (a chevron's
        // two thin strokes, a minus one); the floor catches the empty
        // bake, the ceiling catches the blob (the mixed-units bug this
        // pin was born from drew every pixel).
        const std::size_t alpha_count =
            static_cast<std::size_t>(glyph.w) * glyph.h;
        CHECK(inked > alpha_count / 20);
        CHECK(inked < alpha_count / 2);
    }
}

TEST_CASE("bake_icon_glyph refuses everything outside the set") {
    ws::ui::r2d::font::BakedGlyph glyph;
    CHECK_FALSE(ws::ui::r2d::font::bake_icon_glyph(0xDFFFu, 16.0f, glyph));
    CHECK_FALSE(ws::ui::r2d::font::bake_icon_glyph(
        ws::ui::r2d::font::kIconFirst + ws::ui::r2d::font::kIconCount, 16.0f,
        glyph));
    CHECK_FALSE(ws::ui::r2d::font::bake_icon_glyph('A', 16.0f, glyph));
}

TEST_CASE("icon_codepoint spells the private-use range") {
    using ws::ui::gui::Icon;
    using ws::ui::gui::icon_codepoint;
    CHECK(icon_codepoint(Icon::chevron_down) == 0xE000u);
    CHECK(icon_codepoint(Icon::minus) == 0xE000u + 6u);
}

// --- the stock mapping and its override ----------------------------------------

TEST_CASE("the stock mapping answers the theme font's own glyph") {
    namespace icons_font = ws::ui::r2d::font;
    using ws::ui::gui::Icon;
    // A hand-built font: no inline glyphs, the icon table seeded with a
    // recognizable sprite - publish_baked_font's lazy-table insertion,
    // spelled directly (headless: the renderer's GPU gate stays shut).
    ws::ui::r2d::Font baked(16.0f, 20.0f, nullptr, 0);
    const ws::ui::r2d::Sprite seeded{7u, 3u};
    baked.extras_state()->glyphs.emplace(
        icons_font::kIconFirst,
        ws::ui::r2d::Font::Glyph{seeded, 0.0f, 0, 0, 16, 16});

    ChromeTheme theme = kDefaultChrome;
    theme.font = &baked;
    const ws::ui::r2d::Sprite sprite =
        ws::ui::gui::stock_icon_sprite(theme, Icon::chevron_down);
    CHECK(sprite.page == 7u);
    CHECK(sprite.index == 3u);
    // An icon the font does not carry: the sentinel, never the tofu.
    const ws::ui::r2d::Sprite missing =
        ws::ui::gui::stock_icon_sprite(theme, Icon::close);
    CHECK(missing.page == 0u);
    // A fontless theme: the sentinel again.
    ChromeTheme fontless = kDefaultChrome;
    CHECK(ws::ui::gui::stock_icon_sprite(fontless, Icon::check).page == 0u);
}

namespace {
ws::ui::r2d::Sprite skin_icon(const ChromeTheme&, ws::ui::gui::Icon) {
    return ws::ui::r2d::Sprite{42u, 9u};
}
}  // namespace

TEST_CASE("a skin's icon_of row answers through the fold") {
    using ws::ui::gui::Icon;
    ChromeTheme theme = kDefaultChrome;
    // Stock: the theme font's answer (sentinel here - no font).
    CHECK(ws::ui::gui::icon_sprite_of(theme, Icon::search).page == 0u);
    // The override: one row, every consumer follows.
    ws::ui::gui::WidgetPainters skin = ws::ui::gui::kStockPainters;
    skin.icon_of = &skin_icon;
    theme.painters = &skin;
    const ws::ui::r2d::Sprite sprite =
        ws::ui::gui::icon_sprite_of(theme, Icon::search);
    CHECK(sprite.page == 42u);
    CHECK(sprite.index == 9u);
}

// --- the button variants ---------------------------------------------------------

TEST_CASE("the button variants resolve their faces through the tokens") {
    using namespace ws::ui::gui;
    using ws::ui::gui_test::stub;
    GuiContext gui;
    LayoutCursor layout;
    ChromeTheme theme = kDefaultChrome;
    theme.anim_enabled = false;  // deterministic states, exact ends
    // One row per frame, always at (0,0,60,28): the panel scope resets
    // the cursor, so a pointer at (30,14) is ON the row, and one at
    // (-100,-100) is on nothing. Each style runs the SAME frame TWICE -
    // hover is the SETTLED claim (hot_prev carries it), so the second
    // frame is the one whose state the stub records. The stub counts
    // the panel chrome's outline pair in every frame - the face
    // assertions ride the LAST color, the "no face" one counts past
    // the pair.
    const auto draw_style = [&](ButtonStyle style, bool down = false,
                                bool pressed = false, float px = 30.0f) {
        for (int frame = 0; frame < 2; ++frame) {
            gui.begin_frame(at(px, 14.0f, down, pressed));
            (void)begin_panel(gui, layout, theme, "board",
                              {0.0f, 0.0f, 300.0f, 200.0f});
            (void)button(gui, layout, theme, "b", 60.0f, {}, false, style);
            end_panel(gui, layout);
            gui.end_frame();
            if (frame == 0) {
                ws::ui::gui_test::stub.reset();  // record the settled frame
            }
        }
    };

    // secondary (the default): the plain ladder at rest - byte-identical
    // to the pre-variant look.
    stub.reset();
    draw_style(ButtonStyle::secondary, false, false, -100.0f);
    CHECK(stub.last_rounded_color == theme.control_bg);

    // primary: the accent face, the on-accent label.
    stub.reset();
    draw_style(ButtonStyle::primary, false, false, -100.0f);
    CHECK(stub.last_rounded_color == theme.control_on);
    CHECK(stub.last_quad_color == theme.text_on_accent);  // the fontless bar

    // danger: the danger family.
    stub.reset();
    draw_style(ButtonStyle::danger, false, false, -100.0f);
    CHECK(stub.last_rounded_color == theme.danger);
    CHECK(stub.last_quad_color == theme.text_on_accent);

    // ghost: no face at rest (idle pointer OFF the row; the stub's
    // rounded count is the panel chrome's outline pair alone).
    stub.reset();
    draw_style(ButtonStyle::ghost, false, false, -100.0f);
    CHECK(stub.outlines == 1);
    CHECK(stub.rounded == 2);

    // ghost hovered: the hover face arrives, faded IN at the exact end.
    stub.reset();
    draw_style(ButtonStyle::ghost);
    CHECK(stub.last_rounded_color == theme.control_hovered);

    // primary pressed: the pressed rung, instant.
    stub.reset();
    draw_style(ButtonStyle::primary, true, true);
    CHECK(stub.last_rounded_color == theme.control_pressed);

    // danger pressed: the danger's own pressed rung.
    stub.reset();
    draw_style(ButtonStyle::danger, true, true);
    CHECK(stub.last_rounded_color == theme.danger_pressed);
}

TEST_CASE("the variant default is secondary - today's look, byte for byte") {
    using namespace ws::ui::gui;
    using ws::ui::gui_test::stub;
    GuiContext gui;
    LayoutCursor layout;
    ChromeTheme theme = kDefaultChrome;
    theme.anim_enabled = false;
    // The plain call and the explicit secondary answer the same bytes.
    stub.reset();
    gui.begin_frame(at(-100.0f, -100.0f));
    (void)button(gui, layout, theme, "plain", 60.0f);
    gui.end_frame();
    const std::uint32_t quads = stub.quads;
    const std::uint32_t rounded = stub.rounded;
    const std::uint32_t color = stub.last_rounded_color;
    stub.reset();
    gui.begin_frame(at(-100.0f, -100.0f));
    (void)button(gui, layout, theme, "plain", 60.0f, {}, false,
                 ButtonStyle::secondary);
    gui.end_frame();
    CHECK(stub.quads == quads);
    CHECK(stub.rounded == rounded);
    CHECK(stub.last_rounded_color == color);
}

TEST_CASE("the danger tokens round-trip through the theme json") {
    using namespace ws::ui::gui;
    ThemeData styled = default_dark();
    styled.chrome.danger = 0xAA3322FFu;
    styled.chrome.danger_hover = 0xCC5544FFu;
    styled.chrome.danger_pressed = 0x881A10FFu;

    const std::string text = write_theme(styled);
    CHECK(text.find("\"danger\": \"#AA3322FF\"") != std::string::npos);
    CHECK(text.find("\"danger_hover\": \"#CC5544FF\"") != std::string::npos);
    CHECK(text.find("\"danger_pressed\": \"#881A10FF\"") != std::string::npos);

    const auto over_light = read_theme(text, default_light());
    REQUIRE(over_light);
    CHECK(over_light->chrome.danger == styled.chrome.danger);
    CHECK(over_light->chrome.danger_hover == styled.chrome.danger_hover);
    CHECK(over_light->chrome.danger_pressed ==
          styled.chrome.danger_pressed);
    CHECK(write_theme(*over_light) == text);  // byte-stable
}

TEST_CASE("both stock chromes pass the grown contrast table") {
    using namespace ws::ui::gui;
    CHECK(contrast_failures(kDefaultChrome).empty());
    CHECK(contrast_failures(kLightChrome).empty());
}

// --- the pixel snap --------------------------------------------------------------

TEST_CASE("the snap helpers round half-up and keep the edges") {
    using namespace ws::ui::gui;
    CHECK(snap_v(36.4f) == doctest::Approx(36.0f));
    CHECK(snap_v(36.5f) == doctest::Approx(37.0f));
    CHECK(snap_v(-0.5f) == doctest::Approx(0.0f));
    CHECK(snap_v(10.0f) == doctest::Approx(10.0f));  // integral is identity

    const Rect snapped = snap_rect({10.4f, 20.6f, 100.3f, 50.2f});
    CHECK(snapped.x == doctest::Approx(10.0f));
    CHECK(snapped.y == doctest::Approx(21.0f));
    // The far edges round in place; the size re-derives from them.
    CHECK(snapped.x + snapped.width == doctest::Approx(111.0f));
    CHECK(snapped.y + snapped.height == doctest::Approx(71.0f));

    // An integral rect is its own snap - the whole stock look holds.
    const Rect same = snap_rect({0.0f, 0.0f, 200.0f, 100.0f});
    CHECK(same.x == 0.0f);
    CHECK(same.y == 0.0f);
    CHECK(same.width == doctest::Approx(200.0f));
    CHECK(same.height == doctest::Approx(100.0f));
}
