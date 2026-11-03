// The stage-17b headless render (see svg_dump.hpp): ws::circsim's
// render_symbol flattens each symbol into world-space strokes, this
// file writes them as SVG. One shared geometry (the seam), two
// surfaces (the editor's canvas, this dump) - the strokes cannot
// disagree.

#include "svg_dump.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <ws/circsim/render.hpp>

namespace svg {
namespace {

namespace fs = std::filesystem;

constexpr double kUnit = 24.0;   // px per grid unit
constexpr double kMargin = 0.75; // units of breathing room

const std::string kInk = "#1f2430";  // the body strokes
const std::string kPin = "#b4443c";  // pin markers and names

std::string esc(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c; break;
        }
    }
    return out;
}

std::string num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return buf;
}

// One symbol's picture, body markup in a pixel space of its own:
// x right, y down (the y flip applied), origin at the bounds' top
// left plus the margin.
struct SymbolSvg {
    std::string body;
    ws::circsim::Bounds b;  // grid units, for the viewBox math
};

SymbolSvg draw_one(const ws::circsim::SymbolDef& sym) {
    using ws::circsim::Pos;
    using ws::circsim::SymbolStroke;

    const ws::circsim::Bounds b = ws::circsim::symbol_bounds(sym);
    const auto tx = [&](double x) {
        return (x - b.min_x) * kUnit + kMargin * kUnit;
    };
    const auto ty = [&](double y) {
        return (b.max_y - y) * kUnit + kMargin * kUnit;
    };

    ws::circsim::Part placement{};  // rot 0, no mirror, at origin
    const std::vector<SymbolStroke> strokes =
        ws::circsim::render_symbol(sym, placement);

    std::string body;
    for (const SymbolStroke& s : strokes) {
        const auto px = [&](std::size_t i) {
            return num(tx(s.pts[i].x)) + "," + num(ty(s.pts[i].y));
        };
        switch (s.kind) {
            case SymbolStroke::Kind::Line:
                body += "<line x1=\"" + num(tx(s.pts[0].x)) + "\" y1=\"" +
                        num(ty(s.pts[0].y)) + "\" x2=\"" +
                        num(tx(s.pts[1].x)) + "\" y2=\"" +
                        num(ty(s.pts[1].y)) +
                        "\" stroke=\"" + kInk +
                        "\" stroke-width=\"2\" stroke-linecap=\"round\"/>\n";
                break;
            case SymbolStroke::Kind::Poly: {
                // An open polyline is <polyline>; a closed one is a
                // <polygon> - the seam's closed flag maps straight.
                body += s.closed ? "<polygon points=\"" : "<polyline points=\"";
                for (std::size_t i = 0; i < s.pts.size(); ++i) {
                    if (i > 0) {
                        body += ' ';
                    }
                    body += px(i);
                }
                body += "\" fill=\"none\" stroke=\"" + kInk +
                        "\" stroke-width=\"2\" stroke-linejoin=\"round\""
                        " stroke-linecap=\"round\"/>\n";
                break;
            }
            case SymbolStroke::Kind::PolyFill: {
                // A filled <polygon> - the fill closes the loop.
                body += "<polygon points=\"";
                for (std::size_t i = 0; i < s.pts.size(); ++i) {
                    if (i > 0) {
                        body += ' ';
                    }
                    body += px(i);
                }
                body += "\" fill=\"" + kInk + "\"/>\n";
                break;
            }
            case SymbolStroke::Kind::Circle:
                body += "<circle cx=\"" + num(tx(s.pts[0].x)) + "\" cy=\"" +
                        num(ty(s.pts[0].y)) + "\" r=\"" + num(s.r * kUnit) +
                        "\" fill=\"none\" stroke=\"" + kInk +
                        "\" stroke-width=\"2\"/>\n";
                break;
            case SymbolStroke::Kind::Arc: {
                // Sampled to a polyline, the way the canvas flattens
                // arcs - 16 segments per quarter turn.
                const double span = s.a1 - s.a0;
                const int steps = std::max(
                    2, static_cast<int>(std::ceil(std::fabs(span) / 90.0)) *
                           16);
                constexpr double kRad = 3.14159265358979323846 / 180.0;
                body += "<polyline points=\"";
                for (int i = 0; i <= steps; ++i) {
                    const double a =
                        (s.a0 + span * static_cast<double>(i) / steps) * kRad;
                    const double x = s.pts[0].x + s.r * std::cos(a);
                    const double y = s.pts[0].y + s.r * std::sin(a);
                    if (i > 0) {
                        body += ' ';
                    }
                    body += num(tx(x)) + "," + num(ty(y));
                }
                body += "\" fill=\"none\" stroke=\"" + kInk +
                        "\" stroke-width=\"2\"/>\n";
                break;
            }
            case SymbolStroke::Kind::Dot:
                body += "<circle cx=\"" + num(tx(s.pts[0].x)) + "\" cy=\"" +
                        num(ty(s.pts[0].y)) + "\" r=\"" + num(s.r * kUnit) +
                        "\" fill=\"" + kInk + "\"/>\n";
                break;
            case SymbolStroke::Kind::Text:
                body += "<text x=\"" + num(tx(s.pts[0].x)) + "\" y=\"" +
                        num(ty(s.pts[0].y)) +
                        "\" font-size=\"" + num(0.75 * kUnit) +
                        "\" text-anchor=\"middle\" dominant-baseline=\"middle\" fill=\""
                        + kInk + "\">" + esc(s.text) +
                        "</text>\n";
                break;
        }
    }

    // The pins: a dot on each pin position and its name just outward,
    // so pin placement reads in a review (the redesign moves them).
    for (const ws::circsim::PinDef& pin : sym.pins) {
        const Pos p{pin.dx, pin.dy};
        body += "<circle cx=\"" + num(tx(p.x)) + "\" cy=\"" + num(ty(p.y)) +
                "\" r=\"" + num(0.14 * kUnit) + "\" fill=\"" + kPin + "\"/>\n";
        double ox = 0.0;
        double oy = 0.0;
        switch (((pin.dir % 360) + 360) % 360) {
            case 0: ox = 0.45; break;
            case 90: oy = 0.45; break;
            case 180: ox = -0.45; break;
            default: oy = -0.45; break;
        }
        body += "<text x=\"" + num(tx(p.x + ox)) + "\" y=\"" +
                num(ty(p.y + oy)) + "\" font-size=\"" + num(0.5 * kUnit) +
                "\" text-anchor=\"middle\" dominant-baseline=\"middle\" "
                "fill=\"" + kPin + "\">" + esc(pin.name) + "</text>\n";
    }

    return SymbolSvg{std::move(body), b};
}

bool write_file(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        std::fprintf(stderr, "error: cannot write '%s'\n", path.c_str());
        return false;
    }
    out << text;
    return out.good();
}

}  // namespace

int write_symbol_svgs(const ws::circsim::SymbolLibrary& lib,
                      const std::string& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        std::fprintf(stderr, "error: cannot create '%s': %s\n", dir.c_str(),
                     ec.message().c_str());
        return 2;
    }

    // Draw every symbol once; both outputs reuse the body markup.
    std::vector<SymbolSvg> drawn;
    drawn.reserve(lib.symbols().size());
    for (const ws::circsim::SymbolDef& s : lib.symbols()) {
        drawn.push_back(draw_one(s));
    }

    // One standalone SVG per symbol, fitted to its own bounds.
    for (std::size_t i = 0; i < drawn.size(); ++i) {
        const ws::circsim::SymbolDef& s = lib.symbols()[i];
        const SymbolSvg& d = drawn[i];
        const double w =
            (d.b.max_x - d.b.min_x) * kUnit + 2.0 * kMargin * kUnit;
        const double h =
            (d.b.max_y - d.b.min_y) * kUnit + 2.0 * kMargin * kUnit;
        const std::string svg =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" +
            num(w) + "\" height=\"" + num(h) + "\" viewBox=\"0 0 " + num(w) +
            " " + num(h) + "\">\n" + "<rect width=\"100%\" height=\"100%\" "
            "fill=\"#ffffff\"/>\n" + d.body + "</svg>\n";
        if (!write_file(fs::path(dir) / (s.key + ".svg"), svg)) {
            return 2;
        }
    }

    // The contact sheet: 4 columns of captioned cells, each cell a
    // nested SVG scaled into place.
    constexpr int kCols = 4;
    constexpr double kCellW = 190.0;
    constexpr double kCellH = 170.0;
    constexpr double kArt = 140.0;  // the drawing's slice of the cell
    const int rows = static_cast<int>(
        (drawn.size() + kCols - 1) / kCols);
    const double w = kCols * kCellW + 20.0;
    const double h = static_cast<double>(rows) * kCellH + 20.0;

    std::string sheet =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" +
        num(w) + "\" height=\"" + num(h) + "\" viewBox=\"0 0 " + num(w) +
        " " + num(h) + "\">\n" + "<rect width=\"100%\" height=\"100%\" "
        "fill=\"#ffffff\"/>\n";

    for (std::size_t i = 0; i < drawn.size(); ++i) {
        const ws::circsim::SymbolDef& s = lib.symbols()[i];
        const SymbolSvg& d = drawn[i];
        const double cx = 10.0 + static_cast<double>(i % kCols) * kCellW;
        const double cy = 10.0 + static_cast<double>(i / kCols) * kCellH;
        sheet += "<rect x=\"" + num(cx + 1.0) + "\" y=\"" + num(cy + 1.0) +
                 "\" width=\"" + num(kCellW - 2.0) + "\" height=\"" +
                 num(kCellH - 2.0) +
                 "\" fill=\"none\" stroke=\"#e4e4e8\"/>\n";
        sheet += "<svg x=\"" + num(cx) + "\" y=\"" + num(cy) +
                 "\" width=\"" + num(kCellW) + "\" height=\"" + num(kArt) +
                 "\" viewBox=\"0 0 " + num(kCellW) + " " + num(kArt) +
                 "\" preserveAspectRatio=\"xMidYMid meet\">\n" + d.body +
                 "</svg>\n";
        const std::string caption = s.name + " (" + s.key + ")";
        sheet += "<text x=\"" + num(cx + kCellW * 0.5) + "\" y=\"" +
                 num(cy + kArt + 16.0) +
                 "\" font-size=\"12\" text-anchor=\"middle\" "
                 "fill=\"#3a3f4a\">" + esc(caption) + "</text>\n";
    }
    sheet += "</svg>\n";

    if (!write_file(fs::path(dir) / "symbols.svg", sheet)) {
        return 2;
    }
    return 0;
}

}  // namespace svg
