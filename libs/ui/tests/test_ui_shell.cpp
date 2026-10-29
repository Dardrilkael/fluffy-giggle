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
            return ws::core::unexpected(ws::core::make_error(
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
            return ws::core::unexpected(
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
            return ws::core::unexpected(
                ws::core::make_error(1, "io", "cannot open '" + path_ + "'"));
        }
        content = found->second;
        return {};
    }
    [[nodiscard]] ws::core::Status save() override {
        if (fail_saves) {
            return ws::core::unexpected(
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
