#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <ws/ui/layout.hpp>

TEST_CASE("ui layout: menu pair centers and has deterministic hit edges") {
  using namespace ws::ui::layout::menu;
  const auto buttons = layout(320.0f, 240.0f);
  CHECK(buttons[0].x == doctest::Approx(40.0f));
  CHECK(buttons[1].y ==
        doctest::Approx(buttons[0].y + kButtonH + kButtonGap));
  CHECK(click_action(320.0f, 240.0f,
                     buttons[0].x + buttons[0].w * 0.5f,
                     buttons[0].y + buttons[0].h * 0.5f) == 0);
  // Disambiguate hit call: use layout::hit (the base Rect version) explicitly
  const float right_edge_x = buttons[0].x + buttons[0].w;
  CHECK_FALSE(ws::ui::layout::hit(buttons[0], right_edge_x, buttons[0].y));
  CHECK(click_action(320.0f, 240.0f, 0.0f, 0.0f) == -1);
}

TEST_CASE("ui layout: hotbar slots are centered, indexed and half-open") {
  using namespace ws::ui::layout::hotbar;
  const auto slots = layout(1280.0f, 720.0f);
  CHECK(slots.size() == 9);
  CHECK(slots.front().bounds.x ==
        doctest::Approx((1280.0f - kSlotStep * kSlots) * 0.5f));
  for (int i = 0; i < kSlots; ++i) {
    CHECK(slots[static_cast<std::size_t>(i)].index == i);
    CHECK(selected(1280.0f, 720.0f,
                   slots[static_cast<std::size_t>(i)].bounds.x + 1.0f,
                   slots[static_cast<std::size_t>(i)].bounds.y + 1.0f) == i);
  }
  CHECK(selected(1280.0f, 720.0f, 0.0f, 0.0f) == -1);
}

TEST_CASE("ui layout: generic settings rows place, hit and map values") {
  using namespace ws::ui::layout::settings;
  // layout<Count>(frame_w, frame_h, panel_h, slider_w, slider_h, top, step)
  const auto sliders = layout<3>(
      1280.0f, 720.0f, 520.0f, 300.0f, 62.0f, 12.0f, 112.0f);
  CHECK(sliders[0].index == 0);
  CHECK(sliders[2].index == 2);
  CHECK(value_from_x(sliders[1].bounds, 0.0f, 1.0f,
                     sliders[1].bounds.x) == doctest::Approx(0.0f));
  const float right_x = sliders[1].bounds.x + sliders[1].bounds.w;
  CHECK(value_from_x(sliders[1].bounds, 0.0f, 1.0f, right_x) ==
        doctest::Approx(1.0f));
  const float left_x = sliders[2].bounds.x - 10.0f;
  CHECK(value_from_x(sliders[2].bounds, 32.0f, 256.0f, left_x) ==
        doctest::Approx(32.0f));
  const float hit_x = sliders[2].bounds.x + 5.0f;
  const float hit_y = sliders[2].bounds.y + 2.0f;
  CHECK(selected(sliders, hit_x, hit_y) == 2);
  CHECK(selected(sliders, 0.0f, 0.0f) == -1);
}
