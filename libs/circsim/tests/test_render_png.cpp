// png.hpp's tests: the PNG bytes' structure (signature, chunks, CRCs,
// the zlib wrapper's Adler32 over exactly the filter-0 scanlines), the
// RasterCanvas' coverage math (line rows, disc edges at half coverage,
// even-odd holes, the font's advance walk), and the composition's
// contract (determinism, ppu clamps, unknown symbols and dangling
// wires drawing nothing, the empty sheet). The Huffman bits inside
// IDAT are verified end to end by the Python smoke suite, which
// inflates them with zlib - the one decoder available to us.

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include <ws/circsim/png.hpp>

using ws::circsim::Part;
using ws::circsim::PinRef;
using ws::circsim::PngOptions;
using ws::circsim::RasterCanvas;
using ws::circsim::Schematic;
using ws::circsim::Wire;
using ws::circsim::render_schematic_png;

namespace {

constexpr std::uint32_t kInk = 0x1F2430u;

// An independent CRC32 (the test must not reuse the writer's).
std::uint32_t crc32_test(const std::uint8_t* d, std::size_t n) {
    std::uint32_t table[256];
    for (std::uint32_t i = 0; i < 256u; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) != 0u ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        table[i] = c;
    }
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) {
        c = table[(c ^ d[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

std::uint32_t adler32_test(const std::uint8_t* d, std::size_t n) {
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (std::size_t i = 0; i < n; ++i) {
        a = (a + d[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

std::uint32_t be32(const std::uint8_t* d) {
    return (static_cast<std::uint32_t>(d[0]) << 24) |
           (static_cast<std::uint32_t>(d[1]) << 16) |
           (static_cast<std::uint32_t>(d[2]) << 8) |
           static_cast<std::uint32_t>(d[3]);
}

struct Chunk {
    std::string type;
    std::vector<std::uint8_t> data;
    std::uint32_t crc = 0;
};

std::vector<Chunk> parse_chunks(const std::string& png) {
    std::vector<Chunk> out;
    std::size_t i = 8;  // past the signature
    while (i + 8u <= png.size()) {
        const std::uint8_t* p =
            reinterpret_cast<const std::uint8_t*>(png.data() + i);
        const std::uint32_t len = be32(p);
        REQUIRE(i + 12u + len <= png.size());
        Chunk c;
        c.type = png.substr(i + 4u, 4u);
        c.data.assign(p + 8, p + 8 + len);
        c.crc = be32(p + 8 + len);
        out.push_back(std::move(c));
        i += 12u + len;
        if (c.type == "IEND") {
            break;
        }
    }
    return out;
}

// The divider topology, built by hand like the shipped example.
Schematic divider() {
    Schematic s;
    Part* v = s.add_part(Part::make("v", "V1", "5", {0.0, -4.0}));
    Part* r1 = s.add_part(Part::make("r", "R1", "10k", {6.0, 0.0}));
    Part* r2 = s.add_part(Part::make("r", "R2", "10k", {14.0, 0.0}));
    Part* g1 = s.add_part(Part::make("gnd", "", "", {7.0, -6.0}));

    Wire* w1 = s.add_wire(Wire::endpoints(PinRef{v->id, 0u},
                                          PinRef{r1->id, 0u}));
    w1->waypoints = {{-3.0, 0.0}};
    s.add_wire(Wire::endpoints(PinRef{r1->id, 1u}, PinRef{r2->id, 0u}));
    Wire* w3 = s.add_wire(Wire::endpoints(PinRef{r2->id, 1u},
                                         PinRef{g1->id, 0u}));
    w3->waypoints = {{17.0, -6.0}};
    Wire* w4 = s.add_wire(Wire::endpoints(PinRef{g1->id, 0u},
                                         PinRef{v->id, 1u}));
    w4->waypoints = {{3.0, -6.0}};

    ws::circsim::Label out;
    out.pos = {10.0, 0.0};
    out.name = "out";
    s.add_label(out);
    return s;
}

}  // namespace

TEST_CASE("encode_png writes a structurally valid PNG") {
    RasterCanvas c(3, 2, 0x3366CCu);
    const std::string png = c.encode_png();

    REQUIRE(png.size() > 16u);
    CHECK(png.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0);

    const std::vector<Chunk> chunks = parse_chunks(png);
    REQUIRE(chunks.size() == 3u);
    CHECK(chunks[0].type == "IHDR");
    CHECK(chunks[1].type == "IDAT");
    CHECK(chunks[2].type == "IEND");
    CHECK(chunks[2].data.empty());

    for (const Chunk& ch : chunks) {
        std::string crc_in = ch.type;
        crc_in.append(reinterpret_cast<const char*>(ch.data.data()),
                      ch.data.size());
        CHECK(ch.crc == crc32_test(reinterpret_cast<const std::uint8_t*>(
                                       crc_in.data()),
                                   crc_in.size()));
    }

    REQUIRE(chunks[0].data.size() == 13u);
    CHECK(be32(chunks[0].data.data()) == 3u);       // width
    CHECK(be32(chunks[0].data.data() + 4) == 2u);   // height
    CHECK(chunks[0].data[8] == 8u);                 // bit depth
    CHECK(chunks[0].data[9] == 2u);                 // RGB
    CHECK(chunks[0].data[10] == 0u);
    CHECK(chunks[0].data[11] == 0u);
    CHECK(chunks[0].data[12] == 0u);

    // The zlib stream: 0x78 0x01 header, and the trailer's Adler32 is
    // over exactly the filter-0 scanlines of the known background.
    REQUIRE(chunks[1].data.size() > 6u);
    CHECK(chunks[1].data[0] == 0x78u);
    CHECK(chunks[1].data[1] == 0x01u);
    std::vector<std::uint8_t> raw;
    for (int y = 0; y < 2; ++y) {
        raw.push_back(0u);
        for (int x = 0; x < 3; ++x) {
            raw.push_back(0x33u);
            raw.push_back(0x66u);
            raw.push_back(0xCCu);
        }
    }
    const std::size_t z = chunks[1].data.size();
    CHECK(be32(chunks[1].data.data() + z - 4u) == adler32_test(raw.data(),
                                                              raw.size()));
}

TEST_CASE("stroke_polyline covers its rows and caps its ends") {
    RasterCanvas c(60, 40);
    c.stroke_polyline({{10.0, 20.0}, {50.0, 20.0}}, 2.0, kInk);
    // Width 2 around y=20: the band [19, 21) sweeps whole pixel rows -
    // subsamples sit at .125 offsets, so rows 19 and 20 are fully ink
    // and rows 18 and 21 touch nothing (their nearest subsample is
    // 1.125 out, past r=1).
    for (int x = 15; x <= 45; x += 5) {
        CHECK(c.pixel(x, 19) == kInk);
        CHECK(c.pixel(x, 20) == kInk);
        CHECK(c.pixel(x, 18) == 0xFFFFFFu);
        CHECK(c.pixel(x, 21) == 0xFFFFFFu);
    }
    // Round caps: the start lands inside the segment's span (full
    // ink); the end pixel is the cap's antialiased edge - 13 of its 16
    // subsamples sit inside the unit round cap (the exact blend is
    // (255*3 + ink*13) / 16 per channel).
    CHECK(c.pixel(10, 20) == kInk);
    CHECK(c.pixel(50, 20) == 0x494D57u);
    CHECK(c.pixel(8, 20) == 0xFFFFFFu);

    // Two calls meeting at a point composite normally (png.hpp's
    // contract): darker at the joint than one polyline through it,
    // which collapses the overlap inside the single call.
    RasterCanvas two(30, 30);
    two.stroke_polyline({{5.0, 5.0}, {15.0, 15.0}}, 2.0, kInk);
    two.stroke_polyline({{15.0, 15.0}, {25.0, 5.0}}, 2.0, kInk);
    RasterCanvas one(30, 30);
    one.stroke_polyline({{5.0, 5.0}, {15.0, 15.0}, {25.0, 5.0}}, 2.0, kInk);
    // The joint reads as the same round-cap edge either way: 13 of 16.
    CHECK(one.pixel(15, 15) == 0x494D57u);
    // The second call blends over the first: 0.8125 twice over.
    CHECK(two.pixel(15, 15) == 0x272C37u);
    CHECK(two.pixel(15, 15) != one.pixel(15, 15));
}

TEST_CASE("fill_disc blends its edge at half coverage") {
    RasterCanvas c(60, 40);
    c.fill_disc(30.0, 20.0, 7.5, kInk);
    CHECK(c.pixel(30, 20) == kInk);       // center
    CHECK(c.pixel(30, 26) == kInk);       // dy 6.875 max, well inside
    CHECK(c.pixel(30, 28) == 0xFFFFFFu);  // dy 8.125 min, outside
    // Row 27: subsamples at dy 7.125/7.375 inside, 7.625/7.875 out -
    // 8 of 16 covered, an exact 0.5 blend of ink over white.
    CHECK(c.pixel(30, 27) == 0x8F9298u);
}

TEST_CASE("fill_polygon fills even-odd: the pentagram's center is a hole") {
    // A self-crossing path is the only shape where the fill rule
    // shows: the {5/2} pentagram (one call, five points, 144-degree
    // steps) leaves its inner pentagon OUTSIDE under even-odd, where
    // nonzero would fill it. Center (25,25), radius 20.
    RasterCanvas c(60, 60);
    c.fill_polygon({{25.0, 45.0},
                     {13.24, 8.82},
                     {44.02, 31.18},
                     {5.98, 31.18},
                     {36.76, 8.82}},
                   kInk);
    CHECK(c.pixel(25, 35) == kInk);       // deep in the top lobe
    CHECK(c.pixel(25, 25) == 0xFFFFFFu);  // the hole even-odd leaves
    CHECK(c.pixel(25, 5) == 0xFFFFFFu);   // below the lowest tips
}

TEST_CASE("draw_text walks the font's advances") {
    // The space is a fixed 4 px; unknown bytes add nothing.
    CHECK(RasterCanvas::text_width("W W") ==
          2 * RasterCanvas::text_width("W") + 4.0);
    CHECK(RasterCanvas::text_width("\xC3\xA9") == 0.0);  // non-ASCII

    RasterCanvas c(60, 40);
    c.draw_text("W", 5.0, 5.0, RasterCanvas::HAlign::Left,
                RasterCanvas::VAlign::Top, kInk);
    // A cap: ink present, above the baseline, none below it (W has no
    // descender) and none past the advance.
    bool ink_seen = false;
    for (int y = 0; y < 40; ++y) {
        for (int x = 0; x < 60; ++x) {
            if (c.pixel(x, y) == kInk) {
                ink_seen = true;
                CHECK(x < 5 + static_cast<int>(RasterCanvas::text_width("W")));
                CHECK(y < 5 + RasterCanvas::kTextRows);
                CHECK(y < 5 + 12);  // nothing below the baseline
            }
        }
    }
    REQUIRE(ink_seen);
    CHECK(c.pixel(5, 5 + 15) == 0xFFFFFFu);  // the blank last row
}

TEST_CASE("render_schematic_png is deterministic and clamps ppu") {
    const Schematic s = divider();
    const std::string a = render_schematic_png(s, nullptr, {24});
    const std::string b = render_schematic_png(s, nullptr, {24});
    CHECK(a == b);
    CHECK(a.size() > 500u);

    const std::vector<Chunk> chunks = parse_chunks(a);
    REQUIRE(chunks.size() == 3u);
    CHECK(chunks[0].type == "IHDR");
    const int w = static_cast<int>(be32(chunks[0].data.data()));
    const int h = static_cast<int>(be32(chunks[0].data.data() + 4));
    CHECK(w > 200);   // ~21 units of x span at 24 ppu + margins
    CHECK(w < 2000);
    CHECK(h > 150);
    CHECK(w > h);     // the sheet is wide

    // ppu clamps into [4, 96]: 0 and 1e4 land on the same sheets as 4
    // and 96.
    CHECK(render_schematic_png(s, nullptr, {0}) ==
          render_schematic_png(s, nullptr, {4}));
    CHECK(render_schematic_png(s, nullptr, {10000}) ==
          render_schematic_png(s, nullptr, {96}));

    // An empty schematic still renders: a blank 1x1-unit sheet, the
    // margin math intact (ppu 8: 8 + 2 * round(0.75 * 8) = 20).
    Schematic empty;
    const std::string blank = render_schematic_png(empty, nullptr, {8});
    const std::vector<Chunk> bch = parse_chunks(blank);
    REQUIRE(bch.size() == 3u);
    CHECK(static_cast<int>(be32(bch[0].data.data())) == 20);
    CHECK(static_cast<int>(be32(bch[0].data.data() + 4)) == 20);
}

TEST_CASE("render_schematic_png skips unknown symbols and dangling wires") {
    Schematic s = divider();
    // A part whose symbol nobody carries, and a wire ending on it plus
    // one on a junction that does not exist: none of it may draw, none
    // of it may crash.
    Part* mystery = s.add_part(Part::make("mystery", "U1", "??", {30.0, 30.0}));
    s.add_wire(Wire::endpoints(PinRef{mystery->id, 0u},
                               PinRef{mystery->id, 1u}));
    s.add_wire(Wire::endpoints(ws::circsim::JunctionRef{9999u},
                               PinRef{mystery->id, 0u}));
    const std::string png = render_schematic_png(s, nullptr, {24});
    const std::string plain = render_schematic_png(divider(), nullptr, {24});
    // None of it may draw, none of it may crash - and skipped content
    // cannot move a pixel: the sheet is byte-identical to the clean
    // divider's.
    const std::vector<Chunk> chunks = parse_chunks(png);
    REQUIRE(chunks.size() == 3u);
    CHECK(chunks[2].type == "IEND");
    CHECK(png == plain);
}
