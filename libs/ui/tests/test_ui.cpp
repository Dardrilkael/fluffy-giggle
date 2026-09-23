#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <type_traits>

#include <ws/ui/app.hpp>
#include <ws/ui/widgets.hpp>  // must stay includable without any third-party header

// The shell must be constructible and destructible without a window, GPU or
// Vulkan loader - all of that only happens inside run(). The apps/playground
// app is the real end-to-end smoke test for those paths.

TEST_CASE("AppConfig defaults are sane") {
    ws::ui::AppConfig config;
    CHECK(config.title == "ws app");
    CHECK(config.width == 1280);
    CHECK(config.height == 720);
    CHECK(config.vsync == true);
}

TEST_CASE("App constructs and destructs without run()") {
    ws::ui::AppConfig config;
    config.title = "headless";
    {
        ws::ui::App app{config};
    }  // destroyed here - must not require run() to have been called
    CHECK(true);
}

TEST_CASE("App is neither copyable nor movable") {
    static_assert(!std::is_copy_constructible_v<ws::ui::App>);
    static_assert(!std::is_move_constructible_v<ws::ui::App>);
    CHECK(true);
}
