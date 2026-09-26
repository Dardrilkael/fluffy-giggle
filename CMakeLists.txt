cmake_minimum_required(VERSION 3.28)
project(ws LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(WS_BUILD_APPS "Build applications under apps/" ON)
option(WS_BUILD_TESTS "Build tests and register them with ctest" ON)

# All executables land in build/<preset>/bin - and so does the Android app
# module (the app is a loadable .so there, not an exe).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(WsHelpers)
include(ThirdParty)

if(WS_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(libs)

if(WS_BUILD_APPS)
  add_subdirectory(apps)
endif()

# Dependency-rule enforcement as a ctest test (see tools/check_deps.ps1).
if(WS_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_deps.ps1")
  add_test(NAME check_deps
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_deps.ps1"
            -Root "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
