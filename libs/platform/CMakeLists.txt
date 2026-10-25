# ws::platform is the headless platform-services lib (nextplan stage
# 12): standard locations, the debounced file watch, the recent
# files MRU, the autosave crash-recovery pair, the file dialogs,
# the clipboard, the message boxes, the window title and the
# single-instance grab - the headless halves of stage 12, complete.
# ws::settings is PUBLIC like
# ws::core: its FileSystem + json seams are the workspace's standard and
# the reference lives in a public header (recent.hpp) - the ws::ui
# gui-target precedent. Pure std + those two; the OS halves stay in
# per-platform TUs behind Result-only APIs, and no platform header ever
# crosses into include/.
ws_add_library(platform DEPENDS ws::core ws::settings)

# Exactly one TU of each platform pair builds (the ws::ui windowing
# precedent): the *_win halves are the desktop - the workspace's
# desktop is Windows-only today - and the *_android halves are the
# NDK side. The glob picks up every pair; drop the halves this
# target does not use - a mis-filter fails loudly on the missing
# platform headers.
get_target_property(platform_sources ws_platform SOURCES)
if(platform_sources)
  if(ANDROID)
    list(REMOVE_ITEM platform_sources
      "${CMAKE_CURRENT_SOURCE_DIR}/src/locations_win.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/dialogs_win.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/clipboard_win.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/messagebox_win.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/single_instance_win.cpp")
  else()
    list(REMOVE_ITEM platform_sources
      "${CMAKE_CURRENT_SOURCE_DIR}/src/locations_android.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/dialogs_android.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/clipboard_android.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/messagebox_android.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/single_instance_android.cpp")
  endif()
  set_target_properties(ws_platform PROPERTIES SOURCES "${platform_sources}")
endif()

if(NOT ANDROID AND WIN32)
  # locations_win reads the KnownFolders through SHGetKnownFolderPath
  # (shell32) and frees them through CoTaskMemFree (ole32);
  # dialogs_win hosts the COM pickers (CoInitializeEx/
  # CoCreateInstance, ole32) and parses start folders with
  # SHCreateItemFromParsingName (shell32); clipboard_win's Open/
  # Close/Empty/IsFormatAvailable/Get/SetData and messagebox_win's
  # MessageBoxW ride user32 (the Global* memory family rides the
  # always-default kernel32, unlisted - ws::ui never lists it
  # either). System libs, not third-party targets - PRIVATE,
  # advapi32's precedent in ws::ui.
  target_link_libraries(ws_platform PRIVATE shell32 ole32 user32)
endif()

ws_add_test(platform SOURCES tests/test_platform.cpp)
