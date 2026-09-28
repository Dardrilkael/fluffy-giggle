#include <ws/audio/engine.hpp>

#include <AL/al.h>    // private: never in a public header
#include <AL/alc.h>

#include <cstdio>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include <ws/audio/wav.hpp>

namespace ws::audio {
namespace {

// "alGenSources: OpenAL error 0xA003 (Invalid Value)" - the what names the
// call, the parenthetical is OpenAL's own description. alGetString may
// return null for codes it does not know; never feed that to printf.
std::string al_describe(const char* what, ALenum err) {
  const ALchar* text = alGetString(err);
  char buf[160];
  std::snprintf(buf, sizeof(buf), "%s: OpenAL error 0x%04X (%s)", what, err,
               text != nullptr ? text : "unknown");
  return buf;
}

}  // namespace

struct AudioEngine::Impl {
  ALCdevice* device = nullptr;
  ALCcontext* context = nullptr;

  std::unordered_map<std::uint32_t, ALuint> sounds;  // handle -> buffer
  struct SourceEntry {
    ALuint source;
    std::uint32_t sound;  // owning sound; unload stops its sources
  };
  std::unordered_map<std::uint32_t, SourceEntry> sources;  // handle -> entry

  std::uint32_t next_sound = 1;   // 0 is never issued
  std::uint32_t next_source = 1;
};

// Both special members live here on purpose: MSVC compiles a defaulted
// constructor in-class against the incomplete Impl, which trips
// unique_ptr's can't-delete-incomplete static_assert.
AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine() {
  (void)shutdown();
}

ws::core::Status AudioEngine::init() {
  if (impl_)
    return fail(AudioError::already_initialized, "engine is already initialized");

  auto fresh = std::make_unique<Impl>();
  fresh->device = alcOpenDevice(nullptr);
  if (!fresh->device)
    return fail(AudioError::device_open_failed,
                "alcOpenDevice(default) returned null");
  fresh->context = alcCreateContext(fresh->device, nullptr);
  if (!fresh->context) {
    alcCloseDevice(fresh->device);
    return fail(AudioError::device_open_failed,
                "alcCreateContext returned null");
  }
  if (alcMakeContextCurrent(fresh->context) == ALC_FALSE) {
    alcDestroyContext(fresh->context);
    alcCloseDevice(fresh->device);
    return fail(AudioError::device_open_failed, "alcMakeContextCurrent failed");
  }
  (void)alGetError();  // context setup can leave a sticky AL error; clear it
  impl_ = std::move(fresh);
  return {};
}

ws::core::Status AudioEngine::shutdown() {
  if (!impl_)
    return {};  // shutting down a dead engine is fine - that is the contract
  // Sources first (a buffer bound to a source defers deletion), then the
  // buffers - both need the context current - then the context, then the
  // device.
  for (auto& [handle, entry] : impl_->sources)
    alDeleteSources(1, &entry.source);
  for (auto& [handle, buffer] : impl_->sounds)
    alDeleteBuffers(1, &buffer);
  alcMakeContextCurrent(nullptr);
  alcDestroyContext(impl_->context);
  alcCloseDevice(impl_->device);
  impl_.reset();
  return {};
}

ws::core::Result<SoundHandle> AudioEngine::load_sound(std::string_view wav_path) {
  WS_TRY_ASSIGN(const WavSound wav, load_wav(wav_path));
  const SoundDesc desc{wav.samples.data(), wav.frame_count(), wav.channels,
                       wav.sample_rate};
  return load_sound(desc);
}

ws::core::Result<SoundHandle> AudioEngine::load_sound(const SoundDesc& desc) {
  if (!impl_)
    return fail(AudioError::not_initialized, "load_sound before init()");
  if (desc.samples == nullptr)
    return fail(AudioError::invalid_sound_data, "null sample pointer");
  if (desc.frame_count == 0)
    return fail(AudioError::invalid_sound_data, "zero frames");
  if (desc.channels != 1 && desc.channels != 2)
    return fail(AudioError::invalid_sound_data,
                "channels must be 1 or 2, got " + std::to_string(desc.channels));
  if (desc.sample_rate == 0)
    return fail(AudioError::invalid_sound_data, "sample_rate must be nonzero");
  if (impl_->sounds.size() >= kMaxSounds)
    return fail(AudioError::too_many_sounds,
                "all " + std::to_string(kMaxSounds) + " sound slots are loaded");

  // ALsizei is int: keep the byte count inside it.
  const auto total_samples = std::uint64_t{desc.frame_count} * desc.channels;
  if (total_samples * 2 > 0x7FFF'FFFFu)
    return fail(AudioError::invalid_sound_data,
                "payload past the 2 GiB OpenAL buffer limit");

  const ALenum format = desc.channels == 1 ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;
  ALuint buffer = 0;
  alGenBuffers(1, &buffer);
  if (const ALenum err = alGetError(); err != AL_NO_ERROR)
    return fail(AudioError::backend_error, al_describe("alGenBuffers", err));
  alBufferData(buffer, format, desc.samples,
               static_cast<ALsizei>(total_samples * 2),
               static_cast<ALsizei>(desc.sample_rate));
  if (const ALenum err = alGetError(); err != AL_NO_ERROR) {
    alDeleteBuffers(1, &buffer);
    return fail(AudioError::backend_error, al_describe("alBufferData", err));
  }

  const SoundHandle handle{impl_->next_sound++};
  impl_->sounds.emplace(handle.value, buffer);
  return handle;
}

ws::core::Status AudioEngine::unload_sound(SoundHandle sound) {
  if (!impl_)
    return fail(AudioError::not_initialized, "unload_sound before init()");
  const auto it = impl_->sounds.find(sound.value);
  if (it == impl_->sounds.end())
    return fail(AudioError::invalid_handle, "no such sound handle");
  // Sources bound to this sound stop with it and give their slots back.
  for (auto src = impl_->sources.begin(); src != impl_->sources.end();) {
    if (src->second.sound == sound.value) {
      alSourceStop(src->second.source);
      alDeleteSources(1, &src->second.source);
      src = impl_->sources.erase(src);
    } else {
      ++src;
    }
  }
  alDeleteBuffers(1, &it->second);
  if (const ALenum err = alGetError(); err != AL_NO_ERROR)
    return fail(AudioError::backend_error, al_describe("unload_sound", err));
  impl_->sounds.erase(it);
  return {};
}

ws::core::Result<SourceHandle> AudioEngine::play(SoundHandle sound, float gain) {
  if (!impl_)
    return fail(AudioError::not_initialized, "play before init()");
  const auto it = impl_->sounds.find(sound.value);
  if (it == impl_->sounds.end())
    return fail(AudioError::invalid_handle, "no such sound handle");
  if (impl_->sources.size() >= kMaxSources)
    return fail(AudioError::too_many_sources,
                "all " + std::to_string(kMaxSources) + " source slots are in use");

  ALuint source = 0;
  alGenSources(1, &source);
  if (const ALenum err = alGetError(); err != AL_NO_ERROR)
    return fail(AudioError::backend_error, al_describe("alGenSources", err));
  alSourcei(source, AL_BUFFER, static_cast<ALint>(it->second));
  alSourcef(source, AL_GAIN, gain);
  if (const ALenum err = alGetError(); err != AL_NO_ERROR) {
    alDeleteSources(1, &source);
    return fail(AudioError::backend_error,
                al_describe("play: buffer/gain setup", err));
  }
  alSourcePlay(source);
  if (const ALenum err = alGetError(); err != AL_NO_ERROR) {
    alDeleteSources(1, &source);
    return fail(AudioError::backend_error, al_describe("alSourcePlay", err));
  }

  const SourceHandle handle{impl_->next_source++};
  impl_->sources.emplace(handle.value, Impl::SourceEntry{source, sound.value});
  return handle;
}

ws::core::Status AudioEngine::stop(SourceHandle source) {
  if (!impl_)
    return fail(AudioError::not_initialized, "stop before init()");
  const auto it = impl_->sources.find(source.value);
  if (it == impl_->sources.end())
    return fail(AudioError::invalid_handle, "no such source handle");
  alSourceStop(it->second.source);
  alDeleteSources(1, &it->second.source);
  if (const ALenum err = alGetError(); err != AL_NO_ERROR)
    return fail(AudioError::backend_error, al_describe("stop", err));
  impl_->sources.erase(it);
  return {};
}

ws::core::Status AudioEngine::set_position(SourceHandle source, float x,
                                           float y, float z) {
  if (!impl_)
    return fail(AudioError::not_initialized, "set_position before init()");
  const auto it = impl_->sources.find(source.value);
  if (it == impl_->sources.end())
    return fail(AudioError::invalid_handle, "no such source handle");
  alSource3f(it->second.source, AL_POSITION, x, y, z);
  if (const ALenum err = alGetError(); err != AL_NO_ERROR)
    return fail(AudioError::backend_error, al_describe("set_position", err));
  return {};
}

ws::core::Status AudioEngine::set_gain(SourceHandle source, float gain) {
  if (!impl_)
    return fail(AudioError::not_initialized, "set_gain before init()");
  const auto it = impl_->sources.find(source.value);
  if (it == impl_->sources.end())
    return fail(AudioError::invalid_handle, "no such source handle");
  alSourcef(it->second.source, AL_GAIN, gain);
  if (const ALenum err = alGetError(); err != AL_NO_ERROR)
    return fail(AudioError::backend_error, al_describe("set_gain", err));
  return {};
}

bool AudioEngine::is_playing(SourceHandle source) const {
  if (!impl_)
    return false;
  const auto it = impl_->sources.find(source.value);
  if (it == impl_->sources.end())
    return false;
  ALint state = 0;
  alGetSourcei(it->second.source, AL_SOURCE_STATE, &state);
  if (alGetError() != AL_NO_ERROR)
    return false;  // bool API: unconfirmable is not playing
  return state == AL_PLAYING;
}

ws::core::Status AudioEngine::update() {
  if (!impl_)
    return fail(AudioError::not_initialized, "update before init()");
  for (auto it = impl_->sources.begin(); it != impl_->sources.end();) {
    ALint state = 0;
    alGetSourcei(it->second.source, AL_SOURCE_STATE, &state);
    if (const ALenum err = alGetError(); err != AL_NO_ERROR)
      return fail(AudioError::backend_error,
                  al_describe("update: state query", err));
    if (state != AL_PLAYING) {  // finished one-shot: free the slot
      alDeleteSources(1, &it->second.source);
      if (const ALenum err = alGetError(); err != AL_NO_ERROR)
        return fail(AudioError::backend_error,
                    al_describe("update: alDeleteSources", err));
      it = impl_->sources.erase(it);
    } else {
      ++it;
    }
  }
  return {};
}

ws::core::Status AudioEngine::set_listener_position(float x, float y, float z) {
  if (!impl_)
    return fail(AudioError::not_initialized, "set_listener_position before init()");
  alListener3f(AL_POSITION, x, y, z);
  if (const ALenum err = alGetError(); err != AL_NO_ERROR)
    return fail(AudioError::backend_error,
                al_describe("set_listener_position", err));
  return {};
}

ws::core::Status AudioEngine::set_listener_orientation(float forward_x,
                                                       float forward_y,
                                                       float forward_z, float up_x,
                                                       float up_y, float up_z) {
  if (!impl_)
    return fail(AudioError::not_initialized,
                "set_listener_orientation before init()");
  const ALfloat orientation[6] = {forward_x, forward_y, forward_z,
                                  up_x, up_y, up_z};
  alListenerfv(AL_ORIENTATION, orientation);
  if (const ALenum err = alGetError(); err != AL_NO_ERROR)
    return fail(AudioError::backend_error,
                al_describe("set_listener_orientation", err));
  return {};
}

}  // namespace ws::audio
