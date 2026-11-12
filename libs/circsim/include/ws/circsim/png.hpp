#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ws/circsim/library.hpp>
#include <ws/circsim/model.hpp>

// The raster surface (plot.hpp's promised "raster exporter later"):
// one headless PNG seam in three parts.
//
//   RasterCanvas        - a pixel-space canvas: the analytic shapes
//                         (polylines, discs, even-odd polygons) drawn
//                         with 4x4-supersampled coverage for clean
//                         edges, and the embedded bitmap font's text
//                         (src/png_font.cpp) blitted 1 bpp. The test
//                         seam reads pixels back through pixel().
//   RasterCanvas::encode_png()
//                      - the PNG bytes: 8-bit RGB, filter-0 scanlines,
//                         zlib wrapper, fixed-Huffman deflate with
//                         greedy LZ77 matching (a distance-1 match
//                         collapses every run; white space is almost
//                         all runs). Deterministic byte for byte.
//   render_schematic_png()
//                      - the composition: parts through render_symbol
//                         (the stage-17a seam - one geometry, now four
//                         surfaces: the editor canvas, the SVG dump,
//                         these pixels, the tests), wires as ortho
//                         polylines, junction and silent-join dots
//                         (build_nets reports the joins the editor
//                         paints as auto-junctions), part annotations
//                         (ref value params, one line, above the
//                         origin) and net labels (under the anchor) in
//                         the embedded font.
//
// The one "font" in a headless lib: it is DATA (a generated bitmap
// table, png_font.cpp's header comment carries the provenance), not a
// font service - no system fonts, no metrics, no ui include.
//
// THREADING: a pure read of Schematic + SymbolLibrary (plus one
// build_nets pass for the silent joins); safe to call from any number
// of threads at once, provided no thread is mutating the schematic (or
// the library) concurrently. A RasterCanvas is a value: no globals,
// no shared state. See docs/threading.md.

namespace ws::circsim {

// render_schematic_png's knobs. ppu is pixels per grid unit, clamped
// to [4, 96]; the canvas is further capped at 16384 px a side by
// halving ppu (floor 4) so a runaway schematic cannot ask for a
// monster allocation.
struct PngOptions {
    int ppu = 24;
};

class RasterCanvas {
public:
    enum class HAlign : std::uint8_t { Left, Center, Right };
    enum class VAlign : std::uint8_t { Top, Middle, Bottom };

    // `background` is 0xRRGGBB; the canvas starts solid.
    RasterCanvas(int width, int height, std::uint32_t background = 0xFFFFFFu);

    [[nodiscard]] int width() const { return w_; }
    [[nodiscard]] int height() const { return h_; }

    // The blended pixel at (x, y) as 0xRRGGBB; undefined territory
    // outside the canvas answers the background. The test seam.
    [[nodiscard]] std::uint32_t pixel(int x, int y) const;

    // Open polyline, round-capped ends and joints: every pixel's
    // coverage is the fraction of its 16 subsample points within
    // `width_px` / 2 of the path - overlaps inside one call collapse
    // (a joint never double-darkens), calls composite normally.
    // Fewer than two points draws nothing.
    void stroke_polyline(const std::vector<std::pair<double, double>>& pts,
                          double width_px, std::uint32_t color);

    // Filled disc, the junction dot.
    void fill_disc(double x, double y, double r_px, std::uint32_t color);

    // Even-odd fill (a hole is the second winding).
    void fill_polygon(const std::vector<std::pair<double, double>>& pts,
                      std::uint32_t color);

    // The embedded font: ASCII 32..126, every other byte skipped (no
    // advance, no ink). `scale` repeats each font pixel as a
    // scale x scale block; anchors position the whole string's box.
    // Height at scale 1: kTextRows rows (row 12 is the baseline).
    void draw_text(std::string_view text, double x, double y, HAlign ha,
                   VAlign va, std::uint32_t color, int scale = 1);

    // The advance draw_text walks, in pixels.
    [[nodiscard]] static double text_width(std::string_view text,
                                            int scale = 1);

    static constexpr int kTextRows = 16;

    // PNG bytes for the canvas (8-bit RGB): signature, IHDR, one
    // IDAT, IEND - deterministic, no timestamps.
    [[nodiscard]] std::string encode_png() const;

private:
    void blend(int x, int y, std::uint32_t color, double coverage);

    int w_ = 0;
    int h_ = 0;
    std::uint32_t bg_ = 0xFFFFFFu;
    // RGB8, row-major, 3 * w_ * h_ bytes.
    std::vector<std::uint8_t> rgb_;
};

// Renders the schematic as a PNG (see the file comment): symbols,
// wires, junction and silent-join dots, part annotations, net labels;
// unknown symbols draw nothing (erc reports them); wires ending on
// unresolvable references draw nothing (the build_nets rule). The
// view state is ignored - the sheet always fits its content.
[[nodiscard]] std::string render_schematic_png(
    const Schematic& s, const SymbolLibrary* symbols = nullptr,
    const PngOptions& opts = {});

}  // namespace ws::circsim
