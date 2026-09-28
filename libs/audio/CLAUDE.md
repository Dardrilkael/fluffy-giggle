# libs/audio - CLAUDE.md

Audio in the workspace: WAV decoding now, device playback via OpenAL Soft
next (the backend decision is settled; the engine lands as its own phase).
Consumers see ws::audio names only - no third-party header appears under
include/.

## Purpose
- `errors.hpp`: `AudioError` - the lib's own codes. Every public failure
  returns a `ws::core::Error` with category "audio" and one of these as its
  code; `ws::audio::fail(...)` builds the `unexpected` form.
- `wav.hpp`: `WavSound` (interleaved 16-bit PCM + channels/sample_rate,
  ws-owned) and `load_wav(path) -> Result<WavSound>`. Any bit depth dr_wav
  converts from lands as 16-bit PCM.

## Allowed dependencies
- `ws::core` (Result/Status, logging) - PUBLIC.
- dr_wav (RIFF/WAVE decode) - PRIVATE: `DR_WAV_IMPLEMENTATION` compiles
  exactly once in `src/dr_wav_impl.cpp` (the stb rule) and no public header
  includes it.

## Invariants / rules
- Error handling: public APIs return `ws::core::Result`/`Status` (root
  CLAUDE.md, "Error handling"); codes are `ws::audio::AudioError` values,
  category always "audio", no exceptions cross the boundary. Inside
  ws::audio, `fail(...)` is the enum-taking local - core's raw-code `fail`
  is deliberately hidden there.
- Desktop-only in v1: the lib is gated off the Android build
  (libs/CMakeLists.txt) until an on-device backend decision is made - the
  OpenAL Soft fetch is desktop-only (cmake/ThirdParty.cmake) for the same
  reason.
- Tests build their fixtures in the temp dir at runtime - never commit
  binary fixtures, never write next to the repo.

## Tests
```
ctest --preset debug -R audio
```
