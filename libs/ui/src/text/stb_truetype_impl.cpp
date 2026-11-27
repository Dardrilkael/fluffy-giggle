// The ws::ui archive's stb_truetype implementation TU. stb_image is owned
// by ws::image, so the renderer can use the shared decoder without compiling
// a second STB_IMAGE_IMPLEMENTATION into the same final application.
#define STB_TRUETYPE_IMPLEMENTATION

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
