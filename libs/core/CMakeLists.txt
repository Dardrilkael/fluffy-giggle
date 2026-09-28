ws_add_library(core)

# tl::expected is the sanctioned exception to the PRIVATE link rule: Result is
# core's public API (an alias of tl::expected behind a ws-owned name), so every
# dependent needs the header transitively.
target_link_libraries(ws_core PUBLIC tl::expected)

# logcat is Android's console: ws::core mirrors its lines there (src/log.cpp)
# through the NDK's liblog.
if(ANDROID)
  target_link_libraries(ws_core PRIVATE log)
endif()

ws_add_test(core SOURCES tests/test_core.cpp)
