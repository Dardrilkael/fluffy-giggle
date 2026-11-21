# apps/schematic_test - minimal schematic + canvas wiring test app.
# Uses ws::schematic (the headless routing core) + ws::ui::canvas directly.
# Simple rectangular "components" with pins that can be wired together.

ws_add_app(schematic_test DEPENDS ws::ui ws::schematic ws::jobs ws::platform ws::app ws::settings)