// The sole stb implementation translation unit - the same one-TU rule as
// vma_impl.cpp. stb is header-only from CMake's view (the stb target is an
// INTERFACE library, PRIVATE to ws_ui); this file turns the headers into
// linkable symbols. stb_truetype's implementation joins stb_image's here:
// one third-party implementation TU, not two.
#define STB_IMAGE_IMPLEMENTATION
#define STB_TRUETYPE_IMPLEMENTATION

// stb is a third-party header - keep it clear of our /W4 /WX settings.
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <stb_image.h>
#include <stb_truetype.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
