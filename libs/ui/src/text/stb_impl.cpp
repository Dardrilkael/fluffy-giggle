// The UI's stb_truetype implementation TU. stb_image's implementation is
// owned by ws::image so image decoding has one reusable workspace home.
#define STB_TRUETYPE_IMPLEMENTATION

// stb is a third-party header - keep it clear of our /W4 /WX (MSVC) and
// -Werror (clang) settings.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include <stb_truetype.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#endif
