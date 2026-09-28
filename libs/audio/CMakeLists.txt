ws_add_library(audio DEPENDS ws::core)

# dr_wav decodes RIFF/WAVE - private, like every third-party here. The
# DR_WAV_IMPLEMENTATION TU is src/dr_wav_impl.cpp (the stb rule); no
# third-party header appears under include/.
target_link_libraries(ws_audio PRIVATE dr_wav)

ws_add_test(audio SOURCES tests/test_audio.cpp)
