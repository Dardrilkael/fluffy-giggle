ws_add_library(net DEPENDS ws::core)

# libuv is the async I/O backend - private, like every third-party here:
# uv.h never appears under include/ (only src/backend/uv includes it), and
# apps link only ws::net, never uv.
target_link_libraries(ws_net PRIVATE uv_a)

# Hidden by default on GCC/Clang; WS_NET_API (visibility.hpp) is the only
# exported surface. MSVC needs no flag - its DLLs export nothing without
# __declspec anyway.
if(NOT MSVC)
  target_compile_options(ws_net PRIVATE -fvisibility=hidden)
endif()

ws_add_test(net SOURCES tests/test_api.cpp tests/test_frame.cpp)
# tests drive the internal framing core directly (the ui pattern: src/
# never reaches apps, but tests reach it through this PRIVATE path).
if(TARGET test_net)
  target_include_directories(test_net PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()

# The frame-parser fuzz driver: desktop-only and deliberately NOT in
# ctest - it runs long by design. Smoke it by hand:
#   build/debug/bin/fuzz_net [seed] [iterations]
if(NOT ANDROID)
  add_executable(fuzz_net tests/fuzz_frame_main.cpp)
  target_include_directories(fuzz_net PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  target_link_libraries(fuzz_net PRIVATE ws::net)
  ws_set_warnings(fuzz_net)
endif()
