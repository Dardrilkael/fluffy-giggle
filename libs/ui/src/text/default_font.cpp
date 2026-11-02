// The bundled default face's bake-from-bytes (see default_font.h and
// libs/ui/assets/fonts/README.md): the half r2d::load_default_font
// rides over either embedded weight. Pure CPU, like everything in
// text/.

#include "text/default_font.h"

namespace ws::ui::r2d::font {

BakedFont bake_inter(std::span<const std::uint8_t> ttf, float pixel_height,
                     const FontBakeOptions& options) {
    if (ttf.empty() || pixel_height <= 0.0f) {
        return {};
    }
    // stbtt keeps pointers into the span for the whole bake - the
    // embedded arrays live for the process's lifetime, so the borrow
    // is safe by construction (bake_info never retains the pointer).
    const int offset = stbtt_GetFontOffsetForIndex(ttf.data(), 0);
    stbtt_fontinfo info;
    if (offset < 0 || !stbtt_InitFont(&info, ttf.data(), offset)) {
        return {};  // unreachable with the generated arrays; honest anyway
    }
    return bake_info(info, pixel_height, options);
}

}  // namespace ws::ui::r2d::font
