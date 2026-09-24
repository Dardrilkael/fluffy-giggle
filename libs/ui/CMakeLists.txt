ws_add_library(ui DEPENDS ws::core)

# Every third-party target is PRIVATE on purpose: apps build against ws::
# names only, and no imgui/vulkan/glfw header or define ever escapes this
# library (apps draw through <ws/ui/widgets.hpp>). Static archives still
# forward these targets to the final link, so symbols resolve.
target_link_libraries(ws_ui
  PRIVATE imgui implot glfw volk vma stb)

ws_add_test(ui SOURCES tests/test_ui.cpp)
# tests exercise the internal pure-CPU batch core directly (src/ never
# reaches apps).
target_include_directories(test_ui PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
