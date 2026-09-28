// The one dr_wav translation unit (the stb_impl.cpp rule): dr_wav is a
// header-only library whose implementation compiles exactly once, here.
// dr_wav is PRIVATE to ws::audio - this TU stays the only place that pulls
// it in with the implementation define.
#define DR_WAV_IMPLEMENTATION
#include <dr_wav.h>
