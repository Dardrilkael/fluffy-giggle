# ws::platform is the headless platform-services lib (nextplan stage
# 12): standard locations and the debounced file watch now, then
# dialogs/recent files/autosave on the ws::settings FileSystem + json
# seams. Pure std + ws::core; the OS halves stay in per-platform TUs
# behind Result-only APIs, and no platform header ever crosses into
# include/.
ws_add_library(platform DEPENDS ws::core)

# Exactly one locations TU builds (the ws::ui windowing precedent):
# locations_win is the desktop half - the workspace's desktop is
# Windows-only today - and locations_android is the NDK half. The glob
# picks up both; drop the one this target does not use - a mis-filter
# fails loudly on the missing platform includes.
get_target_property(platform_sources ws_platform SOURCES)
if(platform_sources)
  if(ANDROID)
    list(REMOVE_ITEM platform_sources
      "${CMAKE_CURRENT_SOURCE_DIR}/src/locations_win.cpp")
  else()
    list(REMOVE_ITEM platform_sources
      "${CMAKE_CURRENT_SOURCE_DIR}/src/locations_android.cpp")
  endif()
  set_target_properties(ws_platform PROPERTIES SOURCES "${platform_sources}")
endif()

if(NOT ANDROID AND WIN32)
  # locations_win.cpp reads the KnownFolders through SHGetKnownFolderPath
  # (shell32) and frees them through CoTaskMemFree (ole32). System libs,
  # not third-party targets - PRIVATE, advapi32's precedent in ws::ui.
  target_link_libraries(ws_platform PRIVATE shell32 ole32)
endif()

ws_add_test(platform SOURCES tests/test_platform.cpp)
