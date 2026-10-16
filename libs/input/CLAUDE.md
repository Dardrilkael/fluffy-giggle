# ws::input - CLAUDE.md

The input family, promoted out of ws::ui (stage 3a of the ui split): the
GLFW-free event types apps react to (`App::on_event`), plus `InputFrame`,
the one-frame raw-input snapshot the gui reads. Pure CPU, core-only - no
platform header anywhere under `include/`.

## The one rule: nothing here knows where input comes from
- `include/ws/input/event.hpp`: the `Event` base, `EventType`, the typed
  event structs and `EventDispatcher`. Key/mouse codes are plain ints in
  GLFW's numbering (mouse buttons 0/1/2, printable keys match ASCII) - no
  GLFW header is ever needed to handle these.
- `include/ws/input/input.hpp`: `InputFrame` (mouse/keys, the dt_seconds
  frame clock, the multi-touch table), `PointerId` + its sentinels,
  `PointerView`, the `keys::` constants the gui gives meaning to,
  `frame_input()` (drop-safe without a running app) and
  `set_active_input()` (the shell's install seam - NOT for apps: ws::ui's
  App::run() points frame_input() at its accumulator for the duration of
  the run).
- `src/input.cpp`: InputFrame's out-of-line half - the touch table, the
  frame advance, the primary-touch -> mouse bridge - plus the
  frame_input() glue.
- ws::ui's platform TUs (src/platform there: window_glfw/window_android)
  are the ONLY writers of an InputFrame; this lib never reaches back up -
  ws::ui links ws::input, never the reverse.
- The namespace is still ws::ui::gui in phase A of the promotion; phase B
  sweeps the whole family (events included) to ws::input.

## Tests
- `tests/test_input.cpp`: headless - no window, no GPU, no platform TU.
  Event type/dispatcher round-trips, next_frame's edge clearing, the touch
  table (release-frame retention, same-frame id reuse, the 8-contact
  cap), the primary-touch bridge, gather_pointers/pointer_pos unification.
  The gui-side adoption cases (GuiContext::begin_frame copying the
  snapshot, dt included) stayed with the ui suite.
- Run: `ctest --preset debug -R input` (or the full preset for all).
