// overlay (patch 0012): the interactive canvas's pure math - see
// overlay.hpp for the contracts. The implementations moved here from
// canvas_panel.cpp's anonymous namespace so the locked rule holds
// (no ui include anywhere in libs/circsim) and the rules that decide
// pixels answer under the headless gate.

#include <ws/circsim/overlay.hpp>

#include <algorithm>
#include <cmath>

namespace ws::circsim {

namespace {

// The r2d packing, produced locally: this library must not include
// the ui headers just to spell a color word.
[[nodiscard]] constexpr std::uint32_t pack_rgba(int r, int g, int b,
                                                int a) {
    return (static_cast<std::uint32_t>(r) << 24) |
           (static_cast<std::uint32_t>(g) << 16) |
           (static_cast<std::uint32_t>(b) << 8) |
           static_cast<std::uint32_t>(a);
}

// The ladder's nearest entry (the values are exact binary fractions,
// but a drifted rate still finds its home).
[[nodiscard]] std::size_t nearest_rate(double rate) {
    std::size_t best = 2;  // 1x, the default's own entry
    double best_d = -1.0;
    for (std::size_t i = 0; i < std::size(kOverlayRates); ++i) {
        const double d = std::fabs(kOverlayRates[i] - rate);
        if (best_d < 0.0 || d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

}  // namespace

std::size_t overlay_playhead_sample(const ws::spice::Table& t,
                                    double playhead) {
    const std::size_t rows = t.row_count();
    if (rows == 0) {
        return 0;
    }
    if (t.sweep != ws::spice::SweepKind::Time || t.sweep_axes.empty()) {
        return rows - 1;  // the sweep's end - the settled answer
    }
    const std::span<const double> axis = t.axis(0);
    if (axis.empty()) {
        return rows - 1;
    }
    // The last sample at or before the playhead (a .tran axis is
    // strictly increasing; upper_bound lands past it).
    const auto it = std::upper_bound(axis.begin(), axis.end(), playhead);
    if (it == axis.begin()) {
        return 0;
    }
    return static_cast<std::size_t>(it - axis.begin()) - 1;
}

OverlayWindow overlay_window(const ws::spice::Table& t) {
    OverlayWindow w;
    const std::span<const double> axis = t.axis(0);
    if (!axis.empty()) {
        w.t0 = axis.front();
        w.t1 = axis.back();
    }
    return w;
}

double overlay_scrub(const OverlayWindow& w, double frac) {
    if (!(w.t1 > w.t0)) {
        return w.t0;
    }
    const double f = std::clamp(frac, 0.0, 1.0);
    return w.t0 + f * (w.t1 - w.t0);
}

double overlay_step(double t, const OverlayWindow& w, double dt,
                    double rate, double loop_seconds) {
    const double span = w.t1 - w.t0;
    if (!(span > 0.0) || !(dt > 0.0)) {
        return std::clamp(t, w.t0, w.t1);
    }
    const double pace =
        span * std::max(rate, 0.0) / std::max(loop_seconds, 1e-9);
    t += dt * pace;
    while (t > w.t1) {
        t -= span;  // loop
    }
    if (t < w.t0) {
        t = w.t0;
    }
    return t;
}

double overlay_rate_next(double rate) {
    const std::size_t i = nearest_rate(rate);
    return kOverlayRates[(i + 1u) % std::size(kOverlayRates)];
}

const char* overlay_rate_label(double rate) {
    switch (nearest_rate(rate)) {
    case 0:
        return "1/4x";
    case 1:
        return "1/2x";
    case 3:
        return "2x";
    default:
        return "1x";
    }
}

std::uint32_t overlay_voltage_color(double v, double vmax) {
    const double u =
        std::clamp(v / std::max(vmax, 1e-9), -1.0, 1.0);
    const double a = std::sqrt(std::fabs(u));
    constexpr double kBase[3] = {122.0, 126.0, 134.0};  // neutral ink
    constexpr double kHot[3] = {255.0, 86.0, 60.0};     // positive: red
    constexpr double kCold[3] = {86.0, 150.0, 255.0};   // negative: blue
    const double* hot = u >= 0.0 ? kHot : kCold;
    int rgb[3];
    for (int i = 0; i < 3; ++i) {
        rgb[i] = static_cast<int>(
            std::lround(kBase[i] + (hot[i] - kBase[i]) * a));
    }
    return pack_rgba(rgb[0], rgb[1], rgb[2], 242);
}

double overlay_led_brightness(double amps) {
    // The glow's full-scale and floor: 20 mA reads as full-on,
    // sub-100 uA conduction is dark (the gmin ghost must not glow).
    constexpr double kFullAmps = 0.020;
    constexpr double kFloorAmps = 100e-6;
    const double a = std::fabs(amps);  // sign-blind
    if (a < kFloorAmps) {
        return 0.0;
    }
    return std::sqrt(std::min(1.0, a / kFullAmps));
}

OverlaySnapshot overlay_snapshot(const ws::spice::Table& t,
                                 const SpiceExport& map,
                                 double playhead) {
    OverlaySnapshot snap;
    snap.sample = overlay_playhead_sample(t, playhead);
    // net id -> node token (the export map is token -> net; invert
    // once - the first token a net answers wins, the deck's own
    // one-node-per-net spelling).
    std::unordered_map<std::uint32_t, std::string> token_of;
    token_of.reserve(map.node_net.size());
    for (const auto& [tok, net_id] : map.node_net) {
        token_of.emplace(net_id, tok);
    }
    for (const auto& [net_id, tok] : token_of) {
        const std::span<const double> col = t.signal("v(" + tok + ")");
        if (col.size() > snap.sample &&
            std::isfinite(col[snap.sample])) {
            const double v = col[snap.sample];
            snap.volts.emplace(net_id, v);
            snap.vmax = std::max(snap.vmax, std::fabs(v));
        }
    }
    // part id -> the element's probe current at the same sample (only
    // devices with a probe column answer; resistors stay absent).
    for (const auto& [name, part_id] : map.element_part) {
        const std::span<const double> col = t.signal("i(" + name + ")");
        if (col.size() > snap.sample &&
            std::isfinite(col[snap.sample])) {
            snap.amps.emplace(part_id, col[snap.sample]);
        }
    }
    snap.ok = !snap.volts.empty();
    return snap;
}

}  // namespace ws::circsim
