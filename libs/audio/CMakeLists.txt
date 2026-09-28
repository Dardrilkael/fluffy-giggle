ws_add_library(audio DEPENDS ws::core)

# dr_wav decodes RIFF/WAVE - private, like every third-party here. The
# DR_WAV_IMPLEMENTATION TU is src/dr_wav_impl.cpp (the stb rule); no
# third-party header appears under include/.
target_link_libraries(ws_audio PRIVATE dr_wav)

# OpenAL Soft is the device backend - private the same way. AL/*.h never
# appears under include/ (engine.hpp is a pimpl; only src/ includes AL).
target_link_libraries(ws_audio PRIVATE OpenAL)

ws_add_test(audio SOURCES tests/test_audio.cpp)
