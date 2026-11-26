add_library(ws_gameplay INTERFACE)
add_library(ws::gameplay ALIAS ws_gameplay)
target_include_directories(ws_gameplay
  INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_compile_features(ws_gameplay INTERFACE cxx_std_20)
target_link_libraries(ws_gameplay INTERFACE ws::core)

ws_add_test(gameplay_inventory SOURCES tests/test_inventory.cpp tests/test_chunked_grid3d.cpp LINK ws::gameplay)
