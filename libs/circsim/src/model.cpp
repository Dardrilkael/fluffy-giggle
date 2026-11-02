#include <memory>
#include <cmath>

#include <ws/circsim/model.hpp>

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

}  // namespace ws::circsim
