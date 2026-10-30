// apps/ui_gallery - one screen with every ws::ui::gui widget family on
// it: the widget arc's acceptance view. A menu bar (Alt mnemonics, a
// submenu, checks, radios, a disabled row with its reason tooltip) and
// a right-click / long-press context menu on the chrome strip; a
// scrolling page of sections - buttons, choices, sliders, text, lists,
// a table, tabs, a live job row (the ws::jobs bridge: a worker counting
// under a progress bar, a cancel button, a failure toast), the overlays
// (a raw popup, a raw modal scope, the
// five modal-dialog button sets, the toast store), the disabled scope,
// the custom-look page (the stage-11 story: one slider value and one
// text buffer, each drawn three ways - the composed widgets, a custom
// paint over the stock behavior, and a widget the app owns outright off
// the raw state() seam), and the platform page (the stage-12 story: the
// native file dialogs run on a ws::jobs worker - they block while the
// user decides - with every answer marshalled back through the frame
// drain, the recent-files MRU persisted through the settings seams,
// the clipboard round trip, the file-drop log, and the autosave
// crash-recovery pair whose startup probe asks its yes/no through a
// native message box the same worker way), and the commands page
// (the stage-13 story: one command registry driving a generated menu
// bar, a toolbar and the Ctrl+Shift+P palette, its chords resolved
// live before the frame begins, the keymap editor rebinding the live
// table over the persisted keymap file, and an undo demo whose every
// edit is a command on the stack - the stack the sole executor, a
// typing run coalescing into one step, the Undo/Redo labels composed
// host-side from the step names); and a status bar the page
// keeps clear of. The
// gallery loads no
// assets (its font is the system face, resolved here at the platform
// boundary the same way playground resolves its own), so it builds for
// Windows and Android unchanged: the gui draws through r2d and reads
// one InputFrame, neither of which cares what platform is underneath.
//
// Everything below is caller-owned state driven by immediate-mode
// widget calls - that IS the gui's contract, and GalleryApp's member
// list doubles as the checklist of what a host owns.

#ifdef __ANDROID__
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include <android_native_app_glue.h>  // android_app + the android_main contract
#include <unistd.h>                   // chdir (imgui.ini's home)
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#endif

#include <ws/ui/app.hpp>
#include <ws/input/event.hpp>
#include <ws/input/input.hpp>
#include <ws/app/command.hpp>
#include <ws/app/keymap.hpp>
#include <ws/app/shortcut.hpp>
#include <ws/app/undo.hpp>
#include <ws/jobs/job_system.hpp>
#include <ws/jobs/errors.hpp>
#include <ws/platform/autosave.hpp>
#include <ws/platform/clipboard.hpp>
#include <ws/platform/dialogs.hpp>
#include <ws/platform/errors.hpp>
#include <ws/platform/locations.hpp>
#include <ws/platform/messagebox.hpp>
#include <ws/platform/paths.hpp>
#include <ws/platform/recent.hpp>
#include <ws/platform/single_instance.hpp>
#include <ws/platform/title.hpp>
#include <ws/settings/filesystem.hpp>
#include <ws/ui/gui/command_views.hpp>
#include <ws/ui/gui/gui_context.hpp>
#include <ws/ui/gui/job_progress.hpp>
#include <ws/ui/gui/layout_cursor.hpp>
#include <ws/ui/gui/scoped.hpp>
#include <ws/ui/gui/theme.hpp>
#include <ws/ui/gui/theme_color.hpp>
#include <ws/ui/gui/theme_data.hpp>
#include <ws/ui/gui/widgets.hpp>
#include <ws/ui/renderer2d.hpp>
#include <ws/ui/canvas/canvas2d.hpp>
#include <ws/ui/canvas/view.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace gui = ws::ui::gui;
namespace canvas = ws::ui::canvas;

// The system UI face - the APP picks it, not the ui lib (r2d takes
// caller-resolved paths; font-directory knowledge lives in this TU, the
// platform boundary). One constant per TU, per the house rule. A
// missing face degrades to the fontless v1 bars everywhere, gracefully.
#ifdef __ANDROID__
constexpr const char* kDefaultFontPath = "/system/fonts/Roboto-Regular.ttf";
#else
constexpr const char* kDefaultFontPath = "C:/Windows/Fonts/segoeui.ttf";
#endif

// The growing-font demo's second stacked face (the Han carrier; the
// primary file stacks too, as its own past-ASCII fallback - Latin-1
// supplements live past the inline bake). A missing file refuses
// gracefully: the status bar says so, the line keeps its tofu.
#ifdef __ANDROID__
constexpr const char* kExtraFontPath = "/system/fonts/NotoSansCJK-Regular.ttc";
#else
constexpr const char* kExtraFontPath = "C:/Windows/Fonts/msyh.ttc";
#endif

constexpr std::uint32_t kBackdrop = ws::ui::r2d::rgba(22, 24, 30);
constexpr std::uint32_t kGridLine = ws::ui::r2d::rgba(34, 38, 48);
constexpr float kGridStep = 32.0f;

// The platform page's drop log: the newest kDropLogCap drops stay on
// it, one entry per drop (that entry is the drop's whole path list,
// arrival order).
constexpr std::size_t kDropLogCap = 6;

// The cursor radio's rows, CursorShape's own order: the section walks
// the names, the shapes column is what set_cursor takes.
constexpr const char* const kCursorNames[] = {
    "arrow", "text", "crosshair", "hand",
    "resize ew", "resize ns", "resize all", "not allowed",
};
constexpr ws::ui::CursorShape kCursorShapes[] = {
    ws::ui::CursorShape::arrow,     ws::ui::CursorShape::text,
    ws::ui::CursorShape::crosshair, ws::ui::CursorShape::hand,
    ws::ui::CursorShape::resize_ew, ws::ui::CursorShape::resize_ns,
    ws::ui::CursorShape::resize_all, ws::ui::CursorShape::not_allowed,
};
constexpr int kCursorCount = 8;

// The flood button's wave - fixed literals because the toast store
// borrows its text (a stack buffer would die with the frame).
const char* const kWave[] = {
    "wave 0", "wave 1", "wave 2", "wave 3", "wave 4",
    "wave 5", "wave 6", "wave 7", "wave 8", "wave 9",
};

// The theme page's accent rows: 0 keeps the stock hand-picked faces,
// anything else re-derives the control faces through apply_accent - and
// every selected radio and checked box on the page wears the answer.
struct AccentRow {
    const char* name;
    std::uint32_t color;
};
constexpr AccentRow kAccents[] = {
    {"stock", 0u},
    {"blue", ws::ui::r2d::rgba(80, 140, 230)},
    {"orange", ws::ui::r2d::rgba(235, 145, 60)},
    {"green", ws::ui::r2d::rgba(90, 180, 120)},
    {"magenta", ws::ui::r2d::rgba(215, 80, 170)},
};
constexpr int kAccentCount = 5;

// The growing-font demo line: e-acute, o-umlaut, a-ogonek (all past the
// inline bake - the stacked primary carries them) and the Han codepoint
// (the stacked YaHei/Noto carries it). Hex escapes split per byte, the
// tests' idiom.
constexpr const char* kLazyLine =
    "growing: " "\xC3" "\xA9" "\xC3" "\xB6" "\xC4" "\x85" " " "\xE4" "\xB8" "\x96";

// The super keys' GLFW codes - the two ws::input::keys does not name
// (the command-keymap chord loop composes Mod::super_key from held
// keys, and the ws::app rule is GLFW numbering everywhere). The
// command_keymap.cpp editor precedent for spelling them locally.
constexpr int kLeftSuper = 343;   // GLFW_KEY_LEFT_SUPER
constexpr int kRightSuper = 347;  // GLFW_KEY_RIGHT_SUPER

// The page's sections, one collapsing header each; the bools are the
// headers' own open state (caller-owned, like everything here).
enum Section {
    kTheme,
    kButtons,
    kChoices,
    kSliders,
    kText,
    kLists,
    kTable,
    kTabs,
    kJobs,
    kOverlays,
    kDisabled,
    kCustomLook,
    kPlatform,
    kCommands,
    kCanvas,
    kSectionCount
};

// The platform page's store dir, computed once at construction: the
// OS data location for this app on desktop (made on the spot by the
// Locations seam), the process CWD on android - android_main chdir'd
// into the internal data dir before the app was built, os_location
// refuses there (the activity owns the package dirs), and this app TU
// is the one place that knows the CWD fact. A failed desktop ensure
// falls back to the CWD too: the demo degrades to local files, it
// never refuses to run.
std::string platform_store_dir() {
#ifndef __ANDROID__
    const ws::platform::Locations where{"ws ui gallery"};
    if (where.ensure(ws::platform::Location::data)) {
        const ws::core::Result<std::string> dir =
            where.get(ws::platform::Location::data);
        if (dir.has_value()) {
            return dir.value();
        }
    }
#endif
    return ".";
}

// The undo demo's one command type: a whole-buffer text edit over the
// demo doc. apply() captures the buffer's CURRENT text as the inverse
// INSIDE itself - the stack's rule, because redo replays apply, and at
// redo time the buffer sits exactly where it sat at push - then lands
// the new text (snprintf bounds the write; the doc is 63 chars of
// content, a demo bound like the autosave draft's). The merge hook
// coalesces a same-kind run: the TOP step's command absorbs the newer
// one, so prior_ keeps the run's FIRST text while next_ moves forward
// to the run's end, and the one undo() that reverts to prior_ undoes
// the whole run. "Clear" never merges - it is a discrete gesture (and
// its command body guards the empty doc, so two clears cannot even
// arise back to back).
class DocEdit final : public ws::app::UndoCommand {
public:
    DocEdit(char* doc, int cap, std::string next, std::string kind,
            bool mergeable)
        : doc_(doc),
          cap_(cap),
          next_(std::move(next)),
          kind_(std::move(kind)),
          mergeable_(mergeable) {}

    void apply() override {
        prior_.assign(doc_);  // the inverse, re-captured at every apply
        std::snprintf(doc_, cap_, "%s", next_.c_str());
    }

    void undo() override { std::snprintf(doc_, cap_, "%s", prior_.c_str()); }

    [[nodiscard]] std::string name() const override { return kind_; }

    [[nodiscard]] bool merge_with(const ws::app::UndoCommand& later) override {
        const auto* edit = dynamic_cast<const DocEdit*>(&later);
        if (edit == nullptr || !mergeable_ || !edit->mergeable_ ||
            kind_ != edit->kind_) {
            return false;  // a different gesture never coalesces
        }
        next_ = edit->next_;  // the run's end moves forward; prior_ keeps the start
        return true;
    }

    [[nodiscard]] std::size_t memory_estimate() const override {
        return sizeof(DocEdit) + next_.size() + prior_.size();
    }

private:
    char* doc_;          // the demo doc buffer (app-lifetime member storage)
    int cap_;            // its capacity - snprintf's bound
    std::string next_;   // the text this edit lands
    std::string prior_;  // the text it reverts (captured inside apply)
    std::string kind_;   // "Typing" / "Append" / "Clear": the step name + the merge tag
    bool mergeable_;
};

class GalleryApp final : public ws::ui::App {
public:
    // launch_docs: the command line's documents (open_documents in
    // main) - the open-with hook's half this app owns, fed into the
    // MRU head in on_setup. Empty on android (no argv - the launch
    // intent carries documents, the activity's JNI to read).
    explicit GalleryApp(ws::ui::AppConfig config,
                        std::vector<std::string> launch_docs = {})
        : ws::ui::App(std::move(config)),
          launch_docs_(std::move(launch_docs)) {}

    // One last drain on the way out: the shell stopped pumping at
    // run()'s return, so anything the final jobs posted would
    // otherwise die in the queue with the teardown WARN. This is the
    // jobs lib's "drain before tearing down if completions matter"
    // rule, spelled out where a real app writes it - the body runs
    // before any member destructs, so the completions still see live
    // state.
    ~GalleryApp() override { jobs_.drain(); }

protected:
    void on_setup() override {
        scale_ = ui_scale();
        // The stage-10 start: the OS answer picks the stock document -
        // the theme page flips it (and re-derives an accent onto it)
        // live from there.
        theme_dark_ = prefers_dark();
        theme_pick_ = theme_dark_ ? 0 : 1;
        theme_ = theme_dark_ ? gui::default_dark() : gui::default_light();
        font_px_ = theme_.regular.size;
        rebuild_font();
        sync_chrome();
        install_clipboard(gui_);  // Ctrl+C/X/V in the text inputs
        // The drain seat: once per frame, between the events and
        // on_update, the shell drains this system - every progress
        // report and completion below lands on the main thread, where
        // the mirrors and the toast store already live.
        install_job_system(jobs_);
        // The stage-12 platform half, up before the first frame: the
        // MRU loads (a missing file is the first run - only a failure
        // speaks, one error toast whose bytes the member owns), the
        // native factories answer this platform's set, and a leftover
        // crash marker asks its yes/no through a native box on the
        // worker - that completion lands on this same drain.
        const ws::core::Status recent_loaded = recent_.load();
        if (!recent_loaded.has_value()) {
            recent_text_ = "the recent list: ";
            recent_text_ += recent_loaded.error().message;
            toasts_.push({recent_text_.c_str(), gui::ToastSeverity::error,
                          nullptr, 5.0f});
        }
        // The open-with documents count as use: they feed the MRU
        // head before the first frame - the primary showing them IS
        // this app's answer to forwarding (the second-instance
        // forward channel is the documented cut; this app never sees
        // one, a second copy never runs).
        if (!launch_docs_.empty()) {
            for (const std::string& doc : launch_docs_) {
                (void)recent_.touch(doc);
            }
            (void)recent_.save();
            std::snprintf(status_, sizeof status_, "opened %d from the "
                                                 "command line",
                          static_cast<int>(launch_docs_.size()));
        }
        dialogs_ = ws::platform::make_file_dialogs();
        boxes_ = ws::platform::make_message_boxes();
        refresh_probe();
        if (probe_.has_value() && probe_.value()) {
            offer_recovery();
        }
        // The stage-13 half: the registry (registration order is the
        // menu order within a category), the resolver seeded from the
        // declared chords, then the keymap file - a missing file is
        // the first run and the seed stands; a saved one replaces
        // the whole live table (no provenance - the rebinds ARE the
        // table). The menu bar spec is described once here.
        build_commands();
        const ws::core::Status seeded = resolver_.install_defaults(commands_);
        if (!seeded.has_value()) {
            cmd_text_ = seeded.error().message;
            toasts_.push({cmd_text_.c_str(), gui::ToastSeverity::error,
                          nullptr, 5.0f});
        }
        const ws::core::Status keymap_loaded = keymap_file_.load(resolver_);
        if (!keymap_loaded.has_value()) {
            cmd_text_ = keymap_loaded.error().message;
            toasts_.push({cmd_text_.c_str(), gui::ToastSeverity::error,
                          nullptr, 5.0f});
        }
        menu_builder_.describe({{"&edit", "Edit"},
                                {"&view", "View"},
                                {"&tools", "Tools"},
                                {"&help", "Help"}});
        section_.fill(true);
    }

    // Stage 12's drag-drop-as-events: one FileDroppedEvent per drop,
    // the paths riding the event (copied at the platform edge before
    // the OS callback returned, so appending is plain state work). A
    // drop is an open gesture - every path feeds the MRU, one save
    // after the loop - and the log keeps the newest drops first.
    // Desktop arrival only: on android the handler never fires, the
    // same app builds.
    void on_event(ws::input::Event& event) override {
        ws::input::EventDispatcher dispatcher(event);
        dispatcher.dispatch<ws::input::FileDroppedEvent>(
            [this](ws::input::FileDroppedEvent& drop) {
                if (!drop.paths.empty()) {
                    dropped_.insert(dropped_.begin(), drop.paths);
                    if (dropped_.size() > kDropLogCap) {
                        dropped_.resize(kDropLogCap);
                    }
                    for (const std::string& path : drop.paths) {
                        (void)recent_.touch(path);
                    }
                    (void)recent_.save();
                    std::snprintf(status_, sizeof status_, "%d files dropped",
                                  static_cast<int>(drop.paths.size()));
                }
                return true;  // the gallery owns drops: consumed
            });
    }

    // The canvas page's draw callback: every primitive, joins/caps/dashes,
    // and a 100k-primitive stress test. Drawn in WORLD space (the canvas
    // view set the transform from its pan/zoom).
    void draw_canvas_content(canvas::Canvas2D& c,
                             const canvas::ViewTransform2D& /*view*/) {
        // --- every primitive (row 1: lines, polyline, arc, circle) ---
        canvas::Paint paint;
        paint.width = 2.0f;
        paint.color = 0xFF00CCFFu;  // cyan

        // A line.
        c.stroke_line({0.0f, 0.0f}, {50.0f, 0.0f}, paint);
        // A polyline (open L).
        const canvas::Point ell[] = {{60.0f, 0.0f}, {60.0f, 30.0f}, {90.0f, 30.0f}};
        c.stroke_polyline(ell, 3, paint, false);
        // An arc.
        paint.color = 0xFFFF8800u;  // orange
        c.begin_path();
        c.move_to({110.0f, 30.0f});
        c.arc({130.0f, 30.0f}, 20.0f, 20.0f, 0.0f, canvas::kPi);
        c.stroke(paint);
        // A filled circle.
        c.fill_circle({170.0f, 15.0f}, 15.0f, 0x80FF0000u);  // translucent red
        // A circle outline.
        paint.color = 0xFFFFFFFFu;
        c.stroke_circle({170.0f, 15.0f}, 15.0f, paint);

        // --- joins/caps/dashes (row 2) ---
        paint.width = 4.0f;
        const canvas::Point join_ell[] = {{0.0f, 60.0f}, {30.0f, 60.0f}, {30.0f, 90.0f}};

        // Miter join, butt cap.
        paint.join = canvas::StrokeJoin::miter;
        paint.cap = canvas::StrokeCap::butt;
        paint.color = 0xFF00FF00u;
        c.stroke_polyline(join_ell, 3, paint, false);

        // Bevel join, round cap.
        paint.join = canvas::StrokeJoin::bevel;
        paint.cap = canvas::StrokeCap::round;
        paint.color = 0xFF0000FFu;
        canvas::Point bevel_ell[] = {{60.0f, 60.0f}, {90.0f, 60.0f}, {90.0f, 90.0f}};
        c.stroke_polyline(bevel_ell, 3, paint, false);

        // Round join, square cap.
        paint.join = canvas::StrokeJoin::round;
        paint.cap = canvas::StrokeCap::square;
        paint.color = 0xFFFF00FFu;
        canvas::Point round_ell[] = {{120.0f, 60.0f}, {150.0f, 60.0f}, {150.0f, 90.0f}};
        c.stroke_polyline(round_ell, 3, paint, false);

        // Dashed line.
        paint.dash.lengths[0] = 5.0f;
        paint.dash.lengths[1] = 3.0f;
        paint.dash.count = 2;
        paint.color = 0xFFFFFF00u;
        c.stroke_line({0.0f, 110.0f}, {90.0f, 110.0f}, paint);
        paint.dash = {};  // reset

        // --- beziers + rounded rect (row 3) ---
        paint.color = 0xFF00FFFFu;
        c.begin_path();
        c.move_to({0.0f, 130.0f});
        c.cubic_to({0.0f, 160.0f}, {50.0f, 160.0f}, {50.0f, 130.0f});
        c.stroke(paint);

        paint.color = 0xFFFF0088u;
        c.stroke_rounded_rect(70.0f, 130.0f, 40.0f, 30.0f, 8.0f, paint);
        c.fill_rounded_rect(120.0f, 130.0f, 40.0f, 30.0f, 8.0f, 0x800088FFu);

        // --- filled polygons (row 4) ---
        const canvas::Point star[] = {
            {0.0f, 170.0f}, {10.0f, 175.0f}, {20.0f, 170.0f},
            {16.0f, 180.0f}, {20.0f, 190.0f}, {10.0f, 185.0f},
            {0.0f, 190.0f}, {4.0f, 180.0f}};
        c.fill_polygon(star, 8, 0xFF888800u);

        // --- the 100k primitive stress test ---
        // 100k short lines in a dense grid, all in one stroke_polyline
        // call per row (amortized). The frame-time readout above shows
        // the cost.
        paint.width = 1.0f;
        paint.color = 0x40FFFFFFu;  // faint, so overlap reads as density
        paint.cap = canvas::StrokeCap::butt;
        constexpr int kStressRows = 100;
        constexpr int kStressCols = 1000;
        for (int row = 0; row < kStressRows; ++row) {
            const float y = 220.0f + row * 0.5f;
            c.stroke_line({0.0f, y}, {static_cast<float>(kStressCols), y}, paint);
        }
    }

    void on_update() override {
        namespace r2d = ws::ui::r2d;

        // The backdrop: the app paints its own background (the shell
        // clears depth, never color); the view menu's grid check draws
        // the reference lines behind the panels.
        r2d::begin();
        const r2d::FrameSize frame = r2d::frame_size();
        r2d::draw_quad(0.0f, 0.0f, frame.width, frame.height, kBackdrop);
        if (view_grid_) {
            for (float x = kGridStep; x < frame.width; x += kGridStep) {
                r2d::draw_line(x, 0.0f, x, frame.height, 1.0f, kGridLine);
            }
            for (float y = kGridStep; y < frame.height; y += kGridStep) {
                r2d::draw_line(0.0f, y, frame.width, y, 1.0f, kGridLine);
            }
        }
        r2d::end();

        // The gui block, the playground shape: one screen-space r2d
        // block, the base layer scoped around the page, the toasts
        // drawn after it at their own stratum (50, above the modal
        // veil's 40). The status bar is one control height tall along
        // the bottom edge - that is the toasts' bottom clearance.
        // The chord loop runs BEFORE the frame: a fired command's
        // effects (the toggled grid, the undone doc) are state the
        // widgets below then draw, and the resolver sees only the
        // InputFrame's key edges, never a held repeat.
        const ws::input::InputFrame& frame_in = ws::input::frame_input();
        resolve_chords(frame_in);
        gui_.begin_frame(frame_in);
        r2d::begin();
        gui_.begin_layer(gui::Layer::base);
        draw_gallery(frame);
        gui_.end_layer();
        const int toast_fired =
            gui::draw_toasts(gui_, chrome_, toasts_, chrome_.control_height);
        if (toast_fired >= 0) {
            // The gallery's only action toast is "file deleted" ->
            // "undo", so any fired slot is its reply (a real app maps
            // the slot to its meaning - push returned this id).
            toasts_.push({"undone - the file is back",
                          gui::ToastSeverity::info, nullptr, 3.0f});
        }
        r2d::end();
        r2d::set_layer(0);
        gui_.end_frame();
    }

private:
    // Opens the shared dialog as the about box (the menu items and the
    // context menu both land here).
    void open_about() {
        dialog_kind_ = gui::DialogButtons::ok;
        dialog_title_ = "about the gallery";
        dialog_open_ = true;
    }

    // The context menu's reset: everything with a meaningful default
    // goes back to it, so a wandered-off page recovers in one click.
    void reset_counters() {
        clicks_ = 0;
        fraction_ = 0.6f;
        count_ = 16;
        drag_ = 0.0f;
        fruit_ = 0;
        list_sel_ = 0;
        combo_sel_ = 0;
        table_sel_ = 1;
        tab_ = 0;
        lock_check_ = false;
        lock_slide_ = 0.25f;
        custom_slide_ = 0.5f;
        step_value_ = 3;
        std::snprintf(custom_buf_, sizeof custom_buf_, "skin me");
        std::snprintf(digit_buf_, sizeof digit_buf_, "42");
        toasts_.push({"counters reset", gui::ToastSeverity::info, nullptr,
                      3.0f});
    }

    // ---- the stage-10 theme flow ----
    // The page's two knobs are theme_dark_ (which stock document) and
    // accent_color_ (0 = the stock hand-picked palette); rebuild_theme()
    // re-derives the whole look from them. rebuild_font() is the
    // size-change story: a new bake IS a new font, so the stacked faces
    // re-stack (the app owns that - the growing half belongs to the font
    // that grew it) and the lazy glyphs re-request on first use.

    void rebuild_theme() {
        theme_ = theme_dark_ ? gui::default_dark() : gui::default_light();
        if (accent_color_ != 0u) {
            gui::apply_accent(theme_, accent_color_);
        }
        theme_.regular.size = font_px_;  // the size slider survives flips
        sync_chrome();
    }

    void rebuild_font() {
        font_ = ws::ui::r2d::load_font(kDefaultFontPath, font_px_ * scale_);
        if (!font_.empty() && stack_faces_) {
            (void)ws::ui::r2d::font_add_face(font_, kDefaultFontPath, 0);
            (void)ws::ui::r2d::font_add_face(font_, kExtraFontPath, 1);
        }
        last_baked_px_ = font_px_ * scale_;  // remember what we baked at
    }

    // slider_float fires 'changed' every frame while the pointer is held
    // (the documented contract, widgets.hpp). An unconditional rebuild_font
    // on every such frame re-reads the TTF from disk, re-rasterizes every
    // glyph at the new height and re-uploads the atlas - on a slow box
    // that freezes the UI during a font-px or scale drag. Glyph
    // rasterization is per INTEGER pixel height, so two bake sizes that
    // round to the same integer produce a byte-identical atlas: skip the
    // rebuild in that case. Turns a per-frame re-bake into at most one
    // rebuild per integer pixel of size change - the chrome's font
    // pointer stays valid, the metrics survive, the UI stays responsive.
    void rebuild_font_if_changed() {
        const float want = font_px_ * scale_;
        if (font_.empty() ||
            std::lround(want) != std::lround(last_baked_px_)) {
            rebuild_font();
        }
    }

    void sync_chrome() {
        chrome_ = theme_.chrome.scaled(scale_);
        chrome_.font = font_.empty() ? nullptr : &font_;
    }

    // ---- the platform page (stage 12's bridge) ----
    // Every blocking native call runs as a job: the dialogs and the
    // recovery ask block while the user decides, so a worker holds
    // them and the completions land on the main thread through the
    // drain - the same bridge the counting job rides. The borrowed-
    // bytes rule applies to every toast the completions post: the
    // text formats into a member string first.

    // Re-read the crash probe. ok + nullopt = nothing to recover; a
    // value is the marker's record; an error is the corrupt marker
    // the section shows while the manual pair keeps working.
    void refresh_probe() { probe_ = autosave_.probe(); }

    // The recover action, shared by the startup ask's yes and the
    // manual button: the crashed content becomes the draft (the
    // snprintf truncates to the field - a demo bound, noted here
    // because truncation is a bug everywhere else), the pair clears,
    // the probe re-reads.
    void do_recover() {
        const ws::core::Result<std::string> recovered = autosave_.recover();
        if (!recovered.has_value()) {
            ask_text_ = recovered.error().message;
            toasts_.push({ask_text_.c_str(), gui::ToastSeverity::error,
                          nullptr, 5.0f});
            refresh_probe();
            return;
        }
        std::snprintf(draft_buf_, sizeof draft_buf_, "%s",
                      recovered.value().c_str());
        refresh_probe();
        std::snprintf(status_, sizeof status_, "recovered the crashed draft");
        toasts_.push({"the crashed draft is back", gui::ToastSeverity::info,
                      nullptr, 3.0f});
    }

    // The discard action: the ask's no and the manual button's twin.
    void do_discard() {
        const ws::core::Status dropped = autosave_.discard();
        refresh_probe();
        if (dropped.has_value()) {
            std::snprintf(status_, sizeof status_,
                          "the crashed autosave was dropped");
        } else {
            std::snprintf(status_, sizeof status_, "discard failed: %s",
                          dropped.error().message.c_str());
        }
    }

    // The startup offer: a found marker asks through the native box
    // on the worker (an ask blocks until answered). Yes recovers, no
    // drops; the section's manual pair stays as the fallback either
    // way. A refused ask (android) crosses as the failure toast.
    void offer_recovery() {
        ws::platform::MessageBoxRequest request;
        request.kind = ws::platform::MessageBoxKind::warning;
        request.buttons = ws::platform::MessageBoxButtons::yes_no;
        request.title = "ws ui gallery";
        request.message = "the last session left an autosave - recover it?";
        ws::jobs::JobCallbacks<ws::platform::MessageBoxChoice> cbs;
        cbs.on_complete =
            [this](ws::jobs::JobHandle<ws::platform::MessageBoxChoice>
                       handle) {
                const ws::core::Result<ws::platform::MessageBoxChoice>
                    answer = gui::toast_on_failure(toasts_, ask_text_,
                                                   handle, "the recovery ask");
                if (!answer.has_value()) {
                    return;  // the failure toast already spoke
                }
                if (answer.value() == ws::platform::MessageBoxChoice::yes) {
                    do_recover();
                } else {
                    do_discard();
                }
            };
        ask_out_ = jobs_.submit<ws::platform::MessageBoxChoice>(
            [this, request](ws::jobs::JobContext&)
                -> ws::core::Result<ws::platform::MessageBoxChoice> {
                return boxes_->ask(request);
            },
            ws::jobs::Priority::normal, {}, cbs);
    }

    // One of the three single-path pickers (0 open, 1 save, 2
    // folder). The worker holds the modal; the completion lands
    // between frames and feeds the MRU - an open and a save are both
    // use - while the status line echoes the answer or the
    // dismissal (a dismissed dialog is a normal answer).
    void pick_file(int kind) {
        static constexpr const char* const kDialogNames[] = {
            "the open dialog", "the save dialog", "the folder dialog"};
        ws::platform::FileDialogOptions options;
        options.title = kDialogNames[kind];
        if (kind == 1 && title_doc_[0] != '\0') {
            options.default_path = title_doc_;  // seeds the file-name box
        }
        ws::jobs::JobCallbacks<std::optional<std::string>> cbs;
        cbs.on_complete =
            [this, kind](ws::jobs::JobHandle<std::optional<std::string>>
                             handle) {
                const ws::core::Result<std::optional<std::string>> picked =
                    gui::toast_on_failure(toasts_, pick_text_, handle,
                                          kDialogNames[kind]);
                if (!picked.has_value()) {
                    return;  // the failure toast already spoke
                }
                if (!picked.value().has_value()) {
                    std::snprintf(status_, sizeof status_,
                                  "%s was dismissed", kDialogNames[kind]);
                    return;
                }
                const std::string& path = picked.value().value();
                (void)recent_.touch(path);
                (void)recent_.save();
                std::snprintf(status_, sizeof status_, "%s answered: %s",
                              kDialogNames[kind], path.c_str());
            };
        dialog_out_ = jobs_.submit<std::optional<std::string>>(
            [this, kind, options](ws::jobs::JobContext&)
                -> ws::core::Result<std::optional<std::string>> {
                switch (kind) {
                    case 0:
                        return dialogs_->open_file(options);
                    case 1:
                        return dialogs_->save_file(options);
                    default:
                        break;
                }
                return dialogs_->select_folder(options);
            },
            ws::jobs::Priority::normal, {}, cbs);
    }

    // The multi-select open: several files at once, each one used.
    void pick_files() {
        ws::platform::FileDialogOptions options;
        options.title = "the multi-open dialog";
        ws::jobs::JobCallbacks<std::vector<std::string>> cbs;
        cbs.on_complete =
            [this](ws::jobs::JobHandle<std::vector<std::string>> handle) {
                const ws::core::Result<std::vector<std::string>> picked =
                    gui::toast_on_failure(toasts_, pick_text_, handle,
                                          "the multi-open dialog");
                if (!picked.has_value()) {
                    return;  // the failure toast already spoke
                }
                const std::vector<std::string>& files = picked.value();
                if (files.empty()) {
                    std::snprintf(status_, sizeof status_,
                                  "the multi-open dialog was dismissed");
                    return;
                }
                for (const std::string& file : files) {
                    (void)recent_.touch(file);
                }
                (void)recent_.save();
                std::snprintf(status_, sizeof status_,
                              "opened %d files, first: %s",
                              static_cast<int>(files.size()),
                              files.front().c_str());
            };
        files_out_ = jobs_.submit<std::vector<std::string>>(
            [this, options](ws::jobs::JobContext&)
                -> ws::core::Result<std::vector<std::string>> {
                return dialogs_->open_files(options);
            },
            ws::jobs::Priority::normal, {}, cbs);
    }

    // Compose the title through ws::platform (pure, headless) and
    // hand the answer to the OS through the ui seam - stage 12's
    // split of labor. The echo copy stays for the line below the
    // field: android's host no-ops set_title (the activity's label
    // is manifest-bound), so the echo is the platform-neutral half
    // of the demo.
    void compose_title() {
        const ws::core::Result<std::string> composed =
            ws::platform::window_title("ws ui gallery", title_doc_,
                                        title_dirty_);
        if (composed.has_value()) {
            std::snprintf(title_last_, sizeof title_last_, "%s",
                          composed.value().c_str());
            set_title(composed.value());
            std::snprintf(status_, sizeof status_, "title: %s",
                          composed.value().c_str());
        } else {
            std::snprintf(title_last_, sizeof title_last_, "title failed: %s",
                          composed.error().message.c_str());
        }
    }

    // ---- the commands page (stage 13's bridge) ----
    // The registry drives everything here and the stack is the demo
    // doc's sole executor. Registration refusals are programmer
    // errors, but the demo shows the channel anyway: a refusal names
    // itself in a toast (the borrowed-bytes rule: cmd_text_ holds it).

    void add_command(ws::app::Command command) {
        const ws::core::Status added = commands_.add(std::move(command));
        if (!added.has_value()) {
            cmd_text_ = added.error().message;
            toasts_.push({cmd_text_.c_str(), gui::ToastSeverity::error,
                          nullptr, 5.0f});
        }
    }

    // The registry, built once. Registration order is the menu order
    // within a category, and the declared chords (GLFW numbers, the
    // Mod bits mirroring GLFW_MOD_*) are what install_defaults seeds.
    // The enabled/checked predicates are read live per frame - the
    // Undo/Redo rows gray out with the stack, the view rows wear
    // their checks. Command bodies carry no error channel (the lib's
    // rule): a failing action posts its own toast, and the undo demo
    // shows that pattern twice (a full doc, an empty-branch undo).
    // Every member without its own default initializer is spelled
    // out - .shortcut = {} / .enabled = {} / .checked = {} - because
    // the NDK's clang treats a skipped no-default member as a
    // -Wmissing-field-initializers error (-Wextra -Werror; MSVC
    // carries no such warning), and the empty fill IS the lib's
    // spelling: key 0 = no chord declared, an empty predicate =
    // always enabled / never checked.
    void build_commands() {
        add_command({.id = "edit.undo",
                     .label = "Undo",
                     .category = "Edit",
                     .shortcut = {90, ws::app::Mod::control},
                     .enabled = [this] { return undo_.can_undo(); },
                     .checked = {},
                     .execute = [this](std::string_view) { do_undo(); }});
        add_command({.id = "edit.redo",
                     .label = "Redo",
                     .category = "Edit",
                     .shortcut = {89, ws::app::Mod::control},
                     .enabled = [this] { return undo_.can_redo(); },
                     .checked = {},
                     .execute = [this](std::string_view) { do_redo(); }});
        add_command({.id = "edit.append",
                     .label = "Append Text...",
                     .category = "Edit",
                     .shortcut = {68, ws::app::Mod::control},
                     .needs_argument = true,
                     .enabled = {},
                     .checked = {},
                     .execute = [this](std::string_view argument) {
                         std::string next = undo_doc_;
                         next += argument.empty() ? " word" : " ";
                         if (!argument.empty()) {
                             next.append(argument);
                         }
                         if (next.size() >= sizeof undo_doc_) {
                             cmd_text_ = "the append does not fit the demo doc";
                             toasts_.push({cmd_text_.c_str(),
                                           gui::ToastSeverity::warning,
                                           nullptr, 4.0f});
                             return;
                         }
                         push_doc_edit(next, "Append");
                     }});
        add_command({.id = "edit.clear",
                     .label = "Clear the Demo Doc",
                     .category = "Edit",
                     // chordless on purpose: the keymap editor's
                     // bind-path demo - Record gives it one below.
                     .shortcut = {},
                     .enabled = {},
                     .checked = {},
                     .execute = [this](std::string_view) {
                         if (undo_doc_[0] == '\0') {
                             return;  // an empty doc records nothing
                         }
                         push_doc_edit("", "Clear");
                     }});
        add_command({.id = "edit.mark_saved",
                     .label = "Mark the Doc Saved",
                     .category = "Edit",
                     .shortcut = {},
                     .enabled = {},
                     .checked = {},
                     .execute = [this](std::string_view) { do_mark_saved(); }});
        add_command({.id = "view.grid",
                     .label = "Grid Backdrop",
                     .category = "View",
                     .shortcut = {71, ws::app::Mod::control},
                     .enabled = {},
                     .checked = [this] { return view_grid_; },
                     .execute = [this](std::string_view) {
                         view_grid_ = !view_grid_;
                         std::snprintf(status_, sizeof status_, "grid %s",
                                       view_grid_ ? "on" : "off");
                     }});
        add_command({.id = "view.tooltips",
                     .label = "Tooltips",
                     .category = "View",
                     .shortcut = {},
                     .enabled = {},
                     .checked = [this] { return view_tooltips_; },
                     .execute = [this](std::string_view) {
                         view_tooltips_ = !view_tooltips_;
                         std::snprintf(status_, sizeof status_, "tooltips %s",
                                       view_tooltips_ ? "on" : "off");
                     }});
        add_command({.id = "tools.palette",
                     .label = "Command Palette",
                     .category = "Tools",
                     .shortcut = {80,
                                  ws::app::Mod::control | ws::app::Mod::shift},
                     .enabled = {},
                     .checked = {},
                     .execute = [this](std::string_view) {
                         palette_open_ = true;
                     }});
        add_command({.id = "help.about",
                     .label = "About the Gallery",
                     .category = "Help",
                     .shortcut = {290, ws::app::Mod::none},
                     .enabled = {},
                     .checked = {},
                     .execute = [this](std::string_view) { open_about(); }});
    }

    // Whether the resolver stands down this frame: any overlay that
    // owns the keyboard takes the keys - the palette (its search
    // field), the shared dialog, the raw modal, either menu bar's
    // open chain, the context menu, the combo's dropdown, and the
    // keymap editor's Record capture (a chord being recorded must not
    // also fire the command it would match live).
    [[nodiscard]] bool chords_stand_down() const {
        return palette_open_ || dialog_open_ || raw_modal_open_ ||
               bar_open_ >= 0 || cmd_bar_open_ >= 0 || ctx_open_ ||
               combo_open_ || keymap_state_.recording;
    }

    // The live chord loop: every key edge this frame, with the held
    // modifiers composed into the chord, resolved at the global rung
    // - the demo keeps one context (the ladder's bottom, where every
    // command answers; the text rule never bites because every demo
    // chord carries control or is F1, keys a focused field does not
    // claim). The modifiers' own edges are skipped: pressing Ctrl
    // alone is not a chord. A consumed resolution dispatches through
    // the registry; a pending sequence step stays armed in the
    // resolver for its second chord.
    void resolve_chords(const ws::input::InputFrame& frame) {
        if (chords_stand_down()) {
            return;
        }
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
            if (!frame.key_pressed[key]) {
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
            const ws::app::Resolution hit =
                resolver_.resolve({key, mods}, ws::app::KeyContext::global);
            if (hit.consumed && !hit.command.empty()) {
                dispatch_command(hit.command);
            }
        }
    }

    // The one dispatch funnel: menu items, toolbar fires, palette
    // picks and chords all land here. execute() deliberately ignores
    // enabled() - the views gate their own chrome, the caller decides
    // a direct dispatch - and the registry's Result covers lookup
    // only, so a refusal is the only error there is. Every fire is
    // noted into the palette's recents, whatever entry point it came
    // from - the empty-query list then shows what this app actually
    // runs.
    void dispatch_command(std::string_view id, std::string_view argument = {}) {
        palette_.note_used(id);
        const ws::core::Status done = commands_.execute(id, argument);
        if (!done.has_value()) {
            cmd_text_ = done.error().message;
            toasts_.push({cmd_text_.c_str(), gui::ToastSeverity::error,
                          nullptr, 5.0f});
        }
    }

    // The one funnel for every demo-doc edit. PUSH IS THE EDIT (the
    // stack's rule: the stack is the sole executor): a caller hands
    // over the new text while the buffer already holds it, so the
    // funnel reverts the buffer to the last frame's text and lets the
    // push's apply() land the edit again - apply() then captures its
    // inverse (the reverted text) at the right moment, and redo stays
    // exact. The EditBuffer clamps make the one-frame revert safe:
    // the field re-measures the shorter buffer and the frame-end
    // write-back settles the caret into it. "Clear" is the one
    // non-mergeable kind (see DocEdit).
    void push_doc_edit(const std::string& next, const char* kind) {
        std::snprintf(undo_doc_, sizeof undo_doc_, "%s", last_doc_.c_str());
        const ws::core::Status pushed = undo_.push(std::make_unique<DocEdit>(
            undo_doc_, static_cast<int>(sizeof undo_doc_), next, kind,
            std::string_view{kind} != "Clear"));
        if (!pushed.has_value()) {
            cmd_text_ = pushed.error().message;
            toasts_.push({cmd_text_.c_str(), gui::ToastSeverity::error,
                          nullptr, 4.0f});
        }
    }

    // The undo/redo pair, shared by the chords, the registry's Edit
    // menu, the toolbar and the demo's own buttons. A refusal (the
    // empty branch - a chord fires regardless of enabled()) speaks
    // as a warning toast; a success echoes the reverted doc. Both
    // re-sync last_doc_: the stack moved the buffer, and the funnel's
    // revert source must follow it or the next typing step would
    // revert to a stale text.
    void do_undo() {
        const ws::core::Status step = undo_.undo();
        if (!step.has_value()) {
            cmd_text_ = step.error().message;
            toasts_.push({cmd_text_.c_str(), gui::ToastSeverity::warning,
                          nullptr, 4.0f});
        } else {
            std::snprintf(status_, sizeof status_, "undo: %s", undo_doc_);
        }
        last_doc_ = undo_doc_;
    }

    void do_redo() {
        const ws::core::Status step = undo_.redo();
        if (!step.has_value()) {
            cmd_text_ = step.error().message;
            toasts_.push({cmd_text_.c_str(), gui::ToastSeverity::warning,
                          nullptr, 4.0f});
        } else {
            std::snprintf(status_, sizeof status_, "redo: %s", undo_doc_);
        }
        last_doc_ = undo_doc_;
    }

    // The save point: is_dirty() flips the line below, and the doc
    // reads clean again after undoing back onto the saved position -
    // the position semantics, not a boolean the app tracks.
    void do_mark_saved() {
        const ws::core::Status saved = undo_.mark_saved();
        if (!saved.has_value()) {
            cmd_text_ = saved.error().message;
            toasts_.push({cmd_text_.c_str(), gui::ToastSeverity::warning,
                          nullptr, 4.0f});
        } else {
            std::snprintf(status_, sizeof status_,
                          "the demo doc is marked saved");
        }
    }

    void draw_gallery(const ws::ui::r2d::FrameSize& frame);

    // The custom-look fields' shared paint over a text behavior answer:
    // the selection band under the text, the text at the content x the
    // behavior mapped the caret against (the font, or the fontless bar),
    // and a caret block gated on the reported blink phase. No face, no
    // ring - the chrome around it is the caller's half.
    void paint_bare_field(const gui::Rect& row,
                          const gui::TextInputResult& result,
                          const char* buf);

    // ---- the shell seats ----
    gui::GuiContext gui_;
    gui::LayoutCursor layout_;
    gui::ChromeTheme chrome_ = gui::kDefaultChrome;
    ws::ui::r2d::Font font_;
    float scale_ = 1.0f;

    // ---- the theme demo (stage 10) ----
    gui::ThemeData theme_ = gui::default_dark();
    bool theme_dark_ = true;            // which stock document is active
    int theme_pick_ = 0;                // the dark/light radio row
    std::uint32_t accent_color_ = 0u;   // 0 = the stock hand-picked palette
    int accent_pick_ = 0;               // the accent radio row
    float font_px_ = 16.0f;             // the live bake size (the doc mirrors it)
    float last_baked_px_ = 0.0f;        // font_px_*scale_ at the last rebuild_font;
                                        // rebuild_font_if_changed skips the
                                        // disk+CPU+GPU re-bake when the integer
                                        // pixel height hasn't moved (slider drag)
    bool font_size_dirty_ = false;      // a size slider moved; the font atlas
                                        // re-bake is deferred to the frame the
                                        // slider stopped changing (drag end)
    bool stack_faces_ = false;          // the growing-font demo's stack state
    std::uint32_t lazy_baked_ = 0;      // glyphs landed through font_bake_pending

    // ---- caller-owned widget state (the gallery IS this list) ----
    std::array<bool, kSectionCount> section_{};
    // The canvas page's view + renderer (stage 14).
    canvas::CanvasView canvas_view_{};
    canvas::Renderer2DCanvas canvas_renderer_{};
    float canvas_frame_ms_ = 0.0f;  // the stress test's frame-time readout
    int clicks_ = 0;
    bool checked_ = false;
    bool dead_check_ = true;  // starts checked: a dimmed-on box reads too
    bool switched_ = true;
    bool dead_switch_ = true;
    int fruit_ = 0;
    float fraction_ = 0.6f;
    int count_ = 16;
    float drag_ = 0.0f;
    char name_buf_[32] = "gallery";
    char locked_buf_[32] = "read only";
    int list_sel_ = 0;
    int combo_sel_ = 0;
    bool combo_open_ = false;
    int table_sel_ = 1;
    int tab_ = 0;
    bool popup_open_ = false;
    gui::Rect popup_point_{};  // where the open popup was triggered
    bool raw_modal_open_ = false;
    bool dialog_open_ = false;
    gui::DialogButtons dialog_kind_ = gui::DialogButtons::ok_cancel;
    const char* dialog_title_ = "a modal dialog";
    char dialog_buf_[24] = "typed in the veil";
    int bar_open_ = -1;  // menu_bar's open menu index (-1 = none)
    bool ctx_open_ = false;
    bool view_grid_ = true;
    bool view_tooltips_ = true;
    int flood_size_ = 6;
    bool ctx_snap_ = true;
    bool lock_ = false;
    bool lock_check_ = false;
    float lock_slide_ = 0.25f;
    gui::Toasts toasts_;

    // ---- the custom-look demo (stage 11: the behavior/draw seam) ----
    // One slider value and one text buffer shared by the standard and
    // the custom-paint views of them - the seam's whole point: the same
    // behavior state, several looks. The digits-only field keeps its
    // own buffer, because its rule strips what the others accept.
    float custom_slide_ = 0.5f;             // one value, three slider looks
    char custom_buf_[32] = "skin me";       // one buffer, two field looks
    gui::TextInputState underline_edit_;    // the underline field's edit pair
    char digit_buf_[16] = "42";             // the digits-only field's buffer
    gui::TextInputState digit_edit_;        // its edit pair
    int step_value_ = 3;                    // the app-own stepper's lit count

    // The last event's one-line echo (the status bar's tail); every
    // widget reaction above writes here, nothing clears it.
    char status_[96] = "nothing has happened yet";

    // ---- the jobs demo (the ws::jobs bridge) ----
    // Two workers: enough to show the pool without spinning a thread
    // per core on a phone. Caller-owned, like everything here - the
    // shell only drains it.
    ws::jobs::JobSystem jobs_{2};
    // The counting job's mirror + handle; the row reads both every
    // frame, the drain fills the mirror between them.
    gui::JobProgress work_mirror_;
    ws::jobs::JobHandle<int> work_{};
    // The failure toasts' long-lived text - the toast store borrows
    // its bytes (the flood literals' rule), so they must outlive the
    // toast: members, never stack strings.
    std::string work_text_;
    std::string fail_text_;

    // ---- the platform page's state (stage 12) ----
    // fs_ first: recent_ and autosave_ borrow it, and the in-class
    // initializers run in declaration order. The store dir was made
    // by platform_store_dir() - the OS data location on desktop, the
    // CWD on android - so both members persist real files.
    ws::settings::StdFileSystem fs_;
    std::string store_dir_ = platform_store_dir();
    ws::platform::RecentFiles recent_{
        fs_, ws::platform::join_path(store_dir_, "recent.json"), 8};
    ws::platform::Autosave autosave_{
        fs_, "gallery draft", ws::platform::join_path(store_dir_, "autosave")};
    // The native pickers and asker, this platform's set (the COM
    // family + MessageBoxW on windows, the refusals on android),
    // built once in on_setup. The handles gate the buttons so one
    // native thing is out at a time - the worker pool could hold
    // two, but the demo reads clearer one at a time.
    std::unique_ptr<ws::platform::FileDialogs> dialogs_;
    std::unique_ptr<ws::platform::MessageBoxes> boxes_;
    ws::jobs::JobHandle<std::optional<std::string>> dialog_out_{};
    ws::jobs::JobHandle<std::vector<std::string>> files_out_{};
    ws::jobs::JobHandle<ws::platform::MessageBoxChoice> ask_out_{};
    // The crash probe's live answer, re-read by refresh_probe(): ok
    // + nullopt = nothing to recover, a value = the marker's record,
    // an error = the corrupt marker the section shows.
    ws::core::Result<std::optional<ws::platform::Recovery>> probe_{};
    // The drop log: one entry per drop (the drop's whole path list),
    // newest first, capped at kDropLogCap by on_event.
    std::vector<std::vector<std::string>> dropped_;
    // The open-with documents main parsed out of argv (empty on
    // android), consumed by on_setup.
    std::vector<std::string> launch_docs_;
    // The sections' own state: the autosave draft (the "document"
    // the crash story edits), the title page's document name + dirty
    // bit (window_title composes, set_title hands the answer to the
    // OS), the last composed title echoed as a line so android sees
    // it too (its host no-ops the seam), the cursor radio's pick,
    // and the clipboard read-backs' echo buffer.
    char draft_buf_[64] = "the gallery draft";
    char title_doc_[32] = "untitled.txt";
    bool title_dirty_ = false;
    char title_last_[96] = "never composed";
    int cursor_pick_ = 0;
    char clip_buf_[96] = "(nothing read back yet)";
    // The completions' toast texts - the store borrows bytes, so
    // members, like work_text_/fail_text_ above.
    std::string recent_text_;
    std::string pick_text_;
    std::string ask_text_;

    // ---- the commands page's state (stage 13) ----
    // The app-framework trio, caller-owned (the lib's rule): the
    // registry built once in on_setup, the resolver seeded from the
    // declared chords then overlaid by the keymap file, the undo
    // stack over the demo doc. keymap_file_ rides the settings seam
    // like recent_/autosave_ above it - the whole live table
    // persists, no provenance (the resolver keeps none).
    ws::app::CommandRegistry commands_;
    ws::app::ShortcutResolver resolver_;
    ws::app::Keymap keymap_file_{
        fs_, ws::platform::join_path(store_dir_, "keymap.json")};
    ws::app::UndoStack undo_;
    // The views' caller-owned state: the menu builder (described
    // once, rebuilt every frame), the palette (its recents survive
    // closes), the keymap editor, and the toolbar's chosen ids - the
    // toolbar is a caller's id list by design, not the registry's
    // whole walk.
    gui::CommandMenuBuilder menu_builder_;
    gui::CommandPalette palette_;
    gui::KeymapEditor keymap_state_;
    std::vector<std::string> toolbar_ids_{"edit.undo", "edit.redo",
                                          "edit.append", "view.grid",
                                          "help.about"};
    bool palette_open_ = false;
    int cmd_bar_open_ = -1;  // the commands menu bar's open index (-1 none)
    // The undo demo's doc buffer and the funnel's revert source: the
    // text as of the last sync (the section's end, and every stack
    // move in do_undo/do_redo). last_doc_ is initialized from the
    // buffer's own initializer, in declaration order.
    char undo_doc_[64] = "the demo doc";
    std::string last_doc_ = undo_doc_;
    // The dynamic Undo/Redo button labels, host-composed from the
    // stack's own step names (the bridge header never names the undo
    // stack). The "###" tails keep the widget ids stable while the
    // display text changes - the id-redirect rule.
    char undo_label_[40] = "undo (nothing)###undo step";
    char redo_label_[40] = "redo (nothing)###redo step";
    // The command page's toast texts (the borrowed-bytes rule again).
    std::string cmd_text_;
};

void GalleryApp::paint_bare_field(const gui::Rect& row,
                                  const gui::TextInputResult& result,
                                  const char* buf) {
    namespace r2d = ws::ui::r2d;
    // The marks ride the font's line box centered on the row, or the
    // half-height bar fontless - the app re-derives what the stock
    // widgets spell through their private helpers.
    const float mark_h = chrome_.font != nullptr
                             ? chrome_.font->line_height()
                             : row.height * 0.5f;
    const float mark_y = row.y + (row.height - mark_h) * 0.5f;
    const float content_x = row.x + chrome_.padding;

    // The selection band first, so the text draws over it - the span
    // arrives already clamped; the visibility gates are this look's.
    if (result.focused && result.sel_x1 > result.sel_x0) {
        r2d::draw_quad(result.sel_x0, mark_y, result.sel_x1 - result.sel_x0,
                       mark_h, chrome_.control_on);
    }
    if (chrome_.font != nullptr) {
        (void)r2d::draw_text(content_x, mark_y, buf, *chrome_.font,
                            chrome_.text);
    } else if (buf[0] != '\0') {
        // The fontless v1 bar at the per-char estimate, like every
        // label the chrome draws without a face.
        r2d::draw_quad(content_x, mark_y,
                       chrome_.label_char_width *
                           static_cast<float>(std::strlen(buf)),
                       mark_h, chrome_.text);
    }
    if (result.focused && result.blink < 0.5f) {
        r2d::draw_quad(result.caret_x, mark_y, 2.0f * scale_, mark_h,
                       chrome_.text);
    }
}

void GalleryApp::draw_gallery(const ws::ui::r2d::FrameSize& frame) {
    // ---- the modal dialog first: the veil-first rule ------------------------
    // The veil is frame-wide the frame it rises, so the call must run
    // before the widgets it blocks. One call site plays every standard
    // button set (the section's buttons pick `kind_`); the body shows
    // the dialog's own cursor and id scope - a live text input the
    // veil traps Tab inside - and the none set's one way out, its own
    // button (a none dialog takes no Enter and no Escape at all).
    if (dialog_open_) {
        const gui::DialogEvent ev = gui::modal_dialog(
            gui_, chrome_, "gallery dialog", dialog_title_, dialog_kind_,
            dialog_open_, 300.0f * scale_,
            [this](gui::GuiContext& g, gui::LayoutCursor& l,
                   const gui::ChromeTheme& t) {
                gui::text(l, t, "the veil blocks the page below;", -1.0f);
                gui::text(l, t, "Tab stays trapped in here, and a toast",
                          -1.0f);
                gui::text(l, t, "still rides above it (50 over 40)", -1.0f);
                gui::text_input(g, l, t, "name", dialog_buf_,
                                static_cast<int>(sizeof dialog_buf_), -1.0f);
                if (dialog_kind_ == gui::DialogButtons::none) {
                    if (gui::button(g, l, t, "close this dialog", -1.0f)) {
                        dialog_open_ = false;
                    }
                }
            });
        if (ev.button >= 0) {
            std::snprintf(status_, sizeof status_, "dialog: %s",
                          ev.accepted ? "accepted" : "dismissed");
            toasts_.push({ev.accepted ? "the dialog was accepted"
                                      : "the dialog was dismissed",
                          ev.accepted ? gui::ToastSeverity::info
                                      : gui::ToastSeverity::warning,
                          nullptr, 3.0f});
        }
    }

    // ---- the command palette: veil-first with the dialog ---------------------
    // The centered modal over the registry: fuzzy search, the LIVE
    // chords in the Shortcut column (rebind one in the keymap editor
    // and this list renames too), recents first on an empty query.
    // It must draw before the widgets it blocks - the same veil-first
    // rule as the dialog above - and the chord that opened it is
    // still in this frame's edges, so the palette answers no keys on
    // its opening frame (its own rule, the modal_dialog's). `open` by
    // value: the palette never clears the caller's flag itself.
    if (palette_open_) {
        const gui::PaletteEvent cmd_palette_ev = gui::command_palette(
            gui_, chrome_, "command palette", palette_open_, commands_,
            &resolver_, ws::app::KeyContext::global, palette_,
            360.0f * scale_);
        if (cmd_palette_ev.dispatch) {
            dispatch_command(cmd_palette_ev.command, cmd_palette_ev.argument);
        }
        if (cmd_palette_ev.close) {
            palette_open_ = false;
        }
    }

    // ---- the chrome strip: menu bar + the context-menu target ---------------
    // One panel holds both rows: the bar's titles flow left along the
    // top, and the strip as a whole is the context menu's region -
    // right-click it (press and hold on touch) and the menu opens at
    // the pointer. The item arrays are per-frame stack data, the menus'
    // discipline: re-described every frame, what fires comes back as a
    // pointer into this frame's own arrays.
    const float strip_h = chrome_.padding * 2.0f +
                          chrome_.control_height * 2.0f + chrome_.spacing;
    const gui::Rect strip{0.0f, 0.0f, frame.width, strip_h};
    const gui::Rect strip_content =
        gui::begin_panel(gui_, layout_, chrome_, "gallery bar", strip);

    const gui::MenuItem recent[] = {{"alpha.txt"}, {"beta.txt"}};
    const gui::MenuItem demo_items[] = {
        {"&new", {}, "Ctrl+N"},
        {"open...", {}, "Ctrl+O"},
        {.kind = gui::MenuItemKind::separator},
        {"recent", {}, nullptr, recent,
         static_cast<int>(sizeof(recent) / sizeof(recent[0]))},
        {.kind = gui::MenuItemKind::separator},
        {"&about the gallery", {}, "F1"},
    };
    const gui::MenuItem view_items[] = {
        {.label = "grid backdrop", .kind = gui::MenuItemKind::check,
         .checked = view_grid_},
        {.label = "tooltips", .kind = gui::MenuItemKind::check,
         .checked = view_tooltips_},
        {.kind = gui::MenuItemKind::separator},
        {.label = "calm floods", .kind = gui::MenuItemKind::radio,
         .checked = flood_size_ == 3},
        {.label = "lively floods", .kind = gui::MenuItemKind::radio,
         .checked = flood_size_ == 6},
        {.label = "storm floods", .kind = gui::MenuItemKind::radio,
         .checked = flood_size_ == 10},
    };
    const gui::MenuItem help_items[] = {
        {"&about", {}, "F1"},
        {.label = "docs", .shortcut = "F2", .disabled = true,
         .disabled_reason = "not shipped yet"},
    };
    const gui::MenuDesc bar[] = {
        {"&demo", demo_items, sizeof(demo_items) / sizeof(demo_items[0])},
        {"&view", view_items, sizeof(view_items) / sizeof(view_items[0])},
        {"&help", help_items, sizeof(help_items) / sizeof(help_items[0])},
    };
    const gui::MenuEvent ev = gui::menu_bar(gui_, layout_, chrome_, "the bar",
                                            bar, 3, bar_open_);
    if (ev.item != nullptr) {
        // Identity against this frame's own arrays is the whole wiring;
        // checks and radios flip caller state (the rows show it next
        // frame) and the rest speak through toasts.
        if (ev.item == &demo_items[0]) {
            toasts_.push({"nothing to save yet", gui::ToastSeverity::info,
                          nullptr, 3.0f});
        } else if (ev.item == &demo_items[1]) {
            toasts_.push({"the gallery owns no files",
                          gui::ToastSeverity::info, nullptr, 3.0f});
        } else if (ev.item == &recent[0]) {
            toasts_.push({"opened alpha.txt", gui::ToastSeverity::info,
                          nullptr, 3.0f});
        } else if (ev.item == &recent[1]) {
            toasts_.push({"opened beta.txt", gui::ToastSeverity::info,
                          nullptr, 3.0f});
        } else if (ev.item == &demo_items[5] || ev.item == &help_items[0]) {
            open_about();
        } else if (ev.item == &view_items[0]) {
            view_grid_ = !view_grid_;
        } else if (ev.item == &view_items[1]) {
            view_tooltips_ = !view_tooltips_;
        } else if (ev.item == &view_items[3]) {
            flood_size_ = 3;
        } else if (ev.item == &view_items[4]) {
            flood_size_ = 6;
        } else if (ev.item == &view_items[5]) {
            flood_size_ = 10;
        }
    }
    // The strip's second row, truncated to the content width so a
    // narrow phone clips the hint instead of spilling past the chrome.
    gui::text(layout_, chrome_,
              "right-click (or press and hold) this strip - a context menu "
              "opens at the pointer",
              strip_content.width);
    gui::end_panel(gui_, layout_);

    const gui::MenuItem ctx_items[] = {
        {"reset the counters", {}, "Ctrl+R"},
        {.label = "snap to grid", .kind = gui::MenuItemKind::check,
         .checked = ctx_snap_},
        {.kind = gui::MenuItemKind::separator},
        {"about the gallery", {}, "F1"},
        {.label = "delete the gallery", .shortcut = "Del",
         .disabled = true, .disabled_reason = "there is nothing to delete"},
    };
    const gui::MenuEvent ctx = gui::context_menu(gui_, layout_, chrome_,
                                                 "gallery ctx", strip,
                                                 ctx_items,
                                                 sizeof(ctx_items) / sizeof(ctx_items[0]),
                                                 ctx_open_);
    if (ctx.item != nullptr) {
        if (ctx.item == &ctx_items[0]) {
            reset_counters();
        } else if (ctx.item == &ctx_items[1]) {
            ctx_snap_ = !ctx_snap_;
        } else if (ctx.item == &ctx_items[3]) {
            open_about();
        }
    }

    // ---- the page: a scroll panel of sections -------------------------------
    // The scroll panel is itself a gallery piece (wheel + touch drag,
    // the persistent offset), and it gives the sections room on a
    // phone. It stops short of the status bar along the bottom edge.
    const float status_h = chrome_.control_height;
    const gui::Rect page_bounds{0.0f, strip_h, frame.width,
                                std::max(0.0f, frame.height - strip_h -
                                                   status_h)};
    const gui::Rect page = gui::begin_scroll_panel(gui_, layout_, chrome_,
                                                   "gallery page", page_bounds);
    const float half = (page.width - chrome_.spacing) * 0.5f;

    // --- theme (stage 10): the document, the accent, the growing font ----
    if (gui::collapsing_header(gui_, layout_, chrome_,
                               "theme: accent, sizes, the growing font",
                               section_[kTheme])) {
        // The live OS answer - read per call, never cached (no OS theme
        // event exists on either platform), so flipping the system
        // setting recolors this line without a restart. The radios pick
        // the ACTIVE document; "adopt" snaps them to whatever the OS
        // says right now.
        char os_line[80];
        std::snprintf(os_line, sizeof os_line,
                      "the OS says %s mode right now - live, no restart",
                      prefers_dark() ? "dark" : "light");
        gui::text(layout_, chrome_, os_line, -1.0f);
        if (gui::radio(gui_, layout_, chrome_, "dark", theme_pick_, 0)) {
            theme_dark_ = true;
            rebuild_theme();
            std::snprintf(status_, sizeof status_, "theme: dark");
        }
        layout_.same_line();
        if (gui::radio(gui_, layout_, chrome_, "light", theme_pick_, 1)) {
            theme_dark_ = false;
            rebuild_theme();
            std::snprintf(status_, sizeof status_, "theme: light");
        }
        layout_.same_line();
        if (gui::button(gui_, layout_, chrome_, "adopt the OS answer",
                        -1.0f)) {
            theme_dark_ = prefers_dark();
            theme_pick_ = theme_dark_ ? 0 : 1;
            rebuild_theme();
            std::snprintf(status_, sizeof status_,
                          "theme: adopted the OS answer");
        }

        // The accent rows. Each pick re-derives control_on / pressed /
        // hovered / disabled from one color - and the selected radio
        // itself already wears the answer, so the row below demos the
        // pick with no extra code at all.
        for (int i = 0; i < kAccentCount; ++i) {
            if (gui::radio(gui_, layout_, chrome_, kAccents[i].name,
                           accent_pick_, i)) {
                accent_color_ = kAccents[i].color;
                rebuild_theme();
                std::snprintf(status_, sizeof status_, "accent: %s",
                              kAccents[i].name);
            }
            if (i + 1 < kAccentCount) {
                layout_.same_line();
            }
        }

        // The size knobs. A size change IS a new font - the whole bake
        // again, the stack re-added, the lazy glyphs re-requested on
        // first use - and the scale slider multiplies every metric and
        // the bake size with it.
        //
        // PERF: slider_float fires 'changed' every frame during a drag.
        // rebuild_font() re-reads the TTF from disk, re-rasterizes all 95
        // baked glyphs and re-uploads the atlas - ~50-200 ms per call on
        // a slow box. Calling it every changed frame freezes the UI.
        // Instead we DEFER the rebuild: sync_chrome() (cheap - just the
        // scaled metrics + the font pointer, no disk/CPU/GPU work) runs
        // every frame so the layout tracks the slider live, and the
        // expensive font atlas re-bake runs only on the frame the slider
        // STOPPED changing (the drag ended). A full drag is then ONE
        // rebuild, not dozens.
        bool size_changed_this_frame = false;
        if (gui::slider_float(gui_, layout_, chrome_, "font px", font_px_,
                              10.0f, 28.0f, half)) {
            theme_.regular.size = font_px_;
            sync_chrome();
            size_changed_this_frame = true;
            font_size_dirty_ = true;
            std::snprintf(status_, sizeof status_, "font re-baked at %.0f px",
                          font_px_);
        }
        layout_.same_line();
        if (gui::slider_float(gui_, layout_, chrome_, "scale x", scale_,
                              0.75f, 2.0f, half)) {
            sync_chrome();
            size_changed_this_frame = true;
            font_size_dirty_ = true;
            std::snprintf(status_, sizeof status_, "ui scale: %.2f", scale_);
        }
        // The sliders did NOT change this frame but the font is dirty:
        // the drag just ended (or the user clicked once and released).
        // Rebuild the atlas now - one re-bake per drag, not per frame.
        if (font_size_dirty_ && !size_changed_this_frame) {
            rebuild_font_if_changed();
            sync_chrome();
            font_size_dirty_ = false;
        }

        // The growing font, live. The line below draws tofu for
        // everything past ASCII until its codepoints are requested and
        // baked; the request + bake pair runs every frame (the intended
        // cadence - both no-op once the queue is dry), the button stacks
        // the primary file as its own past-ASCII fallback plus a Han
        // face, and the font-px slider above proves the re-bake story:
        // every rebuild starts the lazy half empty and the same line
        // heals itself within a frame or two.
        if (!font_.empty()) {
            (void)ws::ui::r2d::font_request_text(font_, kLazyLine);
            lazy_baked_ += ws::ui::r2d::font_bake_pending(font_, 8u);
        }
        gui::text(layout_, chrome_, kLazyLine, -1.0f);
        if (gui::button(gui_, layout_, chrome_, "stack the faces", half)) {
            stack_faces_ = true;
            const ws::core::Status primary =
                ws::ui::r2d::font_add_face(font_, kDefaultFontPath, 0);
            const ws::core::Status extra =
                ws::ui::r2d::font_add_face(font_, kExtraFontPath, 1);
            if (primary.has_value() && extra.has_value()) {
                std::snprintf(status_, sizeof status_,
                              "stacked: the primary + the Han face");
            } else if (primary.has_value()) {
                std::snprintf(status_, sizeof status_,
                              "stacked the primary; the Han face was refused");
            } else {
                std::snprintf(status_, sizeof status_,
                              "the stack was refused - is there a font?");
            }
        }
        layout_.same_line();
        char lazy_line[48];
        std::snprintf(lazy_line, sizeof lazy_line, "lazily baked glyphs: %u",
                      lazy_baked_);
        gui::text(layout_, chrome_, lazy_line, half);

        // The document's faces, one swatch row each: a panel_bg-backed
        // quad (so the translucent veil and border read) then the name
        // and the packed value. Raw r2d quads at layout-cursor rects -
        // decorative, no id, nothing focusable, like the grid backdrop.
        const struct {
            const char* name;
            std::uint32_t face;
        } swatches[] = {
            {"panel_bg", theme_.chrome.panel_bg},
            {"control_bg", theme_.chrome.control_bg},
            {"control_hovered", theme_.chrome.control_hovered},
            {"control_pressed", theme_.chrome.control_pressed},
            {"control_disabled", theme_.chrome.control_disabled},
            {"control_on", theme_.chrome.control_on},
            {"text", theme_.chrome.text},
            {"border", theme_.chrome.border},
            {"veil", theme_.chrome.veil},
            {"toast_info", theme_.chrome.toast_info},
            {"toast_warning", theme_.chrome.toast_warning},
            {"toast_error", theme_.chrome.toast_error},
        };
        const float swatch_px = 16.0f * scale_;
        for (const auto& sw : swatches) {
            const gui::Rect swatch = layout_.next(swatch_px, swatch_px);
            ws::ui::r2d::draw_quad(swatch.x, swatch.y, swatch.width,
                                   swatch.height, theme_.chrome.panel_bg);
            ws::ui::r2d::draw_quad(swatch.x, swatch.y, swatch.width,
                                   swatch.height, sw.face);
            layout_.same_line();
            char face_line[48];
            std::snprintf(face_line, sizeof face_line, "%s  0x%08X", sw.name,
                          sw.face);
            gui::text(layout_, chrome_, face_line, -1.0f);
        }
        if (accent_color_ != 0u) {
            const gui::Rect swatch = layout_.next(swatch_px, swatch_px);
            ws::ui::r2d::draw_quad(swatch.x, swatch.y, swatch.width,
                                   swatch.height, theme_.chrome.panel_bg);
            ws::ui::r2d::draw_quad(swatch.x, swatch.y, swatch.width,
                                   swatch.height, accent_color_);
            layout_.same_line();
            char face_line[48];
            std::snprintf(face_line, sizeof face_line, "accent  0x%08X",
                          accent_color_);
            gui::text(layout_, chrome_, face_line, -1.0f);
        }

        // The contrast answer, live: the checked pairs (AA text 4.5:1,
        // the state-carrying faces 3:1) re-measured against the ACTIVE
        // document every frame - an accent that could not win shows
        // here by design, not as a surprise.
        const std::vector<gui::ContrastFailure> failures =
            gui::contrast_failures(theme_.chrome);
        if (failures.empty()) {
            gui::text(layout_, chrome_,
                      "contrast: every checked pair passes (text 4.5:1, "
                      "state faces 3:1)", -1.0f);
        } else {
            for (const gui::ContrastFailure& failure : failures) {
                char ratio_line[96];
                std::snprintf(ratio_line, sizeof ratio_line,
                              "contrast: %s measures %.2f:1 - needs %.1f:1",
                              failure.pair, failure.ratio, failure.required);
                gui::text(layout_, chrome_, ratio_line, -1.0f);
            }
        }
    }

    // --- buttons -----------------------------------------------------------
    if (gui::collapsing_header(gui_, layout_, chrome_, "buttons",
                               section_[kButtons])) {
        if (gui::button(gui_, layout_, chrome_, "click me", half)) {
            ++clicks_;
        }
        layout_.same_line();
        if (gui::button(gui_, layout_, chrome_, "hover me (tooltip)", half)) {
            ++clicks_;
        }
        if (view_tooltips_) {
            // The one-line form: rest delay, placement and sizing folded
            // into one call, keyed on the same label the button was
            // laid under and anchored at the pointer (a point anchor -
            // the hint hangs below it).
            gui::tooltip_text(
                gui_, layout_, chrome_, "hover me (tooltip)",
                {gui_.input().mouse_x, gui_.input().mouse_y, 0.0f, 0.0f},
                "a one-line hint, delayed and placed for you");
        }
        if (gui::button(gui_, layout_, chrome_, "a dead button", half, {},
                        true)) {
            ++clicks_;  // the disabled row can never report
        }
        layout_.same_line();
        if (gui::button(gui_, layout_, chrome_, "a fill-width row", -1.0f)) {
            ++clicks_;
        }
        if (gui::button(gui_, layout_, chrome_,
                        "hover me too - a two-row tooltip", -1.0f)) {
            ++clicks_;
        }
        if (view_tooltips_) {
            // The scope form: the delay gate plus a scope that holds
            // any widgets - two text rows here, on the pointer-following
            // bounds playground's hint uses.
            const gui::WidgetId rich_id =
                gui_.ids().id("hover me too - a two-row tooltip");
            if (gui::tooltip_delayed(gui_, rich_id, chrome_)) {
                const float tip_h = chrome_.control_height * 2.0f +
                                   chrome_.spacing +
                                   chrome_.padding * 2.0f;
                const gui::Rect tip_bounds{gui_.input().mouse_x +
                                               14.0f * scale_,
                                           gui_.input().mouse_y +
                                               18.0f * scale_,
                                           260.0f * scale_, tip_h};
                if (gui::begin_tooltip(gui_, layout_, chrome_, "rich tip",
                                       tip_bounds, true)) {
                    gui::text(layout_, chrome_,
                              "the scope form holds any widgets -", -1.0f);
                    gui::text(layout_, chrome_,
                              "two rows here, a button would work too",
                              -1.0f);
                    gui::end_tooltip(gui_, layout_);
                }
            }
        }
        char clicks_line[48];
        std::snprintf(clicks_line, sizeof clicks_line, "clicks so far: %d",
                      clicks_);
        gui::text(layout_, chrome_, clicks_line, -1.0f);
    }

    // --- choices ------------------------------------------------------------
    if (gui::collapsing_header(gui_, layout_, chrome_, "choices",
                               section_[kChoices])) {
        if (gui::checkbox(gui_, layout_, chrome_, "a checkbox", checked_)) {
            // the widget already flipped the bool; the branch is the
            // app's own reaction slot
        }
        layout_.same_line();
        (void)gui::checkbox(gui_, layout_, chrome_, "a dead checkbox",
                           dead_check_, true);
        if (gui::toggle_switch(gui_, layout_, chrome_, "a toggle switch",
                               switched_)) {
        }
        layout_.same_line();
        (void)gui::toggle_switch(gui_, layout_, chrome_, "a dead switch",
                                 dead_switch_, true);
        gui::text(layout_, chrome_,
                  "pick a fruit - one shared int is the whole radio group:",
                  -1.0f);
        const char* const fruits[] = {"apple", "banana", "cherry"};
        for (int i = 0; i < 3; ++i) {
            if (gui::radio(gui_, layout_, chrome_, fruits[i], fruit_, i)) {
                std::snprintf(status_, sizeof status_, "fruit: %s", fruits[i]);
            }
            if (i < 2) {
                layout_.same_line();
            }
        }
    }

    // --- sliders & drag ------------------------------------------------------
    if (gui::collapsing_header(gui_, layout_, chrome_, "sliders & drag",
                               section_[kSliders])) {
        if (gui::slider_float(gui_, layout_, chrome_, "fill", fraction_, 0.0f,
                              1.0f, half)) {
        }
        layout_.same_line();
        (void)gui::slider_float(gui_, layout_, chrome_, "dead", fraction_,
                                0.0f, 1.0f, half, true);
        if (gui::slider_int(gui_, layout_, chrome_, "count", count_, 0, 100,
                            half)) {
        }
        layout_.same_line();
        if (gui::drag_float(gui_, layout_, chrome_, "drag", drag_, -10.0f,
                            10.0f, 0.05f, half)) {
        }
        gui::text(layout_, chrome_,
                  "the fill slider feeds the progress bar below it:", -1.0f);
        gui::progress(layout_, chrome_, fraction_, -1.0f);
    }

    // --- text ------------------------------------------------------------------
    if (gui::collapsing_header(gui_, layout_, chrome_, "text",
                               section_[kText])) {
        if (gui::text_input(gui_, layout_, chrome_, "name", name_buf_,
                            static_cast<int>(sizeof name_buf_), half)) {
            // true on the frame the TEXT changed; caret and selection
            // moves answer false
            std::snprintf(status_, sizeof status_, "typed: %s", name_buf_);
        }
        layout_.same_line();
        (void)gui::text_input(gui_, layout_, chrome_, "locked", locked_buf_,
                              static_cast<int>(sizeof locked_buf_), half,
                              true);
        gui::text(layout_, chrome_,
                  "a static label - no id, no hit-test, nothing focusable",
                  -1.0f);
        // Enter is not the input's own: the app reads the focus plus the
        // key edge and gives the field its commit semantics.
        const gui::WidgetId name_id = gui_.ids().id("name");
        if (gui_.is_focused(name_id) &&
            ws::input::frame_input().key_pressed[ws::input::keys::kEnter]) {
            std::snprintf(status_, sizeof status_, "committed: %s",
                          name_buf_);
        }
        gui::separator(layout_, chrome_);
        gui::text(layout_, chrome_, "a separator above", -1.0f);
    }

    // --- lists ------------------------------------------------------------------
    if (gui::collapsing_header(gui_, layout_, chrome_, "lists",
                               section_[kLists])) {
        gui::text(layout_, chrome_,
                  "selectable rows - the selection stays the caller's:",
                  -1.0f);
        const char* const rows[] = {"first row", "second row", "third row"};
        for (int i = 0; i < 3; ++i) {
            if (gui::selectable(gui_, layout_, chrome_, rows[i],
                                list_sel_ == i)) {
                list_sel_ = i;
            }
        }
        gui::text(layout_, chrome_,
                  "a combo - the dropdown is a popup the widget places:",
                  -1.0f);
        const char* const picks[] = {"red", "green", "blue", "black"};
        if (gui::combo(gui_, layout_, chrome_, "color", picks, 4, combo_sel_,
                       combo_open_, -1.0f)) {
            std::snprintf(status_, sizeof status_, "color: %s",
                          picks[combo_sel_]);
        }
    }

    // --- table -------------------------------------------------------------------
    if (gui::collapsing_header(gui_, layout_, chrome_, "table",
                               section_[kTable])) {
        const gui::TableColumn cols[] = {
            {"name", 0.0f},  // stretch: fixed columns take theirs first
            {"size", 72.0f},
            {"modified", 96.0f},
        };
        const char* const cells[] = {
            "notes.txt",  "2 kB",   "today",
            "shot.png",   "140 kB", "yesterday",
            "wallet.dat", "1 MB",   "last year",
            "todo.md",    "3 kB",   "this morning",
        };
        (void)gui::table(gui_, layout_, chrome_, "files", cols, 3, cells, 4,
                         &table_sel_);
        char sel_line[48];
        std::snprintf(sel_line, sizeof sel_line, "selected row: %d",
                      table_sel_);
        gui::text(layout_, chrome_, sel_line, -1.0f);
    }

    // --- tabs & the statics they hold --------------------------------------------
    if (gui::collapsing_header(gui_, layout_, chrome_, "tabs & statics",
                               section_[kTabs])) {
        const char* const tabs[] = {"spinner", "busy bar", "a panel"};
        (void)gui::tab_bar(gui_, layout_, chrome_, "pages", tabs, 3, tab_,
                           page.width);
        if (tab_ == 0) {
            // The spinner's label ids it and never draws - the row is
            // the dots alone, eight of them chasing on the frame clock.
            gui::spinner(gui_, layout_, chrome_, "spin");
            layout_.same_line();
            gui::text(layout_, chrome_, "eight dots chasing on the frame clock",
                      -1.0f);
        } else if (tab_ == 1) {
            gui::progress_indeterminate(gui_, layout_, chrome_, "busy",
                                         -1.0f);
        } else {
            // A nested panel: chrome + clip + id scope, the outer cursor
            // parked while it is open.
            const gui::Rect nested_bounds =
                layout_.next(page.width * 0.6f,
                             chrome_.control_height * 2.0f + chrome_.padding);
            const gui::Rect nested = gui::begin_panel(gui_, layout_, chrome_,
                                                      "nested", nested_bounds);
            gui::text(layout_, chrome_, "a panel inside the page", -1.0f);
            if (gui::button(gui_, layout_, chrome_, "inside", nested.width)) {
                ++clicks_;
            }
            gui::end_panel(gui_, layout_);
        }
    }

    // --- jobs: a live row (the ws::jobs bridge) ------------------------------------
    if (gui::collapsing_header(gui_, layout_, chrome_, "jobs: a live row",
                               section_[kJobs])) {
        // "run a job": the counting demo - 100 steps of 20 ms (about
        // two seconds of worker time), one progress report per step.
        // The mirror fills on the drain (install_job_system put that
        // between frames, right before on_update), the row below reads
        // it live, and on_complete lands back on this thread:
        // toast_on_failure for the error case, a status echo for
        // success, quiet for cancelled - the helper's own contract.
        if (gui::button(gui_, layout_, chrome_, "run a job", half)) {
            work_mirror_.reset();
            ws::jobs::JobCallbacks<int> cbs = work_mirror_.callbacks<int>();
            cbs.on_complete = [this](ws::jobs::JobHandle<int> handle) {
                const ws::core::Result<int> result = gui::toast_on_failure(
                    toasts_, work_text_, handle, "the counting job");
                if (result.has_value()) {
                    std::snprintf(status_, sizeof status_,
                                  "the job finished: counted to %d",
                                  result.value());
                } else if (result.error().code ==
                           static_cast<std::int32_t>(
                               ws::jobs::JobError::cancelled)) {
                    std::snprintf(status_, sizeof status_,
                                  "the counting job was cancelled");
                }
            };
            work_ = jobs_.submit<int>(
                // job, not ctx: the menu ctx (MenuEvent) is still live in
                // this scope - MSVC /W4 (C4456/57) rejects the shadow.
                [](ws::jobs::JobContext& job) -> ws::core::Result<int> {
                    for (int step = 1; step <= 100; ++step) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(20));
                        WS_TRY(job.check_cancel());
                        char message[32];
                        std::snprintf(message, sizeof message,
                                      "counted %d/100", step);
                        job.report_progress(step / 100.0f, message);
                    }
                    return 100;
                },
                ws::jobs::Priority::normal, {}, cbs);
        }
        layout_.same_line();
        // "fail a job": the other completion story - a fake failure
        // 400 ms in, and the toast the instant it lands. No row, no
        // mirror: the toast IS the demo.
        if (gui::button(gui_, layout_, chrome_, "fail a job", half)) {
            ws::jobs::JobCallbacks<void> cbs;
            cbs.on_complete = [this](ws::jobs::JobHandle<void> handle) {
                (void)gui::toast_on_failure(toasts_, fail_text_, handle,
                                             "the failing job");
            };
            (void)jobs_.submit<void>(
                [](ws::jobs::JobContext&) -> ws::core::Status {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(400));
                    return ws::jobs::fail(ws::jobs::JobError::invalid_argument,
                                           "the demo's fake failure");
                },
                ws::jobs::Priority::normal, {}, cbs);
        }
        // The row itself: "label: state" line, the state-colored bar,
        // the cancel button - drawn every frame, the "no job" idle
        // look before the first run and after every terminal state.
        (void)gui::job_progress(gui_, layout_, chrome_, "the counting job",
                                work_, work_mirror_);
        gui::text(layout_, chrome_,
                  "two workers; progress and completions cross to the "
                  "main thread between frames",
                  -1.0f);
    }

    // --- overlays: popup, modal, dialogs, toasts ----------------------------------
    if (gui::collapsing_header(gui_, layout_, chrome_,
                               "overlays: popup, modal, dialogs, toasts",
                               section_[kOverlays])) {
        // The raw popup: the caller owns `open`, places the window and
        // lays the rows. The anchor is the press point captured when
        // the button opened it - the context menu's own anchoring rule
        // (a point anchor: the window hangs below it).
        if (gui::button(gui_, layout_, chrome_, "open a popup", half)) {
            popup_open_ = !popup_open_;
            popup_point_ = gui::Rect{gui_.input().mouse_x,
                                     gui_.input().mouse_y, 0.0f, 0.0f};
        }
        layout_.same_line();
        if (gui::button(gui_, layout_, chrome_, "a raw modal scope", half)) {
            raw_modal_open_ = !raw_modal_open_;  // begin_modal reads it BY VALUE
        }
        if (popup_open_) {
            const gui::Rect pop_bounds = gui::place_popup(
                popup_point_, 170.0f,
                chrome_.control_height * 3.0f + chrome_.spacing * 2.0f +
                    chrome_.padding * 2.0f);
            // The guard form (scoped.hpp): the same begin/end pair as
            // one block local, so a return from inside the rows cannot
            // strand the scope.
            const gui::ScopedPopup pop(gui_, layout_, chrome_,
                                       "gallery popup", popup_point_,
                                       pop_bounds, popup_open_);
            if (pop) {
                const char* const popup_rows[] = {"toast an info",
                                                  "toast a warning",
                                                  "just close"};
                for (int i = 0; i < 3; ++i) {
                    if (gui::selectable(gui_, layout_, chrome_, popup_rows[i],
                                        false)) {
                        if (i < 2) {
                            toasts_.push({i == 0 ? "a popup said hello"
                                                 : "a popup said watch out",
                                          i == 0
                                              ? gui::ToastSeverity::info
                                              : gui::ToastSeverity::warning,
                                          nullptr, 3.0f});
                        }
                        popup_open_ = false;
                    }
                }
            }  // end_popup rides the guard
        }
        if (raw_modal_open_) {
            const float mw = 280.0f * scale_;
            const float mh = 120.0f * scale_;
            const gui::Rect modal_bounds{(frame.width - mw) * 0.5f,
                                         (frame.height - mh) * 0.5f, mw, mh};
            if (gui::begin_modal(gui_, layout_, chrome_, "raw modal",
                                 modal_bounds, raw_modal_open_)) {
                gui::text(layout_, chrome_,
                          "the raw modal scope: the caller places", -1.0f);
                gui::text(layout_, chrome_,
                          "it, draws its rows and closes it", -1.0f);
                if (gui::button(gui_, layout_, chrome_, "close", -1.0f)) {
                    raw_modal_open_ = false;
                }
                gui::end_modal(gui_, layout_);
            }
        }

        // The stock dialog sets, one button each: the one call site at
        // the top of the frame plays whatever kind these pick (the
        // about-box entries reuse it too). Stock sets clear `open`
        // themselves - Esc counts as a dismiss, Enter as an accept once
        // the veil has settled; the none set takes neither and has no
        // way out but its own body button.
        if (gui::button(gui_, layout_, chrome_, "ok only", half)) {
            dialog_kind_ = gui::DialogButtons::ok;
            dialog_title_ = "an ok-only dialog";
            dialog_open_ = true;
        }
        layout_.same_line();
        if (gui::button(gui_, layout_, chrome_, "ok + cancel", half)) {
            dialog_kind_ = gui::DialogButtons::ok_cancel;
            dialog_title_ = "a modal dialog";
            dialog_open_ = true;
        }
        if (gui::button(gui_, layout_, chrome_, "yes + no", half)) {
            dialog_kind_ = gui::DialogButtons::yes_no;
            dialog_title_ = "a yes/no question";
            dialog_open_ = true;
        }
        layout_.same_line();
        if (gui::button(gui_, layout_, chrome_, "retry + cancel", half)) {
            dialog_kind_ = gui::DialogButtons::retry_cancel;
            dialog_title_ = "something failed";
            dialog_open_ = true;
        }
        if (gui::button(gui_, layout_, chrome_, "no buttons at all", -1.0f)) {
            dialog_kind_ = gui::DialogButtons::none;
            dialog_title_ = "a dialog with no buttons";
            dialog_open_ = true;
        }

        // The toast store's contracts, one button each: the three
        // severities, an action button (its click dismisses the toast
        // and reports the slot - on_update answers it), a sticky toast
        // (duration 0: only a click removes it), the flood policy (the
        // view menu sizes the wave) and clear. The count reads live.
        // The texts are literals on purpose - a toast borrows its bytes
        // and re-reads them every frame it draws.
        if (gui::button(gui_, layout_, chrome_, "info", half)) {
            toasts_.push({"saved the layout", gui::ToastSeverity::info,
                          nullptr, 4.0f});
        }
        layout_.same_line();
        if (gui::button(gui_, layout_, chrome_, "warn", half)) {
            toasts_.push({"the disk is almost full",
                          gui::ToastSeverity::warning, nullptr, 4.0f});
        }
        if (gui::button(gui_, layout_, chrome_, "error", half)) {
            toasts_.push({"lost the connection", gui::ToastSeverity::error,
                          nullptr, 4.0f});
        }
        layout_.same_line();
        if (gui::button(gui_, layout_, chrome_, "action", half)) {
            toasts_.push({"file deleted", gui::ToastSeverity::info, "undo",
                          5.0f});
        }
        if (gui::button(gui_, layout_, chrome_, "sticky", half)) {
            toasts_.push({"this one stays until clicked",
                          gui::ToastSeverity::info, nullptr, 0.0f});
        }
        layout_.same_line();
        if (gui::button(gui_, layout_, chrome_, "flood", half)) {
            for (int i = 0; i < flood_size_; ++i) {
                toasts_.push({kWave[i], gui::ToastSeverity::info, nullptr,
                              1.5f});
            }
        }
        if (gui::button(gui_, layout_, chrome_, "clear", half)) {
            toasts_.clear();
        }
        layout_.same_line();
        char live[32];
        std::snprintf(live, sizeof live, "live: %d", toasts_.count());
        gui::text(layout_, chrome_, live, half);
    }

    // --- the disabled scope ------------------------------------------------------
    if (gui::collapsing_header(gui_, layout_, chrome_, "the disabled scope",
                               section_[kDisabled])) {
        gui::text(layout_, chrome_,
                  "one begin_disabled greys everything inside it and",
                  -1.0f);
        gui::text(layout_, chrome_,
                  "refuses it the pointer, the wheel and the Tab walk:",
                  -1.0f);
        (void)gui::checkbox(gui_, layout_, chrome_, "lock the rows below",
                            lock_);
        gui_.begin_disabled(lock_);
        if (gui::button(gui_, layout_, chrome_, "a locked button", half)) {
            ++clicks_;  // unreachable while the lock holds
        }
        layout_.same_line();
        (void)gui::checkbox(gui_, layout_, chrome_, "a locked checkbox",
                            lock_check_);
        if (gui::slider_float(gui_, layout_, chrome_, "a locked slider",
                              lock_slide_, 0.0f, 1.0f, -1.0f)) {
        }
        gui_.end_disabled();
    }

    // --- custom look: the stage-11 behavior/draw seam ----------------------
    // One slider value and one text buffer, each drawn three ways: the
    // composed widgets; the same behavior under the app's own paint;
    // and the stepper, a widget the app owns outright - its own value
    // mapping over the raw citizenship seam, its own pixels. The ids
    // come from the same label well every composed row draws from.
    if (gui::collapsing_header(gui_, layout_, chrome_,
                               "custom look: the behavior/draw seam",
                               section_[kCustomLook])) {
        gui::text(layout_, chrome_,
                  "one slider value, three ways to draw it:", -1.0f);

        // The composed row: the stock behavior and the stock look.
        (void)gui::slider_float(gui_, layout_, chrome_, "standard",
                               custom_slide_, 0.0f, 1.0f, -1.0f);

        // Custom draw on the default behavior: the pixel bar. The same
        // click-to-set, drag and arrow steps slider_behavior runs, the
        // value written into the one shared member - painted as chunky
        // blocks instead of the rail-and-handle look. The face ladder
        // shows through the result: the off blocks warm on hover.
        gui::text(layout_, chrome_, "pixel bar - custom draw", -1.0f);
        const gui::Rect pixel_row =
            layout_.next(page.width, chrome_.control_height);
        const gui::SliderResult pixel = gui::slider_behavior(
            gui_, gui_.ids().id("pixel bar"), pixel_row, custom_slide_,
            0.0f, 1.0f, (1.0f - 0.0f) / 100.0f);
        {
            constexpr int kBlocks = 12;
            const float gap = 2.0f * scale_;
            const float block_w = std::max(
                0.0f, (pixel_row.width - gap * (kBlocks - 1)) / kBlocks);
            const std::uint32_t off =
                pixel.state == gui::WidgetState::Hovered
                    ? chrome_.control_hovered
                    : chrome_.control_disabled;
            for (int i = 0; i < kBlocks; ++i) {
                const bool lit = custom_slide_ >= (i + 0.5f) / kBlocks;
                ws::ui::r2d::draw_quad(
                    pixel_row.x + i * (block_w + gap),
                    pixel_row.y + 3.0f * scale_, block_w,
                    pixel_row.height - 6.0f * scale_,
                    lit ? chrome_.control_on : off);
            }
        }

        // Fully custom: the app owns the stepper. state() is the whole
        // citizenship seam - one hit-test, the press capture, the Tab
        // walk through register_focusable - and the mapping (the
        // pointer quantized to ten steps, the arrows stepping while
        // focused) and the paint are the app's own.
        gui::text(layout_, chrome_, "step leds - fully custom", -1.0f);
        const gui::Rect step_row =
            layout_.next(page.width, chrome_.control_height);
        const gui::WidgetId step_id = gui_.ids().id("step leds");
        gui_.register_focusable(step_id);
        const gui::WidgetResult step = gui_.state(step_id, step_row, false);
        constexpr int kSteps = 10;
        if (step.hit.pressed) {
            const ws::input::PointerView* finger =
                gui_.pointer(step.hit.pointer);
            if (finger != nullptr && finger->down) {
                const float along = std::clamp(
                    (finger->x - step_row.x) / step_row.width, 0.0f, 1.0f);
                step_value_ = std::min(
                    kSteps - 1, static_cast<int>(along * kSteps + 0.5f));
            }
        }
        if (gui_.is_focused(step_id)) {
            if (gui_.input().key_pressed[ws::input::keys::kLeft]) {
                step_value_ = std::max(0, step_value_ - 1);
            }
            if (gui_.input().key_pressed[ws::input::keys::kRight]) {
                step_value_ = std::min(kSteps - 1, step_value_ + 1);
            }
        }
        {
            const std::uint32_t off =
                step.state == gui::WidgetState::Hovered
                    ? chrome_.control_hovered
                    : chrome_.control_disabled;
            const float gap = 3.0f * scale_;
            const float block_w = std::max(
                0.0f, (step_row.width - gap * (kSteps - 1)) / kSteps);
            for (int i = 0; i < kSteps; ++i) {
                ws::ui::r2d::draw_quad(
                    step_row.x + i * (block_w + gap),
                    step_row.y + 4.0f * scale_, block_w,
                    step_row.height - 8.0f * scale_,
                    i < step_value_ ? chrome_.control_on : off);
            }
            if (gui_.is_focused(step_id)) {
                // The app's own focus mark, since no stock ring runs.
                ws::ui::r2d::draw_quad(
                    step_row.x, step_row.y + step_row.height - 2.0f * scale_,
                    step_row.width, 2.0f * scale_, chrome_.control_on);
            }
        }

        gui::text(layout_, chrome_,
                  "one text buffer, three fields over it:", -1.0f);

        // The composed field, over the shared buffer.
        if (gui::text_input(gui_, layout_, chrome_, "standard field",
                            custom_buf_,
                            static_cast<int>(sizeof custom_buf_), -1.0f)) {
            std::snprintf(status_, sizeof status_, "typed: %s", custom_buf_);
        }

        // Custom draw on the default behavior: the underline field over
        // the SAME buffer - a second view of one caller-owned string,
        // under its own editing pair. The behavior answers the caret
        // and the selection as pixels; this look draws them its own
        // way, no rounded face, no ring.
        gui::text(layout_, chrome_, "underline - custom draw", -1.0f);
        const gui::Rect line_row =
            layout_.next(page.width, chrome_.control_height);
        const gui::TextInputResult line = gui::text_input_behavior(
            gui_, gui_.ids().id("underline field"), line_row, custom_buf_,
            static_cast<int>(sizeof custom_buf_), underline_edit_, chrome_);
        if (line.edited) {
            std::snprintf(status_, sizeof status_, "typed: %s", custom_buf_);
        }
        ws::ui::r2d::draw_quad(
            line_row.x, line_row.y + line_row.height - 2.0f * scale_,
            line_row.width, 2.0f * scale_,
            line.focused ? chrome_.control_on : chrome_.border);
        paint_bare_field(line_row, line, custom_buf_);

        // Both custom: the digits-only field - the app's own rule on
        // the same behavior (a non-digit the frame typed is stripped
        // from the buffer the app owns; the behavior's own write-back
        // clamp settles the caret into the shorter line next frame)
        // and its own look, a boxed inset.
        gui::text(layout_, chrome_, "digits only - custom rules", -1.0f);
        const gui::Rect digit_row =
            layout_.next(page.width, chrome_.control_height);
        const gui::TextInputResult digits = gui::text_input_behavior(
            gui_, gui_.ids().id("digits field"), digit_row, digit_buf_,
            static_cast<int>(sizeof digit_buf_), digit_edit_, chrome_);
        if (digits.edited) {
            int keep = 0;
            for (int i = 0; digit_buf_[i] != '\0'; ++i) {
                if (digit_buf_[i] >= '0' && digit_buf_[i] <= '9') {
                    digit_buf_[keep++] = digit_buf_[i];
                }
            }
            digit_buf_[keep] = '\0';
            std::snprintf(status_, sizeof status_, "digits: %s", digit_buf_);
        }
        ws::ui::r2d::draw_quad(digit_row.x, digit_row.y, digit_row.width,
                               digit_row.height, chrome_.border);
        ws::ui::r2d::draw_quad(digit_row.x + 1.0f, digit_row.y + 1.0f,
                               std::max(0.0f, digit_row.width - 2.0f),
                               std::max(0.0f, digit_row.height - 2.0f),
                               chrome_.panel_bg);
        paint_bare_field(digit_row, digits, digit_buf_);
    }

    // --- platform (stage 12): dialogs, recent files, clipboard, drops,
    //     the autosave pair, the title, the cursor ------------------------
    if (gui::collapsing_header(
            gui_, layout_, chrome_,
            "platform: dialogs, recent files, clipboard, drops, autosave",
            section_[kPlatform])) {
        // The pickers: each button submits its native modal to a
        // worker (the dialog blocks while the user decides) and the
        // answer crosses back through the frame drain - the counting
        // job's bridge again. The buttons lock while one is out: one
        // native thing at a time reads cleaner than two.
        const bool dialog_out = dialog_out_.busy() || files_out_.busy();
        if (gui::button(gui_, layout_, chrome_, "open a file", half, {},
                        dialog_out)) {
            pick_file(0);
            std::snprintf(status_, sizeof status_, "the open dialog is out");
        }
        layout_.same_line();
        if (gui::button(gui_, layout_, chrome_, "save as", half, {},
                        dialog_out)) {
            pick_file(1);
            std::snprintf(status_, sizeof status_, "the save dialog is out");
        }
        if (gui::button(gui_, layout_, chrome_, "pick a folder", half, {},
                        dialog_out)) {
            pick_file(2);
            std::snprintf(status_, sizeof status_, "the folder dialog is out");
        }
        layout_.same_line();
        if (gui::button(gui_, layout_, chrome_, "open several", half, {},
                        dialog_out)) {
            pick_files();
            std::snprintf(status_, sizeof status_,
                          "the multi-open dialog is out");
        }
        if (dialog_out) {
            gui::spinner(gui_, layout_, chrome_, "the dialog spinner");
            layout_.same_line();
            gui::text(layout_, chrome_,
                      "a native dialog is out - the loop never blocked",
                      layout_.remaining_width());
        }

        // The MRU: menu_items() answers the rows (newest first, the
        // lazy prune already run, same-named siblings already
        // disambiguated); a click opens - promotes to the head and
        // saves. The pin button rides each row's right edge; a pinned
        // row survives the cap and the missing-file prune.
        const std::vector<ws::platform::RecentItem> rows = recent_.menu_items();
        if (rows.empty()) {
            gui::text(layout_, chrome_,
                      "(nothing recent yet - open, save or drop a file)",
                      page.width);
        } else {
            const float pin_w = 72.0f * scale_;
            const float row_w =
                std::max(0.0f, page.width - pin_w - chrome_.spacing);
            for (const ws::platform::RecentItem& item : rows) {
                char label[160];
                std::snprintf(label, sizeof label, "%s%s", item.label.c_str(),
                              item.pinned ? "   [pinned]" : "");
                if (gui::selectable(gui_, layout_, chrome_, label, false,
                                    row_w)) {
                    (void)recent_.touch(item.path);
                    (void)recent_.save();
                    std::snprintf(status_, sizeof status_, "recent: opened %s",
                                  item.path.c_str());
                }
                layout_.same_line();
                if (gui::button(gui_, layout_, chrome_,
                                item.pinned ? "unpin" : "pin", pin_w)) {
                    // pin/unpin answer bool, not Status: a refusal
                    // means the path was not in the list (a prune
                    // raced the click) - rare, and the status says
                    // so rather than inventing an error.
                    const bool moved = item.pinned
                                           ? recent_.unpin("", item.path)
                                           : recent_.pin("", item.path);
                    (void)recent_.save();
                    if (moved) {
                        std::snprintf(status_, sizeof status_, "%s %s",
                                      item.pinned ? "unpinned" : "pinned",
                                      item.path.c_str());
                    } else {
                        std::snprintf(status_, sizeof status_,
                                      "the pin move was refused");
                    }
                }
            }
        }

        // The clipboard round trip - the one platform call the loop
        // CAN afford inline (no human decides anything), so these
        // ride the frame directly. Text, a typed payload, an image:
        // the demo's own content, this app's consent - the suite
        // never touches a clipboard.
        if (gui::button(gui_, layout_, chrome_, "copy the draft", half)) {
            const ws::core::Status copied =
                ws::platform::set_clipboard_text(draft_buf_);
            std::snprintf(status_, sizeof status_, "%s",
                          copied.has_value()
                              ? "copied the draft as text"
                              : copied.error().message.c_str());
        }
        layout_.same_line();
        if (gui::button(gui_, layout_, chrome_, "read the text back", half)) {
            const ws::core::Result<std::optional<std::string>> read =
                ws::platform::clipboard_text();
            if (!read.has_value()) {
                std::snprintf(status_, sizeof status_, "read failed: %s",
                              read.error().message.c_str());
            } else if (!read.value().has_value()) {
                std::snprintf(clip_buf_, sizeof clip_buf_,
                              "(the clipboard holds no text)");
                std::snprintf(status_, sizeof status_,
                              "the clipboard holds no text");
            } else {
                std::snprintf(clip_buf_, sizeof clip_buf_, "%s",
                              read.value().value().c_str());
                std::snprintf(status_, sizeof status_, "read the text back");
            }
        }
        if (gui::button(gui_, layout_, chrome_, "copy typed bytes", half)) {
            const ws::core::Status copied = ws::platform::set_clipboard_payload(
                "ws gallery demo", draft_buf_);
            std::snprintf(status_, sizeof status_, "%s",
                          copied.has_value()
                              ? "copied the draft under the payload type"
                              : copied.error().message.c_str());
        }
        layout_.same_line();
        if (gui::button(gui_, layout_, chrome_, "read the bytes back", half)) {
            const ws::core::Result<std::optional<std::string>> read =
                ws::platform::clipboard_payload("ws gallery demo");
            if (!read.has_value()) {
                std::snprintf(status_, sizeof status_, "read failed: %s",
                              read.error().message.c_str());
            } else if (!read.value().has_value()) {
                std::snprintf(clip_buf_, sizeof clip_buf_,
                              "(the clipboard holds no such payload)");
                std::snprintf(status_, sizeof status_,
                              "the clipboard holds no such payload");
            } else {
                std::snprintf(clip_buf_, sizeof clip_buf_, "%s",
                              read.value().value().c_str());
                std::snprintf(status_, sizeof status_, "read the bytes back");
            }
        }
        if (gui::button(gui_, layout_, chrome_, "copy a 2x2 image", half)) {
            ws::platform::ClipboardImage image;  // a 2x2 RGBA checker
            image.width = 2;
            image.height = 2;
            image.bytes.assign("\xff\x00\x00\xff" "\x00\xff\x00\xff"
                               "\x00\xff\x00\xff" "\xff\x00\x00\xff",
                              16);
            const ws::core::Status copied =
                ws::platform::set_clipboard_image(image);
            std::snprintf(status_, sizeof status_, "%s",
                          copied.has_value()
                              ? "copied the 2x2 checker"
                              : copied.error().message.c_str());
        }
        layout_.same_line();
        if (gui::button(gui_, layout_, chrome_, "read the image back", half)) {
            const ws::core::Result<std::optional<ws::platform::ClipboardImage>>
                read = ws::platform::clipboard_image();
            if (!read.has_value()) {
                std::snprintf(status_, sizeof status_, "read failed: %s",
                              read.error().message.c_str());
            } else if (!read.value().has_value()) {
                std::snprintf(clip_buf_, sizeof clip_buf_,
                              "(the clipboard holds no image)");
                std::snprintf(status_, sizeof status_,
                              "the clipboard holds no image");
            } else {
                const ws::platform::ClipboardImage& image = read.value().value();
                std::snprintf(clip_buf_, sizeof clip_buf_,
                              "image %dx%d, %d bytes", image.width,
                              image.height,
                              static_cast<int>(image.bytes.size()));
                std::snprintf(status_, sizeof status_, "read the image back");
            }
        }
        gui::text(layout_, chrome_, clip_buf_, page.width);

        // The drop zone: chrome only - the whole window takes the
        // drop (the OS owns the target and reports no position), so
        // the quad is an invitation, not a hit area. The log below
        // is what on_event recorded, newest first.
        const float zone_h = chrome_.control_height * 1.5f;
        const gui::Rect zone = layout_.next(page.width, zone_h);
        ws::ui::r2d::draw_rounded_quad(zone.x, zone.y, zone.width, zone.height,
                                       chrome_.corner_radius,
                                       chrome_.panel_bg);
        const float edge = std::max(1.0f, 1.0f * scale_);
        ws::ui::r2d::draw_quad(zone.x, zone.y, zone.width, edge,
                               chrome_.border);
        ws::ui::r2d::draw_quad(zone.x, zone.y + zone.height - edge, zone.width,
                               edge, chrome_.border);
        ws::ui::r2d::draw_quad(zone.x, zone.y, edge, zone.height,
                               chrome_.border);
        ws::ui::r2d::draw_quad(zone.x + zone.width - edge, zone.y, edge,
                               zone.height, chrome_.border);
        gui::text(layout_, chrome_,
                  "drop files anywhere on the window - the log keeps the "
                  "newest drops",
                  page.width);
        if (dropped_.empty()) {
            gui::text(layout_, chrome_, "(no files dropped yet)", page.width);
        } else {
            for (const std::vector<std::string>& drop : dropped_) {
                if (drop.empty()) {
                    continue;
                }
                char drop_line[160];
                std::snprintf(drop_line, sizeof drop_line, "drop: %s%s",
                              drop.front().c_str(),
                              drop.size() > 1 ? " (+more)" : "");
                gui::text(layout_, chrome_, drop_line, page.width);
            }
        }

        // The crash-recovery pair. The demo's crash is simply closing
        // the window without "finish cleanly" - the next run's probe
        // finds the marker, and the startup ask (or this manual
        // pair) settles it. The snapshot buttons lock while the
        // native ask is out.
        gui::text_input(gui_, layout_, chrome_, "the draft", draft_buf_,
                        static_cast<int>(sizeof draft_buf_), page.width);
        if (gui::button(gui_, layout_, chrome_, "snapshot the draft", half, {},
                        ask_out_.busy())) {
            const ws::core::Status saved = autosave_.snapshot(draft_buf_);
            refresh_probe();
            std::snprintf(status_, sizeof status_, "%s",
                          saved.has_value()
                              ? "snapshotted - the marker is down"
                              : saved.error().message.c_str());
        }
        layout_.same_line();
        if (gui::button(gui_, layout_, chrome_, "finish cleanly", half, {},
                        ask_out_.busy())) {
            const ws::core::Status finished = autosave_.finish();
            refresh_probe();
            std::snprintf(status_, sizeof status_, "%s",
                          finished.has_value()
                              ? "finished - the pair is cleared"
                              : finished.error().message.c_str());
        }
        // The probe's live answer as a line, whatever it holds: the
        // waiting marker, the corrupt one, or nothing to do.
        char probe_line[160];
        if (!probe_.has_value()) {
            std::snprintf(probe_line, sizeof probe_line,
                          "the marker reads corrupt: %s",
                          probe_.error().message.c_str());
        } else if (probe_.value()) {
            std::snprintf(probe_line, sizeof probe_line,
                          "a crashed autosave waits - snapshot clock %lld",
                          static_cast<long long>(probe_.value()->saved_ms));
        } else {
            std::snprintf(probe_line, sizeof probe_line,
                          "no crashed autosave - snapshot, then just close "
                          "the window");
        }
        gui::text(layout_, chrome_, probe_line, page.width);
        if (ask_out_.busy()) {
            gui::text(layout_, chrome_, "asking through the native box...",
                      page.width);
        } else {
            // The manual pair: the fallback the startup ask drives
            // too. recover() can fail (nothing to recover); the
            // failure toast says so.
            if (gui::button(gui_, layout_, chrome_, "recover", half)) {
                do_recover();
            }
            layout_.same_line();
            if (gui::button(gui_, layout_, chrome_, "discard", half)) {
                do_discard();
            }
        }

        // The title, composed live on every change (Notepad's
        // behavior): the field and the dirty bit feed the composer,
        // the seam hands the answer to the OS, the echo line shows
        // it on every platform.
        if (gui::text_input(gui_, layout_, chrome_, "document", title_doc_,
                            static_cast<int>(sizeof title_doc_), half)) {
            compose_title();
        }
        layout_.same_line();
        if (gui::checkbox(gui_, layout_, chrome_, "unsaved work",
                         title_dirty_)) {
            compose_title();
        }
        gui::text(layout_, chrome_, title_last_, page.width);

        // The cursor shapes: a radio walk over the eight standard
        // pointers, four to a row, set_cursor applied the moment the
        // pick moves (arrow hands the default back).
        for (int i = 0; i < kCursorCount; ++i) {
            if (i > 0 && i % 4 != 0) {
                layout_.same_line();
            }
            if (gui::radio(gui_, layout_, chrome_, kCursorNames[i],
                           cursor_pick_, i)) {
                set_cursor(kCursorShapes[i]);
                std::snprintf(status_, sizeof status_, "cursor: %s",
                              kCursorNames[i]);
            }
        }
    }

    // --- commands (stage 13): the registry as menus, the palette,
    //     the keymap editor, the undo demo --------------------------
    if (gui::collapsing_header(gui_, layout_, chrome_,
                               "commands: menus, palette, keymap editor, undo",
                               section_[kCommands])) {
        gui::text(layout_, chrome_,
                  "one registry drives this page: the menu bar and the "
                  "toolbar are generated from it, the palette searches it, "
                  "and the chords fire through the resolver before the frame "
                  "begins (the strip bar's chord texts are static strings; "
                  "these columns are live)",
                  page.width);

        // The registry as menus, rebuilt every frame (the menus'
        // discipline: per-frame data). The hints read the LIVE
        // binding table, so a rebind in the keymap editor below
        // renames this bar's columns the same frame.
        menu_builder_.build(commands_, &resolver_,
                            ws::app::KeyContext::global);
        const gui::MenuEvent cmd_ev = gui::menu_bar(
            gui_, layout_, chrome_, "commands menu", menu_builder_.menus(),
            menu_builder_.count(), cmd_bar_open_);
        const std::string_view menu_fired =
            menu_builder_.command_of(cmd_ev.item);
        if (!menu_fired.empty()) {
            dispatch_command(menu_fired);
        }

        // The toolbar: a caller-chosen id list, not the registry's
        // whole walk - the row is this app's own composition.
        const std::string_view toolbar_fired = gui::command_toolbar(
            gui_, layout_, chrome_, "commands toolbar", commands_,
            toolbar_ids_);
        if (!toolbar_fired.empty()) {
            dispatch_command(toolbar_fired);
        }

        // The palette opener + its live chord hint - the same text
        // the Tools menu shows (one spelling, describe_chord's rule).
        if (gui::button(gui_, layout_, chrome_, "open the palette", half)) {
            palette_open_ = true;
        }
        layout_.same_line();
        const ws::app::Command* palette_cmd = commands_.find("tools.palette");
        const std::string palette_chord =
            palette_cmd != nullptr
                ? gui::command_hint(*palette_cmd, &resolver_,
                                    ws::app::KeyContext::global)
                : std::string{};
        char palette_hint[96];
        if (palette_chord.empty()) {
            std::snprintf(palette_hint, sizeof palette_hint,
                          "the palette command is unbound - rebind it below");
        } else {
            std::snprintf(palette_hint, sizeof palette_hint,
                          "or press %s - the resolver fires the chord",
                          palette_chord.c_str());
        }
        gui::text(layout_, chrome_, palette_hint,
                  layout_.remaining_width());

        // The undo demo: a 64-char doc whose EVERY edit is a command
        // on the stack - the funnel reverts the buffer and the push's
        // apply() lands the edit, so the stack is the sole executor
        // and redo stays exact. The buttons' "###" tails keep their
        // ids stable while the display text follows the step names;
        // they are host-composed because the bridge header never
        // names the undo stack.
        gui::text(layout_, chrome_,
                  "the demo doc - every edit is a command on the undo stack",
                  page.width);
        if (gui::text_input(gui_, layout_, chrome_, "the demo doc",
                            undo_doc_, static_cast<int>(sizeof undo_doc_),
                            page.width)) {
            // The field already wrote the edit: the argument binds its
            // new text first, then the funnel reverts the buffer and
            // the push's apply() lands the edit again.
            push_doc_edit(undo_doc_, "Typing");
        }
        const std::string undo_name = undo_.name_to_undo();
        const std::string redo_name = undo_.name_to_redo();
        if (undo_name.empty()) {
            std::snprintf(undo_label_, sizeof undo_label_,
                          "undo (nothing)###undo step");
        } else {
            std::snprintf(undo_label_, sizeof undo_label_,
                          "undo %s###undo step", undo_name.c_str());
        }
        if (redo_name.empty()) {
            std::snprintf(redo_label_, sizeof redo_label_,
                          "redo (nothing)###redo step");
        } else {
            std::snprintf(redo_label_, sizeof redo_label_,
                          "redo %s###redo step", redo_name.c_str());
        }
        if (gui::button(gui_, layout_, chrome_, undo_label_, half, {},
                        !undo_.can_undo())) {
            do_undo();
        }
        layout_.same_line();
        if (gui::button(gui_, layout_, chrome_, redo_label_, half, {},
                        !undo_.can_redo())) {
            do_redo();
        }
        // The rest of the doc vocabulary, dispatched through the
        // registry so every entry point runs the same command bodies
        // (the empty-argument append is the menu's path; the palette
        // prompts for one).
        if (gui::button(gui_, layout_, chrome_, "append a word", half)) {
            dispatch_command("edit.append");
        }
        layout_.same_line();
        if (gui::button(gui_, layout_, chrome_, "clear", half)) {
            dispatch_command("edit.clear");
        }
        char doc_line[110];
        std::snprintf(doc_line, sizeof doc_line,
                      "the doc is %s - %d undo step(s), %d redo step(s)",
                      undo_.is_dirty() ? "dirty" : "clean at the save point",
                      static_cast<int>(undo_.undo_count()),
                      static_cast<int>(undo_.redo_count()));
        gui::text(layout_, chrome_, doc_line, half);
        layout_.same_line();
        if (gui::button(gui_, layout_, chrome_, "mark it saved", half)) {
            do_mark_saved();
        }
        last_doc_ = undo_doc_;  // the funnel's revert source, synced

        // The keymap editor over the live table: the context picker,
        // a search, Record. A rebind here renames every hint on this
        // page the same frame - menus, palette, this table - because
        // they all read the one resolver.
        gui::keymap_editor(gui_, layout_, chrome_, "keymap", commands_,
                           resolver_, keymap_state_);
        if (gui::button(gui_, layout_, chrome_, "save the keymap", half)) {
            const ws::core::Status saved = keymap_file_.save(resolver_);
            if (!saved.has_value()) {
                cmd_text_ = saved.error().message;
                toasts_.push({cmd_text_.c_str(), gui::ToastSeverity::error,
                              nullptr, 5.0f});
            } else {
                std::snprintf(status_, sizeof status_,
                              "the keymap was saved");
            }
        }
        layout_.same_line();
        if (gui::button(gui_, layout_, chrome_, "reset to defaults", half)) {
            resolver_.reset_to_defaults();
            std::snprintf(status_, sizeof status_,
                          "the keymap was reset to the declared chords");
        }
        gui::text(layout_, chrome_,
                  "rebind something above, save, and restart - the rebind "
                  "survives; reset restores the declared chords",
                  page.width);
    }

    // --- canvas (stage 14) ------------------------------------------------
    // A zoomable canvas with every primitive, joins/caps/dashes, an
    // adaptive grid, and a 100k-primitive stress test with a frame-time
    // readout. The CanvasView owns the pan/zoom; wheel zooms toward the
    // cursor, middle-drag pans.
    if (gui::collapsing_header(gui_, layout_, chrome_, "canvas",
                               section_[kCanvas])) {
        // Allocate a 600x400 rect for the canvas view.
        constexpr float kCanvasW = 600.0f;
        constexpr float kCanvasH = 400.0f;
        const gui::Rect canvas_rect = {layout_.cursor_x(), layout_.cursor_y(),
                                       kCanvasW, kCanvasH};
        layout_.advance(kCanvasH + chrome_.padding);

        // Feed this frame's input to the canvas view (the gui's input
        // is the same InputFrame the platform pumped).
        const auto& in = gui_.input();
        canvas_view_.feed_mouse_moved(in.mouse_x, in.mouse_y);
        for (int b = 0; b < 3; ++b) {
            if (in.mouse_pressed[b]) {
                canvas_view_.feed_mouse_button(b, true);
            }
            if (in.mouse_released[b]) {
                canvas_view_.feed_mouse_button(b, false);
            }
        }
        if (in.scroll_dy != 0.0f) {
            canvas_view_.feed_scroll(in.mouse_x, in.mouse_y, 0.0f, in.scroll_dy);
        }
        canvas_view_.feed_key(32, in.key_down[32]);  // space

        // The draw callback: every primitive + joins/caps/dashes + the
        // stress test. Drawn in WORLD space (the canvas view set the
        // transform). Measure the draw time for the stress readout.
        canvas_view_.set_draw_callback(
            [this](canvas::Canvas2D& c, const canvas::ViewTransform2D& v) {
                draw_canvas_content(c, v);
            });

        // Time the draw + feed the frame-time readout.
        const auto t0 = std::chrono::steady_clock::now();
        canvas_view_.draw(canvas_rect, canvas_renderer_, true);
        const auto t1 = std::chrono::steady_clock::now();
        canvas_frame_ms_ = std::chrono::duration<float, std::milli>(t1 - t0).count();

        // The frame-time readout (drawn in screen space, after the canvas).
        char ms_line[80];
        std::snprintf(ms_line, sizeof ms_line, "frame: %.2f ms  zoom: %.2fx",
                      canvas_frame_ms_, canvas_view_.view.zoom);
        gui::text(layout_, chrome_, ms_line, page.width);
        gui::text(layout_, chrome_,
                  "wheel: zoom toward cursor | middle-drag (or space+drag): pan",
                  page.width);
    }

    gui::end_scroll_panel(gui_, layout_, chrome_);

    // The status bar: the one static that places itself - full width,
    // one control height along the bottom edge (the page was sized to
    // leave it that room, and it is the toasts' bottom clearance in
    // on_update). The tail is the last event's echo; the head recomposes
    // every frame into a local buffer, safe because the bar consumes
    // the bytes this frame.
    char line[160];
    std::snprintf(line, sizeof line,
                  "ws ui gallery - %d clicks - %d toasts - grid %s | %s",
                  clicks_, toasts_.count(), view_grid_ ? "on" : "off",
                  status_);
    (void)gui::status_bar(chrome_, line);
}

}  // namespace

#ifdef __ANDROID__
extern "C" void android_main(android_app* app) {
    chdir(app->activity->internalDataPath);
    ws::ui::AppConfig config;
    config.app_handle = app;
    GalleryApp gallery(std::move(config));
    (void)gallery.run();
}
#else
int main(int argc, char* argv[]) {
    // The single-instance grab: a second launch complains through the
    // native box and exits - on this desktop TU, so the complaint is
    // also stage 6b's message box seen from the outside. Android's
    // android_main takes no grab: the manifest's launchMode owns
    // single-instancing there.
    const ws::platform::SingleInstance guard{"ws ui gallery"};
    if (!guard.is_primary()) {
        const std::unique_ptr<ws::platform::MessageBoxes> boxes =
            ws::platform::make_message_boxes();
        ws::platform::MessageBoxRequest request;
        request.kind = ws::platform::MessageBoxKind::warning;
        request.buttons = ws::platform::MessageBoxButtons::ok;
        request.title = "ws ui gallery";
        request.message =
            "another ws ui gallery is already running - this copy exits";
        (void)boxes->ask(request);
        return 1;
    }
    ws::ui::AppConfig config;
    config.title = "ws ui gallery";
    config.width = 1000;
    config.height = 700;
    GalleryApp gallery(std::move(config),
                       ws::platform::open_documents(argc, argv));
    return gallery.run();
}
#endif
