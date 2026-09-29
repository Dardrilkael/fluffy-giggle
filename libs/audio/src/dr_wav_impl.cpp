// The one dr_wav translation unit (the stb_impl.cpp rule): dr_wav is a
// header-only library whose implementation compiles exactly once, here.
// dr_wav is PRIVATE to ws::audio - this TU stays the only place that pulls
// it in with the implementation define.
#define DR_WAV_IMPLEMENTATION

// dr_wav is a third-party header - keep it clear of our /W4 /WX (MSVC) and
// -Wall -Wextra -Wpedantic -Werror (clang, the Android build) settings.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include <dr_wav.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#endif
