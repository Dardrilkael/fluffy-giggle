// glyph_probe - text contrast acceptance view
//
// Exercises the shader-based text contrast curve (batch.frag's
// text_coverage()) across all theme colors. No assets: system font only.
// Exits after ~6 seconds (360 frames at 60fps) for CI-friendly verification.

#include <ws/ui/app.hpp>
#include <ws/ui/renderer2d.hpp>
#include <ws/core/log.hpp>

#include <string>
#include <vector>

using namespace ws::ui;

struct GlyphProbeApp : App {
    GlyphProbeApp() : App(AppConfig{.title = "Glyph Probe - Text Contrast Acceptance",
                                     .width = 1280,
                                     .height = 720}) {}

    void on_setup() override {
        // Enable shader-based text contrast (default, but explicit)
        r2d::FontBakeOptions opts;
        opts.shader_contrast = true;

        // Load bundled default font (Inter Regular) - no system font walk needed
        font_ = r2d::load_default_font(18.0f, opts);
        if (font_.empty()) {
            WS_LOG_ERROR("Failed to load default font");
            return;
        }

        // A/B variants for the sharpness investigation (right-hand column).
        // 16 px = the circsim chrome body size at ui_scale 1.0.
        r2d::FontBakeOptions base;  // shader contrast, oversample 3, no fit
        r2d::FontBakeOptions fit = base;
        fit.fit_vertical = true;
        r2d::FontBakeOptions fit_os1 = fit;
        fit_os1.oversample_x = 1;
        r2d::FontBakeOptions os1 = base;
        os1.oversample_x = 1;
        variants_.push_back({"A  old: fit off, os3", r2d::load_default_font(16.0f, base)});
        variants_.push_back({"B  os1 only", r2d::load_default_font(16.0f, os1)});
        variants_.push_back({"C  vertical fit, os3", r2d::load_default_font(16.0f, fit)});
        variants_.push_back({"D  vertical fit, os1", r2d::load_default_font(16.0f, fit_os1)});
        variants_.push_back({"E  SDF (canvas labels)", r2d::load_default_font(16.0f, base.with_sdf())});

        // Test strings - various text to show contrast behavior
        test_strings_ = {
            "The quick brown fox jumps over the lazy dog",
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
            "abcdefghijklmnopqrstuvwxyz",
            "0123456789 !@#$%^&*()",
            "Shader-based contrast: gamma/boost by luminance",
            "Light text on dark: should lift midtones (gamma 1.6, boost 1.1)",
            "Dark text on light: should be subtle (gamma 1.05, boost 1.0)",
            "Mixed: The QUICK brown FOX jumps",
            "Unicode: αβγδε ζηθικ λμνξο πρστυ φχψω",
            "CJK: 字形測試 測試字形",
        };
    }

    void on_event(ws::input::Event&) override {}

    void on_update() override {
        r2d::begin();

        // Draw background - split screen for light/dark themes
        const auto fb_size = r2d::frame_size();
        const float half_h = fb_size.height * 0.5f;

        // Top half: dark background (simulates dark theme)
        r2d::draw_quad(0, 0, fb_size.width, half_h, r2d::rgba(31, 31, 36, 255));
        // Bottom half: light background (simulates light theme)
        r2d::draw_quad(0, half_h, fb_size.width, half_h, r2d::rgba(242, 242, 248, 255));

        // Draw test strings on both backgrounds
        const float line_h = 22.0f;
        const float start_y = 40.0f;
        const float padding = 20.0f;

        for (size_t i = 0; i < test_strings_.size(); ++i) {
            const float y = start_y + i * line_h;

            // Light text on dark background (top half)
            r2d::draw_text(padding, y, test_strings_[i].c_str(), font_,
                r2d::rgba(242, 242, 248, 255));

            // Dark text on light background (bottom half)
            r2d::draw_text(padding, half_h + y, test_strings_[i].c_str(), font_,
                r2d::rgba(26, 26, 31, 255));
        }

        // Draw a center divider
        r2d::draw_quad(0, half_h - 1, fb_size.width, 2, r2d::rgba(128, 128, 128, 255));

        // A/B column: the same sentence, five bakes, on both backgrounds.
        // Compare the TOPS of x/e/o/a and the cap tops (T, H) - with the
        // vertical fit they should be one crisp row, not a gray one.
        const float col_x = 700.0f;
        const char* sample = "Hexagon of the quick onyx axe - 0123 ready";
        for (size_t i = 0; i < variants_.size(); ++i) {
            const float y = start_y + static_cast<float>(i) * 44.0f;
            const auto& v = variants_[i];
            r2d::draw_text(col_x, y, v.label, font_, r2d::rgba(179, 179, 204, 255));
            r2d::draw_text(col_x, y + 20.0f, sample, v.font, r2d::rgba(242, 242, 248, 255));
            r2d::draw_text(col_x, half_h + y, v.label, font_, r2d::rgba(51, 51, 77, 255));
            r2d::draw_text(col_x, half_h + y + 20.0f, sample, v.font, r2d::rgba(26, 26, 31, 255));
        }

        // Draw labels for the two halves
        r2d::draw_text(padding, 10.0f, "DARK THEME (light text)", font_,
            r2d::rgba(204, 204, 230, 255));
        r2d::draw_text(padding, half_h + 10.0f, "LIGHT THEME (dark text)", font_,
            r2d::rgba(51, 51, 77, 255));

        // Draw info text at bottom
        const float info_y = fb_size.height - 60.0f;
        r2d::draw_text(padding, info_y,
            "Shader contrast: ON (inv_gamma = mix(1/1.6, 1/1.05, 1-L), boost = mix(1.1, 1.0, 1-L))",
            font_, r2d::rgba(179, 179, 204, 255));
        r2d::draw_text(padding, info_y + 20.0f,
            "Coverage sampled from alpha channel (.a) - NOT red (.r)",
            font_, r2d::rgba(179, 230, 179, 255));

        r2d::end();

        // Auto-exit after ~6 seconds (360 frames at 60fps)
        frame_count_++;
        if (frame_count_ >= 360) {
            request_stop();
        }
    }

private:
    struct Variant {
        const char* label;
        r2d::Font font;
    };
    r2d::Font font_;
    std::vector<Variant> variants_;
    std::vector<std::string> test_strings_;
    int frame_count_ = 0;
};

int main(int /*argc*/, char** /*argv*/) {
    // Logging is auto-initialized by the App shell
    GlyphProbeApp app;
    return app.run();
}