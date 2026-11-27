ws_add_library(image DEPENDS ws::core)

# stb is private: only this library's implementation and decoder source need
# the third-party header. Public consumers see ws-owned image types only.
target_link_libraries(ws_image PRIVATE stb::stb)

ws_add_test(image SOURCES tests/test_image.cpp)
