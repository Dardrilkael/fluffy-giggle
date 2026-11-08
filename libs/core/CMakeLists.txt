ws_add_library(core)

# tl::expected is the sanctioned exception to the PRIVATE link rule: Result is
# core's public API (an alias of tl::expected behind a ws-owned name), so every
# dependent needs the header transitively.
target_link_libraries(ws_core PUBLIC tl::expected)

# logcat is Android's console: ws::core mirrors its lines there (src/log.cpp)
# through the NDK's liblog.
if(ANDROID)
  target_link_libraries(ws_core PRIVATE log)
endif()

# log.cpp uses standard C I/O (fopen, fseek, ftell, etc.) which MSVC deprecates
# in favor of *_s variants. Suppress C4996 warnings-as-errors for this target.
if(MSVC)
  target_compile_definitions(ws_core PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

ws_add_test(core SOURCES tests/test_core.cpp)

# The test also uses std::fopen/fclose in file sink tests.
if(MSVC)
  target_compile_definitions(test_core PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
