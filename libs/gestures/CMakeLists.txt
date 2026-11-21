# ws::gestures - the multi-touch arbiter, extracted once for every app.
# THE CONTRACT (arbiter.hpp's own header): frame-in, actions-out - a
# pure state machine over ws::input::InputFrame's touch table that
# answers the one question a touch-capable surface must not improvise:
# "given this frame's contacts, what gesture is in flight and what
# should the view do?" The first finger of a pinch is never a click
# (the deferred-tap verdict), slop and the tap window are policy, the
# pinch zoom compounds per emitted frame, an OS-cancelled contact
# resolves to nothing.
#
# Pure std + ws::input: no rendering, no ui, no app types - the same
# purity class as ws::input itself (the platform TUs are InputFrame's
# only writers; this lib is one of its many readers). Consumers: the
# editor canvas now; the ui CanvasView policy and playground demo can
# retire their private pinch math onto this arbiter later.

ws_add_library(gestures DEPENDS ws::core ws::input)

ws_add_test(gestures SOURCES tests/test_gestures.cpp)
