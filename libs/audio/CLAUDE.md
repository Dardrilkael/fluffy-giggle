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
  includes it.
- OpenAL Soft 1.25.2 (device playback) - PRIVATE: only `src/audio_engine.cpp`
  includes `AL/*`; `engine.hpp` is a pimpl. Built STATIC (LIBTYPE), so there
  is no DLL to deploy.

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
- Desktop-only in v1: the lib is gated off the Android build
  (libs/CMakeLists.txt) until an on-device backend decision is made - both
  third-party fetches are desktop-only (cmake/ThirdParty.cmake) for the
  same reason.
- Tests build their fixtures in the temp dir at runtime - never commit
  binary fixtures, never write next to the repo. Engine tests play at gain
  0: the suite must stay silent.

## Tests
```
ctest --preset debug -R audio
```
Device playback is exercised for real (the default output device opens);
a machine with no audio device will fail the engine cases.
