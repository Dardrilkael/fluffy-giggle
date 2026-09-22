include_guard(GLOBAL)

# All third-party dependencies, fetched at pinned tags.
# doctest and glfw ship proper CMake support and are used directly.
# Vulkan-Headers, volk, VMA, Dear ImGui and ImPlot get small hand-written
# targets below (marked SYSTEM so their warnings never leak into our build).

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# Some pinned third-party releases predate CMake 3.5; allow them to configure.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

FetchContent_Declare(doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG v2.4.11
  GIT_SHALLOW TRUE)

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

# doctest is a single header - its repo CMake predates CMake 3.5, so instead of
# building it we just expose the header as an interface target.
FetchContent_GetProperties(doctest)
if(NOT doctest_POPULATED)
  FetchContent_Populate(doctest)
endif()
add_library(doctest INTERFACE)
add_library(doctest::doctest ALIAS doctest)
target_include_directories(doctest SYSTEM INTERFACE "${doctest_SOURCE_DIR}")

FetchContent_Declare(vulkan_headers
  GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
  GIT_TAG v1.3.296
  GIT_SHALLOW TRUE)
FetchContent_Declare(volk
  GIT_REPOSITORY https://github.com/zeux/volk.git
  GIT_TAG vulkan-sdk-1.3.296.0
  GIT_SHALLOW TRUE)
FetchContent_Declare(vma
  GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
  GIT_TAG v3.2.1
  GIT_SHALLOW TRUE)
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.5-docking
  GIT_SHALLOW TRUE)
FetchContent_Declare(implot
  GIT_REPOSITORY https://github.com/epezent/implot.git
  GIT_TAG v0.16
  GIT_SHALLOW TRUE)

foreach(dep vulkan_headers volk vma imgui implot)
  FetchContent_GetProperties(${dep})
  if(NOT ${dep}_POPULATED)
    FetchContent_Populate(${dep})
  endif()
endforeach()

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

# --- VulkanMemoryAllocator (header-only interface; implementation TU lives in libs/ui) ---
add_library(vma INTERFACE)
add_library(vma::vma ALIAS vma)
target_include_directories(vma SYSTEM INTERFACE "${vma_SOURCE_DIR}/include")
target_link_libraries(vma INTERFACE volk)

# --- Dear ImGui (+ GLFW and Vulkan backends, docking branch) ----------------
add_library(imgui STATIC
  "${imgui_SOURCE_DIR}/imgui.cpp"
  "${imgui_SOURCE_DIR}/imgui_draw.cpp"
  "${imgui_SOURCE_DIR}/imgui_tables.cpp"
  "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
  "${imgui_SOURCE_DIR}/imgui_demo.cpp"
  "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
  "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp")
target_include_directories(imgui SYSTEM PUBLIC
  "${imgui_SOURCE_DIR}"
  "${imgui_SOURCE_DIR}/backends")
target_link_libraries(imgui PUBLIC glfw volk)
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK)

# --- ImPlot -----------------------------------------------------------------
add_library(implot STATIC
  "${implot_SOURCE_DIR}/implot.cpp"
  "${implot_SOURCE_DIR}/implot_items.cpp")
target_include_directories(implot SYSTEM PUBLIC "${implot_SOURCE_DIR}")
target_link_libraries(implot PUBLIC imgui)
