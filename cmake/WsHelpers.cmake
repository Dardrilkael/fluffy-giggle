include_guard(GLOBAL)

include(WsWarnings)

# ws_add_library(<name> [DEPENDS <target>...])
#
# Creates a static library target ws_<name> with alias ws::<name>.
# Sources are globbed from src/*.cpp (CONFIGURE_DEPENDS); public headers
# live in include/ws/<name>/. There must be at least one .cpp in src/.
function(ws_add_library name)
  cmake_parse_arguments(ARG "" "" "DEPENDS" ${ARGN})
  file(GLOB_RECURSE _srcs CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
  if(NOT _srcs)
    message(FATAL_ERROR
      "ws_add_library(${name}): no sources found in ${CMAKE_CURRENT_SOURCE_DIR}/src. "
      "Add at least one .cpp file.")
  endif()
  add_library(ws_${name} STATIC ${_srcs})
  add_library(ws::${name} ALIAS ws_${name})
  target_include_directories(ws_${name}
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_features(ws_${name} PUBLIC cxx_std_20)
  ws_set_warnings(ws_${name})
  if(ARG_DEPENDS)
    target_link_libraries(ws_${name} PUBLIC ${ARG_DEPENDS})
  endif()
endfunction()

# ws_add_app(<name> [SOURCES <file>...] [DEPENDS <target>...])
#
# Creates an executable. If SOURCES is omitted, globs src/*.cpp.
function(ws_add_app name)
  cmake_parse_arguments(ARG "" "" "DEPENDS;SOURCES" ${ARGN})
  if(NOT ARG_SOURCES)
    file(GLOB_RECURSE ARG_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
  endif()
  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "ws_add_app(${name}): no sources.")
  endif()
  if(ANDROID)
    # Android apps are loadable modules: the OS dlopens lib<name>.so and
    # calls ANativeActivity_onCreate, which the glue .c defines - -u keeps
    # the linker from dropping it (nothing else in the module references
    # it). The glue is platform code, not ours: -w on its .c keeps Google's
    # warnings out of our -Werror build.
    add_library(${name} MODULE ${ARG_SOURCES})
    set_target_properties(${name} PROPERTIES PREFIX "lib")
    set_source_files_properties(
      "${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c"
      PROPERTIES COMPILE_OPTIONS "-w")
    target_sources(${name} PRIVATE
      "${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c")
    target_include_directories(${name} PRIVATE
      "${ANDROID_NDK}/sources/android/native_app_glue")
    target_link_options(${name} PRIVATE "-uANativeActivity_onCreate")
    # NDK platform libs (glue logging, backends): the platform surface, not
    # a third-party leak - the workspace rule's target is GLFW/ImGui-style
    # deps; android/log are link-only, no headers escape the app TU.
    target_link_libraries(${name} PRIVATE android log)
  else()
    add_executable(${name} ${ARG_SOURCES})
  endif()
  target_compile_features(${name} PRIVATE cxx_std_20)
  ws_set_warnings(${name})
  if(ARG_DEPENDS)
    target_link_libraries(${name} PRIVATE ${ARG_DEPENDS})
  endif()
endfunction()

# ws_add_test(<lib> SOURCES <file>... [DEPENDS <target>...])
#
# Creates test_<lib> linking ws::<lib> + doctest, and registers it with ctest.
# No-op when WS_BUILD_TESTS is OFF.
function(ws_add_test lib)
  if(NOT WS_BUILD_TESTS)
    return()
  endif()
  cmake_parse_arguments(ARG "" "" "SOURCES;DEPENDS" ${ARGN})
  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "ws_add_test(${lib}): SOURCES is required.")
  endif()
  add_executable(test_${lib} ${ARG_SOURCES})
  target_link_libraries(test_${lib} PRIVATE ws::${lib} doctest::doctest ${ARG_DEPENDS})
  target_compile_features(test_${lib} PRIVATE cxx_std_20)
  ws_set_warnings(test_${lib})
  add_test(NAME ${lib} COMMAND test_${lib})
endfunction()
