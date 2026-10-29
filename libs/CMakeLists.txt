# WS_BUILD_LIBS subset support: the add_subdirectory calls below are each
# gated on the lib being wanted. "Wanted" = the WS_BUILD_LIBS cache list is
# empty (the default: the whole workspace) or the lib is in it, or it is a
# dependency of a wanted lib (the closure below spells the graph out, so a
# preset naming only "spice" still configures core+math+spice). Order stays
# dependency-first: core, math, then everything else.

# The libs/ dependency graph (first-party deps only; ws::core is implicit in
# every entry and spelled anyway for clarity):
#   math    <- core
#   input   <- core
#   gfx     <- core
#   jobs    <- core
#   settings<- core
#   app     <- core, settings
#   platform<- core, settings
#   ui      <- core, math, input, gfx, jobs, settings, app
#   audio   <- core
#   net     <- core
#   physics <- core
#   spice   <- core, math
function(ws_lib_deps out lib)
  set(deps "")
  if(lib STREQUAL "math")
    set(deps core)
  elseif(lib STREQUAL "input")
    set(deps core)
  elseif(lib STREQUAL "gfx")
    set(deps core)
  elseif(lib STREQUAL "jobs")
    set(deps core)
  elseif(lib STREQUAL "settings")
    set(deps core)
  elseif(lib STREQUAL "app")
    set(deps core settings)
  elseif(lib STREQUAL "platform")
    set(deps core settings)
  elseif(lib STREQUAL "ui")
    set(deps core math input gfx jobs settings app)
  elseif(lib STREQUAL "audio")
    set(deps core)
  elseif(lib STREQUAL "net")
    set(deps core)
  elseif(lib STREQUAL "physics")
    set(deps core)
  elseif(lib STREQUAL "spice")
    set(deps core math)
  endif()
  set(${out} "${deps}" PARENT_SCOPE)
endfunction()

# Computes the transitive closure of WS_BUILD_LIBS over ws_lib_deps.
function(ws_lib_closure out)
  set(result "")
  set(queue ${WS_BUILD_LIBS})
  while(queue)
    list(POP_FRONT queue lib)
    if(lib IN_LIST result)
      continue()
    endif()
    list(APPEND result ${lib})
    ws_lib_deps(deps "${lib}")
    list(APPEND queue ${deps})
  endwhile()
  set(${out} "${result}" PARENT_SCOPE)
endfunction()

function(ws_lib_wants out lib)
  set(want TRUE)
  if(WS_BUILD_LIBS AND NOT lib IN_LIST WS_ALL_LIBS)
    set(want FALSE)
  endif()
  set(${out} ${want} PARENT_SCOPE)
endfunction()

ws_lib_closure(WS_ALL_LIBS)
set(WS_ALL_LIBS "${WS_ALL_LIBS}" CACHE INTERNAL "libs/ closure actually configured")

# One `ws_want_<lib>` variable per lib, computed up front - CMake's if()
# cannot call functions, and spelling the result out keeps the gating below
# readable.
foreach(_lib core math input gfx jobs settings app ui audio net physics platform spice)
  ws_lib_wants(_want ${_lib})
  set(ws_want_${_lib} ${_want})
endforeach()
unset(_lib)
unset(_want)

if(ws_want_core)
  add_subdirectory(core)
endif()
if(ws_want_math)
  # ws::math: dense Matrix<T> + LU and ws-owned fixed-size vecmath; the spice
  # arc grows the sparse symbolic-reuse LU facade here (its MNA solver's
  # numerics home, consumed PRIVATE - never a ws::math type in spice headers).
  add_subdirectory(math)
endif()
# ws::input is the input family promoted out of ui (stage 3a): the event
# types + the InputFrame snapshot, pure CPU and GLFW-free (codes ride as
# plain ints). ws::ui's platform TUs are the only writers of an InputFrame,
# so ui links this lib - never the other way around.
if(ws_want_input)
  add_subdirectory(input)
endif()
# ws::gfx is the Vulkan device layer promoted out of ui (stage 3b): the
# loader/instance, the surface-via-callback, device/queue selection, the
# VMA allocator. volk/vma stay PRIVATE here (handles cross to ui as
# opaque void*), and ui is the only consumer - never the other way
# around.
if(ws_want_gfx)
  add_subdirectory(gfx)
endif()
if(ws_want_jobs)
  # ws::jobs is the headless job system (nextplan stage 8): a Result-shaped
  # pool with priorities, cooperative cancellation and coalesced progress +
  # completions marshalled to the owner thread. Pure std + ws::core - no
  # third-party anywhere; consumers (ws::ui first) drain() once per frame.
  add_subdirectory(jobs)
endif()
if(ws_want_settings)
  # ws::settings is the headless settings store (nextplan stage 9): a typed
  # schema (defaults + constraints), layered sources with provenance, change
  # observers and JSON persistence through a minimal FileSystem seam.
  # Pure std + ws::core - no third-party anywhere.
  add_subdirectory(settings)
endif()
if(ws_want_app)
  # ws::app is the headless app-framework services lib (nextplan stage
  # 13): the command registry, then shortcut resolution with user
  # rebinding and the undo stack - the headless halves of stage 13; the
  # ui views over them (menu/toolbar builders, the command palette, the
  # keymap editor) live in ws::ui. Pure std + ws::core; ws::settings
  # joins when keymap persistence lands.
  add_subdirectory(app)
endif()
if(ws_want_ui)
  add_subdirectory(ui)
endif()
if(ws_want_audio)
  # ws::audio builds everywhere: the engine pimpl is OpenAL Soft on both
  # platforms, and OpenAL Soft's own device layer goes AAudio-via-Oboe on
  # Android (cmake/ThirdParty.cmake fetches Oboe there, PRIVATE like every
  # third-party).
  add_subdirectory(audio)
endif()
if(ws_want_net)
  # ws::net is cross-platform on the same libuv backend everywhere: TCP/UDP +
  # LAN discovery, uv private to src/backend/uv (the glfw/openal fetch
  # pattern, unconditional).
  add_subdirectory(net)
endif()
if(ws_want_physics)
  # ws::physics is cross-platform on the same two backends everywhere:
  # Box2D (2D) + Jolt (3D), both private to src/backend and both fetched
  # unconditionally - the 2D and 3D surfaces build identically on Android.
  add_subdirectory(physics)
endif()
if(ws_want_platform)
  # ws::platform is the headless platform-services lib (nextplan stage 12):
  # standard locations and lexical paths now, then dialogs/recent
  # files/autosave/watch on the ws::settings FileSystem + json seams. The
  # os halves stay in per-platform TUs behind Result-only APIs; apps never
  # see a platform header.
  add_subdirectory(platform)
endif()
if(ws_want_spice)
  # ws::spice is the headless circuit simulator (DESIGN.md): netlist parser,
  # elaboration with subckt flattening, MNA assembly over the ws::math sparse
  # LU facade (linked PRIVATE - no ws::math type in a spice header), and the
  # analysis drivers. Pure std + ws::core + ws::math; its CLI is apps/spice.
  add_subdirectory(spice)
endif()
