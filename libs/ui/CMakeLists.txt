ws_add_library(ui DEPENDS ws::core)

# ImGui/ImPlot are part of ws::ui's public surface (apps include <imgui.h> /
# <implot.h> to draw in on_frame()), so they must propagate to consumers.
# GLFW, volk and VMA are pimpl implementation details.
target_link_libraries(ws_ui
  PUBLIC imgui implot
  PRIVATE glfw volk vma)

ws_add_test(ui SOURCES tests/test_ui.cpp)
