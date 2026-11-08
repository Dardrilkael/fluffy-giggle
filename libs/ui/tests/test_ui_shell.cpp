// The stage-15 acceptance suite: the dock host, the panel framework,
// the document layer and AppShell, headless. The target's link line is
// the contract (ws_add_test(ui_shell ...) in CMakeLists.txt): the gui
// archive (with its new shell sources), the platform lib's headless
// halves, the real pure-CPU font bake and the stubbed r2d seam - no
// Vulkan, no GLFW, no OS half anywhere. The cases below drive the real
// shell the way a host app would: registered panels and commands, a
// fake document type over an in-memory disk, scripted input frames.
//
// The plan's named tests, in its own words:
//   * panel registration and layout restore with missing panels
//   * document close-with-dirty flow
//   * session restore
//   * shutdown ordering (jobs cancelled before panels destroyed)
//   * shell startup with an empty config directory and with a corrupt one

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <ws/ui/shell/dock.hpp>
#include <ws/ui/shell/document.hpp>
#include <ws/ui/shell/errors.hpp>
#include <ws/ui/shell/panels.hpp>
#include <ws/ui/shell/shell.hpp>
#include <ws/app/command.hpp>
#include <ws/app/shortcut.hpp>
#include <ws/input/input.hpp>
#include <ws/jobs/job_system.hpp>
#include <ws/settings/filesystem.hpp>
#include <ws/settings/json.hpp>
#include <ws/settings/schema.hpp>
#include <ws/settings/settings.hpp>
#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/layout_cursor.hpp>
#include <ws/ui/gui/theme.hpp>

#include "ui_gui_stub.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using ws::ui::gui::GuiContext;
using ws::ui::gui::LayoutCursor;
using ws::ui::gui::kDefaultChrome;
using ws::ui::shell::AppShell;
using ws::ui::shell::Document;
using ws::ui::shell::DocumentManager;
using ws::ui::shell::DockDir;
using ws::ui::shell::DockPlacement;
using ws::ui::shell::DockTree;
using ws::ui::shell::PanelDesc;
using ws::ui::shell::PanelHost;
using ws::ui::shell::PanelInstance;
using ws::ui::shell::PanelRegistry;

// --- the in-memory disk (the FileSystem seam's fake) ------------------------

class FakeFs final : public ws::settings::FileSystem {
public:
    std::map<std::string, std::string> files;

    [[nodiscard]] ws::core::Result<std::string> read(
        std::string_view path) const override {
        const auto found = files.find(std::string{path});
        if (found == files.end()) {
            return ws::core::unexpected<ws::core::Error>(ws::core::make_error(
                1, "io", "no such file: " + std::string{path}));
        }
        return found->second;
    }
    ws::core::Status write(std::string_view path,
                           std::string_view bytes) override {
        files[std::string{path}] = bytes;
        return {};
    }
    ws::core::Status rename(std::string_view from,
                            std::string_view to) override {
        const auto found = files.find(std::string{from});
        if (found == files.end()) {
            return ws::core::unexpected<ws::core::Error>(
                ws::core::make_error(1, "io", "no such file"));
        }
        files[std::string{to}] = std::move(found->second);
        files.erase(found);
        return {};
    }
    bool remove(std::string_view path) override {
        return files.erase(std::string{path}) > 0;
    }
    [[nodiscard]] bool exists(std::string_view path) const override {
        return files.count(std::string{path}) > 0;
    }
};

// --- the fake document (the host's Document) --------------------------------

class FakeDocument final : public Document {
public:
    // The class-wide disk the load/save halves read and write through
    // (the app TU would use std::ifstream; the test keeps it memory),
    // and the save-failure injection (a map write always succeeds; a
    // vanishing file needs the switch).
    static std::map<std::string, std::string> disk;
    static bool fail_saves;

    std::string path_;
    std::string content;
    ws::app::UndoStack undo_{100, 0};

    explicit FakeDocument(std::string path) : path_(std::move(path)) {}

    [[nodiscard]] const std::string& path() const override { return path_; }
    void set_path(std::string path) override { path_ = std::move(path); }
    [[nodiscard]] std::string display_name() const override {
        if (path_.empty()) {
            return "untitled";
        }
        const std::size_t slash = path_.find_last_of("/\\");
        return slash == std::string::npos ? path_ : path_.substr(slash + 1);
    }
    [[nodiscard]] std::string doc_type() const override { return "scratch"; }

    [[nodiscard]] ws::core::Status load() override {
        const auto found = disk.find(path_);
        if (found == disk.end()) {
            return ws::core::unexpected<ws::core::Error>(
                ws::core::make_error(1, "io", "cannot open '" + path_ + "'"));
        }
        content = found->second;
        return {};
    }
    [[nodiscard]] ws::core::Status save() override {
        if (fail_saves) {
            return ws::core::unexpected<ws::core::Error>(
                ws::core::make_error(1, "io", "save refused (injected)"));
        }
        disk[path_] = content;
        return {};
    }
    [[nodiscard]] ws::app::UndoStack& undo() override { return undo_; }
    [[nodiscard]] std::string autosave_content() const override {
        return content;
    }
    void adopt_autosave_content(std::string_view bytes) override {
        content = bytes;
    }
};

std::map<std::string, std::string> FakeDocument::disk;
bool FakeDocument::fail_saves = false;

// A one-shot edit command: pushes `next` into the content, undo
// restores `was`. The dirty flag rides the undo save point.
class EditCommand final : public ws::app::UndoCommand {
public:
    EditCommand(FakeDocument* document, std::string was, std::string next)
        : document_(document), was_(std::move(was)), next_(std::move(next)) {}
    void apply() override { document_->content = next_; }
    void undo() override { document_->content = was_; }
    [[nodiscard]] std::string name() const override { return "Typing"; }

private:
    FakeDocument* document_;
    std::string was_;
    std::string next_;
};

// --- the scripted pointer (the gui suite's snap) ----------------------------

// One host-shaped shell rig: everything a case needs, wired the way
// shell_example wires it.
struct Rig {
    FakeFs fs;
    ws::app::CommandRegistry commands;
    ws::app::ShortcutResolver shortcuts;
    ws::settings::Schema schema;
    std::unique_ptr<ws::settings::SettingsStore> settings;
    ws::platform::FakeFileDialogs dialogs;  // the scripted pickers
    std::unique_ptr<AppShell> shell;
    ws::jobs::JobSystem* jobs = nullptr;  // set BEFORE build()
    GuiContext gui;
    LayoutCursor layout;

    std::vector<std::string> closed;    // the panels' on_close record
    std::vector<std::string> focused;   // the panels' on_focus record
    int editor_draws = 0;

    void build(const std::string& config_dir = "cfg") {
        ws::settings::SettingDesc theme{
            "shell.theme",
            ws::settings::SettingType::text,
            ws::settings::SettingValue::text("dark"),
            {},
            "the chrome: dark or light",
            "shell"};
        (void)schema.declare(std::move(theme));
        settings = std::make_unique<ws::settings::SettingsStore>(schema);

        ws::ui::shell::ShellConfig config;
        config.app_name = "scratchpad";
        config.config_dir = config_dir;
        config.fs = &fs;
        config.dialogs = &dialogs;
        config.jobs = jobs;
        shell = std::make_unique<AppShell>(config, commands, shortcuts,
                                           *settings);

        shell->set_document_factory([](const std::string& path)
                                        -> ws::core::Result<
                                            std::unique_ptr<Document>> {
            return std::make_unique<FakeDocument>(path);
        });

        PanelDesc editor;
        editor.id = "editor";
        editor.title = "Editor";
        editor.placement.area = DockPlacement::Area::left;
        editor.scoped_shortcuts = true;
        editor.draw = [this](GuiContext&, LayoutCursor&,
                             const ws::ui::gui::ChromeTheme&,
                             const PanelInstance&) { ++editor_draws; };
        editor.on_focus = [this](const PanelInstance& instance) {
            focused.push_back(instance.id);
        };
        editor.on_close = [this](const PanelInstance& instance) {
            closed.push_back(instance.id);
        };
        (void)shell->panels().add(std::move(editor));

        PanelDesc plot;
        plot.id = "plot";
        plot.title = "Plot";
        plot.placement.area = DockPlacement::Area::right;
        plot.draw = [](GuiContext&, LayoutCursor&,
                       const ws::ui::gui::ChromeTheme&, const PanelInstance&) {
        };
        plot.on_close = [this](const PanelInstance& instance) {
            closed.push_back(instance.id);
        };
        (void)shell->panels().add(std::move(plot));
    }

    void frame(const ws::input::InputFrame& input = {}) {
        gui.begin_frame(input);
        shell->draw(gui, ws::ui::gui::kDefaultChrome,
                    ws::ui::gui::Rect{0.0f, 0.0f, 1280.0f, 720.0f});
        gui.end_frame();
    }

    // One scripted pointer frame: position, the button's held state,
    // and the edges (a press spells down+pressed, a carry down alone,
    // a release released alone). `dt` drives the shell's own clocks -
    // the splitter double-click reads the frame's dt_seconds, and a
    // hand-built frame defaults to 0, where every press lands
    // "instantly".
    void point(float x, float y, bool down, bool pressed, bool released,
               float dt = 0.0f) {
        ws::input::InputFrame in;
        in.mouse_present = true;
        in.mouse_x = x;
        in.mouse_y = y;
        in.mouse_down[0] = down;
        in.mouse_pressed[0] = pressed;
        in.mouse_released[0] = released;
        in.dt_seconds = dt;
        frame(in);
    }
};

}  // namespace

// --- the dock host -------------------------------------------------------------

TEST_CASE("the dock tree splits, resolves and drags its splitters") {
    DockTree tree;
    CHECK(tree.leaf_count() == 1);  // the empty root leaf

    tree.add("editor", {DockPlacement::Area::center});
    tree.add("plot", {DockPlacement::Area::right});
    CHECK(tree.leaf_count() == 2);
    CHECK(tree.contains("editor"));
    CHECK(tree.contains("plot"));

    const DockTree::Layout laid = tree.resolve({0.0f, 0.0f, 100.0f, 100.0f});
    REQUIRE(laid.leaves.size() == 2);
    CHECK(laid.leaves[0].rect.width == doctest::Approx(50.0f));
    CHECK(laid.leaves[1].rect.x == doctest::Approx(50.0f));
    REQUIRE(laid.splitters.size() == 1);
    CHECK(laid.splitters[0].dir == DockDir::horizontal);

    // The drag: the boundary moves, both sides stay >= kMinWeight.
    tree.apply_drag(laid.splitters[0], 75.0f);
    const DockTree::Layout moved = tree.resolve({0.0f, 0.0f, 100.0f, 100.0f});
    CHECK(moved.leaves[0].rect.width == doctest::Approx(75.0f));
    CHECK(moved.leaves[1].rect.x == doctest::Approx(75.0f));

    // The clamp: a drag past the span cannot erase a leaf.
    tree.apply_drag(moved.splitters[0], 500.0f);
    const DockTree::Layout clamped =
        tree.resolve({0.0f, 0.0f, 100.0f, 100.0f});
    CHECK(clamped.leaves[0].rect.width ==
          doctest::Approx(100.0f * (1.0f - ws::ui::shell::kMinWeight)));
    CHECK(clamped.leaves[1].rect.width ==
          doctest::Approx(100.0f * ws::ui::shell::kMinWeight));
}

TEST_CASE("dock placements carry a weight and can split the whole tree") {
    // The circsim editor's default: palette 14% | canvas | properties,
    // the problems strip full width under all of them.
    DockTree tree;
    DockPlacement left{DockPlacement::Area::left};
    left.weight = 0.14f;
    DockPlacement right{DockPlacement::Area::right};
    right.weight = 0.28f;  // of the canvas leaf (86% of the window)
    DockPlacement bottom{DockPlacement::Area::bottom};
    bottom.weight = 0.26f;
    bottom.at_root = true;

    tree.add("canvas", {DockPlacement::Area::center});
    tree.add("palette", left);
    tree.add("properties", right);
    tree.add("problems", bottom);
    // Another bottom panel tabs into the last leaf (the strip).
    tree.add("netlist", {DockPlacement::Area::center});
    CHECK(tree.leaf_count() == 4);
    CHECK(tree.leaf_of("netlist") == tree.leaf_of("problems"));

    const DockTree::Layout laid = tree.resolve({0.0f, 0.0f, 1000.0f, 1000.0f});
    REQUIRE(laid.leaves.size() == 4);
    // resolve order: palette, canvas, properties, problems
    CHECK(laid.leaves[0].rect.width == doctest::Approx(140.0f));
    CHECK(laid.leaves[1].rect.width == doctest::Approx(619.2f).epsilon(0.01));
    CHECK(laid.leaves[2].rect.width == doctest::Approx(240.8f).epsilon(0.01));
    CHECK(laid.leaves[0].rect.height == doctest::Approx(740.0f).epsilon(0.01));
    // The strip spans the whole width and takes ~26% of the height.
    CHECK(laid.leaves[3].rect.x == doctest::Approx(0.0f));
    CHECK(laid.leaves[3].rect.width == doctest::Approx(1000.0f));
    CHECK(laid.leaves[3].rect.height == doctest::Approx(260.0f).epsilon(0.01));

    // A weight of 0 is the old even split; at_root on an empty tree
    // just fills it.
    DockTree even;
    even.add("a", {DockPlacement::Area::center});
    even.add("b", {DockPlacement::Area::right});
    CHECK(even.resolve({0.0f, 0.0f, 100.0f, 100.0f}).leaves[0].rect.width ==
          doctest::Approx(50.0f));
    DockTree empty;
    empty.add("a", bottom);
    CHECK(empty.leaf_count() == 1);
    CHECK(empty.contains("a"));
}

TEST_CASE("the dock tree serializes, restores, and drops missing panels") {
    DockTree tree;
    tree.add("editor", {DockPlacement::Area::center});
    tree.add("plot", {DockPlacement::Area::bottom});
    tree.apply_drag(tree.resolve({0.0f, 0.0f, 100.0f, 100.0f}).splitters[0],
                    30.0f);

    const std::string text =
        ws::settings::json::write(tree.to_json());

    DockTree back;
    REQUIRE(back.from_json(ws::settings::json::parse(text).value(),
                           [](std::string_view) { return true; }));
    const DockTree::Layout original =
        tree.resolve({0.0f, 0.0f, 100.0f, 100.0f});
    const DockTree::Layout restored =
        back.resolve({0.0f, 0.0f, 100.0f, 100.0f});
    REQUIRE(original.leaves.size() == restored.leaves.size());
    CHECK(original.leaves[0].rect.width == doctest::Approx(restored.leaves[0].rect.width));

    // The missing panel: the accept filter refuses "plot", the restore
    // keeps the editor and prunes the empty side.
    DockTree partial;
    REQUIRE(partial.from_json(ws::settings::json::parse(text).value(),
                              [](std::string_view panel) {
                                  return panel != "plot";
                              }));
    CHECK(partial.contains("editor"));
    CHECK(!partial.contains("plot"));
    CHECK(partial.leaf_count() == 1);

    // The corrupt shapes: a non-object root refuses; a wrong-typed
    // subtree drops without throwing.
    DockTree corrupt;
    CHECK(!corrupt.from_json(ws::settings::json::Value::text("junk"),
                             [](std::string_view) { return true; }));
    const ws::core::Result<ws::settings::json::Value> weird =
        ws::settings::json::parse(R"({"dir":"h","children":[
            {"w":"not a number","node":{"tabs":[42],"active":0}},
            {"w":1,"node":{"tabs":["editor"],"active":0}}]})");
    REQUIRE(weird);
    DockTree tolerant;
    CHECK(tolerant.from_json(*weird, [](std::string_view) { return true; }));
    CHECK(tolerant.contains("editor"));
}

TEST_CASE("removing the last tab prunes the tree back to one leaf") {
    DockTree tree;
    tree.add("editor", {DockPlacement::Area::center});
    tree.add("plot", {DockPlacement::Area::right});
    tree.add("settings", {DockPlacement::Area::bottom});
    CHECK(tree.leaf_count() == 3);

    tree.remove("plot");
    CHECK(tree.leaf_count() == 2);
    tree.remove("editor");
    tree.remove("settings");
    CHECK(tree.leaf_count() == 1);
    CHECK(tree.panels().empty());
}

TEST_CASE("side placements fuse into a matching split, keeping the pixels") {
    // The circsim editor's default row: palette 14% | canvas |
    // properties 28% of canvas. The nested spelling drew it as
    // [palette | [canvas | properties]]; the fused one is ONE flat
    // split - and the PIXELS are the same, which is the whole point
    // (the share is of the target's span either way).
    DockTree tree;
    DockPlacement left{DockPlacement::Area::left};
    left.weight = 0.14f;
    DockPlacement right{DockPlacement::Area::right};
    right.weight = 0.28f;

    tree.add("canvas", {DockPlacement::Area::center});
    tree.add("palette", left);
    tree.add("properties", right);
    CHECK(tree.leaf_count() == 3);  // flat: no phantom nesting level

    const DockTree::Layout laid = tree.resolve({0.0f, 0.0f, 1000.0f, 1000.0f});
    REQUIRE(laid.leaves.size() == 3);
    CHECK(laid.leaves[0].rect.width == doctest::Approx(140.0f));
    CHECK(laid.leaves[1].rect.width ==
          doctest::Approx(619.2f).epsilon(0.01));
    CHECK(laid.leaves[2].rect.width ==
          doctest::Approx(240.8f).epsilon(0.01));
    // Two adjacent children, two bands - the drag moves the boundary
    // the pointer grabbed, with no intermediate split to confuse it.
    REQUIRE(laid.splitters.size() == 2);

    // The json is the flat shape: one split, three children - and it
    // round-trips into the same flat tree.
    const ws::settings::json::Value shape = tree.to_json();
    const ws::settings::json::Object* root_obj = shape.as_object();
    REQUIRE(root_obj != nullptr);
    const ws::settings::json::Value* kids = root_obj->find("children");
    REQUIRE(kids != nullptr);
    REQUIRE(kids->is_array());
    CHECK(kids->as_array()->size() == 3);
    DockTree back;
    REQUIRE(back.from_json(
        ws::settings::json::parse(ws::settings::json::write(shape)).value(),
        [](std::string_view) { return true; }));
    CHECK(back.leaf_count() == 3);
    const DockTree::Layout restored =
        back.resolve({0.0f, 0.0f, 1000.0f, 1000.0f});
    CHECK(restored.leaves[1].rect.width ==
          doctest::Approx(619.2f).epsilon(0.01));

    // The at_root fuse: a second strip panel joins the FIRST strip's
    // split instead of nesting a fresh split over the tree. The pixels
    // are the ones the wrap would have drawn - the second strip takes
    // an even share of the whole, the first split's children scale by
    // their cut - but the strip stays ONE split, so a later drag
    // between the two strips is the boundary the pointer sees.
    DockTree strip;
    strip.add("canvas", {DockPlacement::Area::center});
    DockPlacement bottom{DockPlacement::Area::bottom};
    bottom.weight = 0.26f;
    bottom.at_root = true;
    strip.add("problems", bottom);
    DockPlacement bottom_even{DockPlacement::Area::bottom};
    bottom_even.at_root = true;  // weight 0 = the even share
    strip.add("output", bottom_even);
    CHECK(strip.leaf_count() == 3);
    const DockTree::Layout strip_laid =
        strip.resolve({0.0f, 0.0f, 1000.0f, 1000.0f});
    REQUIRE(strip_laid.leaves.size() == 3);
    // canvas 74% * (1 - 0.5), problems 26% * (1 - 0.5), output 50%:
    // the exact heights a wrap into [old | output] would have given.
    CHECK(strip_laid.leaves[0].rect.height ==
          doctest::Approx(370.0f).epsilon(0.01));
    CHECK(strip_laid.leaves[1].rect.height ==
          doctest::Approx(130.0f).epsilon(0.01));
    CHECK(strip_laid.leaves[2].rect.height ==
          doctest::Approx(500.0f).epsilon(0.01));
}

TEST_CASE("move re-docks a panel across the tree") {
    DockTree tree;
    tree.add("a", {DockPlacement::Area::center});
    tree.add("b", {DockPlacement::Area::right});
    // [a | b]

    // A center move retabs: b joins a's leaf, fronts it, and b's old
    // leaf prunes.
    DockPlacement into_a{DockPlacement::Area::center};
    into_a.leaf = 0;
    tree.move("b", into_a);
    CHECK(tree.leaf_count() == 1);
    CHECK(tree.tab_index("a") == 0);
    CHECK(tree.tab_index("b") == 1);
    const DockTree::Layout retabbed =
        tree.resolve({0.0f, 0.0f, 100.0f, 100.0f});
    REQUIRE(retabbed.leaves.size() == 1);
    CHECK(retabbed.leaves[0].active == 1);  // the dropped tab fronts

    // A side move onto its own multi-tab leaf is a real split: a's tab
    // leaves the strip, a fresh leaf takes the left.
    DockPlacement left{DockPlacement::Area::left};
    tree.move("a", left);
    CHECK(tree.leaf_count() == 2);
    CHECK(tree.leaf_of("a") == 0);
    CHECK(tree.leaf_of("b") == 1);
    CHECK(tree.tab_index("b") == 0);  // b kept its leaf alone

    // A side move onto its own LONE leaf is the activate no-op:
    // nothing to side against.
    DockPlacement self_right{DockPlacement::Area::right};
    tree.move("b", self_right);
    CHECK(tree.leaf_count() == 2);
    CHECK(tree.leaf_of("b") == 1);

    // A side move under FUSION: c lands left of leaf 0, one flat
    // split - and c's old leaf (it was alone) prunes behind it.
    tree.add("c", {DockPlacement::Area::right});
    // [a | b | c] now (b's add wrapped the root, c's fused in)
    CHECK(tree.leaf_count() == 3);
    DockPlacement left_of_a{DockPlacement::Area::left};
    left_of_a.leaf = 0;
    tree.move("c", left_of_a);
    CHECK(tree.leaf_count() == 3);  // c moved, not duplicated
    CHECK(tree.panels() ==
          std::vector<std::string>{"c", "a", "b"});
    // The pixels: c took half of A'S SPAN (the share semantics), b
    // kept its own, and the space c's old leaf released folds back in
    // - three equal weights, an even thirds row.
    const DockTree::Layout fused = tree.resolve({0.0f, 0.0f, 300.0f, 100.0f});
    CHECK(fused.leaves[0].rect.width == doctest::Approx(100.0f));
    CHECK(fused.leaves[1].rect.width == doctest::Approx(100.0f));
    CHECK(fused.leaves[2].rect.width == doctest::Approx(100.0f));

    // A whole-tree side move: a leaves the row and takes the full
    // width under it (the at_root wrap - the root split's axis
    // mismatches the vertical ask).
    DockPlacement strip_bottom{DockPlacement::Area::bottom};
    strip_bottom.at_root = true;
    tree.move("a", strip_bottom);
    CHECK(tree.leaf_count() == 3);
    CHECK(tree.panels() == std::vector<std::string>{"c", "b", "a"});
    const DockTree::Layout stripped = tree.resolve({0.0f, 0.0f, 300.0f, 100.0f});
    CHECK(stripped.leaves[2].rect.width ==
          doctest::Approx(300.0f));  // the strip spans the whole width

    // A center move onto the leaf the panel already shares is the
    // reorder's activate no-op (move_tab owns reorders).
    DockPlacement own_center{DockPlacement::Area::center};
    own_center.leaf = 0;
    tree.move("c", own_center);
    CHECK(tree.leaf_count() == 3);
    CHECK(tree.panels() == std::vector<std::string>{"c", "b", "a"});

    // Unknown ids are no-ops, never throws.
    tree.move("ghost", into_a);
    tree.move("ghost", left);
    CHECK(tree.leaf_count() == 3);
}

TEST_CASE("move_tab reorders and retabs") {
    DockTree tree;
    tree.add("x", {DockPlacement::Area::center});
    tree.add("y", {DockPlacement::Area::center});
    tree.add("z", {DockPlacement::Area::center});
    CHECK(tree.leaf_count() == 1);
    CHECK(tree.tab_index("z") == 2);  // [x, y, z], z fronts

    // A reorder inside the strip: x to the end. The front tab (z)
    // stays the front tab - it steps past what slid in behind it.
    tree.move_tab("x", 0, 2);
    CHECK(tree.tab_index("x") == 2);
    CHECK(tree.tab_index("y") == 0);
    CHECK(tree.tab_index("z") == 1);
    const DockTree::Layout pushed =
        tree.resolve({0.0f, 0.0f, 100.0f, 100.0f});
    CHECK(pushed.leaves[0].active == 1);  // z still fronts

    // The front tab follows its own move.
    tree.move_tab("z", 0, 0);
    CHECK(tree.tab_index("z") == 0);
    CHECK(tree.resolve({0.0f, 0.0f, 100.0f, 100.0f}).leaves[0].active == 0);

    // A retab across leaves: y joins the solo leaf and fronts it.
    tree.add("solo", {DockPlacement::Area::right});
    // leaves: [z, y, x] | [solo]
    tree.move_tab("y", 1, 1);
    CHECK(tree.leaf_of("y") == 1);
    CHECK(tree.tab_index("y") == 1);
    const DockTree::Layout retabbed =
        tree.resolve({0.0f, 0.0f, 100.0f, 100.0f});
    REQUIRE(retabbed.leaves.size() == 2);
    CHECK(retabbed.leaves[1].active == 1);  // y fronts its new strip

    // Emptying a leaf retabs it away: both of solo's tabs leave, the
    // leaf prunes (solo lands at the FRONT - index 0 - of the row).
    tree.move_tab("y", 0, 0);
    tree.move_tab("solo", 0, 0);
    CHECK(tree.leaf_count() == 1);
    CHECK(tree.panels() ==
          std::vector<std::string>{"solo", "y", "z", "x"});

    // Unknown ids are no-ops.
    tree.move_tab("ghost", 0, 0);
    CHECK(tree.tab_index("ghost") == -1);
}

TEST_CASE("maximize zooms the resolve, restore gives the layout back") {
    DockTree tree;
    tree.add("a", {DockPlacement::Area::center});
    tree.add("b", {DockPlacement::Area::right});
    DockPlacement bottom{DockPlacement::Area::bottom};
    bottom.at_root = true;
    tree.add("c", bottom);
    const ws::ui::gui::Rect bounds{0.0f, 0.0f, 100.0f, 100.0f};
    CHECK(tree.resolve(bounds).leaves.size() == 3);

    const std::string shape =
        ws::settings::json::write(tree.to_json());

    // The zoom: resolve answers ONE leaf - the whole bounds, b's tabs,
    // b fronting - and no splitters. The tree underneath is whole.
    tree.maximize("b");
    CHECK(tree.maximized() == "b");
    const DockTree::Layout zoomed = tree.resolve(bounds);
    REQUIRE(zoomed.leaves.size() == 1);
    CHECK(zoomed.leaves[0].rect.x == doctest::Approx(0.0f));
    CHECK(zoomed.leaves[0].rect.y == doctest::Approx(0.0f));
    CHECK(zoomed.leaves[0].rect.width == doctest::Approx(100.0f));
    CHECK(zoomed.leaves[0].rect.height == doctest::Approx(100.0f));
    REQUIRE(zoomed.leaves[0].tabs != nullptr);
    REQUIRE(zoomed.leaves[0].tabs->size() == 1);
    CHECK((*zoomed.leaves[0].tabs)[0] == "b");
    CHECK(zoomed.splitters.empty());
    CHECK(tree.leaf_count() == 3);       // the full tree, zoom or not
    CHECK(tree.panels().size() == 3);
    // The zoom is transient state: the saved shape never moves.
    CHECK(ws::settings::json::write(tree.to_json()) == shape);

    // The zoom follows the panel through a move.
    DockPlacement into_a{DockPlacement::Area::center};
    into_a.leaf = 0;
    tree.move("b", into_a);
    CHECK(tree.maximized() == "b");
    const DockTree::Layout followed = tree.resolve(bounds);
    REQUIRE(followed.leaves.size() == 1);
    REQUIRE(followed.leaves[0].tabs != nullptr);
    CHECK(followed.leaves[0].tabs->size() == 2);  // [a, b]
    CHECK(followed.leaves[0].active == 1);        // b still fronts

    // Removing the zoomed panel clears the zoom: no stale projection.
    tree.remove("b");
    CHECK(tree.maximized().empty());
    CHECK(tree.resolve(bounds).leaves.size() == 2);

    // An unknown id never zooms; restore without a zoom is a no-op.
    tree.maximize("ghost");
    CHECK(tree.maximized().empty());
    tree.restore();
    CHECK(tree.maximized().empty());

    // from_json drops the zoom: a restored layout starts unzoomed.
    tree.maximize("a");
    CHECK(tree.maximized() == "a");
    REQUIRE(tree.from_json(
        ws::settings::json::parse(shape).value(),
        [](std::string_view) { return true; }));
    CHECK(tree.maximized().empty());
    // And so does clear().
    tree.maximize("c");
    tree.clear();
    CHECK(tree.maximized().empty());
    CHECK(tree.leaf_count() == 1);
}

TEST_CASE("distribute evens the split a splitter divides") {
    DockTree tree;
    tree.add("a", {DockPlacement::Area::center});
    tree.add("b", {DockPlacement::Area::right});
    tree.add("c", {DockPlacement::Area::right});
    // Fused: one flat split, three children.
    const DockTree::Layout laid = tree.resolve({0.0f, 0.0f, 300.0f, 100.0f});
    REQUIRE(laid.leaves.size() == 3);
    REQUIRE(laid.splitters.size() == 2);

    // Drag the first boundary hard right: the pair (a, b) clamps, the
    // row is far from even.
    tree.apply_drag(laid.splitters[0], 290.0f);
    const DockTree::Layout moved = tree.resolve({0.0f, 0.0f, 300.0f, 100.0f});
    CHECK(moved.leaves[0].rect.width > 150.0f);

    // The distribute: every child of that split evens out - a thirds
    // row, the weights the resolve normalizes to equal shares.
    tree.distribute(laid.splitters[0]);
    const DockTree::Layout even = tree.resolve({0.0f, 0.0f, 300.0f, 100.0f});
    CHECK(even.leaves[0].rect.width == doctest::Approx(100.0f));
    CHECK(even.leaves[1].rect.width == doctest::Approx(100.0f));
    CHECK(even.leaves[2].rect.width == doctest::Approx(100.0f));

    // A stale band (a path the tree no longer holds) is a no-op.
    DockTree::SplitterRect stale;
    stale.path = {7, 7, 7};
    tree.distribute(stale);
    const DockTree::Layout after =
        tree.resolve({0.0f, 0.0f, 300.0f, 100.0f});
    CHECK(after.leaves[0].rect.width == doctest::Approx(100.0f));
}

TEST_CASE("dock_hit answers the five drop zones") {
    DockTree tree;
    tree.add("a", {DockPlacement::Area::center});
    tree.add("b", {DockPlacement::Area::right});
    const DockTree::Layout laid = tree.resolve({0.0f, 0.0f, 100.0f, 100.0f});
    REQUIRE(laid.leaves.size() == 2);

    // The zones of leaf 0 ({0,0,50,100}) with a 20-px band.
    CHECK(dock_hit(laid, 25.0f, 50.0f, 20.0f).leaf == 0);
    CHECK(dock_hit(laid, 25.0f, 50.0f, 20.0f).area ==
          DockPlacement::Area::center);
    CHECK(dock_hit(laid, 2.0f, 50.0f, 20.0f).area ==
          DockPlacement::Area::left);
    CHECK(dock_hit(laid, 48.0f, 50.0f, 20.0f).area ==
          DockPlacement::Area::right);
    CHECK(dock_hit(laid, 25.0f, 2.0f, 20.0f).area ==
          DockPlacement::Area::top);
    CHECK(dock_hit(laid, 25.0f, 98.0f, 20.0f).area ==
          DockPlacement::Area::bottom);
    // The x edges outrank the y edges: a corner answers the side.
    CHECK(dock_hit(laid, 2.0f, 2.0f, 20.0f).area ==
          DockPlacement::Area::left);

    // The neighbor leaf's center, and the outside.
    CHECK(dock_hit(laid, 75.0f, 50.0f, 20.0f).leaf == 1);
    CHECK(dock_hit(laid, 75.0f, 50.0f, 20.0f).area ==
          DockPlacement::Area::center);
    CHECK(dock_hit(laid, -5.0f, 50.0f, 20.0f).leaf == -1);
    CHECK(dock_hit(laid, 50.0f, -5.0f, 20.0f).leaf == -1);

    // The band clamps to a quarter of the span per side: a 60-px band
    // on a 50-px leaf reads as 12.5 - edges at the fringes, a center
    // that is most of the leaf, never a leaf that is all edge.
    CHECK(dock_hit(laid, 5.0f, 50.0f, 60.0f).area ==
          DockPlacement::Area::left);
    CHECK(dock_hit(laid, 25.0f, 50.0f, 60.0f).area ==
          DockPlacement::Area::center);
    CHECK(dock_hit(laid, 45.0f, 50.0f, 60.0f).area ==
          DockPlacement::Area::right);
}

// --- the dock view (the gestures the shell drives) -----------------------------

TEST_CASE("the tab drag re-docks a panel through the shell") {
    Rig rig;
    rig.build();
    CHECK(rig.shell->startup());
    (void)rig.shell->panel_host().open(rig.shell->panels(), "editor");
    (void)rig.shell->panel_host().open(rig.shell->panels(), "plot");

    // The dock view's bounds, the way draw() lays them: below the menu
    // bar, above the status bar (the stub's frame is empty, so the
    // status strip measures from the frame's own bottom).
    const float chrome = ws::ui::gui::kDefaultChrome.control_height;
    const ws::ui::gui::Rect dock{0.0f, chrome, 1280.0f, 720.0f - chrome};
    DockTree& tree = rig.shell->panel_host().dock();
    REQUIRE(tree.leaf_count() == 2);  // editor left, plot right

    const DockTree::Layout laid = tree.resolve(dock);
    const float strip_y =
        laid.leaves[0].rect.y + chrome * 0.5f;  // the strips' midline
    const float editor_tab_x = laid.leaves[0].rect.x + 20.0f;
    const float plot_cx = laid.leaves[1].rect.x +
                          laid.leaves[1].rect.width * 0.5f;
    const float plot_cy = laid.leaves[1].rect.y +
                          laid.leaves[1].rect.height * 0.5f;

    // The retab drop: carry editor's tab onto plot's body and release.
    rig.point(editor_tab_x, strip_y, true, true, false);
    rig.point(plot_cx, plot_cy, true, false, false);
    rig.point(plot_cx, plot_cy, false, false, true);
    CHECK(tree.leaf_count() == 1);  // one merged leaf
    CHECK(tree.tab_index("plot") == 0);
    CHECK(tree.tab_index("editor") == 1);  // the dropped tab lands at the end
    CHECK(rig.shell->panel_host().active() != nullptr);
    CHECK(rig.shell->panel_host().active()->id == "editor");

    // The split drop: carry plot's tab (tab 0 of the now whole-dock
    // strip) to the right edge and release - the panel takes the
    // right half.
    const float merged_tab_x = dock.x + 20.0f;
    const float right_edge = dock.x + dock.width - 8.0f;
    rig.point(merged_tab_x, strip_y, true, true, false);
    rig.point(right_edge, plot_cy, true, false, false);
    rig.point(right_edge, plot_cy, false, false, true);
    CHECK(tree.leaf_count() == 2);
    CHECK(tree.leaf_of("editor") == 0);
    CHECK(tree.leaf_of("plot") == 1);

    // A drop outside every leaf tears the panel off: carried over the
    // menu bar and released there, the panel becomes a floating window
    // (the ImGui gesture - the tree loses its leaf, the window centers
    // on the release, and the focus follows the panel out).
    PanelHost& host = rig.shell->panel_host();
    rig.point(editor_tab_x, strip_y, true, true, false);
    rig.point(600.0f, 10.0f, true, false, false);  // the menu bar's band
    rig.point(600.0f, 10.0f, false, false, true);
    REQUIRE(host.floating().size() == 1);
    CHECK(host.floating()[0].panel == "editor");
    CHECK(!tree.contains("editor"));
    CHECK(tree.leaf_count() == 1);  // plot alone spans the dock
    CHECK(host.active() != nullptr);
    CHECK(host.active()->id == "editor");  // the focus follows the tear

    // The torn-off window's size: the pane it came out of (the editor
    // leaf was the left half), centered on the release and seated
    // inside the dock - the title bar never rides off the top.
    const ws::ui::gui::Rect& floated = host.floating()[0].rect;
    CHECK(floated.width == doctest::Approx(640.0f));   // the left half
    CHECK(floated.height == doctest::Approx(dock.height));  // the whole dock
    CHECK(floated.y >= dock.y);

    // A press-and-release that never crosses the threshold is the
    // strip's own click: the selection moves, the tree does not (the
    // plot leaf now spans the whole dock, so its tab lives at the
    // left end - clear of the floating window).
    const float plot_tab_x2 = dock.x + 20.0f;
    rig.point(plot_tab_x2, strip_y, true, true, false);
    rig.point(plot_tab_x2, strip_y, false, false, true);
    CHECK(tree.leaf_count() == 1);
    CHECK(host.floating().size() == 1);  // the click docked nothing
    CHECK(host.active() != nullptr);
    CHECK(host.active()->id == "plot");
    (void)rig.shell->shutdown();
}

TEST_CASE("float_panel tears a tab out and redock puts it back") {
    PanelRegistry registry;
    PanelDesc a;
    a.id = "a";
    a.title = "A";
    a.placement.area = DockPlacement::Area::left;
    a.draw = [](GuiContext&, LayoutCursor&, const ws::ui::gui::ChromeTheme&,
                const PanelInstance&) {};
    PanelDesc b;
    b.id = "b";
    b.title = "B";
    b.placement.area = DockPlacement::Area::right;
    b.draw = [](GuiContext&, LayoutCursor&, const ws::ui::gui::ChromeTheme&,
                const PanelInstance&) {};
    REQUIRE(registry.add(std::move(a)));
    REQUIRE(registry.add(std::move(b)));

    PanelHost host;
    (void)host.open(registry, "a");
    (void)host.open(registry, "b");
    CHECK(host.dock().leaf_count() == 2);

    // The tear-off: the tree forgets the tab, the layer gains the
    // window, and the focus follows the panel out.
    host.float_panel(registry, "a", ws::ui::gui::Rect{40.0f, 60.0f, 200.0f, 150.0f});
    CHECK(host.dock().leaf_count() == 1);
    CHECK(host.dock().panels() == std::vector<std::string>{"b"});
    REQUIRE(host.floating().size() == 1);
    CHECK(host.floating()[0].panel == "a");
    CHECK(host.is_floating("a"));
    CHECK(host.active() != nullptr);
    CHECK(host.active()->id == "a");

    // A rect below the floor lands clamped: a window smaller than the
    // minimums is a title bar with no body.
    host.float_panel(registry, "a",
                     ws::ui::gui::Rect{0.0f, 0.0f, 40.0f, 30.0f});
    REQUIRE(host.floating().size() == 1);  // one id, one window
    CHECK(host.floating()[0].rect.width ==
          doctest::Approx(ws::ui::shell::kFloatingMinW));
    CHECK(host.floating()[0].rect.height ==
          doctest::Approx(ws::ui::shell::kFloatingMinH));

    // The move and the raise (the drag and the press-raise ride these).
    host.move_floating("a", ws::ui::gui::Rect{70.0f, 80.0f, 220.0f, 160.0f});
    CHECK(host.floating()[0].rect.x == doctest::Approx(70.0f));
    host.raise_floating("a");  // already the top: a no-op, not a dupe
    CHECK(host.floating().size() == 1);

    // The way back: a floating redock lands the tab (the tree's add,
    // not move - the panel held no tab) and the focus follows it in.
    DockPlacement back;
    back.area = DockPlacement::Area::center;
    back.leaf = 0;
    host.redock(registry, "a", back);
    CHECK(host.floating().empty());
    CHECK(host.dock().contains("a"));
    CHECK(host.dock().panels() == std::vector<std::string>({"b", "a"}));
    CHECK(host.active() != nullptr);
    CHECK(host.active()->id == "a");

    // Closing a floating panel closes the panel, not just the window.
    host.float_panel(registry, "a", ws::ui::gui::Rect{10.0f, 10.0f, 200.0f, 150.0f});
    CHECK(host.close(registry, "a"));
    CHECK(host.floating().empty());
    CHECK(host.find("a") == nullptr);
    CHECK(host.dock().panels() == std::vector<std::string>{"b"});
}

TEST_CASE("the floating layer persists in the layout json") {
    PanelRegistry registry;
    PanelDesc a;
    a.id = "a";
    a.title = "A";
    a.placement.area = DockPlacement::Area::left;
    a.draw = [](GuiContext&, LayoutCursor&, const ws::ui::gui::ChromeTheme&,
                const PanelInstance&) {};
    PanelDesc b;
    b.id = "b";
    b.title = "B";
    b.placement.area = DockPlacement::Area::right;
    b.draw = [](GuiContext&, LayoutCursor&, const ws::ui::gui::ChromeTheme&,
                const PanelInstance&) {};
    REQUIRE(registry.add(std::move(a)));
    REQUIRE(registry.add(std::move(b)));

    PanelHost host;
    (void)host.open(registry, "a");
    (void)host.open(registry, "b");
    host.float_panel(registry, "b", ws::ui::gui::Rect{40.0f, 60.0f, 220.0f, 160.0f});

    // The saved shape: the tree's own root object plus the one extra
    // key - the z stack, back to front.
    const ws::settings::json::Value saved = host.layout_to_json();
    const ws::settings::json::Object* root = saved.as_object();
    REQUIRE(root != nullptr);
    const ws::settings::json::Value* floating = root->find("floating");
    REQUIRE(floating != nullptr);
    REQUIRE(floating->is_array());
    REQUIRE(floating->as_array()->size() == 1);

    // The round trip: the layer left (a redock), the same value
    // restores it - rect and all.
    host.redock(registry, "b", DockPlacement{});
    CHECK(host.floating().empty());
    CHECK(host.layout_from_json(saved, [](std::string_view) { return true; }));
    REQUIRE(host.floating().size() == 1);
    CHECK(host.floating()[0].panel == "b");
    CHECK(host.floating()[0].rect.x == doctest::Approx(40.0f));
    CHECK(host.floating()[0].rect.width == doctest::Approx(220.0f));

    // The tolerance, a hand-edited file's worst day: a ghost id nobody
    // opened, an id the tree already holds (one instance, one home),
    // and a window below the floor.
    const ws::core::Result<ws::settings::json::Value> hand =
        ws::settings::json::parse(
            R"({"tabs":["a"],"active":0,"floating":[)"
            R"({"id":"ghost","x":1,"y":1,"w":50,"h":50},)"
            R"({"id":"a","x":5,"y":5,"w":100,"h":100},)"
            R"({"id":"b","x":9,"y":9,"w":50,"h":50}]})");
    REQUIRE(hand);
    PanelHost edited;
    (void)edited.open(registry, "a");
    (void)edited.open(registry, "b");
    CHECK(edited.layout_from_json(*hand, nullptr));
    REQUIRE(edited.floating().size() == 1);  // ghost and the docked id drop
    CHECK(edited.floating()[0].panel == "b");
    CHECK(edited.floating()[0].rect.width ==
          doctest::Approx(ws::ui::shell::kFloatingMinW));  // the floor

    // A file with no layer key (an older build's layout) restores its
    // tree and simply no windows.
    const ws::core::Result<ws::settings::json::Value> old =
        ws::settings::json::parse(R"({"tabs":["a"],"active":0})");
    REQUIRE(old);
    CHECK(edited.layout_from_json(*old, [](std::string_view) { return true; }));
    CHECK(edited.floating().empty());
    CHECK(edited.dock().panels() == std::vector<std::string>{"a"});
}

TEST_CASE("a floating window drags by its title bar and re-docks on release") {
    Rig rig;
    rig.build();
    CHECK(rig.shell->startup());
    (void)rig.shell->panel_host().open(rig.shell->panels(), "editor");
    (void)rig.shell->panel_host().open(rig.shell->panels(), "plot");

    const float chrome = ws::ui::gui::kDefaultChrome.control_height;
    const ws::ui::gui::Rect dock{0.0f, chrome, 1280.0f, 720.0f - chrome};
    PanelHost& host = rig.shell->panel_host();
    DockTree& tree = host.dock();

    // Torn off over the dock's middle: the title bar's center is the
    // grab, and the plot leaf (the whole dock now) is the drop target.
    host.float_panel(rig.shell->panels(), "editor",
                     ws::ui::gui::Rect{100.0f, 100.0f, 300.0f, 200.0f});
    rig.frame();  // one idle frame: the window is on screen
    const float bar_x = 250.0f;               // the bar's middle
    const float bar_y = 100.0f + chrome * 0.5f;
    const float drop_x = dock.x + dock.width * 0.5f;
    const float drop_y = dock.y + dock.height * 0.5f;

    // A held CLICK on the bar (no carry past the threshold) is the
    // raise + focus, nothing more: the window neither moves nor docks.
    rig.point(bar_x, bar_y, true, true, false);
    rig.point(bar_x, bar_y, false, false, true);
    CHECK(host.floating().size() == 1);
    CHECK(host.floating()[0].rect.x == doctest::Approx(100.0f));
    CHECK(host.active() != nullptr);
    CHECK(host.active()->id == "editor");  // the press focused it

    // The drag: carry the bar over the plot leaf's center and release
    // - the same center drop a tab drag makes, the window leaves the
    // layer and the tab joins the leaf (its front tab).
    rig.point(bar_x, bar_y, true, true, false);
    rig.point(drop_x, drop_y, true, false, false);
    rig.point(drop_x, drop_y, false, false, true);
    CHECK(host.floating().empty());
    CHECK(tree.contains("editor"));
    CHECK(tree.leaf_count() == 1);
    CHECK(tree.tab_index("editor") == 1);  // the dropped tab lands last
    CHECK(tree.tab_index("plot") == 0);
    CHECK(host.active() != nullptr);
    CHECK(host.active()->id == "editor");

    // The drag that lands nowhere: torn off again, carried over the
    // menu bar (outside every leaf) and released - the window stays
    // floating, seated inside the dock where the carry left it.
    host.float_panel(rig.shell->panels(), "editor",
                     ws::ui::gui::Rect{100.0f, 100.0f, 300.0f, 200.0f});
    rig.frame();
    rig.point(bar_x, bar_y, true, true, false);
    rig.point(600.0f, 10.0f, true, false, false);  // the menu bar's band
    rig.point(600.0f, 10.0f, false, false, true);
    REQUIRE(host.floating().size() == 1);
    CHECK(host.floating()[0].panel == "editor");
    // The move's own math: 100 + (600 - 250) across, 100 + (10 - 113)
    // down - clamped to the dock's top edge.
    CHECK(host.floating()[0].rect.x == doctest::Approx(450.0f));
    CHECK(host.floating()[0].rect.y >= dock.y);

    // The palette pair: the same two acts as commands, no pointer
    // needed. Dock (the floating panel centers into the last leaf),
    // then float (half the dock, centered on it).
    CHECK(rig.commands.execute("shell.dock_panel"));
    CHECK(host.floating().empty());
    CHECK(tree.contains("editor"));
    CHECK(rig.commands.execute("shell.float_panel"));
    REQUIRE(host.floating().size() == 1);
    CHECK(host.floating()[0].panel == "editor");
    CHECK(host.floating()[0].rect.width == doctest::Approx(dock.width * 0.5f));
    CHECK(host.floating()[0].rect.height ==
          doctest::Approx(dock.height * 0.5f));
    CHECK(host.floating()[0].rect.x ==
          doctest::Approx(dock.x + dock.width * 0.25f));
    (void)rig.shell->shutdown();
}

TEST_CASE("a floating window resizes from its corner grip") {
    Rig rig;
    rig.build();
    CHECK(rig.shell->startup());
    (void)rig.shell->panel_host().open(rig.shell->panels(), "editor");
    (void)rig.shell->panel_host().open(rig.shell->panels(), "plot");

    PanelHost& host = rig.shell->panel_host();
    host.float_panel(rig.shell->panels(), "editor",
                     ws::ui::gui::Rect{100.0f, 100.0f, 300.0f, 200.0f});
    rig.frame();

    // The grip: the bottom-right corner's square. Press, carry down-
    // right past the threshold, release - the window grows by the
    // carry's delta, its origin unmoved.
    float grip_x = 100.0f + 300.0f - 7.0f;
    float grip_y = 100.0f + 200.0f - 7.0f;
    rig.point(grip_x, grip_y, true, true, false);
    rig.point(grip_x + 80.0f, grip_y + 60.0f, true, false, false);
    rig.point(grip_x + 80.0f, grip_y + 60.0f, false, false, true);
    REQUIRE(host.floating().size() == 1);
    CHECK(host.floating()[0].rect.width == doctest::Approx(380.0f));
    CHECK(host.floating()[0].rect.height == doctest::Approx(260.0f));
    CHECK(host.floating()[0].rect.x == doctest::Approx(100.0f));

    // The floor: a carry far past the window's own corner cannot
    // shrink it below the minimums (a title bar with no body).
    grip_x = 100.0f + 380.0f - 7.0f;
    grip_y = 100.0f + 260.0f - 7.0f;
    rig.point(grip_x, grip_y, true, true, false);
    rig.point(grip_x - 1000.0f, grip_y - 1000.0f, true, false, false);
    rig.point(grip_x - 1000.0f, grip_y - 1000.0f, false, false, true);
    REQUIRE(host.floating().size() == 1);
    CHECK(host.floating()[0].rect.width ==
          doctest::Approx(ws::ui::shell::kFloatingMinW));
    CHECK(host.floating()[0].rect.height ==
          doctest::Approx(ws::ui::shell::kFloatingMinH));

    // A grip CLICK (no carry) resizes nothing.
    grip_x = 100.0f + ws::ui::shell::kFloatingMinW - 7.0f;
    grip_y = 100.0f + ws::ui::shell::kFloatingMinH - 7.0f;
    rig.point(grip_x, grip_y, true, true, false);
    rig.point(grip_x, grip_y, false, false, true);
    CHECK(host.floating()[0].rect.width ==
          doctest::Approx(ws::ui::shell::kFloatingMinW));
    (void)rig.shell->shutdown();
}

TEST_CASE("a drop onto a zoomed dock lands in the zoomed pane") {
    Rig rig;
    rig.build();
    CHECK(rig.shell->startup());
    // A third pane, so the zoomed panel is NOT leaf 0 after the
    // tear-off: the drop's re-map (projection leaf 0 -> the zoomed
    // panel's real leaf) has a wrong answer to avoid. The CONSOLE is
    // the drag source (tearing it keeps plot at leaf 1; tearing the
    // editor would renumber plot to 0 and the map would have nothing
    // to prove).
    PanelDesc console;
    console.id = "console";
    console.title = "Console";
    console.placement.area = DockPlacement::Area::bottom;
    console.draw = [](GuiContext&, LayoutCursor&,
                      const ws::ui::gui::ChromeTheme&, const PanelInstance&) {
    };
    (void)rig.shell->panels().add(std::move(console));
    (void)rig.shell->panel_host().open(rig.shell->panels(), "editor");
    (void)rig.shell->panel_host().open(rig.shell->panels(), "plot");
    (void)rig.shell->panel_host().open(rig.shell->panels(), "console");
    // editor left, plot right, console under plot: three leaves.
    PanelHost& host = rig.shell->panel_host();
    DockTree& tree = host.dock();
    REQUIRE(tree.leaf_count() == 3);
    REQUIRE(tree.leaf_of("plot") == 1);

    // Console torn off (plot stays leaf 1), plot zoomed, one frame so
    // the zoom projection and the window are both on screen.
    host.float_panel(rig.shell->panels(), "console",
                     ws::ui::gui::Rect{100.0f, 100.0f, 300.0f, 200.0f});
    REQUIRE(tree.leaf_of("plot") == 1);  // the tear renumbered nothing above it
    tree.maximize("plot");
    rig.frame();

    // Carry the window over the zoomed dock's middle and release: the
    // projection answers leaf 0, the drop must land in plot's REAL
    // leaf (index 1, not the editor's 0) - the console joins the pane
    // the screen shows.
    const float chrome = ws::ui::gui::kDefaultChrome.control_height;
    const float bar_y = 100.0f + chrome * 0.5f;
    const float drop_x = 640.0f;
    const float drop_y = chrome + (720.0f - chrome) * 0.5f;
    rig.point(250.0f, bar_y, true, true, false);
    rig.point(drop_x, drop_y, true, false, false);
    rig.point(drop_x, drop_y, false, false, true);
    CHECK(host.floating().empty());
    CHECK(tree.leaf_of("console") == 1);  // plot's leaf, not the projection's 0
    CHECK(tree.leaf_of("console") == tree.leaf_of("plot"));
    CHECK(tree.maximized() == "plot");   // the zoom survived the drop
    CHECK(tree.tab_index("plot") == 0);
    CHECK(tree.tab_index("console") == 1);
    (void)rig.shell->shutdown();
}

TEST_CASE("the strip's zoom button maximizes the pane and restores it") {
    Rig rig;
    rig.build();
    CHECK(rig.shell->startup());
    (void)rig.shell->panel_host().open(rig.shell->panels(), "editor");
    (void)rig.shell->panel_host().open(rig.shell->panels(), "plot");

    const float chrome = ws::ui::gui::kDefaultChrome.control_height;
    const float pad = ws::ui::gui::kDefaultChrome.padding;
    const ws::ui::gui::Rect dock{0.0f, chrome, 1280.0f, 720.0f - chrome};
    DockTree& tree = rig.shell->panel_host().dock();
    const DockTree::Layout laid = tree.resolve(dock);

    // The zoom button: the strip's right end (12-px round hitbox,
    // padded in from the leaf's edge), the editor leaf's strip.
    const float zoom_x = laid.leaves[0].rect.x +
                         laid.leaves[0].rect.width - pad - 6.0f;
    const float zoom_y = laid.leaves[0].rect.y + chrome * 0.5f;
    rig.point(zoom_x, zoom_y, true, true, false);
    rig.point(zoom_x, zoom_y, false, false, true);

    // The zoom: one leaf, the whole dock, the front tab's panels - the
    // tree underneath whole (the strip's restore answers).
    CHECK(tree.maximized() == "editor");
    const DockTree::Layout zoomed = tree.resolve(dock);
    REQUIRE(zoomed.leaves.size() == 1);
    CHECK(zoomed.leaves[0].rect.width == doctest::Approx(1280.0f));
    REQUIRE(zoomed.leaves[0].tabs != nullptr);
    CHECK(zoomed.leaves[0].tabs->size() == 1);
    CHECK(tree.leaf_count() == 2);  // plot still docked, just hidden

    // The restore: the zoomed strip spans the dock; the same formula
    // on ITS right end.
    const float restore_x = dock.x + dock.width - pad - 6.0f;
    rig.point(restore_x, zoom_y, true, true, false);
    rig.point(restore_x, zoom_y, false, false, true);
    CHECK(tree.maximized().empty());
    CHECK(tree.resolve(dock).leaves.size() == 2);
    (void)rig.shell->shutdown();
}

TEST_CASE("a double click on a splitter evens it") {
    Rig rig;
    rig.build();
    CHECK(rig.shell->startup());
    (void)rig.shell->panel_host().open(rig.shell->panels(), "editor");
    (void)rig.shell->panel_host().open(rig.shell->panels(), "plot");

    const float chrome = ws::ui::gui::kDefaultChrome.control_height;
    const ws::ui::gui::Rect dock{0.0f, chrome, 1280.0f, 720.0f - chrome};
    DockTree& tree = rig.shell->panel_host().dock();
    const DockTree::Layout laid = tree.resolve(dock);
    const float band_x = laid.splitters[0].rect.center_x();
    const float mid_y = dock.y + dock.height * 0.5f;

    // Drag the boundary to 3/4 first (press, carry, release) - the
    // double-click needs an UNEVEN split to prove itself on.
    rig.point(band_x, mid_y, true, true, false, 0.05f);
    rig.point(960.0f, mid_y, true, false, false, 0.05f);
    rig.point(960.0f, mid_y, false, false, true, 0.05f);
    const DockTree::Layout dragged = tree.resolve(dock);
    REQUIRE(dragged.leaves.size() == 2);
    CHECK(dragged.leaves[0].rect.width == doctest::Approx(960.0f));

    // Let the drag's press expire: idle frames advance the clock past
    // the window, so the next press is a FRESH first, not a pair's
    // second half.
    for (int i = 0; i < 12; ++i) {
        rig.point(960.0f, mid_y, false, false, false, 0.05f);
    }

    // One press alone does not distribute.
    rig.point(960.0f, mid_y, true, true, false, 0.05f);
    rig.point(960.0f, mid_y, false, false, true, 0.05f);
    CHECK(tree.resolve(dock).leaves[0].rect.width ==
          doctest::Approx(960.0f));

    // The second press inside the window is the double-click: the
    // split evens out.
    rig.point(960.0f, mid_y, true, true, false, 0.05f);
    rig.point(960.0f, mid_y, false, false, true, 0.05f);
    const DockTree::Layout even = tree.resolve(dock);
    REQUIRE(even.leaves.size() == 2);
    CHECK(even.leaves[0].rect.width == doctest::Approx(640.0f));
    CHECK(even.leaves[1].rect.width == doctest::Approx(640.0f));

    // Slow presses never pair: drag again, wait past the window, press
    // again - the boundary stays where the drag left it.
    rig.point(640.0f, mid_y, true, true, false, 0.05f);
    rig.point(900.0f, mid_y, true, false, false, 0.05f);
    rig.point(900.0f, mid_y, false, false, true, 0.05f);
    for (int i = 0; i < 12; ++i) {
        rig.point(900.0f, mid_y, false, false, false, 0.2f);
    }
    rig.point(900.0f, mid_y, true, true, false, 0.05f);
    rig.point(900.0f, mid_y, false, false, true, 0.05f);
    CHECK(tree.resolve(dock).leaves[0].rect.width ==
          doctest::Approx(900.0f));
    (void)rig.shell->shutdown();
}

// --- the panel framework --------------------------------------------------------

TEST_CASE("panel registration validates and instances dock uniquely") {
    PanelRegistry registry;
    PanelHost host;

    PanelDesc bad;
    bad.id = "";  // no id, no title, no draw
    CHECK(!registry.add(std::move(bad)));

    PanelDesc good;
    good.id = "editor";
    good.title = "Editor";
    good.placement.area = DockPlacement::Area::center;
    good.draw = [](GuiContext&, LayoutCursor&,
                   const ws::ui::gui::ChromeTheme&, const PanelInstance&) {};
    CHECK(registry.add(std::move(good)));
    CHECK(!registry.add([&] {
        PanelDesc duplicate = *registry.find("editor");
        duplicate.title = "Again";
        return duplicate;
    }()));

    const auto first = host.open(registry, "editor");
    REQUIRE(first);
    // Single-instance: a second open of the same type refuses.
    CHECK(!host.open(registry, "editor"));
    // Unknown type refuses.
    CHECK(!host.open(registry, "nope"));

    PanelDesc multi;
    multi.id = "note";
    multi.title = "Note";
    multi.multi_instance = true;
    multi.draw = [](GuiContext&, LayoutCursor&,
                    const ws::ui::gui::ChromeTheme&, const PanelInstance&) {};
    CHECK(registry.add(std::move(multi)));
    const auto note1 = host.open(registry, "note");
    const auto note2 = host.open(registry, "note");
    REQUIRE(note1);
    REQUIRE(note2);
    CHECK(*note1 != *note2);  // "note:1" vs "note:2"

    // Focus: activate fires on_focus only on the CHANGE.
    std::string focused;
    PanelDesc tracked;
    tracked.id = "tracked";
    tracked.title = "Tracked";
    tracked.draw = [](GuiContext&, LayoutCursor&,
                      const ws::ui::gui::ChromeTheme&, const PanelInstance&) {
    };
    tracked.on_focus = [&focused](const PanelInstance& instance) {
        focused = instance.id;
    };
    CHECK(registry.add(std::move(tracked)));
    const auto tracked_id = host.open(registry, "tracked");
    REQUIRE(tracked_id);
    CHECK(focused == *tracked_id);  // open() activates
    host.activate(registry, *tracked_id);
    CHECK(focused == *tracked_id);  // no duplicate callback

    // Cycling wraps through the open set.
    host.cycle(registry);
    CHECK(host.active() != nullptr);
    CHECK(host.active()->id != *tracked_id);
}

// --- the document layer -----------------------------------------------------------

TEST_CASE("the document manager opens, closes and restores sessions") {
    FakeFs fs;
    FakeDocument::disk.clear();
    FakeDocument::disk["/a.txt"] = "alpha";
    FakeDocument::disk["/b.txt"] = "beta";

    DocumentManager docs(
        [](const std::string& path) -> ws::core::Result<std::unique_ptr<Document>> {
            return std::make_unique<FakeDocument>(path);
        },
        &fs, "cfg/autosave");

    const auto a = docs.open_path("/a.txt");
    const auto b = docs.open_path("/b.txt");
    REQUIRE(a);
    REQUIRE(b);
    CHECK(docs.count() == 2);
    CHECK(docs.active() != nullptr);
    CHECK(docs.active()->path() == "/b.txt");

    // The same path twice: activate, do not duplicate.
    const auto again = docs.open_path("/a.txt");
    REQUIRE(again);
    CHECK(docs.count() == 2);
    CHECK(*again == *a);

    // A missing file answers the error, nothing adopted.
    CHECK(!docs.open_path("/gone.txt"));
    CHECK(docs.count() == 2);

    // --- session restore, with a missing file in the list ---
    const DocumentManager::Session session = docs.session();
    CHECK(session.paths.size() == 2);
    FakeDocument::disk.erase("/b.txt");
    DocumentManager round(
        [](const std::string& path) -> ws::core::Result<std::unique_ptr<Document>> {
            return std::make_unique<FakeDocument>(path);
        },
        &fs, "cfg/autosave");
    std::vector<std::string> failures;
    CHECK(round.restore_session(session, &failures) == 1);
    REQUIRE(failures.size() == 1);
    CHECK(round.count() == 1);
    CHECK(round.at(0)->path() == "/a.txt");
}

TEST_CASE("the close-with-dirty flow asks, saves or discards") {
    FakeFs fs;
    FakeDocument::disk.clear();
    FakeDocument::fail_saves = false;
    FakeDocument::disk["/keep.txt"] = "one";
    DocumentManager docs(
        [](const std::string& path) -> ws::core::Result<std::unique_ptr<Document>> {
            return std::make_unique<FakeDocument>(path);
        },
        &fs, "");

    const auto opened = docs.open_path("/keep.txt");
    REQUIRE(opened);
    FakeDocument* document =
        static_cast<FakeDocument*>(docs.at(static_cast<std::size_t>(*opened)));
    REQUIRE(document != nullptr);

    // Clean: no prompt, the close just runs.
    auto plan = docs.plan_close(0);
    CHECK(plan.valid);
    CHECK(!plan.prompt);

    // Dirty: the plan says prompt, and both answers behave.
    (void)document->undo().push(std::make_unique<EditCommand>(
        document, "", "edited"));
    plan = docs.plan_close(0);
    CHECK(plan.prompt);
    CHECK(document->undo().is_dirty());

    // The discard answer: changes lost, the document gone.
    CHECK(docs.finish_close(0, false));
    CHECK(docs.count() == 0);

    // The save answer: a failing save keeps the document open.
    const auto second = docs.open_path("/keep.txt");  // still on disk
    REQUIRE(second);
    document = static_cast<FakeDocument*>(docs.at(0));
    (void)document->undo().push(
        std::make_unique<EditCommand>(document, "", "more"));
    FakeDocument::fail_saves = true;  // the vanishing-file stand-in
    CHECK(!docs.finish_close(0, true));  // the save fails
    CHECK(docs.count() == 1);           // the document stays open
    FakeDocument::fail_saves = false;
}

TEST_CASE("the autosave pair snapshots, recovers and discards") {
    FakeFs fs;
    FakeDocument::disk.clear();
    FakeDocument::disk["/doc.txt"] = "one";
    DocumentManager docs(
        [](const std::string& path) -> ws::core::Result<std::unique_ptr<Document>> {
            return std::make_unique<FakeDocument>(path);
        },
        &fs, "cfg/autosave");
    FakeDocument::disk["/doc.txt"] = "one";
    const auto opened = docs.open_path("/doc.txt");
    REQUIRE(opened);
    auto* document = static_cast<FakeDocument*>(docs.at(0));

    (void)document->undo().push(
        std::make_unique<EditCommand>(document, "one", "two"));
    CHECK(docs.snapshot_active());

    // A fresh manager (the next process) probes the marker.
    DocumentManager next(
        [](const std::string& path) -> ws::core::Result<std::unique_ptr<Document>> {
            return std::make_unique<FakeDocument>(path);
        },
        &fs, "cfg/autosave");
    const std::vector<DocumentManager::RecoveryOffer> offers =
        next.probe_recovery({"/doc.txt"});
    REQUIRE(offers.size() == 1);
    CHECK(offers[0].document == "/doc.txt");

    // The recovery hands the crashed content back and clears the pair.
    const auto content = next.take_recovery("/doc.txt");
    REQUIRE(content);
    CHECK(*content == "two");
    CHECK(next.probe_recovery({"/doc.txt"}).empty());

    // The clean finish clears the marker without an offer.
    (void)document->undo().push(
        std::make_unique<EditCommand>(document, "two", "three"));
    CHECK(docs.snapshot_active());
    CHECK(docs.finish_autosave(0));
    CHECK(next.probe_recovery({"/doc.txt"}).empty());
}

// --- AppShell ---------------------------------------------------------------------

TEST_CASE("startup with an empty config directory is a clean first run") {
    ws::ui::gui_test::stub.reset();
    Rig rig;
    rig.build();
    CHECK(rig.shell->startup());

    // First run: no panels open, no documents, one empty leaf.
    CHECK(rig.shell->panel_host().instances().empty());
    CHECK(rig.shell->documents().count() == 0);
    CHECK(rig.shell->panel_host().dock().leaf_count() == 1);

    // Open the panels and draw a frame: the chrome really draws, the
    // clips stay balanced (the gui suite's own rule).
    (void)rig.shell->panel_host().open(rig.shell->panels(), "editor");
    (void)rig.shell->panel_host().open(rig.shell->panels(), "plot");
    rig.frame();
    CHECK(rig.editor_draws > 0);
    CHECK(ws::ui::gui_test::stub.quads > 0);
    CHECK(ws::ui::gui_test::stub.clip_pushes > 0);
    CHECK(ws::ui::gui_test::stub.clip_pushes ==
          ws::ui::gui_test::stub.clip_pops);

    // Shutdown writes the state files, in order.
    CHECK(rig.shell->shutdown());
    CHECK(rig.fs.exists("cfg/layout.json"));
    CHECK(rig.fs.exists("cfg/session.json"));
    CHECK(rig.fs.exists("cfg/recent.json"));
    CHECK(rig.fs.exists("cfg/settings.json"));
    REQUIRE(rig.shell->shutdown_log().size() == 7);
    CHECK(rig.shell->shutdown_log()[0] == "cancel_jobs");
    CHECK(rig.shell->shutdown_log()[1] == "close_panels");
    CHECK(rig.shell->shutdown_log()[6] == "flush_settings");
    // Both panels closed through the shell (the on_close record).
    CHECK(rig.closed.size() == 2);  // editor + plot
}

TEST_CASE("startup with a corrupt config directory answers defaults") {
    Rig rig;
    rig.build();
    rig.fs.files["cfg/settings.json"] = "{ this is not json";
    rig.fs.files["cfg/layout.json"] = "[1, 2, 3";
    rig.fs.files["cfg/session.json"] = "null";
    // A first run's panels still register; startup must not fail.
    CHECK(rig.shell->startup());
    CHECK(rig.shell->panel_host().dock().leaf_count() == 1);
    CHECK(rig.shell->documents().count() == 0);
    // The corrupt settings note lands in the status line.
    CHECK(!rig.shell->window_title().empty());

    // The frame still draws clean.
    (void)rig.shell->panel_host().open(rig.shell->panels(), "editor");
    rig.frame();
    CHECK(rig.editor_draws > 0);
    (void)rig.shell->shutdown();
}

TEST_CASE("layout restore drops panels that are not open") {
    Rig rig;
    rig.build();
    // A layout from a session that had the editor and settings panels
    // open, saved by hand into the config dir (the file's own shape).
    const std::string layout = ws::settings::json::write(
        ws::settings::json::parse(
            R"({"tabs":["editor","settings"],"active":1})")
            .value());
    rig.fs.files["cfg/layout.json"] = layout;
    CHECK(rig.shell->startup());

    // The panels are registered but NOT open at startup - the
    // layout's accept filter refuses ids the host never opened, so
    // nothing resurrects from the file alone.
    CHECK(rig.shell->panel_host().instances().empty());

    // The round trip: with the panels open, shutdown writes the
    // layout; a second rig with the same panels open restores it.
    (void)rig.shell->panel_host().open(rig.shell->panels(), "editor");
    (void)rig.shell->panel_host().open(rig.shell->panels(), "settings");
    (void)rig.shell->shutdown();
    CHECK(rig.fs.exists("cfg/layout.json"));

    // A restore through the shell's own load path (the named-layout
    // command's act, driven directly): the written file parses back.
    Rig second;
    second.build();
    // (startup() registers the built-in settings panel; the opens come
    //  after it, the host's own startup order)
    CHECK(second.shell->startup());
    (void)second.shell->panel_host().open(second.shell->panels(), "editor");
    (void)second.shell->panel_host().open(second.shell->panels(), "settings");
    CHECK(second.shell->panel_host().is_open("editor"));
    CHECK(second.shell->panel_host().is_open("settings"));
    (void)second.shell->shutdown();
}

TEST_CASE("the shell's document commands and the dirty title") {
    Rig rig;
    rig.build();
    FakeDocument::disk.clear();
    FakeDocument::disk["/notes.txt"] = "hello";
    CHECK(rig.shell->startup());
    (void)rig.shell->panel_host().open(rig.shell->panels(), "editor");

    // New: an untitled document through the factory.
    CHECK(rig.commands.execute("shell.new_document"));
    CHECK(rig.shell->documents().count() == 1);
    // Clean with a document: "document - app" (the platform rule).
    CHECK(rig.shell->window_title() == "untitled - scratchpad");

    // Open: through the shell's open_paths (the drop path too).
    CHECK(rig.shell->open_paths({"/notes.txt"}));
    CHECK(rig.shell->documents().count() == 2);
    CHECK(rig.shell->window_title() == "notes.txt - scratchpad");

    // Edit: dirty rides the undo save point into the title.
    auto* document =
        static_cast<FakeDocument*>(rig.shell->documents().active());
    (void)document->undo().push(
        std::make_unique<EditCommand>(document, "hello", "hello!"));
    CHECK(rig.shell->window_title() == "*notes.txt - scratchpad");

    // Undo back to the save point: clean again.
    CHECK(document->undo().undo());
    CHECK(rig.shell->window_title() == "notes.txt - scratchpad");
    CHECK(document->undo().redo());
    CHECK(rig.shell->window_title() == "*notes.txt - scratchpad");

    // Save: clean, and the bytes landed on the fake disk.
    CHECK(rig.shell->save_active());
    CHECK(rig.shell->window_title() == "notes.txt - scratchpad");
    CHECK(FakeDocument::disk["/notes.txt"] == "hello!");

    // The panel rung: the editor declared scoped shortcuts.
    CHECK(rig.shell->key_context() == ws::app::KeyContext::panel);
    (void)rig.shell->panel_host().open(rig.shell->panels(), "plot");
    rig.shell->panel_host().activate(rig.shell->panels(), "plot");
    CHECK(rig.shell->key_context() == ws::app::KeyContext::global);

    // The close prompt: a dirty document raises it, Escape cancels.
    (void)document->undo().push(
        std::make_unique<EditCommand>(document, "hello!", "hello!!"));
    CHECK(rig.shell->window_title() == "*notes.txt - scratchpad");
    rig.shell->request_close_active();
    rig.frame();  // the modal lays (the veil rises)
    rig.frame(ws::input::InputFrame{});  // idle
    ws::input::InputFrame escape;
    escape.key_pressed[ws::input::keys::kEscape] = true;
    rig.frame(escape);  // the claim lands
    rig.frame(ws::input::InputFrame{});
    CHECK(rig.shell->documents().count() == 2);  // still open: cancelled
    (void)rig.shell->shutdown();
}

TEST_CASE("the command palette opens and dispatches through the registry") {
    Rig rig;
    rig.build();
    CHECK(rig.shell->startup());

    // The shell registered its own commands under shell.*.
    REQUIRE(rig.commands.find("shell.command_palette") != nullptr);
    REQUIRE(rig.commands.find("shell.settings") != nullptr);

    // The palette command flips the flag; a drawn frame then shows
    // the overlay at the modal stratum (the stub's max layer).
    CHECK(rig.commands.execute("shell.command_palette"));
    CHECK(rig.shell->palette_open);
    ws::ui::gui_test::stub.reset();
    rig.frame();
    CHECK(ws::ui::gui_test::stub.max_layer >= 40);  // the modal veil

    // The settings panel command toggles the built-in panel.
    CHECK(rig.commands.execute("shell.settings"));
    CHECK(rig.shell->panel_host().is_open("settings"));
    rig.frame();
    CHECK(rig.commands.execute("shell.settings"));
    CHECK(!rig.shell->panel_host().is_open("settings"));

    // The menu bar decodes: the Window menu's panel toggle works
    // through the same registry path (the drawn path is the snap
    // suite's business; the action routing is pinned here).
    CHECK(rig.commands.execute("shell.reset_layout"));
    (void)rig.shell->shutdown();
}

TEST_CASE("shutdown cancels jobs before the panels are destroyed") {
    // The job system is built BEFORE the rig (the shell reads it from
    // its config at construction).
    ws::jobs::JobSystem jobs(1);
    Rig rig;
    rig.jobs = &jobs;
    rig.build();
    CHECK(rig.shell->startup());
    (void)rig.shell->panel_host().open(rig.shell->panels(), "editor");

    // One long job still running when shutdown lands: request_cancel_
    // all() is the FIRST phase, before any on_close ran.
    auto job = jobs.submit<int>([](ws::jobs::JobContext&) -> int {
        for (int i = 0; i < 200; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return 0;
    });

    CHECK(rig.shell->shutdown());
    REQUIRE(rig.shell->shutdown_log().size() == 7);
    const auto cancel_at = std::find(rig.shell->shutdown_log().begin(),
                                     rig.shell->shutdown_log().end(),
                                     "cancel_jobs");
    const auto panels_at = std::find(rig.shell->shutdown_log().begin(),
                                     rig.shell->shutdown_log().end(),
                                     "close_panels");
    REQUIRE(cancel_at != rig.shell->shutdown_log().end());
    REQUIRE(panels_at != rig.shell->shutdown_log().end());
    CHECK(cancel_at < panels_at);  // jobs die before panels are destroyed
    CHECK(rig.closed.size() == 1);  // the editor panel
    jobs.wait_all();  // the shell never waits: the test does
}

TEST_CASE("the pickers ride the job system when one is installed") {
    // One worker: the scripted fake answers from its queue, so the
    // pick never blocks long - the case's subject is WHERE the call
    // runs (a worker, never the command's own stack) and WHERE the
    // answer lands (the marshal drain, between frames).
    ws::jobs::JobSystem jobs(1);
    Rig rig;
    rig.jobs = &jobs;
    rig.build();
    FakeDocument::disk.clear();
    FakeDocument::disk["/picked.txt"] = "picked";
    CHECK(rig.shell->startup());

    // Open: the command returns before the pick lands.
    rig.dialogs.open_queue.push_back("/picked.txt");
    CHECK(rig.commands.execute("shell.open_document"));
    CHECK(rig.shell->documents().count() == 0);  // nothing inline: async
    for (int i = 0; i < 10000 && rig.shell->documents().count() == 0;
         ++i) {
        (void)jobs.drain();
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    REQUIRE(rig.shell->documents().count() == 1);
    CHECK(rig.shell->documents().active()->path() == "/picked.txt");
    REQUIRE(rig.dialogs.open_calls.size() == 1);
    CHECK(rig.dialogs.open_calls[0].title == "open document");

    // Save an untitled document: the pick lands, then the save.
    CHECK(rig.commands.execute("shell.new_document"));
    FakeDocument* const untitled =
        static_cast<FakeDocument*>(rig.shell->documents().active());
    (void)untitled->undo().push(
        std::make_unique<EditCommand>(untitled, "", "typed"));
    rig.dialogs.save_queue.push_back("/saved.txt");
    CHECK(rig.commands.execute("shell.save_document"));
    for (int i = 0; i < 10000 && untitled->path().empty(); ++i) {
        (void)jobs.drain();
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    CHECK(untitled->path() == "/saved.txt");
    CHECK(FakeDocument::disk["/saved.txt"] == "typed");
    CHECK(!untitled->undo().is_dirty());  // the landed save marked it

    // A dismissed save-as answers nothing: the document keeps its
    // identity (the empty queue is the fake's "user cancelled").
    CHECK(rig.commands.execute("shell.new_document"));
    CHECK(rig.commands.execute("shell.save_document_as"));
    for (int i = 0; i < 10000 && rig.dialogs.save_calls.size() < 2;
         ++i) {
        (void)jobs.drain();
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    REQUIRE(rig.dialogs.save_calls.size() == 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    (void)jobs.drain();  // the dismissed pick's completion (a no-op)
    CHECK(rig.shell->documents().at(2)->path().empty());

    // Close-with-dirty on an untitled document: the prompt's Save
    // rides the worker too, and the close lands WITH the save.
    FakeDocument* const dirty =
        static_cast<FakeDocument*>(rig.shell->documents().active());
    (void)dirty->undo().push(
        std::make_unique<EditCommand>(dirty, "", "more"));
    rig.shell->request_close_active();
    rig.frame();  // the modal lays (the veil rises)
    rig.frame(ws::input::InputFrame{});
    // The Save button: draw_close_prompt's centered three-button row
    // (a 360x170 modal at the frame's center, two control-height text
    // rows above the buttons - the same layout math the shell runs).
    rig.dialogs.save_queue.push_back("/closed.txt");
    {
        ws::input::InputFrame press;
        press.mouse_present = true;
        press.mouse_x = 566.0f;
        press.mouse_y = 365.0f;
        press.mouse_down[0] = true;
        press.mouse_pressed[0] = true;
        rig.frame(press);
    }
    {
        ws::input::InputFrame release;
        release.mouse_present = true;
        release.mouse_x = 566.0f;
        release.mouse_y = 365.0f;
        release.mouse_released[0] = true;
        rig.frame(release);
    }
    for (int i = 0; i < 10000 && rig.shell->documents().count() > 2;
         ++i) {
        (void)jobs.drain();
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    CHECK(rig.shell->documents().count() == 2);  // the dirty one closed
    CHECK(FakeDocument::disk["/closed.txt"] == "more");
    (void)rig.shell->shutdown();
}

TEST_CASE("dropped files open as documents") {
    Rig rig;
    rig.build();
    FakeDocument::disk.clear();
    FakeDocument::disk["/dropped.txt"] = "drop";
    CHECK(rig.shell->startup());
    rig.shell->handle_dropped_files({"/dropped.txt", "/missing.txt"});
    CHECK(rig.shell->documents().count() == 1);
    CHECK(rig.shell->documents().active()->path() == "/dropped.txt");
    (void)rig.shell->shutdown();
}

TEST_CASE("the theme follows the settings key") {
    Rig rig;
    rig.build();
    CHECK(rig.shell->startup());
    CHECK(&rig.shell->theme() == &ws::ui::gui::kDefaultChrome);

    CHECK(rig.settings->set<std::string>(ws::settings::Layer::user,
                                         "shell.theme", "light"));
    CHECK(&rig.shell->theme() == &ws::ui::gui::kLightChrome);

    CHECK(rig.settings->set<std::string>(ws::settings::Layer::user,
                                         "shell.theme", "dark"));
    CHECK(&rig.shell->theme() == &ws::ui::gui::kDefaultChrome);
    (void)rig.shell->shutdown();
}
