// The PNG/live consistency suite (the E3 export's own gate): the
// Results panel draws twice - live through Canvas2D and headless
// through render_plot_png's RasterCanvas - and this file pins the two
// paths' SEMANTICS to one answer. The cases feed the export the same
// tables the panel suite feeds the live draw (ramp/mixed/family
// shapes built with the same helpers) and assert, through the
// RasterCanvas pixel seam, the behaviors the old export got wrong:
// the two-spine fit (a mixed V/I table earns its right axis), the
// per-run series shape (a .step family never chords its passes), the
// envelope fit, the marker placement, and the hidden-signal
// exclusion per spine.
//
// The pixel seam reads exact colors (the raster is deterministic:
// supersampled coverage over a solid background), so the asserts
// below can demand a series color AT a mapped coordinate and absence
// of it at a seam - the strongest shape checks a headless suite can
// make without decoding the PNG (encode_png itself is gated by
// test_render_png).
//
// The parity half drives the shared math directly: for the same
// table, the export's home ranges must equal the live plot's home
// ranges - both ride ws::circsim::expand_range over the same
// per-spine envelopes, so the check is on the seams that feed them
// (the classifier and the envelope walk), computed here through the
// same public calls the panel and the exporter both use.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "../src/editor.hpp"
#include "../src/results_panel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#include <ws/circsim/plot.hpp>
#include <ws/spice/results.hpp>
#include <ws/spice/run.hpp>
#include <ws/ui/renderer2d.hpp>

namespace {

namespace editor_ns = editor;
using ws::circsim::AxisRuns;
using ws::circsim::PlotRange;
using ws::circsim::PlotYAxis;

// The PNG canvas' own constants (render_plot_png's): the frame the
// pixel asserts map through. Duplicated here as the test's own
// knowledge of the layout under test, not a second implementation.
constexpr int kPngW = 1280;
constexpr int kPngH = 720;
constexpr int kML = 72, kMB = 48, kMT = 40;
constexpr int kMR = 24, kMRAxis = 72;

// The series palette the panel draws with (series_color's wheel,
// i mod 8 - the same table the exporter and the live draw ride).
[[nodiscard]] std::uint32_t series_rgb(std::size_t i) {
    constexpr std::uint32_t kSeries[] = {
        ws::ui::r2d::rgba(224, 90, 78),   ws::ui::r2d::rgba(78, 161, 224),
        ws::ui::r2d::rgba(88, 181, 92),   ws::ui::r2d::rgba(211, 154, 42),
        ws::ui::r2d::rgba(154, 111, 211), ws::ui::r2d::rgba(78, 201, 192),
        ws::ui::r2d::rgba(211, 90, 158),  ws::ui::r2d::rgba(143, 154, 78),
    };
    return (kSeries[i % 8u] >> 8u) & 0xFFFFFFu;
}

// The decode: render_plot_png answers PNG bytes; the pixel seam the
// asserts need is RasterCanvas::pixel - so the cases below rebuild
// the SAME raster by calling the export's building blocks? No - they
// do something simpler and stronger: the export's pure inputs are
// driven through a tiny RasterCanvas-free probe that mirrors the
// export's LAYOUT (the same margins and fit math) only to compute
// WHERE things must land; the raster itself is asserted by decoding
// the PNG's raw scanlines (filter-0, 8-bit RGB - the encoder's own
// contract, parseable in 30 lines below).

// --- the PNG decode (filter-0 scanlines; the encoder writes no
// filters but 0, per png.hpp's own contract) ---
struct PngImage {
    int w = 0, h = 0;
    std::vector<std::uint32_t> px;  // 0xRRGGBB
};

[[nodiscard]] std::uint32_t be32(const unsigned char* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

// The PNG IDAT decoder, validated by design against the writer's
// own contract (png.hpp): filter-0 scanlines, ONE fixed-Huffman
// block, MSB-first Huffman codes, LSB-first fields. The reader
// materializes the whole bit stream first (byte-major, LSB-first
// within each byte) and walks an index - no reader state to
// desynchronize, every bit read exactly once. The length/distance
// tables are the RFC's (the same values the writer emits, spelled
// independently here so the two must agree - and the Adler32 check
// in decode_png is the end-to-end gate that catches any drift).
class Inflate {
public:
    explicit Inflate(const unsigned char* data, std::size_t size) {
        bits_.reserve(size * 8u);
        for (std::size_t b = 0; b < size; ++b) {
            for (int k = 0; k < 8; ++k) {
                bits_.push_back((data[b] >> k) & 1u);
            }
        }
    }

    [[nodiscard]] std::vector<unsigned char> run() {
        // Skip the 2-byte zlib header (16 bits); walk to end-of-block.
        at_ = 16;
        const unsigned bfinal = lsb(1);
        const unsigned btype = lsb(2);
        REQUIRE(bfinal == 1u);  // one block per stream (the writer's)
        REQUIRE(btype == 1u);   // fixed Huffman only (the writer's)
        out_.clear();
        for (;;) {
            const unsigned sym = fixed_lit();
            if (sym < 256u) {
                out_.push_back(static_cast<unsigned char>(sym));
                continue;
            }
            if (sym == 256u) {
                return out_;  // end of block
            }
            const unsigned li = sym - 257u;
            REQUIRE(li < 29u);
            const unsigned len =
                kLenBase[li] +
                (kLenExtra[li] != 0u ? lsb(kLenExtra[li]) : 0u);
            const unsigned dc = msb(5);
            REQUIRE(dc < 30u);
            const unsigned dist =
                kDistBase[dc] +
                (kDistExtra[dc] != 0u ? lsb(kDistExtra[dc]) : 0u);
            REQUIRE(dist <= out_.size());
            for (unsigned k = 0; k < len; ++k) {
                out_.push_back(out_[out_.size() - dist]);
            }
        }
    }

private:
    // LSB-first field (block headers, extra bits).
    unsigned lsb(unsigned count) {
        unsigned v = 0;
        for (unsigned k = 0; k < count; ++k) {
            if (at_ + k < bits_.size()) {
                v |= static_cast<unsigned>(bits_[at_ + k]) << k;
            }
        }
        at_ += count;
        return v;
    }
    // MSB-first Huffman code.
    unsigned msb(unsigned count) {
        unsigned v = 0;
        for (unsigned k = 0; k < count; ++k) {
            if (at_ + k < bits_.size()) {
                v = (v << 1) | static_cast<unsigned>(bits_[at_ + k]);
            }
        }
        at_ += count;
        return v;
    }

    // Fixed-Huffman's literal/length tree, decoded the canonical way:
    // the code's own value bits MSB-first, then one bucket walk. The
    // code spaces (RFC 1951 s3.2.6, prefix-free):
    //   7 bits  0000000..0010111 (0x00..0x17)   -> symbols 256..279
    //   8 bits  00110000..10111111 (0x30..0xBF) -> literals 0..143
    //   8 bits  11000000..11000101 (0xC0..0xC5) -> symbols 280..287
    //   9 bits  110010000..111111111 (0x190..0x1FF) -> literals 144..255
    // The 7-bit PREFIXES bucket (top 7 of each space): 0x00..0x17 the
    // 7-bit symbols; 0x18..0x5F the 8-bit literals (top 7 of
    // 0x30..0xBF); 0x60..0x62 the 8-bit 280s (top 7 of 0xC0..0xC5);
    // 0x64..0x7F the 9-bit literals (top 7 of 0x190..0x1FF: 0x190's
    // top 7 is 0b1100100 = 0x64, 0x1FF's is 0b1111111 = 0x7F). The
    // gaps (0x63, 0x80..0xFF) are unused space - no valid code starts
    // there, so a corrupt stream fails by name.
    unsigned fixed_lit() {
        unsigned v = msb(7);
        if (v <= 0x17u) {
            return v + 256u;           // 256..279 (EOB is 0000000)
        }
        if (v <= 0x5Fu) {
            v = (v << 1) | lsb(1);     // the 8th bit
            REQUIRE(v >= 0x30u);
            return v - 0x30u;          // literals 0..143
        }
        if (v <= 0x62u) {
            v = (v << 1) | lsb(1);     // the 8th bit
            REQUIRE(v >= 0xC0u);
            REQUIRE(v <= 0xC7u);
            return v - 0xC0u + 280u;   // 280..287 (writer: 280..285)
        }
        REQUIRE(v >= 0x64u);
        REQUIRE(v <= 0x7Fu);
        v = (v << 1) | lsb(1);         // the 8th bit
        v = (v << 1) | lsb(1);         // the 9th bit
        REQUIRE(v >= 0x190u);
        return v - 0x190u + 144u;     // literals 144..255
    }

    static constexpr unsigned kLenBase[29] = {
        3,   4,   5,   6,   7,   8,   9,   10,  11,  13,
        15,  17,  19,  23,  27,  31,  35,  43,  51,  59,
        67,  83,  99,  115, 131, 163, 195, 227, 258};
    static constexpr unsigned kLenExtra[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static constexpr unsigned kDistBase[30] = {
        1,   2,   3,   4,   5,   7,    9,    13,   17, 25,
        33,  49,  65, 97,  129, 193,  257,  385,  513, 769,
        1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289,
        16385, 24577};
    static constexpr unsigned kDistExtra[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

    std::vector<unsigned char> bits_;
    std::vector<unsigned char> out_;
    std::size_t at_ = 0;
};


[[nodiscard]] PngImage decode_png(const std::string& png) {
    PngImage img;
    REQUIRE(png.size() > 8u);
    REQUIRE(png.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0);
    std::size_t at = 8;
    std::string idat;
    while (at + 8 <= png.size()) {
        const std::size_t len = be32(
            reinterpret_cast<const unsigned char*>(png.data() + at));
        const std::string type = png.substr(at + 4, 4);
        const std::size_t body = at + 8;
        if (type == "IHDR") {
            REQUIRE(len == 13u);
            img.w = static_cast<int>(
                be32(reinterpret_cast<const unsigned char*>(
                    png.data() + body)));
            img.h = static_cast<int>(be32(
                reinterpret_cast<const unsigned char*>(
                    png.data() + body + 4)));
            REQUIRE(png[body + 8] == 8);   // bit depth
            REQUIRE(png[body + 9] == 2);   // RGB
            REQUIRE(png[body + 12] == 0);  // no interlace
        } else if (type == "IDAT") {
            idat.append(png, body, len);
        } else if (type == "IEND") {
            break;
        }
        at = body + len + 4;  // +CRC
    }
    REQUIRE(img.w > 0);
    REQUIRE(img.h > 0);
    Inflate inflate(reinterpret_cast<const unsigned char*>(idat.data()),
                    idat.size());
    const std::vector<unsigned char> raw = inflate.run();
    // The end-to-end gate: the zlib trailer's Adler32 must match the
    // decoded scanlines (the writer computes it over exactly these
    // bytes - a decoder that drops or repeats a single bit fails
    // here, never in the pixel asserts below).
    {
        std::size_t a = 1;
        std::size_t b = 0;
        for (unsigned char c : raw) {
            a = (a + c) % 65521u;
            b = (b + a) % 65521u;
        }
        const std::uint32_t adler =
            static_cast<std::uint32_t>((b << 16) | a);
        const std::uint32_t trailer =
            be32(reinterpret_cast<const unsigned char*>(
                idat.data() + idat.size() - 4));
        CHECK(adler == trailer);
    }
    REQUIRE(raw.size() >= static_cast<std::size_t>(img.h) *
                               (1u + 3u * static_cast<unsigned>(img.w)));
    img.px.resize(static_cast<std::size_t>(img.w) * img.h);
    for (int y = 0; y < img.h; ++y) {
        const unsigned char* row =
            raw.data() + static_cast<std::size_t>(y) *
                             (1 + 3 * img.w);
        REQUIRE(row[0] == 0);  // filter 0, the encoder's own contract
        for (int x = 0; x < img.w; ++x) {
            const unsigned char* p = row + 1 + 3 * x;
            img.px[static_cast<std::size_t>(y) * img.w + x] =
                (static_cast<std::uint32_t>(p[0]) << 16) |
                (static_cast<std::uint32_t>(p[1]) << 8) |
                static_cast<std::uint32_t>(p[2]);
        }
    }
    return img;
}

// The pixel accessor + scan helpers.
[[nodiscard]] std::uint32_t px(const PngImage& img, int x, int y) {
    return img.px[static_cast<std::size_t>(y) * img.w + x];
}
[[nodiscard]] bool any_pixel(const PngImage& img, std::uint32_t color,
                             int x0, int y0, int x1, int y1) {
    for (int y = std::max(0, y0); y <= std::min(img.h - 1, y1); ++y) {
        for (int x = std::max(0, x0); x <= std::min(img.w - 1, x1); ++x) {
            if (px(img, x, y) == color) {
                return true;
            }
        }
    }
    return false;
}
[[nodiscard]] int count_pixel(const PngImage& img, std::uint32_t color,
                              int x0, int y0, int x1, int y1) {
    int n = 0;
    for (int y = std::max(0, y0); y <= std::min(img.h - 1, y1); ++y) {
        for (int x = std::max(0, x0); x <= std::min(img.w - 1, x1); ++x) {
            if (px(img, x, y) == color) {
                ++n;
            }
        }
    }
    return n;
}

// --- the tables -------------------------------------------------------------

[[nodiscard]] ws::spice::Table ramp_table(std::size_t rows) {
    ws::spice::Table t;
    t.analysis = ws::spice::AnalysisKind::Tran;
    t.sweep = ws::spice::SweepKind::Time;
    t.sweep_names = {"time"};
    t.sweep_axes.emplace_back();
    t.sweep_axes.back().reserve(rows);
    for (std::size_t i = 0; i < rows; ++i) {
        t.sweep_axes.back().push_back(1.0e-4 * static_cast<double>(i));
    }
    ws::spice::Signal out;
    out.name = "v(out)";
    out.data.reserve(rows);
    for (std::size_t i = 0; i < rows; ++i) {
        out.data.push_back(5.0 * static_cast<double>(i) /
                           static_cast<double>(rows - 1u));
    }
    ws::spice::Signal cur;
    cur.name = "i(V1)";
    cur.data.reserve(rows);
    for (std::size_t i = 0; i < rows; ++i) {
        const double x = static_cast<double>(i) /
                         static_cast<double>(rows - 1u);
        cur.data.push_back(1.0e-3 * std::sin(2.0 * 3.14159265358979 * x));
    }
    t.signals.push_back(std::move(out));
    t.signals.push_back(std::move(cur));
    return t;
}

// The .step family shape: the inner axis rises over 0..0.2 then walks
// back (the wrap), `passes` times.
[[nodiscard]] ws::spice::Table step_family_table(std::size_t rows,
                                                 std::size_t passes) {
    ws::spice::Table t = ramp_table(rows);
    t.analysis = ws::spice::AnalysisKind::Dc;
    for (std::size_t i = 0; i < t.sweep_axes[0].size(); ++i) {
        t.sweep_axes[0][i] =
            1.0e-4 * static_cast<double>(i < rows / 2u ? i
                                                       : rows - 1u - i);
    }
    (void)passes;
    return t;
}

// A many-pass family with DIFFERENT pass lengths (the envelope case:
// the last pass is longer than the others - a front/back walk clips
// it, the union envelope does not).
[[nodiscard]] ws::spice::Table ragged_family_table() {
    ws::spice::Table t;
    t.analysis = ws::spice::AnalysisKind::Tran;
    t.sweep = ws::spice::SweepKind::Time;
    t.sweep_names = {"time"};
    // Two passes: the first spans 0..0.1 (100 rows), the second
    // 0..0.2 (200 rows) - the axis RESETS mid-table and the second
    // pass reaches FURTHER than the first.
    t.sweep_axes.emplace_back();
    t.sweep_axes.back().reserve(300);
    for (std::size_t i = 0; i < 100u; ++i) {
        t.sweep_axes.back().push_back(1.0e-3 * static_cast<double>(i));
    }
    for (std::size_t i = 0; i < 200u; ++i) {
        t.sweep_axes.back().push_back(1.0e-3 * static_cast<double>(i));
    }
    ws::spice::Signal out;
    out.name = "v(out)";
    out.data.reserve(300u);
    for (std::size_t i = 0; i < 300u; ++i) {
        // Pass 1 rides high, pass 2 rides low - the two bands never
        // overlap, so a chord between the passes would cross the
        // empty middle the asserts scan.
        out.data.push_back(i < 100u ? 1.0 : -1.0);
    }
    t.signals.push_back(std::move(out));
    return t;
}

}  // namespace

// --- the axis-scaling cases --------------------------------------------------

TEST_CASE("png: a voltage-only table keeps the single-spine geometry") {
    ws::spice::Table t = ramp_table(64u);
    t.signals.erase(t.signals.begin() + 1);  // drop i(V1)
    const std::string png =
        editor_ns::render_plot_png(t, nullptr, "time", nullptr, 0);
    const PngImage img = decode_png(png);

    // No right spine: the frame's right edge sits at kW - kMR (24),
    // and the right label strip (the widened 72px margin) does not
    // exist - no series ink beyond the frame.
    const double iw = kPngW - kML - kMR;
    const int frame_r = static_cast<int>(kML + iw);
    REQUIRE(frame_r > 0);
    REQUIRE(frame_r < kPngW);
    // Series ink lives INSIDE the frame (the ramp spans it).
    CHECK(any_pixel(img, series_rgb(0), kML + 1, kMT + 1, frame_r - 2,
                    kMT + 700 - kMB - 2));
    // No ink in the right label strip (beyond the narrow margin).
    CHECK_FALSE(any_pixel(img, series_rgb(0), frame_r + 2, 0, kPngW - 1,
                          kPngH - 1));
}

TEST_CASE("png: a mixed V/I table earns its right spine") {
    const ws::spice::Table t = ramp_table(64u);
    const std::string png =
        editor_ns::render_plot_png(t, nullptr, "time", nullptr, 0);
    const PngImage img = decode_png(png);

    // The widened margin: the frame's right edge is at kW - 72, so
    // both series live inside it, and the right label strip's tick
    // MARKS sit just past the edge (grid gray 0x2A2F38, 3px long).
    const double iw = kPngW - kML - kMRAxis;
    const int frame_r = static_cast<int>(kML + iw);
    REQUIRE(frame_r < kPngW - 8);
    // Both series drew (v left spine red, i right spine blue).
    CHECK(any_pixel(img, series_rgb(0), kML + 1, kMT + 1, frame_r - 2,
                    kPngH - kMB - 2));
    CHECK(any_pixel(img, series_rgb(1), kML + 1, kMT + 1, frame_r - 2,
                    kPngH - kMB - 2));

    // The INDEPENDENT transforms, the flattening regression: the
    // current rides its own spine. In the old one-scale export the
    // amps (-1m..1m against volts 0..5) drew as a flat line AT the
    // 0-row of the volts' window; on their own 5%-padded +/-1m window
    // they span a real band. The detector: the blue series' GLOBAL
    // y extent over the whole frame (a column shows only the line AT
    // that x - the amplitude story lives across columns: the sine's
    // top and bottom peaks sit in different columns).
    const int band_top = kMT + 1;
    const int band_bot = kPngH - kMB - 2;
    int blue_min_y = band_bot;
    int blue_max_y = band_top;
    for (int y = band_top; y <= band_bot; ++y) {
        for (int x = static_cast<int>(kML) + 1; x < frame_r - 1; ++x) {
            if (px(img, x, y) == series_rgb(1)) {
                blue_min_y = std::min(blue_min_y, y);
                blue_max_y = std::max(blue_max_y, y);
            }
        }
    }
    const int span = blue_max_y - blue_min_y;
    // A flattened sine would answer < 6 px (one line width). A real
    // amplitude on its own spine answers most of the 632-px frame
    // (the sine's peak-to-peak ~0.95 of the padded window). Demand
    // at least 25.
    REQUIRE(blue_min_y <= blue_max_y);
    CHECK(span >= 25);

    // The right spine's tick labels exist: light gray text pixels in
    // the right label strip (x in (frame_r, kPngW), the widened 72px
    // band's left part), a column the old export left empty.
    int label_ink = 0;
    for (int y = kMT + 1; y < kPngH - kMB; ++y) {
        for (int x = frame_r + 2; x < std::min(kPngW, frame_r + 60); ++x) {
            const std::uint32_t c = px(img, x, y);
            if (c != 0x1E2229u && c != 0x2A2F38u && c != series_rgb(0) &&
                c != series_rgb(1)) {
                ++label_ink;
            }
        }
    }
    CHECK(label_ink >= 40);
}

TEST_CASE("png: hiding the current column drops the right spine") {
    const ws::spice::Table t = ramp_table(64u);
    std::vector<std::uint8_t> vis{1u, 0u};  // v(out) shown, i(V1) hidden
    const std::string png = editor_ns::render_plot_png(
        t, vis.data(), "time", nullptr, 0);
    const PngImage img = decode_png(png);

    // No right spine: the narrow margin (the frame reaches 1280-24)
    // and the hidden series drew nothing.
    const double iw = kPngW - kML - kMR;
    const int frame_r = static_cast<int>(kML + iw);
    // No blue series ink anywhere (hidden), no right-label strip ink.
    CHECK_FALSE(any_pixel(img, series_rgb(1), 0, 0, kPngW - 1, kPngH - 1));
    int strip_ink = 0;
    for (int y = kMT + 1; y < kPngH - kMB; ++y) {
        for (int x = frame_r + 2; x < std::min(kPngW, frame_r + 60); ++x) {
            const std::uint32_t c = px(img, x, y);
            if (c != 0x1E2229u) {
                ++strip_ink;
            }
        }
    }
    CHECK(strip_ink == 0);
}

TEST_CASE("png: hiding the voltage column leaves the right spine alone") {
    // The visibility rule is PER SPINE: hiding v(out) must not flatten
    // the amps into the volts' window - the visible current column
    // still draws against its OWN fit, and the export still earns
    // its right axis (a visible current column exists).
    const ws::spice::Table t = ramp_table(64u);
    std::vector<std::uint8_t> vis{0u, 1u};  // v hidden, i shown
    const std::string png = editor_ns::render_plot_png(
        t, vis.data(), "time", nullptr, 0);
    const PngImage img = decode_png(png);

    // The right spine exists (the margin widened): the frame's right
    // edge is at 1280-72, so ink at x > 1280-72+2 does not exist
    // except the right label strip's own text.
    CHECK_FALSE(any_pixel(img, series_rgb(0), 0, 0, kPngW - 1, kPngH - 1));
    // The amps span a real band on their own spine (the flattening
    // regression again, now with the LEFT spine empty: the sane-empty
    // 0/1 window must not squeeze the right one). The detector is the
    // series' GLOBAL y extent (the sine's peaks live in different
    // columns).
    const double iw = kPngW - kML - kMRAxis;
    int lo = kPngH;
    int hi = 0;
    for (int y = kMT + 1; y < kPngH - kMB - 1; ++y) {
        for (int x = static_cast<int>(kML) + 2;
             x < static_cast<int>(kML + iw) - 2; ++x) {
            if (px(img, x, y) == series_rgb(1)) {
                lo = std::min(lo, y);
                hi = std::max(hi, y);
            }
        }
    }
    REQUIRE(lo <= hi);
    CHECK(hi - lo >= 25);
}

TEST_CASE("png: a current-only table rides the right spine alone") {
    ws::spice::Table t = ramp_table(64u);
    t.signals.erase(t.signals.begin());  // drop v(out)
    const std::string png =
        editor_ns::render_plot_png(t, nullptr, "time", nullptr, 0);
    const PngImage img = decode_png(png);
    // The sole series is an i(...) - it earns the right spine, and it
    // spans a real band (not flattened against a volts window). The
    // detector is the series' GLOBAL y extent.
    const double iw = kPngW - kML - kMRAxis;
    int lo = kPngH;
    int hi = 0;
    for (int y = kMT + 1; y < kPngH - kMB - 1; ++y) {
        for (int x = static_cast<int>(kML) + 2;
             x < static_cast<int>(kML + iw) - 2; ++x) {
            if (px(img, x, y) == series_rgb(0)) {
                lo = std::min(lo, y);
                hi = std::max(hi, y);
            }
        }
    }
    REQUIRE(lo <= hi);
    CHECK(hi - lo >= 25);
}

// --- the stepped-axis cases --------------------------------------------------

TEST_CASE("png: a .step family never chords its passes") {
    // The wrap shape: the axis rises 0..0.2 then walks back (two
    // passes). The ramp's y rides 0..5 on pass 1 (rising half) and
    // 5..0 on pass 2 (falling half) - as ONE polyline that chorded
    // the reset, the same v(out) values would still connect, so the
    // chord detector needs a y pattern that only a chord creates.
    //
    // The detector: the export must NOT draw ink AT THE SEAM COLUMN
    // beyond the passes' own values. The strongest assertion is on
    // the RAGGED family below (bands that never overlap); here the
    // assertion is structural - each pass spans the full frame width
    // (the union envelope), and the passes' ink at the seam shows the
    // break: the last sample of pass 1 and the first of pass 2 are
    // 10 rows apart (0.2 vs 0.0 on the axis - the reset), so a
    // continuous polyline would have a near-vertical segment AT the
    // seam column between the passes' last/first y's. With the break,
    // the seam column carries exactly the two pass-end values' ink
    // and NOTHING BETWEEN except where a pass itself crosses it.
    const ws::spice::Table t = step_family_table(4000u, 2u);
    const std::string png =
        editor_ns::render_plot_png(t, nullptr, "time", nullptr, 0);
    const PngImage img = decode_png(png);

    // Sanity: the series drew at all.
    CHECK(any_pixel(img, series_rgb(0), 0, kMT, kPngW - 1, kPngH - 1));
    // The envelope: both passes fit (x spans 0..0.1999), so ink
    // reaches both frame columns (a front/back clip would lose the
    // rising pass's far end).
    const double iw = kPngW - kML - kMR;
    CHECK(any_pixel(img, series_rgb(0), static_cast<int>(kML) + 1, kMT + 1,
                    static_cast<int>(kML + iw * 0.15), kPngH - kMB - 2));
}

TEST_CASE("png: a ragged family overlays without chording") {
    // THE chord regression: pass 1 rides y=1 (100 rows, x 0..0.1),
    // pass 2 rides y=-1 (200 rows, x 0..0.2). The two bands are a
    // full frame apart in y; the passes' own polylines are flat lines.
    // A polyline that CONNECTS the passes draws a diagonal from
    // (x=0.1's end, y=1) down to (x=0.0's start, y=-1) - and since
    // the passes share the x range 0..0.1, the chord crosses the
    // MIDDLE of the frame diagonally, through the empty band between
    // the two flat lines. The asserts: (a) the empty middle band
    // (y around the frame's center third, away from both flat lines)
    // carries no v(out) ink; (b) BOTH flat lines exist at their own
    // rows (the envelope fits both passes, not just the last one).
    const ws::spice::Table t = ragged_family_table();
    const std::string png =
        editor_ns::render_plot_png(t, nullptr, "time", nullptr, 0);
    const PngImage img = decode_png(png);

    // The fit: y in {-1, 1} -> expand_range(-1, 1) = [-1.1, 1.1]; the
    // two flat lines sit at sy(1) and sy(-1) = 5%/95% of the frame's
    // height. The empty middle band: the middle 50% of the frame
    // (rows 25%..75% of ih) carries no series ink between the lines.
    const double iw = kPngW - kML - kMR;
    const double ih = kPngH - kMT - kMB;
    const int y_line_hi = static_cast<int>(kMT + ih * 0.05);
    const int y_line_lo = static_cast<int>(kMT + ih * 0.95);
    const int y_mid_lo = static_cast<int>(kMT + ih * 0.25);
    const int y_mid_hi = static_cast<int>(kMT + ih * 0.75);
    // (b) both lines exist.
    CHECK(any_pixel(img, series_rgb(0), static_cast<int>(kML) + 1,
                    y_line_hi - 3, static_cast<int>(kML + iw) - 2,
                    y_line_hi + 3));
    CHECK(any_pixel(img, series_rgb(0), static_cast<int>(kML) + 1,
                    y_line_lo - 3, static_cast<int>(kML + iw) - 2,
                    y_line_lo + 3));
    // (a) the middle band is empty (a chord would cross it: from
    // (0.1, 1) to (0.0, -1) the diagonal passes through the center of
    // the frame - the middle band scan catches it).
    CHECK(count_pixel(img, series_rgb(0), static_cast<int>(kML) + 2,
                      y_mid_lo, static_cast<int>(kML + iw) - 3,
                      y_mid_hi) == 0);
    // The ENVELOPE: pass 2 reaches x=0.2 where pass 1 stops at 0.1 -
    // the union envelope spans 0..0.2, so ink reaches the frame's
    // right column (a front/back walk would clip pass 2's far end
    // only if it landed past the last sample - here the last sample
    // IS 0.2, so also check the LEFT column: both passes' x start at
    // 0, both must draw there - the flat lines each span the full
    // frame width).
    CHECK(any_pixel(img, series_rgb(0), static_cast<int>(kML) + 1,
                    y_line_lo - 3, static_cast<int>(kML + iw * 0.1),
                    y_line_lo + 3));
    CHECK(any_pixel(img, series_rgb(0),
                    static_cast<int>(kML + iw * 0.9), y_line_lo - 3,
                    static_cast<int>(kML + iw) - 2, y_line_lo + 3));
}

TEST_CASE("png: a descending sweep draws its own direction") {
    // A strictly DESCENDING axis (a .dc walking its source down): the
    // live plot draws the run in its own direction; the export maps
    // through sx so the pixels land where the DATA lives - the
    // high-x samples on the right, the low-x ones on the left, the
    // y values riding their own positions. A reversed (mirrored)
    // render would swap the ends.
    ws::spice::Table t;
    t.analysis = ws::spice::AnalysisKind::Dc;
    t.sweep = ws::spice::SweepKind::Swept;
    t.sweep_names = {"v-sweep"};
    t.sweep_axes.emplace_back();
    const std::size_t n = 200u;
    t.sweep_axes.back().reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        t.sweep_axes.back().push_back(
            1.0e-3 * static_cast<double>(n - 1u - i));  // 0.199..0
    }
    ws::spice::Signal out;
    out.name = "v(out)";
    out.data.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        // v rises with the sweep INDEX; the sweep's x falls with it -
        // so the LOW x end carries the LOW y, the HIGH x end the HIGH
        // y. (A mirrored draw would put low y at high x.)
        out.data.push_back(static_cast<double>(i) /
                           static_cast<double>(n - 1u));
    }
    t.signals.push_back(std::move(out));
    const std::string png =
        editor_ns::render_plot_png(t, nullptr, "v-sweep", nullptr, 0);
    const PngImage img = decode_png(png);

    // The fit: x in 0..0.199 padded; y in 0..1 padded. The table
    // spells y rising with the sweep INDEX while x falls with it -
    // so LOW x carries HIGH y and HIGH x carries LOW y: the line runs
    // top-left to bottom-right. Probe: the left tenth's ink sits
    // ABOVE the frame's mid row, the right tenth's BELOW it (a
    // mirrored render would swap the ends).
    const double iw = kPngW - kML - kMR;
    const double ih = kPngH - kMT - kMB;
    const int mid_row = static_cast<int>(kMT + ih * 0.5);
    // Left tenth: any ink above the mid row.
    CHECK(any_pixel(img, series_rgb(0), static_cast<int>(kML) + 2,
                    kMT + 2, static_cast<int>(kML + iw * 0.1),
                    mid_row - 2));
    // ...and none below the mid row in that tenth.
    CHECK_FALSE(any_pixel(img, series_rgb(0), static_cast<int>(kML) + 2,
                          mid_row + 2, static_cast<int>(kML + iw * 0.1),
                          kPngH - kMB - 2));
    // Right tenth: any ink below the mid row...
    CHECK(any_pixel(img, series_rgb(0),
                    static_cast<int>(kML + iw * 0.9), mid_row + 2,
                    static_cast<int>(kML + iw) - 2, kPngH - kMB - 2));
    // ...and none above it.
    CHECK_FALSE(any_pixel(img, series_rgb(0),
                          static_cast<int>(kML + iw * 0.9), kMT + 2,
                          static_cast<int>(kML + iw) - 2, mid_row - 2));
}

TEST_CASE("png: plateau and reversal axes draw as their runs") {
    // The axis: 0..3 rising, a plateau at 3 (REPEATED x values - equal
    // neighbours break the runs, so each repeated sample is its own
    // length-1 run), a reversal back to 0. The length-1 runs draw
    // nothing (a polyline needs two points - the LIVE plot's own
    // semantics for a single-sample run, which the export now
    // shares); what must draw is the rising and falling runs, and
    // what must NEVER appear is a chord across the middle band the
    // two flat runs leave empty (a one-polyline export would bridge
    // the reversal diagonally).
    ws::spice::Table t;
    t.analysis = ws::spice::AnalysisKind::Dc;
    t.sweep = ws::spice::SweepKind::Swept;
    t.sweep_names = {"v-sweep"};
    t.sweep_axes.emplace_back();
    ws::spice::Signal out;
    out.name = "v(out)";
    const auto push = [&](double x, double y) {
        t.sweep_axes.back().push_back(x);
        out.data.push_back(y);
    };
    for (int i = 0; i <= 30; ++i) {
        push(0.1 * i, 0.25);            // rise (y low band)
    }
    for (int i = 0; i < 10; ++i) {
        push(3.0, 0.75);                // plateau (repeated x's)
    }
    for (int i = 0; i <= 30; ++i) {
        push(3.0 - 0.1 * i, 0.25);      // reversal (falling)
    }
    t.signals.push_back(std::move(out));
    const std::string png =
        editor_ns::render_plot_png(t, nullptr, "v-sweep", nullptr, 0);
    const PngImage img = decode_png(png);

    const double iw = kPngW - kML - kMR;
    const double ih = kPngH - kMT - kMB;
    // The two drawable runs ride the y=0.25 band (the window fits
    // 0.25..0.75 padded -> 0.25 sits at 95% of the frame's height).
    const int y_lo_line = static_cast<int>(kMT + ih * 0.95);
    CHECK(any_pixel(img, series_rgb(0), static_cast<int>(kML) + 2,
                    y_lo_line - 5, static_cast<int>(kML + iw) - 3,
                    y_lo_line + 5));
    // The rise spans x 0..3 and the fall x 0..3 too (the union
    // envelope): both ends of the frame carry the low band's ink.
    CHECK(any_pixel(img, series_rgb(0), static_cast<int>(kML) + 2,
                    y_lo_line - 5, static_cast<int>(kML + iw * 0.08),
                    y_lo_line + 5));
    CHECK(any_pixel(img, series_rgb(0),
                    static_cast<int>(kML + iw * 0.92), y_lo_line - 5,
                    static_cast<int>(kML + iw) - 2, y_lo_line + 5));
    // The middle band (the 40%..75% rows, between the low band and
    // where the plateau's single samples would sit) carries NO
    // series ink: a chord from the rise's end (x=3, y=0.25) up to
    // the plateau's y=0.75 - or down from it to the fall - crosses
    // this band diagonally. The repeated-x runs being length-1, the
    // honest picture leaves the band empty.
    const int mid_lo = static_cast<int>(kMT + ih * 0.40);
    const int mid_hi = static_cast<int>(kMT + ih * 0.75);
    CHECK(count_pixel(img, series_rgb(0), static_cast<int>(kML) + 2, mid_lo,
                      static_cast<int>(kML + iw) - 3, mid_hi) == 0);
}

TEST_CASE("png: non-finite x and y samples break, never crash") {
    // NaN x's: the runs split around them; NaN y's: the polyline
    // breaks. Both must render (the finite spans) and the encode
    // must still answer a valid image.
    ws::spice::Table t;
    t.analysis = ws::spice::AnalysisKind::Tran;
    t.sweep = ws::spice::SweepKind::Time;
    t.sweep_names = {"time"};
    t.sweep_axes.emplace_back();
    ws::spice::Signal out;
    out.name = "v(out)";
    const double kNaN = std::nan("");
    for (int i = 0; i < 40; ++i) {
        const double x = i < 20 ? 0.1 * i : 0.1 * (i - 20);
        t.sweep_axes.back().push_back(i == 10 ? kNaN : x);
        out.data.push_back(i == 15 ? kNaN : (i < 20 ? 1.0 : -1.0));
    }
    t.signals.push_back(std::move(out));
    const std::string png =
        editor_ns::render_plot_png(t, nullptr, "time", nullptr, 0);
    const PngImage img = decode_png(png);
    REQUIRE(img.w == 1280);
    // The finite spans drew: both bands exist (before/after the NaN x
    // at i=10, before the NaN y at i=15).
    const double ih = kPngH - kMT - kMB;
    const int y_hi = static_cast<int>(kMT + ih * 0.05);
    const int y_lo = static_cast<int>(kMT + ih * 0.95);
    CHECK(any_pixel(img, series_rgb(0), static_cast<int>(kML) + 2,
                    y_hi - 4, kPngW / 2, y_hi + 4));
    CHECK(any_pixel(img, series_rgb(0), kPngW / 2, y_lo - 4,
                    static_cast<int>(kML + kPngW - kML - kMR) - 3,
                    y_lo + 4));
}

TEST_CASE("png: different pass lengths all fit") {
    // The ragged family's envelope case again, now as an explicit
    // x-extent check: the union envelope (0..0.2) puts pass 2's far
    // end at the frame's right edge; a front/back walk over the LAST
    // pass would answer the same 0..0.2 here (the last pass is the
    // long one) - the regression shape is pass 1 LONGER than pass 2,
    // where front/back clips pass 1. Build that.
    ws::spice::Table t;
    t.analysis = ws::spice::AnalysisKind::Tran;
    t.sweep = ws::spice::SweepKind::Time;
    t.sweep_names = {"time"};
    t.sweep_axes.emplace_back();
    // Pass 1: 200 rows over 0..0.2; pass 2: 100 rows over 0..0.1.
    for (std::size_t i = 0; i < 200u; ++i) {
        t.sweep_axes.back().push_back(1.0e-3 * static_cast<double>(i));
    }
    for (std::size_t i = 0; i < 100u; ++i) {
        t.sweep_axes.back().push_back(1.0e-3 * static_cast<double>(i));
    }
    ws::spice::Signal out;
    out.name = "v(out)";
    for (std::size_t i = 0; i < 300u; ++i) {
        // Both passes ride the same y shape (0..1 over each pass's
        // own span) - the asserts are on x extent only.
        const std::size_t k = i < 200u ? i : i - 200u;
        const std::size_t span = i < 200u ? 199u : 99u;
        out.data.push_back(static_cast<double>(k) /
                           static_cast<double>(span));
    }
    t.signals.push_back(std::move(out));
    const std::string png =
        editor_ns::render_plot_png(t, nullptr, "time", nullptr, 0);
    const PngImage img = decode_png(png);

    // The union envelope spans 0..0.2: pass 1's far end (x=0.199)
    // maps to the frame's right edge. A clipped fit (the last pass's
    // 0..0.1) would put x=0.199 OUTSIDE the window - invisible. The
    // ramp's high end (y=1) only exists at x~0.2, so: ink in the
    // frame's TOP-RIGHT corner region.
    const double iw = kPngW - kML - kMR;
    const double ih = kPngH - kMT - kMB;
    CHECK(any_pixel(img, series_rgb(0),
                    static_cast<int>(kML + iw * 0.9),
                    static_cast<int>(kMT + ih * 0.02),
                    static_cast<int>(kML + iw) - 2,
                    static_cast<int>(kMT + ih * 0.12)));
}

// --- the coordinate and range consistency cases ------------------------------

TEST_CASE("png: constant and degenerate signals still earn axes") {
    // A constant v(out)=2.5: expand_range's degenerate rule opens the
    // window around its magnitude (2.5 +/- 2.5*0.05 -> 2.375..2.625),
    // so the line draws mid-frame - NOT flattened to a border by the
    // old export's y_lo-=1/y_hi+=1 hack (which would have put a
    // constant 2.5 at the exact middle of a 1.5..3.5 window - same
    // y-row, so the visible regression is on the TICK LABELS: the
    // live plot labels 2.4/2.5/2.6; the old export labeled 2/3/4).
    // The pixel assert: the line sits at the frame's vertical center
    // (the padded 2.375..2.625 puts 2.5 dead center), and the x tick
    // label row below the frame carries the ladder's text.
    ws::spice::Table t;
    t.analysis = ws::spice::AnalysisKind::Tran;
    t.sweep = ws::spice::SweepKind::Time;
    t.sweep_names = {"time"};
    t.sweep_axes.emplace_back();
    ws::spice::Signal out;
    out.name = "v(out)";
    for (int i = 0; i < 50; ++i) {
        t.sweep_axes.back().push_back(0.1 * i);
        out.data.push_back(2.5);
    }
    t.signals.push_back(std::move(out));
    const std::string png =
        editor_ns::render_plot_png(t, nullptr, "time", nullptr, 0);
    const PngImage img = decode_png(png);
    const double iw = kPngW - kML - kMR;
    const double ih = kPngH - kMT - kMB;
    // The line at the center row (within a few px).
    const int mid = static_cast<int>(kMT + ih * 0.5);
    CHECK(any_pixel(img, series_rgb(0), static_cast<int>(kML) + 2,
                    mid - 3, static_cast<int>(kML + iw) - 3, mid + 3));
}

TEST_CASE("png: negative-only and positive-only ranges") {
    // Two columns: one all-negative volts, one all-positive amps -
    // each spine fits its own sign, the zero rail never appears
    // (zero is outside both windows), and the ink stays inside the
    // frame (no clamped-to-edge flattening).
    ws::spice::Table t;
    t.analysis = ws::spice::AnalysisKind::Tran;
    t.sweep = ws::spice::SweepKind::Time;
    t.sweep_names = {"time"};
    t.sweep_axes.emplace_back();
    ws::spice::Signal neg;
    neg.name = "v(out)";
    ws::spice::Signal pos;
    pos.name = "i(V1)";
    for (int i = 0; i < 60; ++i) {
        t.sweep_axes.back().push_back(0.1 * i);
        neg.data.push_back(-1.0 - 0.5 * (i / 30.0));
        pos.data.push_back(1.0e-3 + 0.5e-3 * (i / 30.0));
    }
    t.signals.push_back(std::move(neg));
    t.signals.push_back(std::move(pos));
    const std::string png =
        editor_ns::render_plot_png(t, nullptr, "time", nullptr, 0);
    const PngImage img = decode_png(png);
    // Both series drew, each within the frame's vertical middle (a
    // negative-only window still maps 0 to ABOVE the frame - the
    // padding keeps the ink inside).
    CHECK(any_pixel(img, series_rgb(0), static_cast<int>(kML) + 2,
                    kMT + 2, static_cast<int>(kPngW - kMRAxis) - 3,
                    kPngH - kMB - 3));
    CHECK(any_pixel(img, series_rgb(1), static_cast<int>(kML) + 2,
                    kMT + 2, static_cast<int>(kPngW - kMRAxis) - 3,
                    kPngH - kMB - 3));
}

TEST_CASE("png: the markers land at their samples' x") {
    // A marker planted at sample k: the accent line (0x3E7ACB) sits
    // at sx(x_at(k)) - the same map the series rides. With a ramp
    // axis 0..0.49 and the frame's known margins, the assert pins
    // the exact column.
    ws::spice::Table t = ramp_table(50u);
    t.signals.erase(t.signals.begin() + 1);
    const int k = 25;  // x = 0.25 of 0..0.49 -> the padded window's ~mid
    const std::string png =
        editor_ns::render_plot_png(t, nullptr, "time", &k, 1);
    const PngImage img = decode_png(png);

    // The fit: ramp_table's axis is 1e-4 * i over 50 samples ->
    // 0..0.0049, padded by expand_range's 5% to [-0.000245,
    // 0.005145]; the marker at k=25 sits at x=0.0025, dead center of
    // the padded window.
    const double iw = kPngW - kML - kMR;
    const double x_lo = -0.000245, x_hi = 0.005145;
    const double mx = 0.0025;
    const int expect = static_cast<int>(
        kML + (mx - x_lo) / (x_hi - x_lo) * iw);
    REQUIRE(expect > kML);
    REQUIRE(expect < kML + iw);
    // The marker's ink at that column: the raster's supersampled
    // coverage BLENDS the stroke into the background (a 1px line's
    // columns read part-marker, part-background), so the assert is a
    // near-match - the column at `expect` (within a few px for the
    // stroke's width and rounding) must read significantly blended
    // toward the marker color over MANY rows of the frame's height,
    // where far columns read pure background.
    const auto marker_ish = [&](int x) {
        // Count rows whose pixel is far enough from the background
        // toward 0x3E7ACB (the blend of bg 0x1E2229 with the marker at
        // >=25% coverage reads clearly shifted; demand a squared
        // distance the background itself never reaches).
        int rows = 0;
        for (int y = kMT + 2; y < kPngH - kMB - 2; ++y) {
            const std::uint32_t c = px(img, x, y);
            const int dr = static_cast<int>((c >> 16) & 0xFFu) - 0x1Eu;
            const int dg = static_cast<int>((c >> 8) & 0xFFu) - 0x22u;
            const int db = static_cast<int>(c & 0xFFu) - 0x29u;
            if (dr * dr + dg * dg + db * db > 20 * 20) {
                ++rows;
            }
        }
        return rows;
    };
    int best_rows = 0;
    for (int dx = -3; dx <= 3; ++dx) {
        best_rows = std::max(best_rows, marker_ish(expect + dx));
    }
    // A full-height marker line answers hundreds of blended rows
    // (the frame is ~632 tall); demand most of it.
    CHECK(best_rows >= 400);
    // And the far columns read the background: no marker ink there.
    CHECK(marker_ish(expect + 40) < 8);
    CHECK(marker_ish(expect - 40) < 8);
}

TEST_CASE("png: the engineering labels spell the magnitudes") {
    // A tiny-magnitude current (nA range): the right spine's tick
    // labels must exist as text ink (the format walk answers "2n"
    // etc.) - gated by pixel count in the right strip, NOT by
    // matching the glyph shapes (the raster font's ink is enough:
    // the strip must carry non-grid text pixels).
    ws::spice::Table t;
    t.analysis = ws::spice::AnalysisKind::Tran;
    t.sweep = ws::spice::SweepKind::Time;
    t.sweep_names = {"time"};
    t.sweep_axes.emplace_back();
    ws::spice::Signal cur;
    cur.name = "i(V1)";
    for (int i = 0; i < 40; ++i) {
        t.sweep_axes.back().push_back(0.1 * i);
        cur.data.push_back(2.0e-9 * std::sin(0.1 * i));
    }
    t.signals.push_back(std::move(cur));
    const std::string png =
        editor_ns::render_plot_png(t, nullptr, "time", nullptr, 0);
    const PngImage img = decode_png(png);
    const double iw = kPngW - kML - kMRAxis;
    const int frame_r = static_cast<int>(kML + iw);
    int strip_text = 0;
    for (int y = kMT + 1; y < kPngH - kMB; ++y) {
        for (int x = frame_r + 2; x < frame_r + 60; ++x) {
            const std::uint32_t c = px(img, x, y);
            if (c != 0x1E2229u && c != 0x2A2F38u) {
                ++strip_text;
            }
        }
    }
    CHECK(strip_text >= 40);  // labels drew
}

TEST_CASE("png: empty and no-visible tables still encode") {
    // No signals at all: the frame, grid and title still answer a
    // valid image (the sane-empty windows).
    ws::spice::Table t;
    t.analysis = ws::spice::AnalysisKind::Tran;
    t.sweep = ws::spice::SweepKind::Time;
    t.sweep_names = {"time"};
    t.sweep_axes.emplace_back();
    for (int i = 0; i < 10; ++i) {
        t.sweep_axes.back().push_back(0.1 * i);
    }
    const std::string png =
        editor_ns::render_plot_png(t, nullptr, "time", nullptr, 0);
    const PngImage img = decode_png(png);
    CHECK(img.w == 1280);
    CHECK(img.h == 720);

    // A table with signals but ALL hidden: same - the empty frame, no
    // series ink.
    ws::spice::Table t2 = ramp_table(20u);
    std::vector<std::uint8_t> vis{0u, 0u};
    const std::string png2 =
        editor_ns::render_plot_png(t2, vis.data(), "time", nullptr, 0);
    const PngImage img2 = decode_png(png2);
    CHECK_FALSE(any_pixel(img2, series_rgb(0), 0, 0, kPngW - 1, kPngH - 1));
    CHECK_FALSE(any_pixel(img2, series_rgb(1), 0, 0, kPngW - 1, kPngH - 1));
}

// --- the shared-math parity (the export's home == the live home) -------------

TEST_CASE("parity: the export's fit rides the live home math") {
    // The seams that feed the fit, computed through the same public
    // calls both consumers ride: the classifier and the envelope
    // walk. For the ragged family (the shape the old front/back walk
    // got wrong), the envelope must be the union of the passes.
    const ws::spice::Table t = ragged_family_table();
    const AxisRuns runs =
        ws::circsim::split_axis_runs(t.sweep_axes[0]);
    REQUIRE(runs.runs.size() == 2u);
    CHECK(runs.finite);
    // The union: pass 1 spans 0..0.1, pass 2 spans 0..0.2.
    CHECK(runs.lo == doctest::Approx(0.0));
    CHECK(runs.hi == doctest::Approx(0.199));
    // The padding both consumers apply:
    const PlotRange px = ws::circsim::expand_range(runs.lo, runs.hi);
    CHECK(px.lo == doctest::Approx(-0.00995));
    CHECK(px.hi == doctest::Approx(0.20895));

    // The classifier: the panel's SeriesStats.right and the export's
    // own right[] both answer signal_y_axis over the same names.
    CHECK(ws::circsim::signal_y_axis("v(out)") == PlotYAxis::Left);
    CHECK(ws::circsim::signal_y_axis("i(V1)") == PlotYAxis::Right);

    // The spine fits: for the mixed table, the left rides the volts'
    // envelope and the right the amps' - independent expand_range
    // calls, the same one the live plot's home uses.
    const ws::spice::Table mixed = ramp_table(64u);
    double v_lo = std::numeric_limits<double>::infinity();
    double v_hi = -std::numeric_limits<double>::infinity();
    double i_lo = std::numeric_limits<double>::infinity();
    double i_hi = -std::numeric_limits<double>::infinity();
    for (const ws::spice::Signal& s : mixed.signals) {
        for (double v : s.data) {
            if (!std::isfinite(v)) {
                continue;
            }
            if (ws::circsim::signal_y_axis(s.name) == PlotYAxis::Right) {
                i_lo = std::min(i_lo, v);
                i_hi = std::max(i_hi, v);
            } else {
                v_lo = std::min(v_lo, v);
                v_hi = std::max(v_hi, v);
            }
        }
    }
    const PlotRange volts = ws::circsim::expand_range(v_lo, v_hi);
    const PlotRange amps = ws::circsim::expand_range(i_lo, i_hi);
    // The amps' window is its OWN: a one-scale flatten over 0..5V
    // would bury the mA sine; the right spine answers a window of
    // millivolts-equivalents (span ~2mA against the volts' ~5V).
    CHECK(amps.hi - amps.lo < (volts.hi - volts.lo) * 0.01);
}
