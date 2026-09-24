// Sole STB_IMAGE_IMPLEMENTATION translation unit - the same one-TU rule as
// vma_impl.cpp. stb_image is header-only from CMake's view (the stb target
// is an INTERFACE library, PRIVATE to ws_ui); this file turns it into
// linkable symbols.
#define STB_IMAGE_IMPLEMENTATION

// stb is a third-party header - keep it clear of our /W4 /WX settings.
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <stb_image.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
