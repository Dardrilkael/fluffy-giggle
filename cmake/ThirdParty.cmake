include_guard(GLOBAL)

# All third-party dependencies, fetched at pinned tags.
# doctest and glfw ship proper CMake support and are used directly.
# Vulkan-Headers, volk, VMA, Dear ImGui and ImPlot get small hand-written
# targets below (marked SYSTEM so their warnings never leak into our build).
#
# Subset builds (WS_BUILD_LIBS, set by e.g. the linux preset to
# "core;math;spice") fetch only what the configured libs wrap. The wants_*
# flags below are the single gate: every fetch and hand-written target in
# this file sits behind one of them, so an unconfigured lib's backend never
# reaches the build. An empty WS_BUILD_LIBS (the default) wants everything -
# the full-workspace build is byte-for-byte what it was before the gate.

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# ws_wants lives in WsHelpers.cmake (included before this file by the root
# CMakeLists) - spelled out here per backend family.
ws_wants(ws_wants_core core)
ws_wants(ws_wants_math math)
ws_wants(ws_wants_ui ui)
ws_wants(ws_wants_audio audio)
ws_wants(ws_wants_net net)
ws_wants(ws_wants_physics physics)
# doctest rides with WS_BUILD_TESTS on desktop, whichever libs are built.
if(NOT ANDROID AND WS_BUILD_TESTS)
  set(ws_wants_doctest TRUE)
else()
  set(ws_wants_doctest FALSE)
endif()

# Some pinned third-party releases predate CMake 3.5; allow them to configure.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# doctest is a single header - its repo CMake predates CMake 3.5, so instead of
# building it we just expose the header as an interface target. Tests are never
# cross-built (the android preset sets WS_BUILD_TESTS=OFF), so the fetch is
# desktop-only too.
#
# Header-only deps use FetchContent_Declare(... SOURCE_SUBDIR <nonexistent>)
# + FetchContent_MakeAvailable: SOURCE_SUBDIR pointing at a directory that
# does not exist in the dep makes MakeAvailable populate the source tree
# WITHOUT calling add_subdirectory on it (CMake skips the missing dir), so
# the legacy FetchContent_Populate pattern is gone for good. CMP0169
# (CMake 4.4+) deprecates FetchContent_Populate; this is the migration.
if(ws_wants_doctest)
  FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.11
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR _ws_no_cmake)  # no CMakeLists.txt to add_subdirectory
  FetchContent_MakeAvailable(doctest)
  add_library(doctest INTERFACE)
  add_library(doctest::doctest ALIAS doctest)
  target_include_directories(doctest SYSTEM INTERFACE "${doctest_SOURCE_DIR}")
  # Pull in doctest_discover_tests() so ws_add_test can register each
  # individual case as its own ctest entry. Best-effort: if the repo
  # structure ever moves the cmake helper, the suite-level ctest entry
  # (added unconditionally by ws_add_test) still covers the run - we just
  # lose the per-case fan-out.
  if(EXISTS "${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake")
    list(APPEND CMAKE_MODULE_PATH "${doctest_SOURCE_DIR}/scripts/cmake")
    include(doctest)
  endif()
endif()

# GLFW is the DESKTOP windowing platform. Android renders to the OS-owned
# ANativeWindow (native_app_glue; see
# libs/ui/src/platform/window_android.cpp), so the fetch - and the target -
# simply do not exist there. Everything that
# references glfw is desktop-only as well: the imgui platform backend below
# and ws_ui's link list in libs/ui/CMakeLists.txt.
if(ws_wants_ui AND NOT ANDROID)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
    GIT_SHALLOW TRUE)

  # glfw ships a real CMake build; use it.
  FetchContent_MakeAvailable(glfw)
endif()

# The ui-only fetches. Everything from here to the stb target is one family:
# ws_ui wraps them all PRIVATE, so a build without ui fetches none of them.
# Header-only / source-only - SOURCE_SUBDIR _ws_no_cmake on every Declare
# makes MakeAvailable populate WITHOUT add_subdirectory (the hand-written
# targets below assemble them; CMP0169 deprecates FetchContent_Populate).
if(ws_wants_ui)
FetchContent_Declare(vulkan_headers
  GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
  GIT_TAG v1.3.296
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR _ws_no_cmake)
FetchContent_Declare(volk
  GIT_REPOSITORY https://github.com/zeux/volk.git
  GIT_TAG vulkan-sdk-1.3.296.0
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR _ws_no_cmake)
FetchContent_Declare(vma
  GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
  GIT_TAG v3.2.1
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR _ws_no_cmake)
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.5-docking
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR _ws_no_cmake)
FetchContent_Declare(implot
  GIT_REPOSITORY https://github.com/epezent/implot.git
  GIT_TAG v0.16
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR _ws_no_cmake)
# stb has no release tags - pinned to a master commit. stb_image.h is
# v2.30 (2024-05-31) at this hash. GIT_SHALLOW is OFF: GitHub blocks
# shallow-fetching arbitrary commit SHAs (it only allows shallow fetch of
# tags and branches), so GIT_SHALLOW TRUE + a raw hash fails at checkout
# with "unable to read tree". The stb repo is small (~20 MB), so a full
# clone is cheap and always works.
FetchContent_Declare(stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20
  SOURCE_SUBDIR _ws_no_cmake)
foreach(dep vulkan_headers volk vma imgui implot stb)
  FetchContent_MakeAvailable(${dep})
endforeach()
endif()  # ws_wants_ui declares

# Eigen (header-only; backing store for ws::math) - its own fetch, outside
# the ui family: math builds without ui. SOURCE_SUBDIR _ws_no_cmake keeps
# MakeAvailable from adding its CMakeLists.txt (we just want the headers).
if(ws_wants_math)
FetchContent_Declare(eigen
  GIT_REPOSITORY https://gitlab.com/libeigen/eigen.git
  GIT_TAG 3.4.0
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR _ws_no_cmake)
FetchContent_MakeAvailable(eigen)
endif()

# tl::expected is the std::expected backport backing ws::core's Result/Status
# (result.hpp). Header-only, CC0, single include/tl/expected.hpp - the doctest/
# dr_wav INTERFACE-target pattern. Unconditional inside the core gate: ws::core
# builds on Android. SOURCE_SUBDIR _ws_no_cmake: just populate the source.
if(ws_wants_core)
FetchContent_Declare(tl_expected
  GIT_REPOSITORY https://github.com/TartanLlama/expected.git
  GIT_TAG v1.1.0
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR _ws_no_cmake)
FetchContent_MakeAvailable(tl_expected)
endif()

if(ws_wants_ui)
# --- Vulkan-Headers ---------------------------------------------------------
add_library(vulkan-headers INTERFACE)
add_library(Vulkan::Headers ALIAS vulkan-headers)
target_include_directories(vulkan-headers SYSTEM INTERFACE "${vulkan_headers_SOURCE_DIR}/include")

# --- volk (meta-loader) -----------------------------------------------------
add_library(volk STATIC "${volk_SOURCE_DIR}/volk.c")
target_include_directories(volk SYSTEM PUBLIC "${volk_SOURCE_DIR}")
target_link_libraries(volk PUBLIC vulkan-headers)
# Consumers must not link against a Vulkan loader or use prototypes directly.
target_compile_definitions(volk PUBLIC VK_NO_PROTOTYPES)
# Android presents its window through VK_KHR_android_surface; volk only
# declares (and loads) that entry point when this is set, so it must cover
# volk.c and every consumer TU alike - hence PUBLIC.
if(ANDROID)
  target_compile_definitions(volk PUBLIC VK_USE_PLATFORM_ANDROID_KHR)
endif()

# --- VulkanMemoryAllocator (header-only interface; implementation TU lives in libs/gfx) ---
add_library(vma INTERFACE)
add_library(vma::vma ALIAS vma)
target_include_directories(vma SYSTEM INTERFACE "${vma_SOURCE_DIR}/include")
target_link_libraries(vma INTERFACE volk)

# --- Dear ImGui (+ platform and Vulkan backends, docking branch) ----------------
# The platform backend IS the windowing platform: GLFW's on desktop, the
# ANativeWindow one on Android. Both come from the same fetch; the imgui
# target just swaps the source and the platform link deps per build.
if(ANDROID)
  set(WS_IMGUI_PLATFORM_BACKEND "${imgui_SOURCE_DIR}/backends/imgui_impl_android.cpp")
  set(WS_IMGUI_PLATFORM_DEPS volk android log)
else()
  set(WS_IMGUI_PLATFORM_BACKEND "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp")
  set(WS_IMGUI_PLATFORM_DEPS glfw volk)
endif()
add_library(imgui STATIC
  "${imgui_SOURCE_DIR}/imgui.cpp"
  "${imgui_SOURCE_DIR}/imgui_draw.cpp"
  "${imgui_SOURCE_DIR}/imgui_tables.cpp"
  "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
  "${imgui_SOURCE_DIR}/imgui_demo.cpp"
  "${WS_IMGUI_PLATFORM_BACKEND}"
  "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp")
target_include_directories(imgui SYSTEM PUBLIC
  "${imgui_SOURCE_DIR}"
  "${imgui_SOURCE_DIR}/backends")
target_link_libraries(imgui PUBLIC ${WS_IMGUI_PLATFORM_DEPS})
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK)

# --- ImPlot -----------------------------------------------------------------
add_library(implot STATIC
  "${implot_SOURCE_DIR}/implot.cpp"
  "${implot_SOURCE_DIR}/implot_items.cpp"
  "${implot_SOURCE_DIR}/implot_demo.cpp")
target_include_directories(implot SYSTEM PUBLIC "${implot_SOURCE_DIR}")
target_link_libraries(implot PUBLIC imgui)

# --- stb (header-only; stb_image decodes textures for libs/ui's 2D renderer) --
# The STB_IMAGE_IMPLEMENTATION translation unit lives in libs/ui
# (src/text/stb_impl.cpp) - the same one-TU rule as VMA's.
add_library(stb INTERFACE)
add_library(stb::stb ALIAS stb)
target_include_directories(stb SYSTEM INTERFACE "${stb_SOURCE_DIR}")
endif()  # ws_wants_ui targets

# --- Eigen (header-only; backing store for ws::math) -------------------------
if(ws_wants_math)
add_library(eigen3 INTERFACE)
add_library(Eigen3::Eigen ALIAS eigen3)
target_include_directories(eigen3 SYSTEM INTERFACE "${eigen_SOURCE_DIR}")
endif()

# --- tl::expected (header-only; ws::core's Result/Status backing store) ------
# The ONE third-party linked PUBLIC: ws::core::Result is this type in the
# public API (result.hpp aliases it behind a ws-owned name), so dependents
# must see the header transitively.
if(ws_wants_core)
add_library(tl_expected INTERFACE)
add_library(tl::expected ALIAS tl_expected)
target_include_directories(tl_expected SYSTEM INTERFACE "${tl_expected_SOURCE_DIR}/include")
endif()

if(ws_wants_audio)
# --- dr_wav (header-only; decodes RIFF/WAVE for ws::audio) --------------------
# dr_wav lives in the dr_libs monorepo, header at its root. The
# DR_WAV_IMPLEMENTATION translation unit lives in libs/audio
# (src/dr_wav_impl.cpp) - the same one-TU rule as stb's. Pure C, so the
# fetch is unconditional within the audio gate: ws::audio builds on Android
# too (OpenAL Soft is fetched for both platforms below). SOURCE_SUBDIR
# _ws_no_cmake: header-only, just populate the source.
FetchContent_Declare(dr_wav
  GIT_REPOSITORY https://github.com/mackron/dr_libs.git
  GIT_TAG wav-0.14.5
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR _ws_no_cmake)
FetchContent_MakeAvailable(dr_wav)
add_library(dr_wav INTERFACE)
add_library(dr_wav::dr_wav ALIAS dr_wav)
target_include_directories(dr_wav SYSTEM INTERFACE "${dr_wav_SOURCE_DIR}")

# --- OpenAL Soft (device playback backend for ws::audio) -----------------------
# A real CMake project - built via FetchContent_MakeAvailable, the glfw
# pattern. LIBTYPE is OpenAL Soft's own variable: STATIC means no DLL to
# deploy, and the target propagates AL_LIBTYPE_STATIC itself so AL/al.h
# binds to the static runtime. Examples/utils/install are off, so nothing
# beyond the library gets configured; note the project bundles fmt
# (fmt-11.2.0, an unconditional core dependency) and builds it too.
#
# Both platforms: ws::audio is the same OpenAL pimpl everywhere, and
# OpenAL Soft picks its own device layer per platform - WASAPI etc. on
# Windows, Oboe on Android. On Android (the ONLY direct AAudio user in
# the workspace): Oboe is Google's official wrapper over the NDK audio
# APIs - AAudio on API 27+, an OpenSL fallback INSIDE Oboe below that
# (upstream replaced its older direct-AAudio backend with Oboe). OpenAL
# Soft consumes it through its OBOE_SOURCE hook - an EXCLUDE_FROM_ALL
# add_subdirectory inside the openal build, so no oboe target escapes to
# us. It must be a CACHE variable: openal's own plain
# set(OBOE_SOURCE "" CACHE STRING ...) would strip a normal variable of
# that name, and its no-FORCE set respects an existing cache entry.
# OpenSL stays OFF so Oboe is the ONE backend (deterministic "AAudio
# only"), and REQUIRE turns a resolution miss into a configure error.
# Oboe stays PRIVATE like every third-party: its headers never leave the
# fetched tree, ws::audio sees only AL through the pimpl.
set(LIBTYPE STATIC)
set(ALSOFT_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ALSOFT_UTILS OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL OFF CACHE BOOL "" FORCE)
if(ANDROID)
  FetchContent_Declare(oboe
    GIT_REPOSITORY https://github.com/google/oboe.git
    GIT_TAG 1.11.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR _ws_no_cmake)
  FetchContent_MakeAvailable(oboe)
  set(OBOE_SOURCE "${oboe_SOURCE_DIR}" CACHE STRING
      "Source directory for Oboe (openal-soft's hook; ws sets it)" FORCE)
  set(ALSOFT_BACKEND_OBOE ON CACHE BOOL "" FORCE)
  set(ALSOFT_REQUIRE_OBOE ON CACHE BOOL "" FORCE)
  set(ALSOFT_BACKEND_OPENSL OFF CACHE BOOL "" FORCE)
  # openal defaults its build to C++20 named modules when the compiler
  # claims support (its ALSOFT_ENABLE_MODULES, ON by default). NDK r27's
  # clang 18 claims it but breaks on libc++'s ranges pipe operator through
  # the module path ("deduced return type cannot be used before it is
  # defined", al/auxeffectslot.cpp). Classic TUs on Android; desktop keeps
  # upstream's default (MSVC builds the modules fine).
  set(ALSOFT_ENABLE_MODULES OFF CACHE BOOL "" FORCE)
endif()
FetchContent_Declare(openal
  GIT_REPOSITORY https://github.com/kcat/openal-soft.git
  GIT_TAG 1.25.2
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(openal)
unset(LIBTYPE)
if(ANDROID)
  unset(OBOE_SOURCE CACHE)
endif()
endif()  # ws_wants_audio

if(ws_wants_net)
# --- libuv (async I/O backend for ws::net) -----------------------------------
# A real CMake project - FetchContent_MakeAvailable, the glfw/openal
# pattern. STATIC like every third-party here: LIBUV_BUILD_SHARED OFF means
# only uv_a builds (no DLL to deploy; ws_net links it PRIVATE). Tests and
# benchmarks are root-project-gated upstream but forced OFF anyway - cache
# vars keep it deterministic across presets. ENABLE_CLANG_TIDY is upstream's
# default-ON; not our build's business. Unconditional fetch: ws::net is
# cross-platform (Windows/Android/Linux/macOS) and libuv's own CMake picks
# the platform sources + system libs per build (ws2_32 etc. on MSVC, dl +
# _GNU_SOURCE on Android, no pthread there).
set(LIBUV_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(ENABLE_CLANG_TIDY OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libuv
  GIT_REPOSITORY https://github.com/libuv/libuv.git
  GIT_TAG v1.53.0
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(libuv)
endif()  # ws_wants_net

if(ws_wants_physics)
# --- Box2D (2D physics backend for ws::physics) --------------------------------
# A real CMake project - FetchContent_MakeAvailable, the glfw/openal/libuv
# pattern. v3's target is `box2d` (alias box2d::box2d), STATIC via the
# BUILD_SHARED_LIBS-unset default like every third-party here, linked PRIVATE
# by ws_physics. SAMPLES/UNIT_TESTS/VALIDATE are declared upstream only when
# the project stands alone, so embedding already skips them - but the cache
# FORCE keeps that deterministic across presets (and VALIDATE would compile
# debug-only runtime checks into the lib; off in everything we ship).
# Unconditional fetch: ws::physics builds the same 2D surface on Android.
set(BOX2D_SAMPLES OFF CACHE BOOL "" FORCE)
set(BOX2D_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(BOX2D_VALIDATE OFF CACHE BOOL "" FORCE)
set(BOX2D_BENCHMARKS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(box2d
  GIT_REPOSITORY https://github.com/erincatto/box2d.git
  GIT_TAG v3.1.1
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(box2d)

# --- Jolt Physics (3D physics backend for ws::physics) -------------------------
# A real CMake project living in the repo's Build/ directory (hence
# SOURCE_SUBDIR), the same MakeAvailable pattern. Embedded - not the root
# CMakeLists - it configures ONLY the core library: upstream gates unit
# tests/samples/viewer on being the top-level project. Target `Jolt`, STATIC:
# JPH_BUILD_SHARED_LIBS pinned OFF. The forced options matter:
#   USE_STATIC_MSVC_RUNTIME_LIBRARY defaults ON under MSVC and would mix /MT
#     objects into our /MD binaries - it must follow OUR project default.
#   FLOATING_POINT_EXCEPTIONS_ENABLED defaults ON: trapped FPEs would abort
#     Debug test runs at the first NaN a deliberately-invalid test produced.
#   CROSS_PLATFORM_DETERMINISTIC stays OFF deliberately - ws::physics
#     promises determinism per binary (same build, same thread count), not
#     bit-identical results across compilers and ISAs.
#   ENABLE_ALL_WARNINGS / INTERPROCEDURAL_OPTIMIZATION / ENABLE_INSTALL are
#     upstream standalone-build concerns, not ours (warnings-as-errors and
#     LTO on a dependency slow every preset for nothing), and the debug
#     renderer / profiler are dead weight a headless wrapper never touches.
#   JPH_USE_DX12/JPH_USE_VK/JPH_USE_MTL gate Jolt's GPU compute (the hair
#     demo pipeline) and its BUILD-TIME HLSL->SPIRV shader compilation. All
#     three default ON on every platform - on the NDK there is no
#     glslc/dxc, so those steps fail outright (Vulkan_GLSLC_EXECUTABLE-
#     NOTFOUND). ws::physics runs plain CPU rigid-body simulation and never
#     touches GPU compute, so all three go OFF everywhere; CPU compute keeps
#     its upstream default.
# The desktop SIMD profile (SSE4.1/AVX2/...) keeps upstream's defaults; those
# options don't apply to Android arm64. Unconditional fetch: ws::physics
# builds the same 3D surface on Android too.
set(CROSS_PLATFORM_DETERMINISTIC OFF CACHE BOOL "" FORCE)
set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
set(JPH_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(jolt
  GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
  GIT_TAG v5.6.0
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR Build)
FetchContent_MakeAvailable(jolt)
endif()  # ws_wants_physics
