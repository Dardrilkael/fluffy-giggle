include_guard(GLOBAL)

include(WsWarnings)

# ---------------------------------------------------------------------------
# libs/ dependency graph (single source of truth)
# ---------------------------------------------------------------------------
#
# Every first-party lib's deps are declared HERE, exactly once. The
# transitive closure (WS_ALL_LIBS) is computed from this graph before
# ThirdParty.cmake is included, so ws_wants() below answers correctly for
# backend fetches even on a subset build that names only a leaf lib
# ("WS_BUILD_LIBS=spice" pulls in core+math+spice). ws_add_library's
# DEPENDS argument cross-checks against this registry (see below) so the
# link list and the gating list cannot drift.
#
# The graph was hand-written before and had drifted in two places that
# silently broke subset builds:
#   * physics links ws::math but the old table listed only core
#   * ui (via ws_ui_gui) links ws::platform but the old table omitted it
# Both are fixed here. Anything you add to a ws_add_library(DEPENDS ...)
# call MUST also appear here, and vice versa - ws_add_library enforces it.

function(_ws_declare_lib name)
  cmake_parse_arguments(ARG "" "" "DEPENDS" ${ARGN})
  set(deps "")
  foreach(d IN LISTS ARG_DEPENDS)
    # Accept either "core" or "ws::core" - the latter is what
    # ws_add_library's DEPENDS uses; both spell the same lib here.
    if(d MATCHES "^ws::(.+)$")
      list(APPEND deps "${CMAKE_MATCH_1}")
    else()
      list(APPEND deps "${d}")
    endif()
  endforeach()
  set_property(GLOBAL PROPERTY ws_lib_deps_${name} "${deps}")
  set_property(GLOBAL APPEND PROPERTY ws_lib_known_libs "${name}")
endfunction()

# The dependency graph. Order does not matter - the closure walks it.
# core is implicit everywhere it's listed; spelling it keeps the table honest.
_ws_declare_lib(core)
_ws_declare_lib(math      DEPENDS core)
_ws_declare_lib(input     DEPENDS core)
_ws_declare_lib(gfx       DEPENDS core)
_ws_declare_lib(jobs      DEPENDS core)
_ws_declare_lib(settings  DEPENDS core)
_ws_declare_lib(app       DEPENDS core settings)
_ws_declare_lib(platform  DEPENDS core settings)
_ws_declare_lib(ui        DEPENDS core math input gfx jobs settings app platform)
_ws_declare_lib(audio     DEPENDS core)
_ws_declare_lib(net       DEPENDS core)
_ws_declare_lib(physics   DEPENDS core math)
_ws_declare_lib(spice     DEPENDS core math)

# Reads a lib's first-party deps from the registry (empty if unknown).
function(ws_lib_deps out lib)
  get_property(deps GLOBAL PROPERTY ws_lib_deps_${lib})
  set(${out} "${deps}" PARENT_SCOPE)
endfunction()

# Transitive closure of WS_BUILD_LIBS over ws_lib_deps. Returns the
# requested set plus everything it transitively needs. Empty
# WS_BUILD_LIBS (the default) -> empty closure -> the whole workspace.
function(ws_lib_closure out)
  set(result "")
  set(queue ${WS_BUILD_LIBS})
  while(queue)
    list(POP_FRONT queue lib)
    if(lib IN_LIST result)
      continue()
    endif()
    list(APPEND result ${lib})
    ws_lib_deps(deps "${lib}")
    list(APPEND queue ${deps})
  endwhile()
  set(${out} "${result}" PARENT_SCOPE)
endfunction()

# Compute the closure NOW - ThirdParty.cmake's ws_wants reads WS_ALL_LIBS,
# and it is included right after this file by the root CMakeLists.
ws_lib_closure(WS_ALL_LIBS)
set(WS_ALL_LIBS "${WS_ALL_LIBS}" CACHE INTERNAL "libs/ closure actually configured")

# WS_BUILD_LIBS (cache STRING, default ""): when non-empty, only the named
# libs under libs/ (plus their transitive first-party deps) configure - and
# only the third-party backends those libs wrap are fetched. Empty/unset =
# the whole workspace, the unchanged default. Lets a headless checkout
# build a pure-console subset without the gui/audio/net/physics stack: the
# linux preset builds "core;math;spice".
function(ws_wants out lib)
  set(want TRUE)
  if(WS_BUILD_LIBS AND NOT lib IN_LIST WS_ALL_LIBS)
    set(want FALSE)
  endif()
  set(${out} ${want} PARENT_SCOPE)
endfunction()

# ws_add_library(<name> [DEPENDS <target>...])
#
# Creates a static library target ws_<name> with alias ws::<name>.
# Sources are globbed from src/*.cpp (CONFIGURE_DEPENDS); public headers
# live in include/ws/<name>/. There must be at least one .cpp in src/.
#
# The DEPENDS argument is cross-checked against the dependency registry in
# this file (the _ws_declare_lib block above): the names must match exactly
# (modulo the ws:: prefix). A drift fails configure loudly - the link graph
# and the gating graph stay in sync without anyone having to remember.
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
  # Cross-check the DEPENDS argument against the registry. The registry is
  # the source of truth for the gating closure; the DEPENDS argument drives
  # target_link_libraries. Every PUBLIC dep named here must also be in the
  # registry (so a subset build knows to fetch its backends). The registry
  # MAY list extra deps - a PRIVATE link (e.g. ws::math from ws::spice, see
  # DESIGN.md §2.2) is in the registry for gating but not in DEPENDS.
  ws_lib_deps(_registered ${name})
  set(_decl "")
  foreach(d IN LISTS ARG_DEPENDS)
    if(d MATCHES "^ws::(.+)$")
      list(APPEND _decl "${CMAKE_MATCH_1}")
    else()
      list(APPEND _decl "${d}")
    endif()
  endforeach()
  foreach(d IN LISTS _decl)
    if(NOT d IN_LIST _registered)
      message(FATAL_ERROR
        "ws_add_library(${name}): DEPENDS lists '${d}' but the registry in "
        "cmake/WsHelpers.cmake does not. Add _ws_declare_lib(${name} DEPENDS ... ${d} ...) "
        "or drop the dep - the registry drives subset-build gating and the "
        "two cannot drift.")
    endif()
  endforeach()
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

# ws_add_test(<lib> SOURCES <file>... [DEPENDS <target>...] [LINK <target>])
#
# Creates test_<lib> linking ws::<lib> + doctest, and registers it with ctest.
# LINK overrides the ws::<lib> default - for the pure-archive suites whose
# lib is not their own name (test_ui_canvas/test_ui_shell link ws::ui_gui,
# the renderer-free archive, not ws::ui).
# No-op when WS_BUILD_TESTS is OFF.
#
# When doctest is the framework (the desktop default), test cases are
# discovered individually via doctest_discover_tests so ctest -j can fan
# out across them and a failure names the failing case rather than just
# the suite. The legacy suite-level ctest entry is kept too so a plain
# `ctest` still runs every test_<lib>.
function(ws_add_test lib)
  if(NOT WS_BUILD_TESTS)
    return()
  endif()
  cmake_parse_arguments(ARG "" "LINK" "SOURCES;DEPENDS" ${ARGN})
  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "ws_add_test(${lib}): SOURCES is required.")
  endif()
  if(NOT ARG_LINK)
    set(ARG_LINK ws::${lib})
  endif()
  add_executable(test_${lib} ${ARG_SOURCES})
  target_link_libraries(test_${lib} PRIVATE ${ARG_LINK} doctest::doctest ${ARG_DEPENDS})
  target_compile_features(test_${lib} PRIVATE cxx_std_20)
  ws_set_warnings(test_${lib})
  # Suite-level entry: always present, runs every case in one process.
  add_test(NAME ${lib} COMMAND test_${lib})
  # Per-case discovery (ctest -j parallelism + named failures). Best-effort:
  # if doctest_discover_tests is unavailable (older CMake) the suite entry
  # above still covers the run.
  if(COMMAND doctest_discover_tests)
    doctest_discover_tests(test_${lib} TEST_PREFIX "${lib}." TEST_LIST FILE_LIST)
  endif()
endfunction()
