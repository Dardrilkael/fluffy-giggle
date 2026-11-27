add_library(ai_terrain_core STATIC
    src/terrain_analyzer.cpp
    src/fuzzy_controller.cpp
    src/region_simulation.cpp
    src/test_terrain.cpp
    src/experiment_output.cpp
)
target_include_directories(ai_terrain_core PUBLIC include)
target_compile_features(ai_terrain_core PUBLIC cxx_std_20)

add_executable(ai_terrain_poc src/main.cpp)
target_link_libraries(ai_terrain_poc PRIVATE ai_terrain_core)

if(WS_BUILD_TESTS)
    add_executable(ai_terrain_poc_tests tests/test_ai_terrain.cpp)
    target_link_libraries(ai_terrain_poc_tests PRIVATE ai_terrain_core)
    add_test(NAME ai_terrain_poc_tests COMMAND ai_terrain_poc_tests)
endif()

foreach(target ai_terrain_core ai_terrain_poc)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()

if(TARGET ai_terrain_poc_tests)
    if(MSVC)
        target_compile_options(ai_terrain_poc_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(ai_terrain_poc_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()
