// Waveform semantics (DESIGN.md §6.2/§6.4). The piece definitions follow
// ngspice exactly; every edge a deck could care about is a breakpoint.

#include "waveform.hpp"

#include <algorithm>
#include <limits>

namespace ws::spice {

bool is_waveform_start(const std::string& lower) {
    return lower.rfind("pulse(", 0) == 0 || lower.rfind("sin(", 0) == 0 ||
           lower.rfind("exp(", 0) == 0 || lower.rfind("pwl(", 0) == 0;
}

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();
// A rise/fall time of exactly zero has no physical meaning (and would
// divide by zero); clamp to something far below any real timestep.
constexpr double kMinEdge = 1e-15;

[[nodiscard]] double clamp_edge(double v) { return v > kMinEdge ? v : kMinEdge; }

}  // namespace

double Waveform::value(double t) const {
    switch (kind) {
        case Kind::Pulse: {
            const double v1 = p[0];
            const double v2 = p[1];
            const double td = p[2];
            const double tr = clamp_edge(p[3]);
            const double tf = clamp_edge(p[4]);
            const double pw = p[5];
            const double per = p[6];
            if (t < td) {
                return v1;
            }
            double tt = t - td;
            if (per > 0.0) {
                tt = std::fmod(tt, per);
            }
            if (tt < tr) {
                return v1 + (v2 - v1) * (tt / tr);
            }
            if (tt < tr + pw) {
                return v2;
            }
            if (tt < tr + pw + tf) {
                return v2 + (v1 - v2) * ((tt - tr - pw) / tf);
            }
            return v1;
        }
        case Kind::Sin: {
            const double vo = p[0];
            const double va = p[1];
            const double freq = p[2] > 0.0 ? p[2] : 1.0;
            const double td = p[3];
            const double theta = p[4];
            const double phase = p[5] * 3.14159265358979323846 / 180.0;
            if (t < td) {
                return vo + va * std::sin(phase);
            }
            const double w = 2.0 * 3.14159265358979323846 * freq * (t - td);
            return vo + va * std::sin(w + phase) * std::exp(-theta * (t - td));
        }
        case Kind::Exp: {
            const double v1 = p[0];
            const double v2 = p[1];
            const double td1 = p[2];
            const double tau1 = clamp_edge(p[3]);
            const double td2 = p[4];
            const double tau2 = clamp_edge(p[5]);
            if (t < td1) {
                return v1;
            }
            if (t < td2) {
                return v1 + (v2 - v1) * (1.0 - std::exp(-(t - td1) / tau1));
            }
            return v2 + (v1 - v2) * (1.0 - std::exp(-(t - td2) / tau2));
        }
        case Kind::Pwl: {
            if (pwl.empty()) {
                return 0.0;
            }
            if (t <= pwl.front().first) {
                return pwl.front().second;
            }
            for (std::size_t i = 1; i < pwl.size(); ++i) {
                if (t <= pwl[i].first) {
                    const auto& [t0, v0] = pwl[i - 1];
                    const auto& [t1, v1] = pwl[i];
                    const double span = t1 - t0;
                    if (span <= 0.0) {
                        return v1;
                    }
                    return v0 + (v1 - v0) * ((t - t0) / span);
                }
            }
            return pwl.back().second;
        }
    }
    return 0.0;
}

double Waveform::next_event(double t) const {
    switch (kind) {
        case Kind::Pulse: {
            const double td = p[2];
            const double tr = clamp_edge(p[3]);
            const double tf = clamp_edge(p[4]);
            const double pw = p[5];
            const double per = p[6];
            const double edges[4] = {td, td + tr, td + tr + pw, td + tr + pw + tf};
            double best = kInf;
            if (t < td) {
                best = td;  // the first edge; later ones can only be larger
            } else {
                // Edges of the current (and, when repeating, the next)
                // cycle; the next event is the smallest one after t. A
                // single-shot pulse (PER <= 0) enumerates its own four
                // edges exactly once.
                const double m = (per > 0.0) ? std::floor((t - td) / per) : 0.0;
                const int extra_cycles = (per > 0.0) ? 1 : 0;
                for (int cycle = 0; cycle <= extra_cycles; ++cycle) {
                    const double base = td + (m + static_cast<double>(cycle)) * per;
                    for (const double e : edges) {
                        const double te = base + (e - td);
                        if (te > t && te < best) {
                            best = te;
                        }
                    }
                }
            }
            return best;
        }
        case Kind::Sin:
            return (t < p[3]) ? p[3] : kInf;
        case Kind::Exp: {
            double best = kInf;
            if (t < p[2]) {
                best = p[2];
            }
            if (t < p[4] && p[4] < best) {
                best = p[4];
            }
            return best;
        }
        case Kind::Pwl: {
            for (const auto& [tc, v] : pwl) {
                (void)v;
                if (tc > t) {
                    return tc;
                }
            }
            return kInf;
        }
    }
    return kInf;
}

void Waveform::finalize(double tstep, double tstop) {
    const double edge = tstep > kMinEdge ? tstep : kMinEdge;
    switch (kind) {
        case Kind::Pulse:
            if (defaulted[3] || p[3] <= 0.0) {
                p[3] = edge;  // TR <- TSTEP (ngspice)
            }
            if (defaulted[4] || p[4] <= 0.0) {
                p[4] = edge;  // TF <- TSTEP
            }
            if (defaulted[5] || p[5] <= 0.0) {
                p[5] = tstop;  // PW <- TSTOP (a single pulse spans the run)
            }
            // PER defaulted stays 0: no repetition (PW already covers the
            // whole run). An explicit PER > 0 repeats.
            break;
        case Kind::Sin:
            if (defaulted[2] || p[2] <= 0.0) {
                p[2] = tstop > 0.0 ? 1.0 / tstop : 1.0;  // FREQ <- 1/TSTOP
            }
            break;
        case Kind::Exp:
            if (defaulted[3] || p[3] <= 0.0) {
                p[3] = edge;  // TAU1 <- TSTEP
            }
            if (defaulted[4]) {
                p[4] = p[2] + edge;  // TD2 <- TD1 + TSTEP
            }
            if (defaulted[5] || p[5] <= 0.0) {
                p[5] = p[3];  // TAU2 <- TAU1
            }
            break;
        case Kind::Pwl:
            break;  // nothing deferred
    }
}

// --- parsing -----------------------------------------------------------------------

ws::core::Result<Waveform> parse_waveform(const std::vector<Token>& tokens,
                                          const ParamTable& params) {
    if (tokens.empty()) {
        return spice_fail([] {
            Diagnostic d;
            d.kind = ErrorKind::Parse;
            d.code = "spice.parse.waveform";
            d.message = "empty waveform";
            return d;
        }());
    }
    // Reassemble the waveform text: tokens[0] starts it; the token ending
    // with ')' closes it (single-token "PULSE(0 1)" or spread across
    // several).
    std::string text = tokens[0].text;
    std::size_t last = 0;
    bool closed = text.size() >= 1 && text.back() == ')';
    for (std::size_t i = 1; i < tokens.size() && !closed; ++i) {
        text += ' ';
        text += tokens[i].text;
        last = i;
        closed = tokens[i].text.size() >= 1 && tokens[i].text.back() == ')';
    }
    if (!closed) {
        return spice_fail([&] {
            Diagnostic d;
            d.kind = ErrorKind::Parse;
            d.code = "spice.parse.waveform";
            d.message = "unterminated waveform '" + tokens[0].text + "' (missing ')')";
            d.loc = tokens[0].loc;
            return d;
        }());
    }
    const SourceLoc loc = tokens[0].loc;

    const std::string lower_text = to_lower(text);
    const std::size_t open = lower_text.find('(');
    const std::size_t close = text.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open) {
        return spice_fail([&] {
            Diagnostic d;
            d.kind = ErrorKind::Parse;
            d.code = "spice.parse.waveform";
            d.message = "malformed waveform '" + text + "'";
            d.loc = loc;
            return d;
        }());
    }
    const std::string head = lower_text.substr(0, open);
    // Args: split the inside on whitespace (tokens were joined with ' ').
    std::vector<std::string> args;
    {
        const std::string inside = text.substr(open + 1, close - open - 1);
        std::size_t i = 0;
        while (i < inside.size()) {
            while (i < inside.size() && (inside[i] == ' ' || inside[i] == '\t')) {
                ++i;
            }
            const std::size_t start = i;
            while (i < inside.size() && inside[i] != ' ' && inside[i] != '\t') {
                ++i;
            }
            if (i > start) {
                args.push_back(inside.substr(start, i - start));
            }
        }
    }

    auto fail = [&](std::string message) -> ws::core::unexpected<ws::core::Error> {
        Diagnostic d;
        d.kind = ErrorKind::Parse;
        d.code = "spice.parse.waveform";
        d.message = std::move(message);
        d.loc = loc;
        return spice_fail(std::move(d));
    };
    auto arg_value = [&](std::size_t i) -> ws::core::Result<double> {
        Token t;
        t.text = args[i];
        t.loc = tokens[std::min(i, last)].loc;
        return resolve_value_token(t, params);
    };

    Waveform w;
    if (head == "pulse") {
        w.kind = Waveform::Kind::Pulse;
        if (args.size() < 2) {
            return fail("PULSE needs at least V1 V2 (TD TR TF PW PER follow)");
        }
        if (args.size() > 7) {
            return fail("PULSE takes at most V1 V2 TD TR TF PW PER");
        }
        for (std::size_t i = 0; i < args.size(); ++i) {
            WS_TRY_ASSIGN(w.p[i], arg_value(i));
        }
        for (std::size_t i = args.size(); i < 7; ++i) {
            w.defaulted[i] = true;  // ngspice defaults, resolved by finalize()
        }
        return w;
    }
    if (head == "sin") {
        w.kind = Waveform::Kind::Sin;
        if (args.size() < 2) {
            return fail("SIN needs at least VO VA (FREQ TD THETA PHASE follow)");
        }
        if (args.size() > 6) {
            return fail("SIN takes at most VO VA FREQ TD THETA PHASE");
        }
        for (std::size_t i = 0; i < args.size(); ++i) {
            WS_TRY_ASSIGN(w.p[i], arg_value(i));
        }
        for (std::size_t i = args.size(); i < 6; ++i) {
            w.defaulted[i] = true;
        }
        return w;
    }
    if (head == "exp") {
        w.kind = Waveform::Kind::Exp;
        if (args.size() < 2) {
            return fail("EXP needs at least V1 V2 (TD1 TAU1 TD2 TAU2 follow)");
        }
        if (args.size() > 6) {
            return fail("EXP takes at most V1 V2 TD1 TAU1 TD2 TAU2");
        }
        for (std::size_t i = 0; i < args.size(); ++i) {
            WS_TRY_ASSIGN(w.p[i], arg_value(i));
        }
        for (std::size_t i = args.size(); i < 6; ++i) {
            w.defaulted[i] = true;
        }
        return w;
    }
    if (head == "pwl") {
        w.kind = Waveform::Kind::Pwl;
        if (args.size() < 2 || args.size() % 2 != 0) {
            return fail("PWL takes pairs: T1 V1 T2 V2 ...");
        }
        double prev_t = -kInf;
        for (std::size_t i = 0; i < args.size(); i += 2) {
            WS_TRY_ASSIGN(const double tc, arg_value(i));
            WS_TRY_ASSIGN(const double vc, arg_value(i + 1));
            if (tc <= prev_t) {
                return fail("PWL corner times must strictly increase");
            }
            prev_t = tc;
            w.pwl.emplace_back(tc, vc);
        }
        return w;
    }
    return fail("unknown waveform '" + head + "' (PULSE, SIN, EXP, PWL)");
}

}  // namespace ws::spice
