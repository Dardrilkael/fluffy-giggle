# libs/audio - CLAUDE.md

Audio in the workspace: WAV decoding and device playback behind a
Result-only API. Consumers see ws::audio names only - no third-party header
appears under include/ (OpenAL lives behind engine.hpp's pimpl).

## Purpose
- `errors.hpp`: `AudioError` - the lib's own codes. Every public failure
  returns a `ws::core::Error` with category "audio" and one of these as its
  code; `ws::audio::fail(...)` builds the `unexpected` form.
- `wav.hpp`: `WavSound` (interleaved 16-bit PCM + channels/sample_rate,
  ws-owned) and `load_wav(path) -> Result<WavSound>`. Any bit depth dr_wav
  converts from lands as 16-bit PCM.
- `engine.hpp`: `AudioEngine` - the device, the sounds, the sources.
  Handles are monotonic ids; 0 is never issued; retired handles are stale
  forever. `init`/`shutdown` (idempotent teardown), `load_sound` (path or
  `SoundDesc`) / `unload_sound`, `play`/`stop`/`set_position`/`set_gain`,
  `update()` reaps finished sources, listener setters. `is_playing` is the
  one bool API: unconfirmable reads as "not playing". One engine per
  process - the ALC context is process-global. Caps: `kMaxSounds` 64,
  `kMaxSources` 32.

## Allowed dependencies
- `ws::core` (Result/Status, logging) - PUBLIC.
- dr_wav (RIFF/WAVE decode) - PRIVATE: `DR_WAV_IMPLEMENTATION` compiles
  exactly once in `src/dr_wav_impl.cpp` (the stb rule) and no public header
  includes it. Pure C, built on every platform.
- OpenAL Soft 1.25.2 (device playback) - PRIVATE: only `src/audio_engine.cpp`
  includes `AL/*`; `engine.hpp` is a pimpl. Built STATIC (LIBTYPE), so there
  is no DLL to deploy. Cross-platform: the SAME pimpl TU runs on Windows
  and Android; OpenAL Soft picks its own device layer per platform
  (WASAPI etc. on Windows).
- Oboe 1.11.0 (Android only) - PRIVATE one level deeper: OpenAL Soft's
  Android device backend IS Oboe (Google's official wrapper - AAudio on
  API 27+, an OpenSL fallback inside Oboe below; upstream replaced its
  old direct-AAudio backend). Fetched Android-only
  (`cmake/ThirdParty.cmake`), consumed through OpenAL Soft's `OBOE_SOURCE`
  hook, so its headers and target never leave the openal build. ws::audio
  sees only `AL/*` on both platforms.

## Invariants / rules
- Error handling: public APIs return `ws::core::Result`/`Status` (root
  CLAUDE.md, "Error handling"); codes are `ws::audio::AudioError` values,
  category always "audio", no exceptions cross the boundary. Inside
  ws::audio, `fail(...)` is the enum-taking local - core's raw-code `fail`
  is deliberately hidden there.
- Every OpenAL call is followed by an `alGetError()` check that converts to
  `backend_error` (the message carries the AL code, via `al_describe`).
  Teardown deletes sources and buffers while the context is still current,
  then releases context and device - in that order.
- Cross-platform since the on-device decision landed: minSdk 26 (the
  playground manifest's floor). Oboe uses AAudio from API 27, so a device
  at exactly 26 still sounds through Oboe's internal OpenSL - a fallback
  inside Oboe, not an OpenAL backend of ours. OpenAL Soft's own OpenSL
  backend is forced OFF: Oboe is the one backend, deterministically
  (`ALSOFT_REQUIRE_OBOE` turns a resolution miss into a configure error).
- Tests build their fixtures in the temp dir at runtime - never commit
  binary fixtures, never write next to the repo. Engine tests play at gain
  0: the suite must stay silent. The suite runs on the host (the android
  preset sets WS_BUILD_TESTS=OFF); the engine cases need a working audio
  endpoint on the machine.

## Tests
```
ctest --preset debug -R audio
```
Device playback is exercised for real (the default output device opens);
a machine with no audio device will fail the engine cases.
