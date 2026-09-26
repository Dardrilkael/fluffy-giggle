ws_add_library(core)

# logcat is Android's console: ws::core mirrors its lines there (src/log.cpp)
# through the NDK's liblog.
if(ANDROID)
  target_link_libraries(ws_core PRIVATE log)
endif()

ws_add_test(core SOURCES tests/test_core.cpp)
