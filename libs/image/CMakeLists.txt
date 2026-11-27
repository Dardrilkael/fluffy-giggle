ws_add_library(image DEPENDS ws::core)

# stb is private: only this library's decoder and encoder implementation
# sources need the third-party headers. Consumers see ws-owned image types.
target_link_libraries(ws_image PRIVATE stb::stb)

ws_add_test(image SOURCES tests/test_image.cpp)
