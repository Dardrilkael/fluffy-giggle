// apps/schematic_test - minimal schematic + canvas wiring test app.
// Uses ws::schematic (the headless routing core) + ws::ui::r2d directly.
// Simple rectangular "components" with pins that can be wired together.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <ws/app/command.hpp>
#include <ws/app/shortcut.hpp>
#include <ws/app/undo.hpp>
#include <ws/input/event.hpp>
#include <ws/input/input.hpp>
#include <ws/jobs/job_system.hpp>
#include <ws/platform/dialogs.hpp>
#include <ws/platform/locations.hpp>
#include <ws/platform/messagebox.hpp>
#include <ws/platform/paths.hpp>
#include <ws/settings/filesystem.hpp>
#include <ws/settings/schema.hpp>
#include <ws/settings/settings.hpp>
#include <ws/ui/app.hpp>
#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/layout_cursor.hpp>
#include <ws/ui/gui/theme.hpp>
#include <ws/ui/gui/widgets.hpp>
#include <ws/ui/renderer2d.hpp>
#include <ws/ui/shell/errors.hpp>
#include <ws/ui/shell/shell.hpp>

#include <ws/schematic/commands.hpp>
#include <ws/schematic/model.hpp>

#ifndef __ANDROID__
#include <ws/platform/single_instance.hpp>
#else
// NDK headers are C - keep them clear of our -Wpedantic settings.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include <android_native_app_glue.h>  // android_app + the android_main contract
#include <unistd.h>                   // chdir
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#endif

namespace {

namespace gui = ws::ui::gui;
namespace shell = ws::ui::shell;
namespace sch = ws::sch;
namespace r2d = ws::ui::r2d;
using gui::GuiContext;
using gui::LayoutCursor;
using ws::math::DVec2;

// The GLFW key codes the app's own chords ride.
constexpr int kKeyW = 87;      // W - wire tool
constexpr int kKeyV = 86;      // V - select tool
constexpr int kKeyR = 82;      // R - rotate
constexpr int kKeyM = 77;      // M - mirror
constexpr int kKeyF = 70;      // F - fit
constexpr int kKeyQ = 81;      // Q - quit
constexpr int kKeyDelete = 46; // Delete
constexpr int kLeftSuper = 343;   // GLFW_KEY_LEFT_SUPER
constexpr int kRightSuper = 347;  // GLFW_KEY_RIGHT_SUPER

// Font paths.
#ifdef __ANDROID__
constexpr const char* kUiFontPath = "/system/fonts/Roboto-Regular.ttf";
constexpr const char* kMonoFontPath = "/system/fonts/DroidSansMono.ttf";
#elif defined(_WIN32)
constexpr const char* kUiFontPath = "C:/Windows/Fonts/segoeui.ttf";
constexpr const char* kMonoFontPath = "C:/Windows/Fonts/consola.ttf";
#else
constexpr const char* kUiFontPath =
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
constexpr const char* kMonoFontPath =
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf";
#endif

[[nodiscard]] bool file_exists(const char* path) {
    return std::filesystem::exists(path);
}

[[nodiscard]] const char* font_or(const char* primary, const char* fallback) {
    return file_exists(primary) ? primary : fallback;
}

// --- simple component model ---------------------------------------------------

struct ComponentPin {
    int local_x = 0;   // offset from component origin
    int local_y = 0;
    int dir = 0;       // 0=right, 90=up, 180=left, 270=down
};

struct ComponentDef {
    std::string name;
    int width = 3;     // in grid units
    int height = 2;
    std::vector<ComponentPin> pins;
};

struct Component {
    std::uint32_t id = 0;
    ComponentDef* def = nullptr;
    sch::NodeId node_id = 0;  // the component's body node (for selection)
    std::vector<sch::NodeId> pin_nodes;
    DVec2 pos{0.0, 0.0};
    int rot = 0;      // 0, 90, 180, 270
    bool mirror = false;
};

inline DVec2 rotate_vec(DVec2 v, int rot) {
    int r = ((rot % 360) + 360) % 360;
    if (r == 0) return v;
    if (r == 90) return {-v.y, v.x};
    if (r == 180) return {-v.x, -v.y};
    return {v.y, -v.x};
}

inline DVec2 transform_pin(const Component& c, const ComponentPin& pin) {
    DVec2 local{static_cast<double>(pin.local_x), static_cast<double>(pin.local_y)};
    if (c.mirror) local.x = -local.x;
    local = rotate_vec(local, c.rot);
    return {c.pos.x + local.x, c.pos.y + local.y};
}

inline int transform_dir(int local_dir, int rot, bool mirror) {
    int d = local_dir;
    if (mirror) {
        // mirror flips horizontal: 0<->180
        if (d == 0) d = 180;
        else if (d == 180) d = 0;
        else if (d == 90) d = 90;     // up stays up
        else if (d == 270) d = 270;   // down stays down
    }
    d = (d + rot) % 360;
    if (d < 0) d += 360;
    return d;
}

// --- the document: schematic + components ------------------------------------

struct SchematicDoc {
    sch::Doc doc;
    std::vector<ComponentDef> defs;
    std::vector<Component> components;
    std::uint32_t next_comp_id = 1;

    // Pin directions stored separately since Node doesn't have dir field
    std::map<sch::NodeId, int> pin_dirs;

    // Built-in component definitions.
    SchematicDoc() {
        // Simple rectangle "box" component with 4 pins (one per side)
        ComponentDef box;
        box.name = "Box";
        box.width = 4;
        box.height = 3;
        box.pins = {
            {2, 0, 0},      // right
            {4, 1, 90},     // top
            {2, 3, 180},    // left
            {0, 1, 270},    // bottom
        };
        defs.push_back(std::move(box));

        // Tall rectangle
        ComponentDef tall;
        tall.name = "Tall";
        tall.width = 3;
        tall.height = 5;
        tall.pins = {
            {1, 0, 0},
            {3, 2, 90},
            {1, 5, 180},
            {0, 2, 270},
        };
        defs.push_back(std::move(tall));

        // Wide rectangle
        ComponentDef wide;
        wide.name = "Wide";
        wide.width = 6;
        wide.height = 2;
        wide.pins = {
            {3, 0, 0},
            {6, 1, 90},
            {3, 2, 180},
            {0, 1, 270},
        };
        defs.push_back(std::move(wide));
    }

    ComponentDef* find_def(const std::string& name) {
        for (auto& d : defs) if (d.name == name) return &d;
        return nullptr;
    }

    Component* find_comp(std::uint32_t id) {
        for (auto& c : components) if (c.id == id) return &c;
        return nullptr;
    }

    // Add a component at the given position.
    Component& add_component(const std::string& def_name, DVec2 pos) {
        ComponentDef* def = find_def(def_name);
        if (!def) {
            // This should not happen in valid test code, but we need a valid
            // return. Create a dummy component that will fail fast if used.
            static Component dummy;
            dummy.id = UINT32_MAX;
            return dummy;
        }

        Component c;
        c.id = next_comp_id++;
        c.def = def;
        c.pos = pos;
        c.rot = 0;
        c.mirror = false;

        // Create a node for the component body (for selection)
        c.node_id = doc.add_junction(pos);

        // Create pin nodes
        for (const auto& pin : def->pins) {
            DVec2 pin_pos = transform_pin(c, pin);
            int pin_dir = transform_dir(pin.dir, c.rot, c.mirror);
            sch::NodeId n = doc.add_pin(c.id, static_cast<std::uint32_t>(c.pin_nodes.size()), pin_pos);
            pin_dirs[n] = pin_dir; // store exit direction
            c.pin_nodes.push_back(n);
        }

        components.push_back(std::move(c));
        return components.back();
    }

    // Remove a component and its connected wires.
    void remove_component(std::uint32_t comp_id) {
        // Remove wires connected to this component's pins
        for (auto it = doc.wires.begin(); it != doc.wires.end();) {
            bool remove = false;
            Component* comp = find_comp(comp_id);
            if (comp) {
                for (auto pid : comp->pin_nodes) {
                    if (it->a == pid || it->b == pid) {
                        remove = true;
                        break;
                    }
                }
            }
            if (remove) {
                it = doc.wires.erase(it);
            } else {
                ++it;
            }
        }

        // Remove the component's nodes
        Component* comp = find_comp(comp_id);
        if (comp) {
            doc.nodes.erase(
                std::remove_if(doc.nodes.begin(), doc.nodes.end(),
                    [&](const sch::Node& n) {
                        return n.id == comp->node_id ||
                               std::find(comp->pin_nodes.begin(), comp->pin_nodes.end(), n.id) != comp->pin_nodes.end();
                    }),
                doc.nodes.end());

            // Remove pin dirs
            for (auto pid : comp->pin_nodes) {
                pin_dirs.erase(pid);
            }
        }

        components.erase(
            std::remove_if(components.begin(), components.end(),
                [&](const Component& c) { return c.id == comp_id; }),
            components.end());
    }

    // Add a wire between two nodes.
    sch::WireId add_wire(sch::NodeId a, sch::NodeId b, int a_dir, int b_dir) {
        return doc.add_wire(a, b, {}, a_dir, b_dir);
    }

    // Find a pin node at the given world position (for wire tool).
    std::optional<std::pair<sch::NodeId, int>> find_pin_at(DVec2 pos, double eps = 0.1) {
        for (auto& c : components) {
            for (std::size_t i = 0; i < c.pin_nodes.size(); ++i) {
                const sch::Node* n = doc.find_node(c.pin_nodes[i]);
                if (n && std::fabs(n->pos.x - pos.x) <= eps && std::fabs(n->pos.y - pos.y) <= eps) {
                    int dir = pin_dirs[c.pin_nodes[i]];
                    return std::make_pair(c.pin_nodes[i], dir);
                }
            }
        }
        return std::nullopt;
    }

    // Find a component at the given world position (for selection).
    Component* find_comp_at(DVec2 pos, double eps = 0.5) {
        for (auto& c : components) {
            const sch::Node* n = doc.find_node(c.node_id);
            if (n && std::fabs(n->pos.x - pos.x) <= eps && std::fabs(n->pos.y - pos.y) <= eps) {
                return &c;
            }
        }
        return nullptr;
    }
};

// --- the camera -------------------------------------------------------------

struct Camera {
    static constexpr double kMinZoom = 3.0;
    static constexpr double kMaxZoom = 400.0;
    static constexpr double kDefaultZoom = 26.0;

    double zoom = kDefaultZoom;
    DVec2 center{0.0, 0.0};
    float vx = 0.0f, vy = 0.0f, vwidth = 0.0f, vheight = 0.0f;

    [[nodiscard]] bool valid() const { return vwidth > 1.0f && vheight > 1.0f; }

    [[nodiscard]] DVec2 w2s(DVec2 world) const {
        return {vx + vwidth * 0.5f + (world.x - center.x) * static_cast<float>(zoom),
                vy + vheight * 0.5f - (world.y - center.y) * static_cast<float>(zoom)};
    }

    [[nodiscard]] DVec2 s2w(DVec2 screen) const {
        return {center.x + (screen.x - vx - vwidth * 0.5f) / static_cast<float>(zoom),
                center.y - (screen.y - vy - vheight * 0.5f) / static_cast<float>(zoom)};
    }

    void zoom_at(DVec2 anchor, double factor) {
        if (!valid()) return;
        DVec2 under = s2w(anchor);
        zoom = std::clamp(zoom * factor, kMinZoom, kMaxZoom);
        const double dx = (vx + vwidth * 0.5f - anchor.x) / zoom;
        const double dy = (anchor.y - vy - vheight * 0.5f) / zoom;
        center = {under.x + dx, under.y + dy};
    }

    void pan_by(DVec2 screen_delta) {
        center.x -= screen_delta.x / zoom;
        center.y += screen_delta.y / zoom;
    }

    void fit(const std::vector<DVec2>& points, double margin = 2.0) {
        if (!valid() || points.empty()) return;
        double min_x = points[0].x, max_x = points[0].x;
        double min_y = points[0].y, max_y = points[0].y;
        for (auto& p : points) {
            min_x = std::min(min_x, p.x);
            max_x = std::max(max_x, p.x);
            min_y = std::min(min_y, p.y);
            max_y = std::max(max_y, p.y);
        }
        double w = (max_x - min_x) + 2.0 * margin;
        double h = (max_y - min_y) + 2.0 * margin;
        if (w <= 0.0 || h <= 0.0) return;
        DVec2 target{(min_x + max_x) * 0.5, (min_y + max_y) * 0.5};
        zoom = std::clamp(std::min(vwidth / w, vheight / h), kMinZoom, kMaxZoom);
        center = target;
    }
};

// --- the app -----------------------------------------------------------------

class SchematicTestApp : public ws::ui::App {
public:
    using ws::ui::App::App;

    void on_setup() override {
        // Load fonts
        font_ui_ = r2d::load_font(font_or(kUiFontPath, ""), 14.0f);
        font_mono_ = r2d::load_font(font_or(kMonoFontPath, ""), 12.0f);
        if (font_ui_.empty() || font_mono_.empty()) {
            fprintf(stderr, "Failed to load fonts\n");
        }

        // Initialize document with some test components
        doc_.add_component("Box", {0.0, 0.0});
        doc_.add_component("Box", {8.0, 0.0});
        doc_.add_component("Tall", {4.0, -5.0});
        doc_.add_component("Wide", {4.0, 5.0});

        // Wire them up
        auto p0 = doc_.find_pin_at({2.0, 0.0});  // right pin of first box
        auto p1 = doc_.find_pin_at({6.0, 0.0});  // left pin of second box
        if (p0 && p1) {
            doc_.add_wire(p0->first, p1->first, p0->second, p1->second);
        }

        // Wire tall to first box
        auto p2 = doc_.find_pin_at({2.0, 1.0});  // top pin of first box
        auto p3 = doc_.find_pin_at({4.0, 0.0});  // bottom pin of tall
        if (p2 && p3) {
            doc_.add_wire(p2->first, p3->first, p2->second, p3->second);
        }

        camera_.fit({{0,0}, {8,0}, {4,-5}, {4,5}}, 3.0);

        // Set up command registry
        commands_ = ws::app::CommandRegistry{};

        (void)commands_.add(ws::app::Command{
            "app.quit", "Quit", "File",
            0,
            ws::app::Shortcut{kKeyQ, ws::app::Mod::super_key},
            false,
            []{ return true; },
            []{ return false; },
            [this](std::string_view) { request_stop(); },
        });

        (void)commands_.add(ws::app::Command{
            "view.fit", "Fit View", "View",
            0,
            ws::app::Shortcut{kKeyF, ws::app::Mod::none},
            false,
            []{ return true; },
            []{ return false; },
            [this](std::string_view) { fit_all(); },
        });

        (void)commands_.add(ws::app::Command{
            "tool.select", "Select Tool", "Tools",
            0,
            ws::app::Shortcut{kKeyV, ws::app::Mod::none},
            false,
            []{ return true; },
            []{ return false; },
            [this](std::string_view) { tool_ = Tool::Select; },
        });

        (void)commands_.add(ws::app::Command{
            "tool.wire", "Wire Tool", "Tools",
            0,
            ws::app::Shortcut{kKeyW, ws::app::Mod::none},
            false,
            []{ return true; },
            []{ return false; },
            [this](std::string_view) { tool_ = Tool::Wire; },
        });

        (void)commands_.add(ws::app::Command{
            "tool.place.box", "Place Box", "Tools",
            0,
            ws::app::Shortcut{49, ws::app::Mod::none}, // 1 key
            false,
            []{ return true; },
            []{ return false; },
            [this](std::string_view) { place_component("Box"); },
        });

        (void)commands_.add(ws::app::Command{
            "tool.place.tall", "Place Tall", "Tools",
            0,
            ws::app::Shortcut{50, ws::app::Mod::none}, // 2 key
            false,
            []{ return true; },
            []{ return false; },
            [this](std::string_view) { place_component("Tall"); },
        });

        (void)commands_.add(ws::app::Command{
            "tool.place.wide", "Place Wide", "Tools",
            0,
            ws::app::Shortcut{51, ws::app::Mod::none}, // 3 key
            false,
            []{ return true; },
            []{ return false; },
            [this](std::string_view) { place_component("Wide"); },
        });

        (void)commands_.add(ws::app::Command{
            "edit.delete", "Delete Selected", "Edit",
            0,
            ws::app::Shortcut{kKeyDelete, ws::app::Mod::none},
            false,
            []{ return true; },
            []{ return false; },
            [this](std::string_view) { delete_selected(); },
        });

        (void)commands_.add(ws::app::Command{
            "edit.rotate", "Rotate", "Edit",
            0,
            ws::app::Shortcut{kKeyR, ws::app::Mod::none},
            false,
            []{ return true; },
            []{ return false; },
            [this](std::string_view) { rotate_selected(); },
        });

        (void)commands_.add(ws::app::Command{
            "edit.mirror", "Mirror", "Edit",
            0,
            ws::app::Shortcut{kKeyM, ws::app::Mod::none},
            false,
            []{ return true; },
            []{ return false; },
            [this](std::string_view) { mirror_selected(); },
        });

        // Set up shortcut resolver
        shortcuts_ = ws::app::ShortcutResolver();
        (void)shortcuts_.install_defaults(commands_, ws::app::KeyContext::global);
    }

    void on_update() override {
        // Resolve shortcuts before the frame (like circsim_editor)
        resolve_chords(ws::input::frame_input());

        // Update camera viewport
        gui::Rect body = layout_.panel_content();
        camera_.vx = body.x;
        camera_.vy = body.y;
        camera_.vwidth = body.width;
        camera_.vheight = body.height;

        // Draw using r2d
        r2d::begin(r2d::Camera2D{
            static_cast<float>(camera_.center.x),
            static_cast<float>(camera_.center.y),
            0.0f,
            static_cast<float>(camera_.zoom)
        });

        draw_grid();
        draw_schematic();

        // Draw UI overlay
        draw_ui();

        r2d::end();
    }

    void resolve_chords(const ws::input::InputFrame& frame) {
        ws::app::Mod mods = ws::app::Mod::none;
        if (frame.key_down[ws::input::keys::kLeftShift] ||
            frame.key_down[ws::input::keys::kRightShift]) {
            mods = mods | ws::app::Mod::shift;
        }
        if (frame.key_down[ws::input::keys::kLeftControl] ||
            frame.key_down[ws::input::keys::kRightControl]) {
            mods = mods | ws::app::Mod::control;
        }
        if (frame.key_down[ws::input::keys::kLeftAlt] ||
            frame.key_down[ws::input::keys::kRightAlt]) {
            mods = mods | ws::app::Mod::alt;
        }
        if (frame.key_down[kLeftSuper] || frame.key_down[kRightSuper]) {
            mods = mods | ws::app::Mod::super_key;
        }
        static constexpr int kModifierKeys[] = {
            ws::input::keys::kLeftShift,   ws::input::keys::kRightShift,
            ws::input::keys::kLeftControl, ws::input::keys::kRightControl,
            ws::input::keys::kLeftAlt,     ws::input::keys::kRightAlt,
            kLeftSuper,                    kRightSuper,
        };
        for (int key = 0; key < ws::input::InputFrame::kMaxKeys; ++key) {
            if (!frame.key_pressed[static_cast<std::size_t>(key)]) {
                continue;
            }
            bool is_modifier = false;
            for (const int mod_key : kModifierKeys) {
                if (key == mod_key) {
                    is_modifier = true;
                    break;
                }
            }
            if (is_modifier) {
                continue;
            }
            const ws::app::Resolution hit = shortcuts_.resolve({key, mods}, ws::app::KeyContext::global);
            if (hit.consumed && !hit.command.empty()) {
                (void)commands_.execute(hit.command);
            }
        }
    }

    void on_event(ws::input::Event& event) override {
        ws::input::EventDispatcher dispatcher{event};
        dispatcher.dispatch<ws::input::WindowCloseEvent>(
            [this](const ws::input::WindowCloseEvent&) {
                request_stop();
                return true;
            });
        // Shortcuts are resolved in on_update using frame_input()
    }

private:
    enum class Tool : uint8_t { Select, Wire, Place };
    Tool tool_ = Tool::Select;

    struct WireDraft {
        bool active = false;
        sch::NodeId start_node = 0;
        int start_dir = 0;
        DVec2 start_pos{0, 0};
        DVec2 cur_pos{0, 0};
    } wire_draft_;

    Component* selected_comp_ = nullptr;
    std::string place_def_ = "Box";

    SchematicDoc doc_;
    Camera camera_;
    ws::ui::r2d::Font font_ui_;
    ws::ui::r2d::Font font_mono_;
    gui::LayoutCursor layout_;
    ws::app::CommandRegistry commands_;
    ws::app::ShortcutResolver shortcuts_;

    void draw_grid() {
        if (!camera_.valid()) return;

        // Grid spacing
        double base_spacing = 1.0; // grid units
        double major_spacing = 5.0;

        // World bounds of viewport
        DVec2 tl = camera_.s2w({camera_.vx, camera_.vy});
        DVec2 br = camera_.s2w({camera_.vx + camera_.vwidth, camera_.vy + camera_.vheight});
        double min_x = std::floor(tl.x / base_spacing) * base_spacing;
        double max_x = std::ceil(br.x / base_spacing) * base_spacing;
        double min_y = std::floor(br.y / base_spacing) * base_spacing;
        double max_y = std::ceil(tl.y / base_spacing) * base_spacing;

        // Minor grid lines
        for (double x = min_x; x <= max_x; x += base_spacing) {
            DVec2 a = camera_.w2s({static_cast<float>(x), static_cast<float>(min_y)});
            DVec2 b = camera_.w2s({static_cast<float>(x), static_cast<float>(max_y)});
            bool major = std::fabs(std::fmod(x, major_spacing)) < 0.001;
            r2d::draw_line(static_cast<float>(a.x), static_cast<float>(a.y),
                          static_cast<float>(b.x), static_cast<float>(b.y),
                          1.0f, major ? r2d::rgba(0x3A, 0x3A, 0x3A, 0xFF) : r2d::rgba(0x2A, 0x2A, 0x2A, 0xFF));
        }
        for (double y = min_y; y <= max_y; y += base_spacing) {
            DVec2 a = camera_.w2s({static_cast<float>(min_x), static_cast<float>(y)});
            DVec2 b = camera_.w2s({static_cast<float>(max_x), static_cast<float>(y)});
            bool major = std::fabs(std::fmod(y, major_spacing)) < 0.001;
            r2d::draw_line(static_cast<float>(a.x), static_cast<float>(a.y),
                          static_cast<float>(b.x), static_cast<float>(b.y),
                          1.0f, major ? r2d::rgba(0x3A, 0x3A, 0x3A, 0xFF) : r2d::rgba(0x2A, 0x2A, 0x2A, 0xFF));
        }

        // Origin axes
        DVec2 ox = camera_.w2s({0.0f, static_cast<float>(min_y)});
        DVec2 ox2 = camera_.w2s({0.0f, static_cast<float>(max_y)});
        r2d::draw_line(static_cast<float>(ox.x), static_cast<float>(ox.y),
                      static_cast<float>(ox2.x), static_cast<float>(ox2.y),
                      1.5f, r2d::rgba(0x4A, 0x4A, 0x4A, 0xFF));
        DVec2 oy = camera_.w2s({static_cast<float>(min_x), 0.0f});
        DVec2 oy2 = camera_.w2s({static_cast<float>(max_x), 0.0f});
        r2d::draw_line(static_cast<float>(oy.x), static_cast<float>(oy.y),
                      static_cast<float>(oy2.x), static_cast<float>(oy2.y),
                      1.5f, r2d::rgba(0x4A, 0x4A, 0x4A, 0xFF));
    }

    void draw_schematic() {
        // Draw wires
        for (const auto& w : doc_.doc.wires) {
            const sch::Node* na = doc_.doc.find_node(w.a);
            const sch::Node* nb = doc_.doc.find_node(w.b);
            if (!na || !nb) continue;

            std::vector<DVec2> path;
            path.push_back(na->pos);
            path.insert(path.end(), w.mid.begin(), w.mid.end());
            path.push_back(nb->pos);

            if (path.size() >= 2) {
                DVec2 prev = camera_.w2s(path[0]);
                for (std::size_t i = 1; i < path.size(); ++i) {
                    DVec2 cur = camera_.w2s(path[i]);
                    r2d::draw_line(static_cast<float>(prev.x), static_cast<float>(prev.y),
                                  static_cast<float>(cur.x), static_cast<float>(cur.y),
                                  2.0f, r2d::rgba(0x00, 0xFF, 0xFF, 0xFF));
                    prev = cur;
                }
            }
        }

        // Draw components
        for (const auto& c : doc_.components) {
            bool sel = (selected_comp_ == &c);

            // Component body rectangle
            DVec2 half_w = {static_cast<double>(c.def->width * 0.5), 0.0};
            DVec2 half_h = {0.0, static_cast<double>(c.def->height * 0.5)};
            half_w = rotate_vec(half_w, c.rot);
            half_h = rotate_vec(half_h, c.rot);
            if (c.mirror) half_w.x = -half_w.x;

            DVec2 corners[4];
            corners[0] = {c.pos.x - half_w.x - half_h.x, c.pos.y - half_w.y - half_h.y};
            corners[1] = {c.pos.x + half_w.x - half_h.x, c.pos.y + half_w.y - half_h.y};
            corners[2] = {c.pos.x + half_w.x + half_h.x, c.pos.y + half_w.y + half_h.y};
            corners[3] = {c.pos.x - half_w.x + half_h.x, c.pos.y - half_w.y + half_h.y};

            for (int i = 0; i < 4; ++i) {
                DVec2 a = camera_.w2s(corners[i]);
                DVec2 b = camera_.w2s(corners[(i + 1) % 4]);
                r2d::draw_line(static_cast<float>(a.x), static_cast<float>(a.y),
                              static_cast<float>(b.x), static_cast<float>(b.y),
                              sel ? 2.5f : 1.5f,
                              sel ? r2d::rgba(0xFF, 0xFF, 0x00, 0xFF) : r2d::rgba(0x00, 0xFF, 0x00, 0xFF));
            }

            // Component name
            DVec2 name_pos = camera_.w2s({c.pos.x, c.pos.y - half_h.y - 0.5});
            if (!font_ui_.empty()) {
                r2d::draw_text(static_cast<float>(name_pos.x), static_cast<float>(name_pos.y),
                              c.def->name.c_str(), font_ui_, r2d::rgba(0xFF, 0xFF, 0xFF, 0xFF));
            }

            // Draw pins
            for (std::size_t i = 0; i < c.pin_nodes.size(); ++i) {
                const sch::Node* n = doc_.doc.find_node(c.pin_nodes[i]);
                if (!n) continue;
                DVec2 p = camera_.w2s(n->pos);
                // Draw pin as small filled circle using draw_quad
                float size = 8.0f;
                r2d::draw_quad(static_cast<float>(p.x) - size*0.5f, static_cast<float>(p.y) - size*0.5f,
                              size, size, r2d::rgba(0xFF, 0x88, 0x00, 0xFF));
                r2d::draw_quad(static_cast<float>(p.x) - size*0.5f - 0.5f, static_cast<float>(p.y) - size*0.5f - 0.5f,
                              size + 1.0f, size + 1.0f, r2d::rgba(0xFF, 0xFF, 0xFF, 0xFF));
            }
        }

        // Draw wire draft preview
        if (wire_draft_.active) {
            const sch::Node* n = doc_.doc.find_node(wire_draft_.start_node);
            if (n) {
                DVec2 a = camera_.w2s(n->pos);
                DVec2 b = camera_.w2s(wire_draft_.cur_pos);
                // Draw dashed line - using multiple segments for now
                r2d::draw_line(static_cast<float>(a.x), static_cast<float>(a.y),
                              static_cast<float>(b.x), static_cast<float>(b.y),
                              1.5f, r2d::rgba(0xFF, 0xFF, 0x00, 0xFF));
            }
        }
    }

    void draw_ui() {
        // Tool indicator
        const char* tool_str = "Select";
        if (tool_ == Tool::Wire) tool_str = "Wire";
        else if (tool_ == Tool::Place) tool_str = "Place";

        DVec2 pos = camera_.w2s({camera_.vx + 10.0f, camera_.vy + 10.0f});
        if (!font_ui_.empty()) {
            r2d::draw_text(static_cast<float>(pos.x), static_cast<float>(pos.y),
                          tool_str, font_ui_, r2d::rgba(0xFF, 0xFF, 0xFF, 0xFF));
        }

        // Help text
        const char* help = "1/2/3: Place Box/Tall/Wide | V: Select | W: Wire | R: Rotate | M: Mirror | Del: Delete | F: Fit | MMB/Space+Drag: Pan | Wheel: Zoom";
        DVec2 help_pos = camera_.w2s({camera_.vx + 10.0f, camera_.vy + camera_.vheight - 30.0f});
        if (!font_mono_.empty()) {
            r2d::draw_text(static_cast<float>(help_pos.x), static_cast<float>(help_pos.y),
                          help, font_mono_, r2d::rgba(0x88, 0x88, 0x88, 0xFF));
        }
    }

    DVec2 snap(DVec2 world) {
        return {std::round(world.x), std::round(world.y)};
    }

    void update_component_nodes(Component& c) {
        // Update component body node
        sch::Node* n = doc_.doc.find_node(c.node_id);
        if (n) n->pos = c.pos;

        // Update pin nodes
        for (std::size_t i = 0; i < c.pin_nodes.size(); ++i) {
            const ComponentPin& pin = c.def->pins[i];
            DVec2 pin_pos = transform_pin(c, pin);
            int pin_dir = transform_dir(pin.dir, c.rot, c.mirror);
            sch::Node* pn = doc_.doc.find_node(c.pin_nodes[i]);
            if (pn) {
                pn->pos = pin_pos;
            }
            doc_.pin_dirs[c.pin_nodes[i]] = pin_dir;
        }
    }

    void place_component(const std::string& def_name) {
        tool_ = Tool::Place;
        place_def_ = def_name;
    }

    void delete_selected() {
        if (selected_comp_) {
            doc_.remove_component(selected_comp_->id);
            selected_comp_ = nullptr;
        }
    }

    void rotate_selected() {
        if (selected_comp_) {
            selected_comp_->rot = (selected_comp_->rot + 90) % 360;
            update_component_nodes(*selected_comp_);
            reroute_wires();
        }
    }

    void mirror_selected() {
        if (selected_comp_) {
            selected_comp_->mirror = !selected_comp_->mirror;
            update_component_nodes(*selected_comp_);
            reroute_wires();
        }
    }

    void reroute_wires() {
        // Re-route all wires using the schematic library's resolve_move
        sch::MoveSession session;
        for (const auto& w : doc_.doc.wires) {
            const sch::Node* na = doc_.doc.find_node(w.a);
            const sch::Node* nb = doc_.doc.find_node(w.b);
            if (!na || !nb) continue;
            session.ends.push_back({w.id, true, na->pos});
            session.ends.push_back({w.id, false, nb->pos});
        }
        if (!session.ends.empty()) {
            sch::Overlay ov = sch::resolve_move(doc_.doc, session);
            // Apply overlay to doc
            for (const auto& [wid, mids] : ov.wirePath) {
                sch::Wire* w = doc_.doc.find_wire(wid);
                if (w) {
                    w->mid.clear();
                    w->mid.insert(w->mid.end(), mids.begin(), mids.end());
                }
            }
        }
    }

    void fit_all() {
        std::vector<DVec2> points;
        for (const auto& c : doc_.components) {
            points.push_back(c.pos);
            for (const auto& pin : c.def->pins) {
                points.push_back(transform_pin(c, pin));
            }
        }
        for (const auto& w : doc_.doc.wires) {
            const sch::Node* na = doc_.doc.find_node(w.a);
            const sch::Node* nb = doc_.doc.find_node(w.b);
            if (na) points.push_back(na->pos);
            if (nb) points.push_back(nb->pos);
            for (const auto& m : w.mid) points.push_back(m);
        }
        camera_.fit(points, 3.0);
    }

    DVec2 drag_start_{0, 0};
    DVec2 drag_comp_pos_{0, 0};
    DVec2 pan_start_{0, 0};
};

} // namespace

#ifdef __ANDROID__
// Android entry: native_app_glue calls android_main on the app's own thread
// and hands over the handle the shell needs (AppConfig::app_handle).
extern "C" void android_main(android_app* app) {
    chdir(app->activity->internalDataPath);
    ws::ui::AppConfig config;
    config.title = "Schematic Test";
    config.app_handle = app;
    SchematicTestApp{config}.run();
}
#else

int main(int /*argc*/, char** /*argv*/) {
    // Single instance
    ws::platform::SingleInstance single("schematic_test");
    if (!single.is_primary()) {
        fprintf(stderr, "Another instance is already running\n");
        return 0;
    }

    // Create and run app
    ws::ui::AppConfig cfg;
    cfg.title = "Schematic Test";
    cfg.width = 1280;
    cfg.height = 720;

    SchematicTestApp app(cfg);
    return app.run();
}

#endif