// Combined stb implementation TU retained for renderer-free tests that compile
// this source directly. The ws::ui archive uses stb_truetype_impl.cpp for
// fonts and receives stb_image symbols from the reusable ws::image library.
#define STB_IMAGE_IMPLEMENTATION
#define STB_TRUETYPE_IMPLEMENTATION

// stb is a third-party header - keep it clear of our /W4 /WX (MSVC) and
// -Werror (clang) settings.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include <stb_image.h>
#include <stb_truetype.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#endif
