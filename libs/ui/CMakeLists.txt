# ws::math is a workspace lib (the fixed-size vecmath behind the 3D camera
# path), not a third-party: PUBLIC like ws::core, which apps already receive
# transitively the same way. The include stays internal to src/ - the public
# header keeps plain floats.
ws_add_library(ui DEPENDS ws::core ws::math)

# Platform sources: exactly one windowing TU builds (the WindowHost seam,
# src/platform/window.h). The glob picks up both; drop the one this platform
# does not
# use - a mis-filter fails loudly (the wrong TU's platform includes are
# absent), so it can never silently double-compile.
get_target_property(ui_sources ws_ui SOURCES)
if(ui_sources)
  if(ANDROID)
    list(REMOVE_ITEM ui_sources
      "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/window_glfw.cpp")
  else()
    list(REMOVE_ITEM ui_sources
      "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/window_android.cpp")
  endif()
  set_target_properties(ws_ui PROPERTIES SOURCES "${ui_sources}")
endif()

# Every third-party target is PRIVATE on purpose: apps build against ws::
# names only, and no imgui/vulkan/glfw header or define ever escapes this
# library (apps draw through <ws/ui/widgets.hpp>). Static archives still
# forward these targets to the final link, so symbols resolve.
if(ANDROID)
  target_link_libraries(ws_ui PRIVATE imgui implot volk vma stb android log)
  # native_app_glue headers for src/platform/window_android.cpp (its .c
  # lands in the
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

# --- Shader workflow targets (opt-in; the default build is unchanged) ------
# The committed spirv.h stays the build input (no shader compiler needed);
# these targets make staleness visible and regeneration explicit. The flags
# and the header format live ONLY in tools/compile_shaders.ps1 - the CMake
# side passes paths and reads the exit code, nothing more.
if(NOT ANDROID AND WIN32)
  set(WS_GLSLANG_EXE "" CACHE FILEPATH
      "Optional explicit shader compiler for ws_ui_check/regen_shaders")
  option(WS_UI_REGEN_SHADERS
         "Fold ws_ui_check_shaders into the build (fail the build on a stale spirv.h)"
         OFF)

  find_program(WS_POWERSHELL_EXE powershell)
  if(WS_POWERSHELL_EXE)
    set(_shaders_script "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/compile_shaders.ps1")

    set(_regen_args -NoProfile -ExecutionPolicy Bypass -File ${_shaders_script})
    if(WS_GLSLANG_EXE)
      list(APPEND _regen_args -Compiler ${WS_GLSLANG_EXE})
    endif()
    # Regenerates the committed header in place. Not in ALL on purpose: a
    # build must never rewrite a source-controlled file unprompted.
    add_custom_target(ws_ui_regen_shaders
      COMMAND ${WS_POWERSHELL_EXE} ${_regen_args}
      VERBATIM)

    # -AllowMissingCompiler: a machine with no glslang anywhere must never
    # fail the build over a target it cannot run - SKIPPED and green.
    set(_check_args -NoProfile -ExecutionPolicy Bypass -File ${_shaders_script}
                    -Check -AllowMissingCompiler)
    if(WS_GLSLANG_EXE)
      list(APPEND _check_args -Compiler ${WS_GLSLANG_EXE})
    endif()
    add_custom_target(ws_ui_check_shaders
      COMMAND ${WS_POWERSHELL_EXE} ${_check_args}
      VERBATIM)

    if(WS_UI_REGEN_SHADERS)
      add_dependencies(ws_ui ws_ui_check_shaders)
    endif()
  endif()
endif()

# Dev-only shader hot reload: an internal provider recompiles the GLSL at
# frame boundaries (see src/shader_provider.*). Declared unconditionally
# so an ON value is CHECKED on every platform (Android and Release both
# refuse it at configure time); the define is desktop-debug-only. Default
# OFF - the default build keeps rendering from the committed spirv.h and
# contains no process-spawning code at all.
option(WS_UI_DEV_SHADER_RELOAD
       "ws::ui dev-only: hot-reload batch.vert/.frag at runtime (debug, desktop)"
       OFF)
if(WS_UI_DEV_SHADER_RELOAD)
  if(ANDROID)
    message(FATAL_ERROR
      "WS_UI_DEV_SHADER_RELOAD is desktop-only (it shells out to glslang at runtime).")
  endif()
  # Single-config Ninja presets: CMAKE_BUILD_TYPE IS the config. The
  # hot-reload path shells out per change and waits the device idle to
  # rebake - dev iteration only, never a Release behavior.
  if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(FATAL_ERROR
      "WS_UI_DEV_SHADER_RELOAD requires a Debug build (CMAKE_BUILD_TYPE=Debug).")
  endif()
  target_compile_definitions(ws_ui PRIVATE WS_UI_DEV_SHADER_RELOAD)
endif()
