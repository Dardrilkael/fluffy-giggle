#include <memory>
#include <cmath>
#include <string>
#include <utility>

#include <ws/circsim/model.hpp>
#include <ws/circsim/text.hpp>

namespace ws::circsim {

Part* Schematic::add_part(Part p) {
    p.id = alloc_id();
    parts_.push_back(std::move(p));
    bump();
    return &parts_.back();
}

Junction* Schematic::add_junction(Junction j) {
    j.id = alloc_id();
    junctions_.push_back(std::move(j));
    bump();
    return &junctions_.back();
}

Wire* Schematic::add_wire(Wire w) {
    w.id = alloc_id();
    wires_.push_back(std::move(w));
    bump();
    return &wires_.back();
}

Label* Schematic::add_label(Label l) {
    l.id = alloc_id();
    labels_.push_back(std::move(l));
    bump();
    return &labels_.back();
}

// --- the loader's wholesale restores -----------------------------------------
// Both *_mut() invariants per container: next_id above every stored id,
// revision bumped. A template keeps the four in one truth.
namespace {

template <typename T, typename IdOf>
void restore_into(std::deque<T>& into, std::deque<T> from,
                  std::uint32_t& next_id, IdOf&& id_of) {
    for (const T& e : from) {
        const std::uint32_t id = id_of(e);
        if (id >= next_id) {
            next_id = id + 1 == 0 ? Schematic::kIdCeiling : id + 1;
        }
    }
    into = std::move(from);
}

}  // namespace

void Schematic::restore_parts(std::deque<Part> p) {
    restore_into(parts_, std::move(p), next_id_,
                 [](const Part& e) { return e.id; });
    bump();
}

void Schematic::restore_junctions(std::deque<Junction> j) {
    restore_into(junctions_, std::move(j), next_id_,
                 [](const Junction& e) { return e.id; });
    bump();
}

void Schematic::restore_wires(std::deque<Wire> w) {
    restore_into(wires_, std::move(w), next_id_,
                 [](const Wire& e) { return e.id; });
    bump();
}

void Schematic::restore_labels(std::deque<Label> l) {
    restore_into(labels_, std::move(l), next_id_,
                 [](const Label& e) { return e.id; });
    bump();
}

namespace {

// Linear scans: the finders serve the editor and the loaders, not hot
// paths (hot paths hold their own maps). Container-generic on purpose.
template <template <typename, typename> typename C, typename T, typename F>
const T* find_in(const C<T, std::allocator<T>>& v, F&& matches) {
    for (const T& t : v) {
        if (matches(t)) {
            return &t;
        }
    }
    return nullptr;
}

}  // namespace

const Part* Schematic::find_part(std::uint32_t id) const {
    return find_in(parts_, [id](const Part& p) { return p.id == id; });
}
Part* Schematic::find_part(std::uint32_t id) {
    return const_cast<Part*>(
        static_cast<const Schematic*>(this)->find_part(id));
}
const Junction* Schematic::find_junction(std::uint32_t id) const {
    return find_in(junctions_, [id](const Junction& j) { return j.id == id; });
}
const Wire* Schematic::find_wire(std::uint32_t id) const {
    return find_in(wires_, [id](const Wire& w) { return w.id == id; });
}
const Label* Schematic::find_label(std::uint32_t id) const {
    return find_in(labels_, [id](const Label& l) { return l.id == id; });
}

// --- the shared analysis validation ------------------------------------------
// Mirrors ws::spice's own semantic gates (.dc step/direction, .tran's
// 0 <= tstart < tstop, .ac's positive-integer N and 0 < fstart <=
// fstop) one layer earlier, in circsim's vocabulary: both entry doors
// (the CLI parser and the .csim loader) reject a bad spec at entry
// with a message naming the value, instead of letting it surface as a
// mid-run engine error. A TranSpec tstep of 0 stays legal - the card
// convention substitutes tstop/100 at export.

namespace {

[[nodiscard]] bool finite(double v) { return std::isfinite(v); }

[[nodiscard]] std::optional<std::string> freq_problem(double fstart,
                                                    double fstop) {
    if (!finite(fstart) || !finite(fstop) || fstart <= 0.0 ||
        fstop < fstart) {
        return "frequencies need 0 < fstart <= fstop (got " +
               fmt_num(fstart) + " .. " + fmt_num(fstop) + ")";
    }
    return std::nullopt;
}

}  // namespace

std::optional<std::string> analysis_spec_problem(const AnalysisSpec& a) {
    if (const OpSpec* op = std::get_if<OpSpec>(&a)) {
        (void)op;
        return std::nullopt;
    }
    if (const DcSpec* dc = std::get_if<DcSpec>(&a)) {
        if (dc->source.empty()) {
            return "the dc sweep names no source (dc V1 0 5 0.1)";
        }
        if (!finite(dc->start) || !finite(dc->stop) || !finite(dc->step)) {
            return "dc sweep values for '" + dc->source +
                   "' must be finite";
        }
        if (dc->step == 0.0) {
            return "dc sweep step for '" + dc->source +
                   "' is zero (got start " + fmt_num(dc->start) + ", stop " +
                   fmt_num(dc->stop) + ", step " + fmt_num(dc->step) + ")";
        }
        if ((dc->stop > dc->start && dc->step < 0.0) ||
            (dc->stop < dc->start && dc->step > 0.0)) {
            return "dc sweep step for '" + dc->source +
                   "' must point toward " + fmt_num(dc->stop) + " (start " +
                   fmt_num(dc->start) + ", step " + fmt_num(dc->step) + ")";
        }
        return std::nullopt;
    }
    if (const TranSpec* tran = std::get_if<TranSpec>(&a)) {
        if (!finite(tran->tstop) || tran->tstop <= 0.0) {
            return "tran needs a positive tstop (got " +
                   fmt_num(tran->tstop) + ")";
        }
        // tstep 0 is the tstop/100 card convention; anything negative
        // or non-finite is not.
        if (!finite(tran->tstep) || tran->tstep < 0.0) {
            return "tran tstep must be 0 (auto) or positive (got " +
                   fmt_num(tran->tstep) + ")";
        }
        if (tran->tstart.has_value() &&
            (!finite(*tran->tstart) || *tran->tstart < 0.0 ||
             *tran->tstart >= tran->tstop)) {
            return "tran needs 0 <= tstart < tstop (got tstart " +
                   fmt_num(*tran->tstart) + ", tstop " +
                   fmt_num(tran->tstop) + ")";
        }
        if (tran->tmax.has_value() &&
            (!finite(*tran->tmax) || *tran->tmax <= 0.0)) {
            return "tran tmax must be positive (got " +
                   fmt_num(*tran->tmax) + ")";
        }
        return std::nullopt;
    }
    if (const AcSpec* ac = std::get_if<AcSpec>(&a)) {
        if (ac->n < 1) {
            return "ac n must be a whole number >= 1 (got " +
                   std::to_string(ac->n) + ")";
        }
        return freq_problem(ac->fstart, ac->fstop);
    }
    const NoiseSpec* ns = std::get_if<NoiseSpec>(&a);
    if (ns->output.empty()) {
        return "noise names no output node (noise v(out) V1 dec 10 1 1k)";
    }
    if (ns->input.empty()) {
        return "noise names no input source (noise v(out) V1 dec 10 1 1k)";
    }
    if (ns->n < 1) {
        return "noise n must be a whole number >= 1 (got " +
               std::to_string(ns->n) + ")";
    }
    if (ns->pts_per_summary < 0) {
        return "noise summary cadence must be >= 0 (got " +
               std::to_string(ns->pts_per_summary) + ")";
    }
    return freq_problem(ns->fstart, ns->fstop);
}

}  // namespace ws::circsim
