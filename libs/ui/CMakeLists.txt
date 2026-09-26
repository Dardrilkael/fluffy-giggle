ws_add_library(ui DEPENDS ws::core)

# Platform sources: exactly one windowing TU builds (the WindowHost seam,
# src/window.h). The glob picks up both; drop the one this platform does not
# use - a mis-filter fails loudly (the wrong TU's platform includes are
# absent), so it can never silently double-compile.
get_target_property(ui_sources ws_ui SOURCES)
if(ui_sources)
  if(ANDROID)
    list(REMOVE_ITEM ui_sources
      "${CMAKE_CURRENT_SOURCE_DIR}/src/window_glfw.cpp")
  else()
    list(REMOVE_ITEM ui_sources
      "${CMAKE_CURRENT_SOURCE_DIR}/src/window_android.cpp")
  endif()
  set_target_properties(ws_ui PROPERTIES SOURCES "${ui_sources}")
endif()

# Every third-party target is PRIVATE on purpose: apps build against ws::
# names only, and no imgui/vulkan/glfw header or define ever escapes this
# library (apps draw through <ws/ui/widgets.hpp>). Static archives still
# forward these targets to the final link, so symbols resolve.
if(ANDROID)
  target_link_libraries(ws_ui PRIVATE imgui implot volk vma stb android log)
  # native_app_glue headers for src/window_android.cpp (its .c lands in the
  # app module in A2; the header-only usage here is enough for the seam).
  target_include_directories(ws_ui PRIVATE
    "${ANDROID_NDK}/sources/android/native_app_glue")
else()
  target_link_libraries(ws_ui PRIVATE imgui implot glfw volk vma stb)
endif()

ws_add_test(ui SOURCES tests/test_ui.cpp)
# ws_add_test no-ops when WS_BUILD_TESTS=OFF (the android preset), so the
# test's tail settings apply only when the target exists.
if(TARGET test_ui)
  # tests exercise the internal pure-CPU batch core directly (src/ never
  # reaches apps).
  target_include_directories(test_ui PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  # renderer2d.h (internal) declares the Vulkan half: the test needs those
  # headers to compile it, though it never drives Vulkan itself.
  target_link_libraries(test_ui PRIVATE volk vma)
endif()
