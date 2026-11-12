#pragma once

// The SAF bridge's android half (see ws/platform/saf.hpp for the pure
// data policy this rides - the platform lib's own, since the stage-11
// lib extraction; the JNI glue stays app-side beside the
// CircsimActivity Java shim it registers its natives against). Android-only: the whole file guards on
// __ANDROID__, so a desktop build includes it as an empty TU.
//
// What lands here: the pickers (the Storage Access Framework's
// OPEN/CREATE DOCUMENT intents, launched through the CircsimActivity
// Java shim and answered on its onActivityResult), the ContentResolver
// byte moves (a picked document copied into the sandbox, a saved
// document pushed back out), and the public seam the app TU wires -
// dialogs() for the shell's FileDialogs slot and fs() for the
// write-through FileSystem everything else rides.
//
// The threading contract (the shell's dispatch_dialog_pick is what
// makes it safe): open_file/save_file run on a ws::jobs WORKER and
// block there until the activity's result arrives; the render loop
// keeps pumping the surface lifecycle while the picker is in front.
// The bridge's own lock guards the pending-pick slot between the
// worker (the waiter) and the Java thread (the answerer).

#ifdef __ANDROID__

struct android_app;

#include <optional>
#include <string>

#include <ws/platform/dialogs.hpp>
#include <ws/settings/filesystem.hpp>

namespace ws::platform::saf {

// Wires the bridge to the glue's handle: registers the JNI natives the
// activity shim calls, builds the sandbox (<files>/circsim
// editor/schematics) and the persisted map, and loads the map a
// previous launch left. Call once from android_main(), before the app
// object runs; answers false when the wiring is impossible (logged),
// in which case dialogs()/fs() answer null and the app degrades to the
// refusing pickers.
bool init(android_app* app);

// The SAF-backed picker set (null before a successful init). Owned by
// the bridge - a process-lifetime static, so the shell's config can
// hold the raw pointer.
[[nodiscard]] ws::platform::FileDialogs* dialogs();

// The write-through FileSystem (null before init): every verb the
// app's documents, autosave and exports ride, with a mapped sandbox
// write additionally pushed to its content URI.
[[nodiscard]] ws::settings::FileSystem* fs();

// The last refused push, if any - the app drains this between frames
// (its toast's own borrowed-bytes cadence).
[[nodiscard]] std::optional<std::string> take_push_error();

}  // namespace ws::platform::saf

#endif  // __ANDROID__
