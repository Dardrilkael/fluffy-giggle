// png.hpp's implementation: the RasterCanvas (4x4-supersampled
// coverage for the analytic shapes, 1 bpp blits for the font), the
// PNG/zlib writer (fixed-Huffman deflate with greedy LZ77 matches),
// and the schematic composition over the stage-17a render seam.

#include "png_font.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <ws/circsim/nets.hpp>
#include <ws/circsim/png.hpp>
#include <ws/circsim/render.hpp>

namespace ws::circsim {
namespace {

// The sheet's palette: svg_dump's ink plus the preview render's two
// text browns, so the PNG reads like the editor and the SVG dump.
constexpr std::uint32_t kInk = 0x1F2430u;        // symbols, wires, dots
constexpr std::uint32_t kPartText = 0x7A2600u;   // ref/value annotations
constexpr std::uint32_t kLabelText = 0xB05600u;  // net labels

constexpr double kMargin = 0.75;  // units of breathing room, svg_dump's
constexpr int kMinPpu = 4;
constexpr int kMaxPpu = 96;
constexpr int kMaxSide = 16384;  // px; the runaway guard's ceiling

constexpr double kDotR = 0.22;     // junction dot radius, grid units
constexpr double kAnnotDy = 2.8;   // annotation top above the origin
constexpr double kLabelDy = 0.55;  // label top below the anchor

// 4x4 subsample offsets inside a pixel.
constexpr double kSub[4] = {0.125, 0.375, 0.625, 0.875};

int clamp_ppu(int ppu) {
    return std::max(kMinPpu, std::min(kMaxPpu, ppu));
}

// Text scale grows with the resolution so annotations keep their
// proportion: round(ppu / 24), at least 1.
int text_scale(int ppu) {
    const int s = static_cast<int>(std::lround(ppu / 24.0));
    return std::max(1, s);
}

double seg_dist(double px, double py, double ax, double ay, double bx,
                double by) {
    const double dx = bx - ax;
    const double dy = by - ay;
    const double len2 = dx * dx + dy * dy;
    double t = 0.0;
    if (len2 > 0.0) {
        t = ((px - ax) * dx + (py - ay) * dy) / len2;
        t = std::max(0.0, std::min(1.0, t));
    }
    const double x = ax + t * dx - px;
    const double y = ay + t * dy - py;
    return std::sqrt(x * x + y * y);
}

// --- PNG plumbing: CRC32, Adler32, chunks, deflate --------------------

// Compile-time constant (constant-initialized, no lazy guard, no
// environment): threading.md's read-pass contract stays true.
constexpr std::array<std::uint32_t, 256> make_crc_table() {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256u; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) != 0u ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        t[i] = c;
    }
    return t;
}
constexpr std::array<std::uint32_t, 256> kCrcTable = make_crc_table();

std::uint32_t crc32(const std::uint8_t* data, std::size_t n) {
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) {
        c = kCrcTable[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

std::uint32_t adler32(const std::uint8_t* data, std::size_t n) {
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (std::size_t i = 0; i < n; ++i) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

void push_be32(std::string& out, std::uint32_t v) {
    out.push_back(static_cast<char>((v >> 24) & 0xFFu));
    out.push_back(static_cast<char>((v >> 16) & 0xFFu));
    out.push_back(static_cast<char>((v >> 8) & 0xFFu));
    out.push_back(static_cast<char>(v & 0xFFu));
}

// Deflate's bit stream is filled LSB first; Huffman codes go in MSB
// first, every other value LSB first (RFC 1951 s3.1.1's rule).
class BitWriter {
public:
    explicit BitWriter(std::string& out) : out_(out) {}

    void raw_bits(std::uint32_t v, int n) {  // a value, LSB first
        for (int i = 0; i < n; ++i) {
            bit((v >> i) & 1u);
        }
    }

    void code_bits(std::uint32_t v, int n) {  // a Huffman code, MSB first
        for (int i = n - 1; i >= 0; --i) {
            bit((v >> i) & 1u);
        }
    }

    void align() {
        while (used_ != 0) {
            bit(0);
        }
    }

private:
    void bit(std::uint32_t b) {
        cur_ |= (b & 1u) << used_;
        if (++used_ == 8) {
            out_.push_back(static_cast<char>(cur_));
            cur_ = 0;
            used_ = 0;
        }
    }

    std::string& out_;
    std::uint8_t cur_ = 0;
    int used_ = 0;
};

// Deflate, fixed Huffman + greedy LZ77: at each position the most
// recent same-3-gram position (within the 32K window) is tried, the
// match extended as far as it honestly reads (a distance-1 match is a
// run: comparing the bytes directly gives the RLE behavior), then a
// literal or a length/distance pair. Bounded by construction: fixed
// codes, no dynamic block, one block per stream.

// RFC 1951 s3.2.5: length symbols 257..285 (base + extra bits).
struct LenSym {
    std::uint16_t sym;
    std::uint16_t base;
    std::uint8_t extra;
};
constexpr LenSym kLens[29] = {
    {257, 3, 0},   {258, 4, 0},   {259, 5, 0},   {260, 6, 0},
    {261, 7, 0},   {262, 8, 0},   {263, 9, 0},   {264, 10, 0},
    {265, 11, 1},  {266, 13, 1},  {267, 15, 1},  {268, 17, 1},
    {269, 19, 2},  {270, 23, 2},  {271, 27, 2},  {272, 31, 2},
    {273, 35, 3},  {274, 43, 3},  {275, 51, 3},  {276, 59, 3},
    {277, 67, 4},  {278, 83, 4},  {279, 99, 4},  {280, 115, 4},
    {281, 131, 5}, {282, 163, 5}, {283, 195, 5}, {284, 227, 5},
    {285, 258, 0},
};

// RFC 1951 s3.2.5: distance symbols 0..29 (5-bit codes, base + extra).
struct DistSym {
    std::uint16_t code;
    std::uint16_t base;
    std::uint8_t extra;
};
constexpr DistSym kDists[30] = {
    {0, 1, 0},     {1, 2, 0},      {2, 3, 0},      {3, 4, 0},
    {4, 5, 1},     {5, 7, 1},      {6, 9, 2},      {7, 13, 2},
    {8, 17, 3},    {9, 25, 3},     {10, 33, 4},    {11, 49, 4},
    {12, 65, 5},   {13, 97, 5},    {14, 129, 6},   {15, 193, 6},
    {16, 257, 7},  {17, 385, 7},   {18, 513, 8},   {19, 769, 8},
    {20, 1025, 9}, {21, 1537, 9},  {22, 2049, 10}, {23, 3073, 10},
    {24, 4097, 11}, {25, 6145, 11}, {26, 8193, 12}, {27, 12289, 12},
    {28, 16385, 13}, {29, 24577, 13},
};

// The 3-gram index: 2^20 flat open-addressing slots (8 MB, fixed),
// most-recent-position-wins. An approximate index is always safe - a
// stale candidate can only be skipped, never trusted past the 32K
// window check.
class ThreeGramIndex {
public:
    ThreeGramIndex()
        : keys_(kSlots, 0u), vals_(kSlots, 0u) {}  // val 0 = empty

    // Returns the stored position, or npos when the gram is unknown.
    [[nodiscard]] std::size_t find(std::uint32_t g) const {
        std::size_t i = spread(g) & (kSlots - 1u);
        while (true) {
            const std::uint32_t v = vals_[i];
            if (v == 0u) {
                return kNpos;
            }
            if (keys_[i] == g) {
                return v - 1u;
            }
            i = (i + 1u) & (kSlots - 1u);
        }
    }

    void insert(std::uint32_t g, std::size_t pos) {
        std::size_t i = spread(g) & (kSlots - 1u);
        while (true) {
            if (vals_[i] == 0u || keys_[i] == g) {
                keys_[i] = g;
                vals_[i] = static_cast<std::uint32_t>(pos + 1u);
                return;
            }
            i = (i + 1u) & (kSlots - 1u);
        }
    }

    static constexpr std::size_t kNpos = static_cast<std::size_t>(-1);

private:
    static constexpr std::size_t kSlots = 1u << 20;

    // A cheap avalanche for byte-triple keys (fmix32's shape).
    [[nodiscard]] static std::uint32_t spread(std::uint32_t x) {
        x ^= x >> 16;
        x *= 0x85EBCA6Bu;
        x ^= x >> 13;
        x *= 0xC2B2AE35u;
        x ^= x >> 16;
        return x;
    }

    std::vector<std::uint32_t> keys_;
    std::vector<std::uint32_t> vals_;  // position + 1; 0 = empty
};

std::string deflate_fixed(const std::uint8_t* data, std::size_t n) {
    std::string out;
    out.reserve(n / 4u + 64u);
    BitWriter w(out);
    w.raw_bits(1, 1);  // BFINAL = 1
    w.raw_bits(1, 2);  // BTYPE = 01 (fixed Huffman)

    const auto gram = [&](std::size_t p) {
        return (static_cast<std::uint32_t>(data[p]) << 16) |
               (static_cast<std::uint32_t>(data[p + 1]) << 8) |
               static_cast<std::uint32_t>(data[p + 2]);
    };
    const auto emit_length = [&](std::size_t m) {
        for (int e = 28; e >= 0; --e) {
            if (m >= kLens[e].base) {
                const std::uint16_t sym = kLens[e].sym;
                if (sym < 280u) {
                    w.code_bits(sym - 256u, 7);  // 257..279
                } else {
                    w.code_bits(0xC0u + (sym - 280u), 8);  // 280..285
                }
                if (kLens[e].extra != 0) {
                    w.raw_bits(static_cast<std::uint32_t>(m - kLens[e].base),
                               kLens[e].extra);
                }
                return;
            }
        }
    };
    const auto emit_distance = [&](std::size_t d) {
        for (int e = 29; e >= 0; --e) {
            if (d >= kDists[e].base) {
                w.code_bits(kDists[e].code, 5);
                if (kDists[e].extra != 0) {
                    w.raw_bits(static_cast<std::uint32_t>(d - kDists[e].base),
                               kDists[e].extra);
                }
                return;
            }
        }
    };

    ThreeGramIndex index;
    std::size_t i = 0;
    while (i < n) {
        std::size_t best_len = 0;
        std::size_t best_j = ThreeGramIndex::kNpos;
        if (i + 3 <= n) {
            const std::uint32_t g = gram(i);
            const std::size_t j = index.find(g);
            if (j != ThreeGramIndex::kNpos && i - j <= 32768u) {
                const std::size_t maxm =
                    std::min<std::size_t>(258u, n - i);
                std::size_t m = 0;
                while (m < maxm && data[j + m] == data[i + m]) {
                    ++m;
                }
                if (m >= 3u) {
                    best_len = m;
                    best_j = j;
                }
            }
        }
        if (best_len >= 3u) {
            emit_length(best_len);
            emit_distance(i - best_j);
            // Every gram the match covered is a future candidate.
            const std::size_t stop = std::min(i + best_len, n - 2u);
            for (std::size_t p = i; p < stop; ++p) {
                index.insert(gram(p), p);
            }
            i += best_len;
        } else {
            const std::uint8_t b = data[i];
            if (b < 144u) {
                w.code_bits(0x30u + b, 8);  // literals 0..143
            } else {
                w.code_bits(0x190u + (b - 144u), 9);  // 144..255
            }
            if (i + 3 <= n) {
                index.insert(gram(i), i);
            }
            ++i;
        }
    }
    w.code_bits(0, 7);  // end of block (256)
    w.align();
    return out;
}

void push_chunk(std::string& out, const char type[4],
                const std::uint8_t* data, std::size_t n) {
    push_be32(out, static_cast<std::uint32_t>(n));
    std::string crc_input;
    crc_input.append(type, 4);
    if (n > 0u) {
        crc_input.append(reinterpret_cast<const char*>(data), n);
    }
    out.append(type, 4);
    if (n > 0u) {
        out.append(reinterpret_cast<const char*>(data), n);
    }
    push_be32(out, crc32(reinterpret_cast<const std::uint8_t*>(
                             crc_input.data()),
                         crc_input.size()));
}

// --- the composition's content bounds ----------------------------------

struct SheetBounds {
    bool empty = true;
    double min_x = 0.0, min_y = 0.0, max_x = 0.0, max_y = 0.0;

    void add(double x, double y) {
        if (empty) {
            min_x = max_x = x;
            min_y = max_y = y;
            empty = false;
            return;
        }
        min_x = std::min(min_x, x);
        min_y = std::min(min_y, y);
        max_x = std::max(max_x, x);
        max_y = std::max(max_y, y);
    }

    void add_box(double x0, double y0, double x1, double y1) {
        add(x0, y0);
        add(x1, y1);
    }

    void grow(double g) {
        if (!empty) {
            min_x -= g;
            min_y -= g;
            max_x += g;
            max_y += g;
        }
    }
};

// The part's placed picture: symbol_bounds' corners through the one
// shared transform (locate.cpp's focus_bounds does the same walk).
SheetBounds placed_bounds(const SymbolDef& sym, const Part& p) {
    const Bounds b = symbol_bounds(sym);
    SheetBounds out;
    const Pos corners[4] = {Pos{b.min_x, b.min_y}, Pos{b.min_x, b.max_y},
                             Pos{b.max_x, b.min_y}, Pos{b.max_x, b.max_y}};
    for (const Pos& c : corners) {
        const Pos w = transform_pos(p, c);
        out.add(w.x, w.y);
    }
    return out;
}

// ref value params, one line, two spaces between tokens.
std::string annotation_text(const SymbolDef& sym, const Part& p) {
    if (sym.spice_letter == 0) {
        return {};  // power/ground symbols never annotate
    }
    std::string out;
    const auto token = [&out](const std::string& t) {
        if (t.empty() || out.empty()) {
            out += t;
        } else {
            out += "  " + t;
        }
    };
    token(p.ref.empty() ? p.symbol : p.ref);
    token(p.value);
    for (const auto& [k, v] : p.params) {
        if (!v.empty()) {
            token(k + "=" + v);
        }
    }
    return out;
}

}  // namespace

// --- RasterCanvas ---------------------------------------------------------

RasterCanvas::RasterCanvas(int width, int height, std::uint32_t background)
    : w_(width), h_(height), bg_(background) {
    rgb_.assign(static_cast<std::size_t>(w_) * h_ * 3u, 0);
    for (int y = 0; y < h_; ++y) {
        for (int x = 0; x < w_; ++x) {
            blend(x, y, bg_, 1.0);
        }
    }
}

std::uint32_t RasterCanvas::pixel(int x, int y) const {
    if (x < 0 || y < 0 || x >= w_ || y >= h_) {
        return bg_;
    }
    const std::size_t i = (static_cast<std::size_t>(y) * w_ + x) * 3u;
    return (static_cast<std::uint32_t>(rgb_[i]) << 16) |
           (static_cast<std::uint32_t>(rgb_[i + 1]) << 8) |
           static_cast<std::uint32_t>(rgb_[i + 2]);
}

void RasterCanvas::blend(int x, int y, std::uint32_t color, double coverage) {
    if (coverage <= 0.0 || x < 0 || y < 0 || x >= w_ || y >= h_) {
        return;
    }
    if (coverage > 1.0) {
        coverage = 1.0;
    }
    const std::size_t i = (static_cast<std::size_t>(y) * w_ + x) * 3u;
    const double keep = 1.0 - coverage;
    const double cr = static_cast<double>((color >> 16) & 0xFFu);
    const double cg = static_cast<double>((color >> 8) & 0xFFu);
    const double cb = static_cast<double>(color & 0xFFu);
    rgb_[i] = static_cast<std::uint8_t>(
        std::lround(rgb_[i] * keep + cr * coverage));
    rgb_[i + 1] = static_cast<std::uint8_t>(
        std::lround(rgb_[i + 1] * keep + cg * coverage));
    rgb_[i + 2] = static_cast<std::uint8_t>(
        std::lround(rgb_[i + 2] * keep + cb * coverage));
}

void RasterCanvas::stroke_polyline(
    const std::vector<std::pair<double, double>>& pts, double width_px,
    std::uint32_t color) {
    if (pts.size() < 2u || width_px <= 0.0) {
        return;
    }
    const double r = width_px / 2.0;
    // One entry per touched pixel: which of its 16 subsamples the path
    // covers - overlaps inside this call collapse before blending.
    std::map<std::uint64_t, std::uint16_t> hits;
    for (std::size_t s = 0; s + 1 < pts.size(); ++s) {
        const auto [ax, ay] = pts[s];
        const auto [bx, by] = pts[s + 1];
        const int x0 = static_cast<int>(std::floor(
                           std::min(ax, bx) - r - 1.0));
        const int x1 = static_cast<int>(std::ceil(std::max(ax, bx) + r + 1.0));
        const int y0 = static_cast<int>(std::floor(
                           std::min(ay, by) - r - 1.0));
        const int y1 = static_cast<int>(std::ceil(std::max(ay, by) + r + 1.0));
        for (int py = y0; py <= y1; ++py) {
            for (int px = x0; px <= x1; ++px) {
                for (int oy = 0; oy < 4; ++oy) {
                    for (int ox = 0; ox < 4; ++ox) {
                        const double sx = px + kSub[ox];
                        const double sy = py + kSub[oy];
                        if (seg_dist(sx, sy, ax, ay, bx, by) <= r) {
                            hits[static_cast<std::uint64_t>(py) *
                                     static_cast<std::uint64_t>(w_) +
                                 static_cast<std::uint64_t>(px)] |=
                                static_cast<std::uint16_t>(1u << (oy * 4 + ox));
                        }
                    }
                }
            }
        }
    }
    for (const auto& [key, bits] : hits) {
        const int px = static_cast<int>(key % static_cast<std::uint64_t>(w_));
        const int py = static_cast<int>(key / static_cast<std::uint64_t>(w_));
        const double coverage =
            static_cast<double>(std::popcount(bits)) / 16.0;
        blend(px, py, color, coverage);
    }
}

void RasterCanvas::fill_disc(double x, double y, double r_px,
                             std::uint32_t color) {
    if (r_px <= 0.0) {
        return;
    }
    const int x0 = static_cast<int>(std::floor(x - r_px - 1.0));
    const int x1 = static_cast<int>(std::ceil(x + r_px + 1.0));
    const int y0 = static_cast<int>(std::floor(y - r_px - 1.0));
    const int y1 = static_cast<int>(std::ceil(y + r_px + 1.0));
    for (int py = y0; py <= y1; ++py) {
        for (int px = x0; px <= x1; ++px) {
            int hits = 0;
            for (int oy = 0; oy < 4; ++oy) {
                for (int ox = 0; ox < 4; ++ox) {
                    const double dx = px + kSub[ox] - x;
                    const double dy = py + kSub[oy] - y;
                    if (dx * dx + dy * dy <= r_px * r_px) {
                        ++hits;
                    }
                }
            }
            if (hits > 0) {
                blend(px, py, color, hits / 16.0);
            }
        }
    }
}

void RasterCanvas::fill_polygon(
    const std::vector<std::pair<double, double>>& pts, std::uint32_t color) {
    if (pts.size() < 3u) {
        return;
    }
    double min_x = pts[0].first, max_x = pts[0].first;
    double min_y = pts[0].second, max_y = pts[0].second;
    for (const auto& [x, y] : pts) {
        min_x = std::min(min_x, x);
        max_x = std::max(max_x, x);
        min_y = std::min(min_y, y);
        max_y = std::max(max_y, y);
    }
    const int x0 = static_cast<int>(std::floor(min_x)) - 1;
    const int x1 = static_cast<int>(std::ceil(max_x)) + 1;
    const int y0 = static_cast<int>(std::floor(min_y)) - 1;
    const int y1 = static_cast<int>(std::ceil(max_y)) + 1;
    const std::size_t n = pts.size();
    for (int py = y0; py <= y1; ++py) {
        for (int px = x0; px <= x1; ++px) {
            int hits = 0;
            for (int oy = 0; oy < 4; ++oy) {
                for (int ox = 0; ox < 4; ++ox) {
                    const double sx = px + kSub[ox];
                    const double sy = py + kSub[oy];
                    bool inside = false;
                    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
                        const double yi = pts[i].second;
                        const double yj = pts[j].second;
                        if ((yi > sy) == (yj > sy)) {
                            continue;
                        }
                        const double xi = pts[i].first;
                        const double xj = pts[j].first;
                        const double cross = xi + (sy - yi) * (xj - xi) / (yj - yi);
                        if (sx < cross) {
                            inside = !inside;
                        }
                    }
                    if (inside) {
                        ++hits;
                    }
                }
            }
            if (hits > 0) {
                blend(px, py, color, hits / 16.0);
            }
        }
    }
}

double RasterCanvas::text_width(std::string_view text, int scale) {
    double w = 0.0;
    for (const char c : text) {
        const PngGlyph* g = png_glyph(static_cast<unsigned char>(c));
        if (g != nullptr) {
            w += static_cast<double>(g->advance) * scale;
        }
    }
    return w;
}

void RasterCanvas::draw_text(std::string_view text, double x, double y,
                             HAlign ha, VAlign va, std::uint32_t color,
                             int scale) {
    if (scale < 1) {
        scale = 1;
    }
    const double total_w = text_width(text, scale);
    const double total_h = static_cast<double>(kTextRows * scale);
    double left = x;
    switch (ha) {
        case HAlign::Center: left = x - total_w / 2.0; break;
        case HAlign::Right: left = x - total_w; break;
        case HAlign::Left: break;
    }
    double top = y;
    switch (va) {
        case VAlign::Middle: top = y - total_h / 2.0; break;
        case VAlign::Bottom: top = y - total_h; break;
        case VAlign::Top: break;
    }
    int gx = static_cast<int>(std::lround(left));
    const int gy = static_cast<int>(std::lround(top));
    for (const char c : text) {
        const PngGlyph* g = png_glyph(static_cast<unsigned char>(c));
        if (g == nullptr) {
            continue;  // non-ASCII: no ink, no advance
        }
        for (int r = 0; r < kTextRows; ++r) {
            const std::uint16_t bits = g->rows[static_cast<std::size_t>(r)];
            if (bits == 0u) {
                continue;
            }
            for (int col = 0; col < 16; ++col) {
                if ((bits & (1u << (15 - col))) == 0u) {
                    continue;
                }
                for (int ry = 0; ry < scale; ++ry) {
                    for (int rx = 0; rx < scale; ++rx) {
                        const int px = gx + col * scale + rx;
                        const int py = gy + r * scale + ry;
                        blend(px, py, color, 1.0);
                    }
                }
            }
        }
        gx += static_cast<int>(g->advance) * scale;
    }
}

std::string RasterCanvas::encode_png() const {
    // The raw stream: filter-0 scanlines over the RGB bytes.
    const std::size_t row_bytes = 1u + static_cast<std::size_t>(w_) * 3u;
    std::vector<std::uint8_t> raw(row_bytes * h_);
    for (int y = 0; y < h_; ++y) {
        const std::size_t ro = static_cast<std::size_t>(y) * row_bytes;
        raw[ro] = 0u;  // filter type None
        std::memcpy(&raw[ro + 1u], &rgb_[static_cast<std::size_t>(y) * w_ * 3u],
                    static_cast<std::size_t>(w_) * 3u);
    }

    // zlib: 0x78 0x01 header, the deflate block, Adler32 (BE) of raw.
    std::string z;
    z.push_back(static_cast<char>(0x78));
    z.push_back(static_cast<char>(0x01));
    z += deflate_fixed(raw.data(), raw.size());
    push_be32(z, adler32(raw.data(), raw.size()));

    std::string out;
    out.reserve(z.size() + 64u);
    out.append("\x89PNG\r\n\x1a\n", 8);
    std::uint8_t ihdr[13];
    const auto be = [](std::uint8_t* d, std::uint32_t v) {
        d[0] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
        d[1] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
        d[2] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
        d[3] = static_cast<std::uint8_t>(v & 0xFFu);
    };
    be(ihdr, static_cast<std::uint32_t>(w_));
    be(ihdr + 4, static_cast<std::uint32_t>(h_));
    ihdr[8] = 8u;   // bit depth
    ihdr[9] = 2u;   // color type: truecolor RGB
    ihdr[10] = 0u;  // compression: deflate
    ihdr[11] = 0u;  // filter method
    ihdr[12] = 0u;  // no interlace
    push_chunk(out, "IHDR", ihdr, sizeof(ihdr));
    push_chunk(out, "IDAT", reinterpret_cast<const std::uint8_t*>(z.data()),
               z.size());
    push_chunk(out, "IEND", nullptr, 0);
    return out;
}

// --- render_schematic_png ------------------------------------------------

std::string render_schematic_png(const Schematic& s,
                                 const SymbolLibrary* symbols,
                                 const PngOptions& opts) {
    const SymbolLibrary& lib =
        symbols != nullptr ? *symbols : builtin_library();

    int ppu = clamp_ppu(opts.ppu);
    const int ts = text_scale(ppu);

    // Resolve everything drawable once; parts whose symbol is unknown
    // (or wires whose endpoints do not resolve) draw nothing - erc and
    // build_nets report those, the raster never guesses.
    struct PartDraw {
        const Part* part;
        const SymbolDef* sym;
        std::string annot;
        SheetBounds bounds;
    };
    std::vector<PartDraw> parts;
    parts.reserve(s.parts().size());
    SheetBounds all;
    for (const Part& p : s.parts()) {
        const SymbolDef* sym = lib.find(p.symbol);
        if (sym == nullptr) {
            continue;
        }
        PartDraw d;
        d.part = &p;
        d.sym = sym;
        d.annot = annotation_text(*sym, p);
        d.bounds = placed_bounds(*sym, p);
        // Stubs from the safety net can poke half a unit past
        // symbol_bounds; the stroke's own width adds another half.
        d.bounds.grow(0.5);
        all.add_box(d.bounds.min_x, d.bounds.min_y, d.bounds.max_x,
                    d.bounds.max_y);
        if (!d.annot.empty()) {
            const double w = RasterCanvas::text_width(
                                  d.annot, ts) / ppu;
            const double h = static_cast<double>(RasterCanvas::kTextRows * ts) /
                              ppu;
            all.add_box(p.pos.x - w / 2.0 - 0.1, p.pos.y + kAnnotDy,
                        p.pos.x + w / 2.0 + 0.1, p.pos.y + kAnnotDy + h);
        }
        parts.push_back(std::move(d));
    }

    struct WireDraw {
        std::vector<std::pair<double, double>> pts;
    };
    std::vector<WireDraw> wires;
    wires.reserve(s.wires().size());
    for (const Wire& w : s.wires()) {
        std::vector<std::pair<double, double>> pts;
        const auto resolve = [&](const WireEndpoint& e) {
            if (const PinRef* pin = std::get_if<PinRef>(&e)) {
                const Part* part = s.find_part(pin->part);
                if (part == nullptr) {
                    return false;
                }
                const SymbolDef* sym = lib.find(part->symbol);
                if (sym == nullptr ||
                    pin->pin >= sym->pins.size()) {
                    return false;
                }
                const Pos at = pin_pos(*sym, *part, pin->pin);
                pts.emplace_back(at.x, at.y);
                return true;
            }
            const JunctionRef* j = std::get_if<JunctionRef>(&e);
            const Junction* found = j != nullptr ? s.find_junction(j->junction)
                                                 : nullptr;
            if (found == nullptr) {
                return false;
            }
            pts.emplace_back(found->pos.x, found->pos.y);
            return true;
        };
        if (!resolve(w.a) || !resolve(w.b)) {
            continue;
        }
        // Waypoints ride between the endpoints, in order.
        std::vector<std::pair<double, double>> path;
        path.reserve(pts.size() + w.waypoints.size());
        path.push_back(pts[0]);
        for (const Pos& wp : w.waypoints) {
            path.emplace_back(wp.x, wp.y);
        }
        path.push_back(pts[1]);
        for (const auto& [x, y] : path) {
            all.add(x, y);
        }
        wires.push_back(WireDraw{std::move(path)});
    }

    for (const Junction& j : s.junctions()) {
        all.add(j.pos.x, j.pos.y);
    }

    // The auto-junction dots build_nets reports (nets.hpp's SilentJoin):
    // the editor paints them; so does the raster.
    const NetBuild built = build_nets(s, symbols);
    for (const SilentJoin& sj : built.silent_joins) {
        all.add(sj.pos.x, sj.pos.y);
    }

    for (const Label& l : s.labels()) {
        const double w = RasterCanvas::text_width(l.name, ts) / ppu;
        const double h = static_cast<double>(RasterCanvas::kTextRows * ts) /
                          ppu;
        all.add_box(l.pos.x - w / 2.0 - 0.1, l.pos.y - kLabelDy - h,
                    l.pos.x + w / 2.0 + 0.1, l.pos.y - kLabelDy);
    }

    if (all.empty) {
        all.add_box(0.0, 0.0, 1.0, 1.0);  // a blank 1x1-unit sheet
    }

    // Fit the canvas; halve ppu (floor kMinPpu) if a side exceeds
    // kMaxSide - a schematic runaway must not ask for a monster. Text
    // extents in the bounds above rode the incoming ppu; if the guard
    // halves it, the drawn text is narrower still - the sheet just
    // breathes a little more. Never clipped either way.
    auto size_px = [&](int p, int* w_out, int* h_out) {
        const double margin = kMargin * p;
        *w_out = static_cast<int>(std::ceil((all.max_x - all.min_x) * p)) +
                 2 * static_cast<int>(std::lround(margin));
        *h_out = static_cast<int>(std::ceil((all.max_y - all.min_y) * p)) +
                 2 * static_cast<int>(std::lround(margin));
    };
    int cw = 0;
    int ch = 0;
    size_px(ppu, &cw, &ch);
    while ((cw > kMaxSide || ch > kMaxSide) && ppu > kMinPpu) {
        ppu = std::max(kMinPpu, ppu / 2);
        size_px(ppu, &cw, &ch);
    }
    // The text scale rode the original ppu; redo it on the final one.
    const int fts = text_scale(ppu);

    RasterCanvas canvas(cw, ch, 0xFFFFFFu);
    const double margin = kMargin * ppu;
    const auto px = [&](double x) {
        return margin + (x - all.min_x) * ppu;
    };
    const auto py = [&](double y) {
        return margin + (all.max_y - y) * ppu;
    };

    // 1. the parts' pictures (the stage-17a seam), ink.
    const double line_w = std::max(2.0, ppu / 12.0);
    for (const PartDraw& d : parts) {
        const std::vector<SymbolStroke> strokes =
            render_symbol(*d.sym, *d.part);
        for (const SymbolStroke& st : strokes) {
            switch (st.kind) {
                case SymbolStroke::Kind::Line:
                case SymbolStroke::Kind::Poly: {
                    std::vector<std::pair<double, double>> path;
                    path.reserve(st.pts.size());
                    for (const Pos& p : st.pts) {
                        path.emplace_back(px(p.x), py(p.y));
                    }
                    if (st.closed && !path.empty()) {
                        path.push_back(path.front());
                    }
                    canvas.stroke_polyline(path, line_w, kInk);
                    break;
                }
                case SymbolStroke::Kind::PolyFill: {
                    std::vector<std::pair<double, double>> path;
                    path.reserve(st.pts.size());
                    for (const Pos& p : st.pts) {
                        path.emplace_back(px(p.x), py(p.y));
                    }
                    canvas.fill_polygon(path, kInk);
                    break;
                }
                case SymbolStroke::Kind::Circle: {
                    // Flattened like svg_dump flattens arcs; 72
                    // segments keeps the sag under the stroke width.
                    std::vector<std::pair<double, double>> path;
                    constexpr double kRad = 3.14159265358979323846 / 180.0;
                    constexpr int kSteps = 72;
                    const double cx = px(st.pts[0].x);
                    const double cy = py(st.pts[0].y);
                    for (int i = 0; i <= kSteps; ++i) {
                        const double a =
                            360.0 * static_cast<double>(i) / kSteps * kRad;
                        path.emplace_back(cx + st.r * ppu * std::cos(a),
                                          cy + st.r * ppu * std::sin(a));
                    }
                    canvas.stroke_polyline(path, line_w, kInk);
                    break;
                }
                case SymbolStroke::Kind::Arc: {
                    const double span = st.a1 - st.a0;
                    const int steps = std::max(
                        2, static_cast<int>(std::ceil(std::fabs(span) / 90.0)) *
                               16);
                    constexpr double kRad = 3.14159265358979323846 / 180.0;
                    std::vector<std::pair<double, double>> path;
                    const double cx = px(st.pts[0].x);
                    const double cy = py(st.pts[0].y);
                    for (int i = 0; i <= steps; ++i) {
                        const double a =
                            (st.a0 + span * static_cast<double>(i) / steps) *
                            kRad;
                        path.emplace_back(cx + st.r * ppu * std::cos(a),
                                          cy + st.r * ppu * std::sin(a));
                    }
                    canvas.stroke_polyline(path, line_w, kInk);
                    break;
                }
                case SymbolStroke::Kind::Dot:
                    canvas.fill_disc(px(st.pts[0].x), py(st.pts[0].y),
                                     st.r * ppu, kInk);
                    break;
                case SymbolStroke::Kind::Text:
                    canvas.draw_text(st.text, px(st.pts[0].x),
                                     py(st.pts[0].y),
                                     RasterCanvas::HAlign::Center,
                                     RasterCanvas::VAlign::Middle, kInk, fts);
                    break;
            }
        }
    }

    // 2. the wires, ink, over the parts like the editor paints them.
    for (const WireDraw& w : wires) {
        std::vector<std::pair<double, double>> path;
        path.reserve(w.pts.size());
        for (const auto& [x, y] : w.pts) {
            path.emplace_back(px(x), py(y));
        }
        canvas.stroke_polyline(path, line_w, kInk);
    }

    // 3. junction and silent-join dots.
    for (const Junction& j : s.junctions()) {
        canvas.fill_disc(px(j.pos.x), py(j.pos.y), kDotR * ppu, kInk);
    }
    for (const SilentJoin& sj : built.silent_joins) {
        canvas.fill_disc(px(sj.pos.x), py(sj.pos.y), kDotR * ppu, kInk);
    }

    // 4. part annotations: one line above the origin.
    for (const PartDraw& d : parts) {
        if (d.annot.empty()) {
            continue;
        }
        canvas.draw_text(d.annot, px(d.part->pos.x),
                         py(d.part->pos.y + kAnnotDy),
                         RasterCanvas::HAlign::Center,
                         RasterCanvas::VAlign::Top, kPartText, fts);
    }

    // 5. net labels: under the anchor.
    for (const Label& l : s.labels()) {
        canvas.draw_text(l.name, px(l.pos.x), py(l.pos.y - kLabelDy),
                         RasterCanvas::HAlign::Center,
                         RasterCanvas::VAlign::Top, kLabelText, fts);
    }

    return canvas.encode_png();
}

}  // namespace ws::circsim
