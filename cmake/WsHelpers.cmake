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
  add_executable(${name} ${ARG_SOURCES})
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
