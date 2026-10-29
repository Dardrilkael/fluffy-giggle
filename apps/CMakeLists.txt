# Apps are added here, one line each. Like libs/ (see libs/CMakeLists.txt),
# each app is gated on its whole lib set being configured: in a WS_BUILD_LIBS
# subset build an app appears iff every ws::<lib> it links exists, so e.g. the
# spice-only preset builds apps/spice and nothing else.
function(ws_app_wants out)
  set(want TRUE)
  if(WS_BUILD_LIBS)
    foreach(lib IN LISTS ARGN)
      if(NOT lib IN_LIST WS_ALL_LIBS)
        set(want FALSE)
      endif()
    endforeach()
  endif()
  set(${out} ${want} PARENT_SCOPE)
endfunction()

# One ws_want_<app> variable per app (CMake's if() cannot call functions).
ws_app_wants(_want playground ui)
set(ws_want_playground ${_want})
ws_app_wants(_want chatter ui net)
set(ws_want_chatter ${_want})
ws_app_wants(_want bounce physics audio ui)
set(ws_want_bounce ${_want})
ws_app_wants(_want ui_gallery ui jobs platform app)
set(ws_want_ui_gallery ${_want})
ws_app_wants(_want wavplayer audio)
set(ws_want_wavplayer ${_want})
ws_app_wants(_want netdemo net)
set(ws_want_netdemo ${_want})
ws_app_wants(_want netchat net)
set(ws_want_netchat ${_want})
ws_app_wants(_want bench_physics physics)
set(ws_want_bench_physics ${_want})
ws_app_wants(_want spice spice)
set(ws_want_spice ${_want})
unset(_want)

if(ws_want_playground)
  add_subdirectory(playground)
endif()
# chatter is a gui app too (netchat's protocol behind a palette gui, built
# for the phone and the desktop alike), so it stays ungated like
# playground - only the console apps below are desktop-only.
if(ws_want_chatter)
  add_subdirectory(chatter)
endif()
if(ws_want_bounce)
  # bounce is the first windowed example to compose three libs: a
  # ws::physics2d pit drawn through ws::ui, with ws::audio thuds on impact.
  add_subdirectory(bounce)
endif()
if(ws_want_ui_gallery)
  # ui_gallery is the gui widget set's acceptance view: one screen with every
  # ws::ui::gui widget family on it, no assets (the system font face, resolved
  # at the platform boundary), so it builds for the phone and the desktop
  # unchanged - ungated for the same reason as the gui apps above.
  add_subdirectory(ui_gallery)
endif()
if(NOT ANDROID)
  if(ws_want_wavplayer)
    # ws::audio builds on Android too (its engine pimpl is OpenAL Soft on both
    # platforms), but wavplayer is a desktop CONSOLE app - a main() that pumps
    # update()/is_playing() with stdout logging - so the app stays gated.
    add_subdirectory(wavplayer)
  endif()
  if(ws_want_netdemo)
    # netdemo is the same kind of console app - ws::net traffic narrated
    # through ws::core logs.
    add_subdirectory(netdemo)
  endif()
  if(ws_want_netchat)
    # netchat too: several terminals (or machines) find each other over
    # discovery and exchange framed text over TCP.
    add_subdirectory(netchat)
  endif()
  if(ws_want_bench_physics)
    # bench_physics is the fourth console app: steps a falling-boxes scene in
    # ws::physics and prints per-tick wall-clock cost per dimension/backend.
    add_subdirectory(bench_physics)
  endif()
  if(ws_want_spice)
    # spice is the ws::spice CLI: parse -> simulate -> write, nothing else.
    add_subdirectory(spice)
  endif()
endif()
