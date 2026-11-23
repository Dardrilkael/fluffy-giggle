# Chatter is the GUI sibling of netchat. It shares the LAN discovery
# and HELLO/SAY protocol, while its presentation is built from the
# stock ws::ui theme, panels, scroll region, text field and button
# widgets. No custom renderer skin or library fork is required.
ws_add_app(chatter DEPENDS ws::ui ws::net)

# MSVC: keep the platform CRT's getenv/time compatibility warnings quiet.
target_compile_definitions(chatter PRIVATE _CRT_SECURE_NO_WARNINGS)

# The real protocol core also has a headless integration test. Keep it
# desktop-only: it exercises LAN discovery + relay TCP on loopback and
# needs no renderer/window.
if(WS_BUILD_TESTS AND NOT ANDROID)
  add_executable(chatter_relay_smoke ../../scripts/relay_smoke.cpp)
  target_include_directories(chatter_relay_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(chatter_relay_smoke PRIVATE ws::net)
  add_test(NAME chatter_relay_smoke COMMAND chatter_relay_smoke)
endif()
