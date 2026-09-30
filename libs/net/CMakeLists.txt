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

ws_add_test(net SOURCES tests/test_api.cpp)
