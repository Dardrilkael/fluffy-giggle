// ws::app's suite: the command registry - the registration refusals
// (empty id/label/callback, the duplicate id), the dispatch round trip
// with the argument payload, the not-found refusal, the enabled/
// checked predicates read live per call, the registration-order
// contract, and the Mod helpers; the shortcut resolver - the bind and
// collision rules, the context ladder with the text rule, the two-step
// sequences driven on a fake clock, rebinding, and the defaults seed
// with its whole-or-nothing clash refusal; the keymap file - the
// install/table_of batch API, the save/load round-trips through the
// settings FileSystem + json seams, the missing-file first run, the
// parse/corrupt/conflict refusals with nothing installed, the
// failed-save contract, import/export, and a real StdFileSystem
// smoke. The undo stack: apply-on-push, the coalescing hook and its
// step boundary, the transaction group with its refusals, the
// save-point dirty flag across undo/redo and its unreachable cases,
// both history bounds with the keep-newest rule, and the
// state-change listeners.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ws/app/command.hpp>
#include <ws/app/errors.hpp>
#include <ws/app/keymap.hpp>
#include <ws/app/shortcut.hpp>
#include <ws/app/undo.hpp>
#include <ws/settings/errors.hpp>
#include <ws/settings/filesystem.hpp>

namespace {

// The "category/code" spelling the whole workspace asserts on.
[[nodiscard]] std::string code_of(const ws::core::Error& error) {
    return std::string{error.category} + "/" + std::to_string(error.code);
}

// A minimal valid command: every refusal case starts from one of
// these and breaks exactly one field.
[[nodiscard]] ws::app::Command make_command(std::string id) {
    ws::app::Command command;
    command.id = std::move(id);
    command.label = "do the thing";
    command.category = "demo";
    command.execute = [](std::string_view) {};
    return command;
}

// The resolver's injected clock in test hands: the counter moves by
// assignment between resolves, so every sequence deadline is exact.
struct FakeClock {
    std::int64_t now = 0;
    std::int64_t operator()() const { return now; }
};

// GLFW numbering: letter keys ride their UPPERCASE ASCII codes
// (S = 83, K = 75, O = 79, Q = 81, T = 84, U = 85, P = 80).
constexpr ws::app::Shortcut ctrl(int key) {
    return ws::app::Shortcut{key, ws::app::Mod::control};
}
constexpr ws::app::Shortcut plain_key(int key) {
    return ws::app::Shortcut{key, ws::app::Mod::none};
}

// The settings seam's test double: an in-memory file map plus a
// write-failure countdown (the settings suite's own fake's shape).
class FakeFs final : public ws::settings::FileSystem {
public:
    std::map<std::string, std::string> files;
    int write_failures = 0;  // writes fail while this counts down

    [[nodiscard]] ws::core::Result<std::string> read(
        std::string_view path) const override {
        const auto found = files.find(std::string{path});
        if (found == files.end()) {
            return ws::settings::fail(ws::settings::SettingsError::io_failed,
                                      "not there (test)");
        }
        return found->second;
    }

    ws::core::Status write(std::string_view path,
                           std::string_view bytes) override {
        if (write_failures > 0) {
            --write_failures;
            return ws::settings::fail(ws::settings::SettingsError::io_failed,
                                      "no write (test)");
        }
        files[std::string{path}] = std::string{bytes};
        return {};
    }

    ws::core::Status rename(std::string_view /*from*/,
                            std::string_view /*to*/) override {
        return {};
    }

    bool remove(std::string_view path) override {
        return files.erase(std::string{path}) > 0;
    }

    [[nodiscard]] bool exists(std::string_view path) const override {
        return files.find(std::string{path}) != files.end();
    }
};

// The undo stack's host model: a document whose text the commands
// move, plus the three command kinds the contracts exercise -
// Append coalesces with its own kind (absorbing the payload into
// one undo), SetField never coalesces and captures its prior, and
// Heavy carries a chosen memory estimate for the bounds tests.
struct Doc {
    std::string text;
    std::vector<std::string> log;  // "a:<chunk>" per apply, "u:<chunk>" per undo
};

class Append final : public ws::app::UndoCommand {
public:
    Append(Doc& doc, std::string chunk) : doc_{doc}, chunk_{std::move(chunk)} {}

    void apply() override {
        doc_.text += chunk_;
        doc_.log.push_back("a:" + chunk_);
    }
    void undo() override {
        doc_.text.resize(doc_.text.size() - chunk_.size());
        doc_.log.push_back("u:" + chunk_);
    }
    [[nodiscard]] std::string name() const override { return "Typing"; }

    // Only with another Append: the top command's chunk grows to
    // cover both, so its one undo() reverts the whole run.
    [[nodiscard]] bool merge_with(
        const ws::app::UndoCommand& next) override {
        const auto* typed = dynamic_cast<const Append*>(&next);
        if (typed == nullptr) {
            return false;
        }
        chunk_ += typed->chunk_;
        return true;
    }
    [[nodiscard]] std::size_t memory_estimate() const override {
        return sizeof(Append) + chunk_.size();
    }

private:
    Doc& doc_;
    std::string chunk_;
};

class SetField final : public ws::app::UndoCommand {
public:
    SetField(Doc& doc, std::string value)
        : doc_{doc}, value_{std::move(value)} {}

    void apply() override {
        prior_ = doc_.text;
        doc_.text = value_;
        doc_.log.push_back("s:" + value_);
    }
    void undo() override {
        doc_.text = prior_;
        doc_.log.push_back("r:" + value_);
    }
    [[nodiscard]] std::string name() const override { return "Set Title"; }

private:
    Doc& doc_;
    std::string value_;
    std::string prior_;
};

class Heavy final : public ws::app::UndoCommand {
public:
    Heavy(Doc& doc, std::string chunk, std::size_t estimate)
        : doc_{doc}, chunk_{std::move(chunk)}, estimate_{estimate} {}

    void apply() override { doc_.text += chunk_; }
    void undo() override { doc_.text.resize(doc_.text.size() - chunk_.size()); }
    [[nodiscard]] std::string name() const override { return "Heavy"; }
    [[nodiscard]] std::size_t memory_estimate() const override {
        return estimate_;
    }

private:
    Doc& doc_;
    std::string chunk_;
    std::size_t estimate_;
};

}  // namespace

TEST_CASE("the Mod helpers compose and test") {
    constexpr ws::app::Mod chord =
        ws::app::Mod::control | ws::app::Mod::shift;
    CHECK(ws::app::has_mod(chord, ws::app::Mod::control));
    CHECK(ws::app::has_mod(chord, ws::app::Mod::shift));
    CHECK_FALSE(ws::app::has_mod(chord, ws::app::Mod::alt));
    CHECK_FALSE(ws::app::has_mod(ws::app::Mod::shift, ws::app::Mod::control));
    CHECK(ws::app::has_mod(ws::app::Mod::none, ws::app::Mod::none));
    CHECK(ws::app::has_mod(chord, ws::app::Mod::none) == false);
    CHECK((chord & ws::app::Mod::control) == ws::app::Mod::control);
}

TEST_CASE("commands register and survive the move") {
    ws::app::CommandRegistry registry;
    ws::app::Command command = make_command("file.open");
    command.label = "Open...";
    command.category = "file";
    command.icon = 7;
    command.shortcut.key = 79;  // GLFW O
    command.shortcut.mods = ws::app::Mod::control;
    command.needs_argument = true;
    REQUIRE(registry.add(std::move(command)).has_value());

    const ws::app::Command* found = registry.find("file.open");
    REQUIRE(found != nullptr);
    CHECK(found->label == "Open...");
    CHECK(found->category == "file");
    CHECK(found->icon == 7);
    CHECK(found->shortcut.key == 79);
    CHECK(found->shortcut.mods == ws::app::Mod::control);
    CHECK(found->needs_argument);
    CHECK(found->execute != nullptr);
    CHECK(registry.size() == 1);
}

TEST_CASE("add refuses the empty id, label and callback") {
    ws::app::CommandRegistry registry;

    ws::app::Command no_id = make_command("");
    const auto id_result = registry.add(std::move(no_id));
    REQUIRE_FALSE(id_result.has_value());
    CHECK(code_of(id_result.error()) == "app/1");
    CHECK_FALSE(id_result.error().message.empty());

    ws::app::Command no_label = make_command("edit.copy");
    no_label.label.clear();
    const auto label_result = registry.add(std::move(no_label));
    REQUIRE_FALSE(label_result.has_value());
    CHECK(code_of(label_result.error()) == "app/1");

    ws::app::Command no_callback = make_command("edit.cut");
    no_callback.execute = nullptr;
    const auto callback_result = registry.add(std::move(no_callback));
    REQUIRE_FALSE(callback_result.has_value());
    CHECK(code_of(callback_result.error()) == "app/1");

    CHECK(registry.size() == 0);  // every refusal stored nothing
}

TEST_CASE("add refuses a duplicate id") {
    ws::app::CommandRegistry registry;
    REQUIRE(registry.add(make_command("app.quit")).has_value());
    const auto result = registry.add(make_command("app.quit"));
    REQUIRE_FALSE(result.has_value());
    CHECK(code_of(result.error()) == "app/1");
    CHECK(registry.size() == 1);
}

TEST_CASE("execute dispatches by id with the argument payload") {
    ws::app::CommandRegistry registry;
    std::string seen_argument;
    int runs = 0;

    ws::app::Command command = make_command("app.echo");
    command.execute = [&](std::string_view argument) {
        seen_argument = std::string{argument};
        ++runs;
    };
    REQUIRE(registry.add(std::move(command)).has_value());

    CHECK(registry.execute("app.echo").has_value());
    CHECK(seen_argument.empty());
    CHECK(registry.execute("app.echo", "the payload").has_value());
    CHECK(seen_argument == "the payload");
    CHECK(runs == 2);
}

TEST_CASE("execute answers not_found for the unknown id") {
    ws::app::CommandRegistry registry;
    REQUIRE(registry.add(make_command("app.only")).has_value());
    const auto result = registry.execute("app.missing");
    REQUIRE_FALSE(result.has_value());
    CHECK(code_of(result.error()) == "app/2");
    CHECK(registry.execute("app.missing").error().message.find(
              "app.missing") != std::string::npos);
}

TEST_CASE("a direct dispatch ignores enabled - it is the caller's gate") {
    ws::app::CommandRegistry registry;
    int runs = 0;
    bool on = false;

    ws::app::Command command = make_command("app.toggle");
    command.enabled = [&on] { return on; };
    command.execute = [&](std::string_view) { ++runs; };
    REQUIRE(registry.add(std::move(command)).has_value());

    const ws::app::Command* found = registry.find("app.toggle");
    REQUIRE(found != nullptr);
    CHECK_FALSE(ws::app::CommandRegistry::is_enabled(*found));
    CHECK(registry.execute("app.toggle").has_value());  // still runs
    CHECK(runs == 1);
}

TEST_CASE("the predicates read live, per call") {
    ws::app::CommandRegistry registry;
    bool on = false;

    ws::app::Command command = make_command("view.zen");
    command.enabled = [&on] { return on; };
    command.checked = [&on] { return on; };
    REQUIRE(registry.add(std::move(command)).has_value());

    ws::app::Command plain = make_command("app.plain");
    REQUIRE(registry.add(std::move(plain)).has_value());

    const ws::app::Command* toggling = registry.find("view.zen");
    const ws::app::Command* bare = registry.find("app.plain");
    REQUIRE(toggling != nullptr);
    REQUIRE(bare != nullptr);

    // An empty predicate never pays a call: always enabled, never
    // checked. A live one re-reads every query.
    CHECK(ws::app::CommandRegistry::is_enabled(*bare));
    CHECK_FALSE(ws::app::CommandRegistry::is_checked(*bare));
    CHECK_FALSE(ws::app::CommandRegistry::is_enabled(*toggling));
    on = true;
    CHECK(ws::app::CommandRegistry::is_enabled(*toggling));
    CHECK(ws::app::CommandRegistry::is_checked(*toggling));
}

TEST_CASE("commands keep registration order") {
    ws::app::CommandRegistry registry;
    REQUIRE(registry.add(make_command("file.open")).has_value());
    REQUIRE(registry.add(make_command("edit.copy")).has_value());
    REQUIRE(registry.add(make_command("view.zen")).has_value());

    const std::vector<ws::app::Command>& commands = registry.commands();
    REQUIRE(commands.size() == 3);
    CHECK(commands[0].id == "file.open");
    CHECK(commands[1].id == "edit.copy");
    CHECK(commands[2].id == "view.zen");

    CHECK(registry.find("nope") == nullptr);
}

TEST_CASE("the resolver binds chords and resolves them") {
    ws::app::ShortcutResolver resolver;
    REQUIRE(resolver.bind(ctrl(83), "file.save", ws::app::KeyContext::global)
                .has_value());

    const ws::app::Resolution hit =
        resolver.resolve(ctrl(83), ws::app::KeyContext::global);
    CHECK(hit.command == "file.save");
    CHECK(hit.consumed);
    CHECK_FALSE(hit.sequence_pending);
    // A plain binding is stateless: it fires every press.
    CHECK(resolver.resolve(ctrl(83), ws::app::KeyContext::global).command ==
          "file.save");

    const ws::app::Shortcut* bound =
        resolver.binding_for("file.save", ws::app::KeyContext::global);
    REQUIRE(bound != nullptr);
    CHECK(*bound == ctrl(83));
    const std::string* owner =
        resolver.command_for(ctrl(83), ws::app::KeyContext::global);
    REQUIRE(owner != nullptr);
    CHECK(*owner == "file.save");
    CHECK(resolver.command_for(ctrl(79), ws::app::KeyContext::global) == nullptr);
    CHECK(resolver.binding_for("file.missing", ws::app::KeyContext::global) ==
          nullptr);

    // An unmatched chord is not the resolver's.
    const ws::app::Resolution miss =
        resolver.resolve(ctrl(79), ws::app::KeyContext::global);
    CHECK(miss.command.empty());
    CHECK_FALSE(miss.consumed);

    REQUIRE(resolver.unbind(ctrl(83), ws::app::KeyContext::global).has_value());
    CHECK_FALSE(resolver.resolve(ctrl(83), ws::app::KeyContext::global).consumed);
    CHECK(resolver.binding_for("file.save", ws::app::KeyContext::global) ==
          nullptr);
}

TEST_CASE("bind refuses empty commands, keyless chords and collisions") {
    ws::app::ShortcutResolver resolver;

    const auto no_command =
        resolver.bind(ctrl(83), "", ws::app::KeyContext::global);
    REQUIRE_FALSE(no_command.has_value());
    CHECK(code_of(no_command.error()) == "app/1");

    const auto no_key = resolver.bind(ws::app::Shortcut{}, "file.save",
                                       ws::app::KeyContext::global);
    REQUIRE_FALSE(no_key.has_value());
    CHECK(code_of(no_key.error()) == "app/1");

    REQUIRE(resolver.bind(ctrl(83), "file.save", ws::app::KeyContext::global)
                .has_value());
    const auto clash =
        resolver.bind(ctrl(83), "edit.other", ws::app::KeyContext::global);
    REQUIRE_FALSE(clash.has_value());
    CHECK(code_of(clash.error()) == "app/3");
    CHECK(clash.error().message.find("Ctrl+S") != std::string::npos);
    CHECK(clash.error().message.find("file.save") != std::string::npos);
    // The first binding stands.
    CHECK(resolver.resolve(ctrl(83), ws::app::KeyContext::global).command ==
          "file.save");

    // The same chord in another context is the point of contexts.
    REQUIRE(resolver
                .bind(ctrl(83), "edit.cut-field",
                      ws::app::KeyContext::text_editing)
                .has_value());
    CHECK(resolver.resolve(ctrl(83), ws::app::KeyContext::text_editing).command ==
          "edit.cut-field");
}

TEST_CASE("a chord is either a plain binding or a sequence prefix") {
    ws::app::ShortcutResolver resolver;

    REQUIRE(resolver.bind_sequence(ctrl(75), ctrl(83), "file.save-all",
                                   ws::app::KeyContext::global, 1000)
                .has_value());
    const auto onto_prefix =
        resolver.bind(ctrl(75), "edit.other", ws::app::KeyContext::global);
    REQUIRE_FALSE(onto_prefix.has_value());
    CHECK(code_of(onto_prefix.error()) == "app/3");

    REQUIRE(resolver.bind(ctrl(79), "file.open", ws::app::KeyContext::global)
                .has_value());
    const auto prefix_onto_plain =
        resolver.bind_sequence(ctrl(79), ctrl(83), "file.open-all",
                               ws::app::KeyContext::global, 1000);
    REQUIRE_FALSE(prefix_onto_plain.has_value());
    CHECK(code_of(prefix_onto_plain.error()) == "app/3");

    const auto duplicate_prefix =
        resolver.bind_sequence(ctrl(75), ctrl(79), "file.save-all-2",
                               ws::app::KeyContext::global, 1000);
    REQUIRE_FALSE(duplicate_prefix.has_value());
    CHECK(code_of(duplicate_prefix.error()) == "app/3");

    const auto no_timeout =
        resolver.bind_sequence(ctrl(81), ctrl(79), "file.quit-all",
                               ws::app::KeyContext::global, 0);
    REQUIRE_FALSE(no_timeout.has_value());
    CHECK(code_of(no_timeout.error()) == "app/1");
}

TEST_CASE("two-step sequences arm, complete, and expire on the clock") {
    FakeClock clock;
    ws::app::ShortcutResolver resolver{[&clock] { return clock.now; }};
    const ws::app::Shortcut first = ctrl(75);   // Ctrl+K
    const ws::app::Shortcut second = ctrl(83);  // Ctrl+S

    REQUIRE(resolver.bind_sequence(first, second, "file.save-all",
                                   ws::app::KeyContext::global, 1000)
                .has_value());

    // The first chord arms: consumed, pending, nothing dispatched.
    clock.now = 0;
    const ws::app::Resolution armed =
        resolver.resolve(first, ws::app::KeyContext::global);
    CHECK(armed.command.empty());
    CHECK(armed.consumed);
    CHECK(armed.sequence_pending);
    CHECK(resolver.sequence_pending());

    // The second chord, in time: the sequence's command.
    clock.now = 500;
    const ws::app::Resolution done =
        resolver.resolve(second, ws::app::KeyContext::global);
    CHECK(done.command == "file.save-all");
    CHECK(done.consumed);
    CHECK_FALSE(done.sequence_pending);
    CHECK_FALSE(resolver.sequence_pending());

    // A plain binding on the second chord answers while nothing is
    // armed - and is outranked the moment a step is pending.
    REQUIRE(resolver.bind(second, "edit.steal", ws::app::KeyContext::global)
                .has_value());
    CHECK(resolver.resolve(second, ws::app::KeyContext::global).command ==
          "edit.steal");
    clock.now = 600;
    CHECK(resolver.resolve(first, ws::app::KeyContext::global).sequence_pending);
    clock.now = 700;
    CHECK(resolver.resolve(second, ws::app::KeyContext::global).command ==
          "file.save-all");
    CHECK_FALSE(resolver.sequence_pending());

    // Late: the deadline expired, the second resolves fresh - and now
    // the plain binding underneath answers.
    clock.now = 800;
    CHECK(resolver.resolve(first, ws::app::KeyContext::global).sequence_pending);
    clock.now = 2500;
    const ws::app::Resolution expired =
        resolver.resolve(second, ws::app::KeyContext::global);
    CHECK(expired.command == "edit.steal");
    CHECK(expired.consumed);
    CHECK_FALSE(resolver.sequence_pending());

    // Pressing the first chord again re-arms with a fresh deadline.
    clock.now = 5000;
    CHECK(resolver.resolve(first, ws::app::KeyContext::global).sequence_pending);
    clock.now = 5500;
    CHECK(resolver.resolve(second, ws::app::KeyContext::global).command ==
          "file.save-all");

    // A chord that is neither the second nor a prefix cancels the
    // pending step and resolves fresh.
    clock.now = 6000;
    CHECK(resolver.resolve(first, ws::app::KeyContext::global).sequence_pending);
    const ws::app::Resolution wrong =
        resolver.resolve(ctrl(81), ws::app::KeyContext::global);
    CHECK(wrong.command.empty());
    CHECK_FALSE(wrong.consumed);
    CHECK_FALSE(resolver.sequence_pending());

    // cancel_pending() drops an armed step on demand.
    CHECK(resolver.resolve(first, ws::app::KeyContext::global).sequence_pending);
    resolver.cancel_pending();
    CHECK_FALSE(resolver.sequence_pending());
}

TEST_CASE("a sequence may share its two steps - the double tap") {
    FakeClock clock;
    ws::app::ShortcutResolver resolver{[&clock] { return clock.now; }};
    const ws::app::Shortcut tap = ctrl(84);  // Ctrl+T
    REQUIRE(resolver.bind_sequence(tap, tap, "view.terminal",
                                   ws::app::KeyContext::global, 500)
                .has_value());

    clock.now = 0;
    CHECK(resolver.resolve(tap, ws::app::KeyContext::global).sequence_pending);
    clock.now = 100;
    const ws::app::Resolution done =
        resolver.resolve(tap, ws::app::KeyContext::global);
    CHECK(done.command == "view.terminal");
    CHECK(done.consumed);
    CHECK_FALSE(done.sequence_pending);
}

TEST_CASE("the context ladder prefers the active rung's binding") {
    ws::app::ShortcutResolver resolver;
    const ws::app::Shortcut chord = ctrl(83);
    REQUIRE(resolver.bind(chord, "global.save", ws::app::KeyContext::global)
                .has_value());
    REQUIRE(resolver.bind(chord, "panel.save", ws::app::KeyContext::panel)
                .has_value());
    REQUIRE(resolver.bind(chord, "text.save", ws::app::KeyContext::text_editing)
                .has_value());

    CHECK(resolver.resolve(chord, ws::app::KeyContext::global).command ==
          "global.save");
    CHECK(resolver.resolve(chord, ws::app::KeyContext::panel).command ==
          "panel.save");
    CHECK(resolver.resolve(chord, ws::app::KeyContext::text_editing).command ==
          "text.save");

    // Rungs above the active one are inert: with no panel binding, a
    // panel-focused press falls through to global - the text rung
    // above never gets a look.
    ws::app::ShortcutResolver without_panel;
    REQUIRE(without_panel.bind(chord, "global.save", ws::app::KeyContext::global)
                .has_value());
    REQUIRE(without_panel
                .bind(chord, "text.save", ws::app::KeyContext::text_editing)
                .has_value());
    CHECK(without_panel.resolve(chord, ws::app::KeyContext::panel).command ==
          "global.save");

    // Same for a sequence prefix: a text-editing chord never arms
    // while a lower rung is active.
    ws::app::ShortcutResolver sequences;
    REQUIRE(sequences.bind_sequence(ctrl(75), ctrl(83), "text.seq",
                                    ws::app::KeyContext::text_editing, 1000)
                .has_value());
    const ws::app::Resolution inert =
        sequences.resolve(ctrl(75), ws::app::KeyContext::global);
    CHECK_FALSE(inert.consumed);
    CHECK_FALSE(inert.sequence_pending);
    CHECK(sequences.resolve(ctrl(75), ws::app::KeyContext::text_editing)
              .sequence_pending);
}

TEST_CASE("the text rule keeps unmodified chords for the field") {
    ws::app::ShortcutResolver resolver;
    REQUIRE(resolver.bind(plain_key(83), "file.save-plain",
                          ws::app::KeyContext::global)
                .has_value());
    REQUIRE(resolver.bind(ctrl(83), "file.save", ws::app::KeyContext::global)
                .has_value());
    REQUIRE(resolver.bind(ws::app::Shortcut{83, ws::app::Mod::shift}, "edit.bulk",
                          ws::app::KeyContext::global)
                .has_value());

    // While a field holds focus, an unmodified chord from the lower
    // rung leaves the key to the field - and shift alone is typing.
    const ws::app::Resolution typed =
        resolver.resolve(plain_key(83), ws::app::KeyContext::text_editing);
    CHECK(typed.command.empty());
    CHECK_FALSE(typed.consumed);
    CHECK_FALSE(resolver
                    .resolve(ws::app::Shortcut{83, ws::app::Mod::shift},
                             ws::app::KeyContext::text_editing)
                    .consumed);
    // A modifier beyond shift makes the chord the resolver's.
    const ws::app::Resolution fired =
        resolver.resolve(ctrl(83), ws::app::KeyContext::text_editing);
    CHECK(fired.consumed);
    CHECK(fired.command == "file.save");

    // Without a field focused, the plain chord is the resolver's.
    const ws::app::Resolution global_hit =
        resolver.resolve(plain_key(83), ws::app::KeyContext::global);
    CHECK(global_hit.consumed);
    CHECK(global_hit.command == "file.save-plain");

    // A binding ON the text-editing rung needs no modifiers - it is
    // the field's own chrome (Esc to leave, say) - but it is inert
    // while a plain panel holds focus.
    REQUIRE(resolver
                .bind(ws::app::Shortcut{256, ws::app::Mod::none}, "edit.leave-field",
                      ws::app::KeyContext::text_editing)
                .has_value());
    const ws::app::Resolution own = resolver.resolve(
        ws::app::Shortcut{256, ws::app::Mod::none}, ws::app::KeyContext::text_editing);
    CHECK(own.consumed);
    CHECK(own.command == "edit.leave-field");
    const ws::app::Resolution dormant = resolver.resolve(
        ws::app::Shortcut{256, ws::app::Mod::none}, ws::app::KeyContext::panel);
    CHECK(dormant.command.empty());
    CHECK_FALSE(dormant.consumed);
}

TEST_CASE("a pending step holds its rung across a focus change") {
    FakeClock clock;
    ws::app::ShortcutResolver resolver{[&clock] { return clock.now; }};
    REQUIRE(resolver.bind_sequence(ctrl(75), plain_key(83), "demo.arm",
                                   ws::app::KeyContext::global, 1000)
                .has_value());

    clock.now = 0;
    CHECK(resolver.resolve(ctrl(75), ws::app::KeyContext::global).sequence_pending);
    // The second chord is unmodified "s" and focus moved into a field:
    // the armed step still completes - the first press was the
    // user's, the timeout bounds it.
    clock.now = 100;
    const ws::app::Resolution completed =
        resolver.resolve(plain_key(83), ws::app::KeyContext::text_editing);
    CHECK(completed.command == "demo.arm");
    CHECK(completed.consumed);
    CHECK_FALSE(resolver.sequence_pending());
}

TEST_CASE("rebinding moves, refuses collisions, and removes") {
    ws::app::ShortcutResolver resolver;
    REQUIRE(resolver.bind(ctrl(83), "file.save", ws::app::KeyContext::global)
                .has_value());
    REQUIRE(resolver.bind(ctrl(84), "edit.bulk", ws::app::KeyContext::global)
                .has_value());

    // Onto another command's chord: refused, both stand.
    const auto stolen =
        resolver.rebind("file.save", ws::app::KeyContext::global, ctrl(84));
    REQUIRE_FALSE(stolen.has_value());
    CHECK(code_of(stolen.error()) == "app/3");
    CHECK(stolen.error().message.find("edit.bulk") != std::string::npos);
    CHECK(*resolver.binding_for("file.save", ws::app::KeyContext::global) ==
          ctrl(83));

    // Onto a free chord: moved, the old one free.
    REQUIRE(resolver.rebind("file.save", ws::app::KeyContext::global, ctrl(85))
                .has_value());
    CHECK(*resolver.binding_for("file.save", ws::app::KeyContext::global) ==
          ctrl(85));
    CHECK(resolver.command_for(ctrl(83), ws::app::KeyContext::global) == nullptr);
    CHECK(*resolver.command_for(ctrl(85), ws::app::KeyContext::global) ==
          "file.save");

    // Onto its own chord: the no-op it reads as.
    REQUIRE(resolver.rebind("file.save", ws::app::KeyContext::global, ctrl(85))
                .has_value());

    // Onto a sequence's first chord: refused.
    REQUIRE(resolver.bind_sequence(ctrl(75), ctrl(79), "demo.seq",
                                   ws::app::KeyContext::global, 1000)
                .has_value());
    const auto onto_prefix =
        resolver.rebind("file.save", ws::app::KeyContext::global, ctrl(75));
    REQUIRE_FALSE(onto_prefix.has_value());
    CHECK(code_of(onto_prefix.error()) == "app/3");

    // Key 0 removes the binding.
    REQUIRE(resolver.rebind("file.save", ws::app::KeyContext::global,
                            ws::app::Shortcut{})
                .has_value());
    CHECK(resolver.binding_for("file.save", ws::app::KeyContext::global) ==
          nullptr);

    // A command with no binding in that context answers not_found.
    const auto unbound =
        resolver.rebind("file.save", ws::app::KeyContext::global, ctrl(83));
    REQUIRE_FALSE(unbound.has_value());
    CHECK(code_of(unbound.error()) == "app/2");
}

TEST_CASE("install_defaults seeds the declared chords, whole or not at all") {
    ws::app::CommandRegistry registry;
    ws::app::Command open = make_command("file.open");
    open.shortcut = ctrl(79);  // Ctrl+O
    REQUIRE(registry.add(std::move(open)).has_value());
    ws::app::Command quit = make_command("file.quit");
    quit.shortcut = ctrl(81);  // Ctrl+Q
    REQUIRE(registry.add(std::move(quit)).has_value());
    ws::app::Command bare = make_command("app.no-chord");
    REQUIRE(registry.add(std::move(bare)).has_value());

    ws::app::ShortcutResolver resolver;
    REQUIRE(resolver.install_defaults(registry).has_value());
    CHECK(*resolver.binding_for("file.open", ws::app::KeyContext::global) ==
          ctrl(79));
    CHECK(*resolver.binding_for("file.quit", ws::app::KeyContext::global) ==
          ctrl(81));
    CHECK(resolver.binding_for("app.no-chord", ws::app::KeyContext::global) ==
          nullptr);
    CHECK(resolver.resolve(ctrl(79), ws::app::KeyContext::global).command ==
          "file.open");

    // A user rebind, then reset: back to the seed.
    REQUIRE(resolver.rebind("file.open", ws::app::KeyContext::global, ctrl(80))
                .has_value());
    resolver.reset_to_defaults();
    CHECK(*resolver.binding_for("file.open", ws::app::KeyContext::global) ==
          ctrl(79));

    // A re-seed replaces the live table too - the seed is
    // declarative data, not merged state.
    REQUIRE(resolver.rebind("file.open", ws::app::KeyContext::global, ctrl(80))
                .has_value());
    REQUIRE(resolver.install_defaults(registry).has_value());
    CHECK(*resolver.binding_for("file.open", ws::app::KeyContext::global) ==
          ctrl(79));

    // Two commands declaring the same chord: nothing seeded, the
    // previous table stands, and the message names both.
    ws::app::CommandRegistry clashing;
    ws::app::Command first = make_command("a.first");
    first.shortcut = ctrl(83);
    REQUIRE(clashing.add(std::move(first)).has_value());
    ws::app::Command second = make_command("a.second");
    second.shortcut = ctrl(83);
    REQUIRE(clashing.add(std::move(second)).has_value());
    const auto clash = resolver.install_defaults(clashing);
    REQUIRE_FALSE(clash.has_value());
    CHECK(code_of(clash.error()) == "app/3");
    CHECK(clash.error().message.find("a.first") != std::string::npos);
    CHECK(clash.error().message.find("a.second") != std::string::npos);
    CHECK(*resolver.binding_for("file.open", ws::app::KeyContext::global) ==
          ctrl(79));
}

TEST_CASE("install replaces every context's table whole or not at all") {
    FakeClock clock;
    ws::app::ShortcutResolver resolver{[&clock] { return clock.now; }};
    REQUIRE(resolver.bind(ctrl(83), "old.save", ws::app::KeyContext::global)
                .has_value());

    ws::app::ShortcutTable global;
    global.bindings.push_back(ws::app::ShortcutBinding{ctrl(79), "file.open"});
    global.bindings.push_back(ws::app::ShortcutBinding{ctrl(81), "file.quit"});
    global.sequences.push_back(
        ws::app::ShortcutSequence{ctrl(75), ctrl(83), "file.save-all", 1000});
    ws::app::ShortcutTable panel;
    panel.bindings.push_back(ws::app::ShortcutBinding{ctrl(84), "panel.trees"});
    ws::app::ShortcutTable text_editing;
    text_editing.bindings.push_back(ws::app::ShortcutBinding{
        ws::app::Shortcut{256, ws::app::Mod::none}, "edit.leave-field"});

    REQUIRE(resolver.install(global, panel, text_editing).has_value());
    const ws::app::ShortcutTable read =
        resolver.table_of(ws::app::KeyContext::global);
    REQUIRE(read.bindings.size() == 2);
    CHECK(read.bindings[0].command == "file.open");
    CHECK(read.bindings[0].chord == ctrl(79));
    CHECK(read.bindings[1].command == "file.quit");
    REQUIRE(read.sequences.size() == 1);
    CHECK(read.sequences[0].command == "file.save-all");
    CHECK(read.sequences[0].timeout_ms == 1000);
    REQUIRE(resolver.table_of(ws::app::KeyContext::panel).bindings.size() == 1);
    CHECK(resolver.table_of(ws::app::KeyContext::text_editing)
              .bindings[0]
              .command == "edit.leave-field");
    // The old live table is gone.
    CHECK(resolver.binding_for("old.save", ws::app::KeyContext::global) ==
          nullptr);
    // An installed sequence arms and completes like a bound one.
    clock.now = 0;
    CHECK(resolver.resolve(ctrl(75), ws::app::KeyContext::global).sequence_pending);
    clock.now = 500;
    CHECK(resolver.resolve(ctrl(83), ws::app::KeyContext::global).command ==
          "file.save-all");

    // A clash inside the batch: refused, the live table stands.
    ws::app::ShortcutTable clashing;
    clashing.bindings.push_back(ws::app::ShortcutBinding{ctrl(79), "file.open"});
    clashing.bindings.push_back(ws::app::ShortcutBinding{ctrl(79), "file.dup"});
    const auto clash = resolver.install(clashing, panel, text_editing);
    REQUIRE_FALSE(clash.has_value());
    CHECK(code_of(clash.error()) == "app/3");
    CHECK(*resolver.binding_for("file.open", ws::app::KeyContext::global) ==
          ctrl(79));

    // A keyless binding: app/1, still nothing changed.
    ws::app::ShortcutTable keyless;
    keyless.bindings.push_back(
        ws::app::ShortcutBinding{ws::app::Shortcut{}, "file.none"});
    const auto no_key = resolver.install(keyless, panel, text_editing);
    REQUIRE_FALSE(no_key.has_value());
    CHECK(code_of(no_key.error()) == "app/1");

    // A plain binding and a prefix on the same chord in one batch:
    // app/3 (the rules bind() runs are the batch's rules too).
    ws::app::ShortcutTable mixed;
    mixed.bindings.push_back(ws::app::ShortcutBinding{ctrl(75), "edit.both"});
    mixed.sequences.push_back(
        ws::app::ShortcutSequence{ctrl(75), ctrl(79), "edit.seq", 500});
    const auto both = resolver.install(mixed, panel, text_editing);
    REQUIRE_FALSE(both.has_value());
    CHECK(code_of(both.error()) == "app/3");

    // A zero timeout deep in the panel batch: app/1, and the already
    // fine global scratch commits nothing either.
    ws::app::ShortcutTable no_timeout;
    no_timeout.sequences.push_back(
        ws::app::ShortcutSequence{ctrl(85), ctrl(85), "edit.tap", 0});
    const auto zero = resolver.install(global, no_timeout, text_editing);
    REQUIRE_FALSE(zero.has_value());
    CHECK(code_of(zero.error()) == "app/1");
    CHECK(resolver.table_of(ws::app::KeyContext::global).bindings.size() == 2);
}

TEST_CASE("the keymap round-trips through a file") {
    FakeFs fs;
    ws::app::ShortcutResolver resolver;
    REQUIRE(resolver.bind(ctrl(83), "file.save", ws::app::KeyContext::global)
                .has_value());
    REQUIRE(resolver.bind(ctrl(79), "file.open", ws::app::KeyContext::global)
                .has_value());
    REQUIRE(resolver
                .bind(plain_key(83), "edit.cut-field",
                      ws::app::KeyContext::text_editing)
                .has_value());
    REQUIRE(resolver.bind_sequence(ctrl(75), ctrl(83), "file.save-all",
                                   ws::app::KeyContext::global, 1000)
                .has_value());
    // A user rebind, to prove the whole live table is what travels.
    REQUIRE(resolver.rebind("file.save", ws::app::KeyContext::global, ctrl(84))
                .has_value());

    ws::app::Keymap writer{fs, "/cfg/keymap.json"};
    REQUIRE(writer.save(resolver).has_value());
    const std::string written = fs.files["/cfg/keymap.json"];
    CHECK(written.find("\"version\": 1") != std::string::npos);

    // A second resolver seeded differently: the file REPLACES it.
    ws::app::ShortcutResolver reader;
    REQUIRE(reader.bind(ctrl(81), "file.quit", ws::app::KeyContext::global)
                .has_value());
    ws::app::Keymap loader{fs, "/cfg/keymap.json"};
    REQUIRE(loader.load(reader).has_value());
    CHECK(*reader.binding_for("file.save", ws::app::KeyContext::global) ==
          ctrl(84));  // the rebind survived the trip
    CHECK(*reader.binding_for("file.open", ws::app::KeyContext::global) ==
          ctrl(79));
    CHECK(*reader.binding_for("edit.cut-field",
                             ws::app::KeyContext::text_editing) == plain_key(83));
    CHECK(reader.binding_for("file.quit", ws::app::KeyContext::global) ==
          nullptr);  // the seed the file replaced
    CHECK(reader.table_of(ws::app::KeyContext::global).sequences.size() == 1);
    CHECK(reader.table_of(ws::app::KeyContext::global)
              .sequences[0]
              .timeout_ms == 1000);

    // Save -> load -> save is byte-identical: table order survives
    // install, and the writer's shape is fixed.
    ws::app::Keymap rewriter{fs, "/cfg/keymap.json"};
    REQUIRE(rewriter.save(reader).has_value());
    CHECK(fs.files["/cfg/keymap.json"] == written);
}

TEST_CASE("a missing keymap is the first run - the seed stands") {
    FakeFs fs;
    ws::app::ShortcutResolver resolver;
    REQUIRE(resolver.bind(ctrl(83), "file.save", ws::app::KeyContext::global)
                .has_value());

    ws::app::Keymap keymap{fs, "/cfg/absent.json"};
    CHECK(keymap.path() == "/cfg/absent.json");
    const ws::core::Status loaded = keymap.load(resolver);
    REQUIRE(loaded.has_value());
    CHECK(*resolver.binding_for("file.save", ws::app::KeyContext::global) ==
          ctrl(83));  // untouched - never wipe a seed on a missing file
}

TEST_CASE("a corrupt keymap is refused with nothing installed") {
    FakeFs fs;
    ws::app::ShortcutResolver resolver;
    REQUIRE(resolver.bind(ctrl(83), "file.save", ws::app::KeyContext::global)
                .has_value());
    ws::app::Keymap keymap{fs, "/cfg/keymap.json"};

    // Grammar trouble is app/4; well-formed json that is not a v1
    // keymap is app/5. Every refusal leaves the seed standing.
    const auto refused = [&](std::string_view bytes, const char* code) {
        fs.files["/cfg/keymap.json"] = std::string{bytes};
        const ws::core::Status loaded = keymap.load(resolver);
        REQUIRE_FALSE(loaded.has_value());
        CHECK(code_of(loaded.error()) == code);
        CHECK(*resolver.binding_for("file.save", ws::app::KeyContext::global) ==
              ctrl(83));
    };

    refused("{ not json", "app/4");
    refused("[]", "app/5");
    refused("{\"version\": 2}", "app/5");
    refused("{\"version\": \"one\"}", "app/5");
    refused("{\"version\": 1, \"contexts\": []}", "app/5");
    refused("{\"version\": 1, \"contexts\": {\"global\": []}}", "app/5");
    refused(
        "{\"version\": 1, \"contexts\": {\"global\": {\"bindings\": [{}]}}}",
        "app/5");
    refused(
        "{\"version\": 1, \"contexts\": {\"global\": {\"bindings\": "
        "[{\"command\": \"x\", \"chord\": {\"key\": 0}}]}}}",
        "app/5");
    refused(
        "{\"version\": 1, \"contexts\": {\"global\": {\"bindings\": "
        "[{\"command\": \"x\", \"chord\": {\"key\": 83, \"mods\": 16}}]}}}",
        "app/5");
    refused(
        "{\"version\": 1, \"contexts\": {\"global\": {\"sequences\": "
        "[{\"command\": \"x\", \"first\": {\"key\": 75}, \"second\": "
        "{\"key\": 83}, \"timeout_ms\": 0}]}}}",
        "app/5");

    // Tolerance: a newer file's unknown keys - a top-level field, an
    // entry field, a context this version does not know - are
    // forward data, never a refusal. The known entries still land,
    // replacing (not merging into) what the resolver held.
    fs.files["/cfg/keymap.json"] =
        "{\"version\": 1, \"future\": 7, \"contexts\": {"
        "\"global\": {\"bindings\": [{\"command\": \"file.open\", "
        "\"chord\": {\"key\": 79, \"mods\": 2}, \"hint\": \"Ctrl+O\"}], "
        "\"extra\": 3}, \"panel\": {}, \"future-rung\": {\"bindings\": []}}}";
    const ws::core::Status loaded = keymap.load(resolver);
    REQUIRE(loaded.has_value());
    CHECK(*resolver.binding_for("file.open", ws::app::KeyContext::global) ==
          ctrl(79));
    CHECK(resolver.binding_for("file.save", ws::app::KeyContext::global) ==
          nullptr);  // replaced, not merged
}

TEST_CASE("a clashing keymap answers the conflict and installs nothing") {
    FakeFs fs;
    fs.files["/cfg/keymap.json"] =
        "{\"version\": 1, \"contexts\": {\"global\": {\"bindings\": ["
        "{\"command\": \"file.open\", \"chord\": {\"key\": 79, \"mods\": 2}}, "
        "{\"command\": \"file.dup\", \"chord\": {\"key\": 79, \"mods\": 2}}]}}}";

    ws::app::ShortcutResolver resolver;
    REQUIRE(resolver.bind(ctrl(83), "file.save", ws::app::KeyContext::global)
                .has_value());
    ws::app::Keymap keymap{fs, "/cfg/keymap.json"};
    const ws::core::Status loaded = keymap.load(resolver);
    REQUIRE_FALSE(loaded.has_value());
    CHECK(code_of(loaded.error()) == "app/3");
    CHECK(loaded.error().message.find("Ctrl+O") != std::string::npos);
    CHECK(*resolver.binding_for("file.save", ws::app::KeyContext::global) ==
          ctrl(83));  // the seed stands
    CHECK(resolver.binding_for("file.open", ws::app::KeyContext::global) ==
          nullptr);
}

TEST_CASE("a failed save leaves the previous file intact") {
    FakeFs fs;
    ws::app::ShortcutResolver resolver;
    REQUIRE(resolver.bind(ctrl(83), "file.save", ws::app::KeyContext::global)
                .has_value());
    ws::app::Keymap keymap{fs, "/cfg/keymap.json"};
    REQUIRE(keymap.save(resolver).has_value());
    const std::string first_save = fs.files["/cfg/keymap.json"];

    // The seam's write fails: its Status propagates AS-IS (category
    // "settings" - the seam's own error, never re-wrapped), and the
    // previous file is untouched.
    fs.write_failures = 1;
    REQUIRE(resolver.rebind("file.save", ws::app::KeyContext::global, ctrl(84))
                .has_value());
    const ws::core::Status failed = keymap.save(resolver);
    REQUIRE_FALSE(failed.has_value());
    CHECK(code_of(failed.error()) == "settings/8");
    CHECK(fs.files["/cfg/keymap.json"] == first_save);

    // The next load still sees the first save's table.
    ws::app::ShortcutResolver reader;
    REQUIRE(keymap.load(reader).has_value());
    CHECK(*reader.binding_for("file.save", ws::app::KeyContext::global) ==
          ctrl(83));  // the pre-rebind chord the first save carried
}

TEST_CASE("import and export move the keymap as text") {
    FakeFs fs;
    ws::app::Keymap keymap{fs, "/cfg/keymap.json"};

    ws::app::ShortcutResolver resolver;
    REQUIRE(resolver.bind(ctrl(79), "file.open", ws::app::KeyContext::global)
                .has_value());
    const std::string text = keymap.export_text(resolver);
    CHECK(text.find("\"version\": 1") != std::string::npos);
    CHECK(text.find("file.open") != std::string::npos);

    // Into a fresh resolver: the same table, the same text back.
    ws::app::ShortcutResolver imported;
    REQUIRE(keymap.import_text(text, imported).has_value());
    CHECK(*imported.binding_for("file.open", ws::app::KeyContext::global) ==
          ctrl(79));
    CHECK(keymap.export_text(imported) == text);

    // Corrupt text refuses with the importer untouched.
    const auto bad = keymap.import_text("{ nope", imported);
    REQUIRE_FALSE(bad.has_value());
    CHECK(code_of(bad.error()) == "app/4");
    CHECK(*imported.binding_for("file.open", ws::app::KeyContext::global) ==
          ctrl(79));
}

TEST_CASE("a StdFileSystem keymap smoke in the OS temp") {
    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / "ws_app_keymap_test.json";
    ws::settings::StdFileSystem fs;

    ws::app::ShortcutResolver resolver;
    REQUIRE(resolver.bind(ctrl(83), "file.save", ws::app::KeyContext::global)
                .has_value());
    ws::app::Keymap writer{fs, file.string()};
    REQUIRE(writer.save(resolver).has_value());

    ws::app::ShortcutResolver reader;
    ws::app::Keymap loader{fs, file.string()};
    REQUIRE(loader.load(reader).has_value());
    CHECK(*reader.binding_for("file.save", ws::app::KeyContext::global) ==
          ctrl(83));

    CHECK(fs.remove(file.string()));
}

TEST_CASE("push applies, undo reverts, redo reapplies") {
    Doc doc;
    ws::app::UndoStack stack;

    // Push IS the edit: apply() runs before the stack records, so
    // the host never pre-applies.
    REQUIRE(stack.push(std::make_unique<SetField>(doc, "one")).has_value());
    CHECK(doc.text == "one");
    REQUIRE(stack.push(std::make_unique<SetField>(doc, "two")).has_value());
    CHECK(doc.text == "two");

    CHECK(stack.can_undo());
    CHECK_FALSE(stack.can_redo());
    CHECK(stack.undo_count() == 2);
    CHECK(stack.redo_count() == 0);
    CHECK(stack.name_to_undo() == "Set Title");
    CHECK(stack.name_to_redo().empty());
    // The default estimate is the BASE-size floor: the stack
    // cannot see the derived object, so a host with payload
    // overrides memory_estimate (Append and Heavy below do).
    CHECK(stack.memory_used() == 2 * sizeof(ws::app::UndoCommand));

    REQUIRE(stack.undo().has_value());
    CHECK(doc.text == "one");
    CHECK(stack.name_to_redo() == "Set Title");
    REQUIRE(stack.redo().has_value());
    CHECK(doc.text == "two");

    // The branches run out: not_found, not a crash. Both steps are
    // redoable, so it takes both redos back before redo drains.
    REQUIRE(stack.undo().has_value());
    REQUIRE(stack.undo().has_value());
    CHECK(doc.text == "");
    const ws::core::Status drained = stack.undo();
    REQUIRE_FALSE(drained.has_value());
    CHECK(code_of(drained.error()) == "app/2");
    CHECK(stack.name_to_undo().empty());  // the undo branch is drained
    REQUIRE(stack.redo().has_value());
    REQUIRE(stack.redo().has_value());
    const ws::core::Status re_drained = stack.redo();
    REQUIRE_FALSE(re_drained.has_value());
    CHECK(code_of(re_drained.error()) == "app/2");

    const ws::core::Status null_push =
        stack.push(std::unique_ptr<SetField>{nullptr});
    REQUIRE_FALSE(null_push.has_value());
    CHECK(code_of(null_push.error()) == "app/1");
}

TEST_CASE("consecutive commands of the same kind coalesce into one step") {
    Doc doc;
    ws::app::UndoStack stack;

    REQUIRE(stack.push(std::make_unique<Append>(doc, "ab")).has_value());
    REQUIRE(stack.push(std::make_unique<Append>(doc, "cd")).has_value());
    REQUIRE(stack.push(std::make_unique<Append>(doc, "ef")).has_value());
    CHECK(doc.text == "abcdef");
    CHECK(doc.log.size() == 3);  // every command applied; coalescing is
                                 // only the history's shape

    // One step: the top absorbed both payloads, and its estimate
    // grew to cover them.
    CHECK(stack.undo_count() == 1);
    CHECK(stack.memory_used() == sizeof(Append) + 6);
    CHECK(stack.name_to_undo() == "Typing");

    // One undo reverts the whole run - the absorbed payload too.
    REQUIRE(stack.undo().has_value());
    CHECK(doc.text == "");
    REQUIRE(stack.redo().has_value());
    CHECK(doc.text == "abcdef");

    // A different kind never coalesces: the offer goes to the top
    // command, whose merge_with says no. Three pushes, three steps.
    Doc mixed;
    ws::app::UndoStack mixing;
    REQUIRE(mixing.push(std::make_unique<Append>(mixed, "x")).has_value());
    REQUIRE(mixing.push(std::make_unique<SetField>(mixed, "title")).has_value());
    REQUIRE(mixing.push(std::make_unique<Append>(mixed, "y")).has_value());
    CHECK(mixed.text == "titley");  // the Append rides on the set text
    CHECK(mixing.undo_count() == 3);
    REQUIRE(mixing.undo().has_value());
    CHECK(mixed.text == "title");  // only the second Append reverted
}

TEST_CASE("a push past a fork drops the redo branch") {
    Doc doc;
    ws::app::UndoStack stack;

    REQUIRE(stack.push(std::make_unique<SetField>(doc, "one")).has_value());
    REQUIRE(stack.undo().has_value());
    CHECK(stack.redo_count() == 1);
    REQUIRE(stack.push(std::make_unique<SetField>(doc, "other")).has_value());

    CHECK(stack.redo_count() == 0);
    const ws::core::Status no_redo = stack.redo();
    REQUIRE_FALSE(no_redo.has_value());
    CHECK(code_of(no_redo.error()) == "app/2");
    CHECK(stack.name_to_redo().empty());
    // The branch rebuilt at the fork point: the pushed-past step is
    // the only survivor, so exactly one undo stands.
    CHECK(stack.undo_count() == 1);
}

TEST_CASE("transactions group many pushes into one named step") {
    Doc doc;
    ws::app::UndoStack stack;

    // Everything under an open transaction is refused: undoing
    // mid-group would corrupt it, and so would redo, mark_saved,
    // clear and set_limits (whose immediate trim would bypass the
    // assembling step). The gates read false while it is open.
    REQUIRE(stack.begin_transaction("Batch Edit").has_value());
    CHECK_FALSE(stack.can_undo());
    CHECK_FALSE(stack.can_redo());
    const ws::core::Status mid_undo = stack.undo();
    REQUIRE_FALSE(mid_undo.has_value());
    CHECK(code_of(mid_undo.error()) == "app/1");
    const ws::core::Status mid_redo = stack.redo();
    REQUIRE_FALSE(mid_redo.has_value());
    CHECK(code_of(mid_redo.error()) == "app/1");
    const ws::core::Status mid_saved = stack.mark_saved();
    REQUIRE_FALSE(mid_saved.has_value());
    CHECK(code_of(mid_saved.error()) == "app/1");
    const ws::core::Status mid_clear = stack.clear();
    REQUIRE_FALSE(mid_clear.has_value());
    CHECK(code_of(mid_clear.error()) == "app/1");
    const ws::core::Status mid_limits = stack.set_limits(1, 0);
    REQUIRE_FALSE(mid_limits.has_value());
    CHECK(code_of(mid_limits.error()) == "app/1");
    const ws::core::Status already_open = stack.begin_transaction("Again");
    REQUIRE_FALSE(already_open.has_value());
    CHECK(code_of(already_open.error()) == "app/1");
    const ws::core::Status mid_null = stack.push(nullptr);
    REQUIRE_FALSE(mid_null.has_value());
    CHECK(code_of(mid_null.error()) == "app/1");

    REQUIRE(stack.push(std::make_unique<SetField>(doc, "X")).has_value());
    REQUIRE(stack.push(std::make_unique<Append>(doc, "y")).has_value());
    REQUIRE(stack.push(std::make_unique<SetField>(doc, "Z")).has_value());
    CHECK(doc.text == "Z");
    REQUIRE(stack.end_transaction().has_value());

    // One step, the transaction's name, not the commands'.
    CHECK(stack.undo_count() == 1);
    CHECK(stack.name_to_undo() == "Batch Edit");

    // ONE undo reverts the whole group - the point of a
    // transaction - running its commands in REVERSE: the log shows
    // the last edit's revert first (r:Z before u:y before r:X).
    REQUIRE(stack.undo().has_value());
    CHECK(doc.text == "");
    CHECK(doc.log == std::vector<std::string>{"s:X", "a:y", "s:Z", "r:Z",
                                              "u:y", "r:X"});
    const ws::core::Status nothing = stack.undo();
    REQUIRE_FALSE(nothing.has_value());
    CHECK(code_of(nothing.error()) == "app/2");

    // Redo replays the same group forward: X, then y, then Z.
    REQUIRE(stack.redo().has_value());
    CHECK(doc.text == "Z");
    CHECK(doc.log.size() == 9);      // three applies, three reverts,
    CHECK(doc.log.back() == "s:Z");  // three re-applies

    const ws::core::Status not_open = stack.end_transaction();
    REQUIRE_FALSE(not_open.has_value());
    CHECK(code_of(not_open.error()) == "app/1");
}

TEST_CASE("an empty transaction records no step and never merges across") {
    Doc doc;
    ws::app::UndoStack stack;

    REQUIRE(stack.push(std::make_unique<SetField>(doc, "one")).has_value());
    REQUIRE(stack.push(std::make_unique<SetField>(doc, "two")).has_value());
    REQUIRE(stack.undo().has_value());  // the redo branch holds "two"

    // An empty transaction changes nothing: no step, the redo
    // branch stands.
    REQUIRE(stack.begin_transaction("No-op").has_value());
    REQUIRE(stack.end_transaction().has_value());
    CHECK(stack.undo_count() == 1);
    CHECK(stack.redo_count() == 1);
    REQUIRE(stack.redo().has_value());
    CHECK(doc.text == "two");

    // The coalescing hook never crosses a step boundary: an Append
    // pushed before begin() stays its own step, the Append inside
    // the transaction starts a fresh one.
    Doc typed;
    ws::app::UndoStack typing;
    REQUIRE(typing.push(std::make_unique<Append>(typed, "a")).has_value());
    REQUIRE(typing.begin_transaction("Second keystroke").has_value());
    REQUIRE(typing.push(std::make_unique<Append>(typed, "b")).has_value());
    REQUIRE(typing.end_transaction().has_value());
    CHECK(typing.undo_count() == 2);  // NOT coalesced into one
    REQUIRE(typing.undo().has_value());
    CHECK(typed.text == "a");  // only "b" reverted
    REQUIRE(typing.undo().has_value());
    CHECK(typed.text == "");

    // The empty name refuses.
    const ws::core::Status nameless = stack.begin_transaction("");
    REQUIRE_FALSE(nameless.has_value());
    CHECK(code_of(nameless.error()) == "app/1");
}

TEST_CASE("the save point tracks dirty across undo and redo") {
    Doc doc;
    ws::app::UndoStack stack;

    REQUIRE(stack.push(std::make_unique<SetField>(doc, "one")).has_value());
    REQUIRE(stack.push(std::make_unique<SetField>(doc, "two")).has_value());
    REQUIRE(stack.mark_saved().has_value());
    CHECK_FALSE(stack.is_dirty());

    // Forward past the save point reads dirty, back onto it clean:
    // the comparison is positional, in both directions.
    REQUIRE(stack.push(std::make_unique<SetField>(doc, "three")).has_value());
    CHECK(stack.is_dirty());
    REQUIRE(stack.undo().has_value());
    CHECK_FALSE(stack.is_dirty());
    REQUIRE(stack.undo().has_value());
    CHECK(stack.is_dirty());
    REQUIRE(stack.redo().has_value());
    CHECK_FALSE(stack.is_dirty());
    REQUIRE(stack.redo().has_value());
    CHECK(stack.is_dirty());

    // A push at a CLEAN position: the save point sits AT the redo
    // boundary - it is the current state - and survives the drop;
    // one undo reads clean again. (It is not "inside" the dropped
    // branch: the branch's steps have not touched it.)
    REQUIRE(stack.undo().has_value());
    CHECK_FALSE(stack.is_dirty());
    REQUIRE(stack.mark_saved().has_value());
    CHECK(stack.redo_count() == 1);
    REQUIRE(stack.push(std::make_unique<SetField>(doc, "four")).has_value());
    CHECK(stack.is_dirty());
    REQUIRE(stack.undo().has_value());
    CHECK_FALSE(stack.is_dirty());
    CHECK(doc.text == "two");

    // Unreachable: a save point INSIDE the dropped redo branch was
    // a state those steps built - dirty is permanent until the
    // next mark_saved, through undo and redo both.
    Doc lost;
    ws::app::UndoStack forgetting;
    REQUIRE(forgetting.push(std::make_unique<SetField>(lost, "one"))
                .has_value());
    REQUIRE(forgetting.mark_saved().has_value());
    REQUIRE(forgetting.undo().has_value());
    CHECK(forgetting.is_dirty());
    REQUIRE(forgetting.push(std::make_unique<SetField>(lost, "two"))
                .has_value());
    CHECK(forgetting.is_dirty());
    REQUIRE(forgetting.undo().has_value());
    CHECK(forgetting.is_dirty());  // NOT back onto the save point
    REQUIRE(forgetting.redo().has_value());
    CHECK(forgetting.is_dirty());
    REQUIRE(forgetting.mark_saved().has_value());
    CHECK_FALSE(forgetting.is_dirty());
}

TEST_CASE("history is bounded by step count, oldest out first") {
    Doc doc;
    ws::app::UndoStack stack{2, 0};

    REQUIRE(stack.push(std::make_unique<SetField>(doc, "1")).has_value());
    REQUIRE(stack.push(std::make_unique<SetField>(doc, "2")).has_value());
    REQUIRE(stack.push(std::make_unique<SetField>(doc, "3")).has_value());
    // "1" evicted: permanently applied, no longer undoable - the
    // doc never loses text to a bound.
    CHECK(stack.undo_count() == 2);
    CHECK(doc.text == "3");
    REQUIRE(stack.undo().has_value());
    CHECK(doc.text == "2");
    REQUIRE(stack.undo().has_value());
    CHECK(doc.text == "1");  // the evicted step is the floor
    const ws::core::Status floored = stack.undo();
    REQUIRE_FALSE(floored.has_value());
    CHECK(code_of(floored.error()) == "app/2");

    // An eviction under a save point at the BASE position (saved
    // with NOTHING applied) makes it unreachable: the old base fell
    // out with the evicted step - it is permanently applied now -
    // so no undo reads clean until mark_saved re-arms it.
    Doc saved;
    ws::app::UndoStack bounded{2, 0};
    REQUIRE(bounded.push(std::make_unique<SetField>(saved, "1")).has_value());
    REQUIRE(bounded.push(std::make_unique<SetField>(saved, "2")).has_value());
    REQUIRE(bounded.undo().has_value());
    REQUIRE(bounded.undo().has_value());  // fully back to base
    REQUIRE(bounded.mark_saved().has_value());
    CHECK_FALSE(bounded.is_dirty());
    REQUIRE(bounded.push(std::make_unique<SetField>(saved, "3")).has_value());
    REQUIRE(bounded.push(std::make_unique<SetField>(saved, "4")).has_value());
    REQUIRE(bounded.push(std::make_unique<SetField>(saved, "5")).has_value());
    CHECK(bounded.is_dirty());  // "3" fell out, the save point with it
    REQUIRE(bounded.undo().has_value());
    CHECK(saved.text == "4");
    CHECK(bounded.is_dirty());  // dirty all the way down
    REQUIRE(bounded.undo().has_value());
    CHECK(saved.text == "3");  // the permanent floor
    CHECK(bounded.is_dirty());
    const ws::core::Status floored_again = bounded.undo();
    REQUIRE_FALSE(floored_again.has_value());
    CHECK(code_of(floored_again.error()) == "app/2");
    REQUIRE(bounded.mark_saved().has_value());
    CHECK_FALSE(bounded.is_dirty());
}

TEST_CASE("history is bounded by the memory estimate, newest kept") {
    Doc doc;
    ws::app::UndoStack stack{0, 250};

    REQUIRE(stack.push(std::make_unique<Heavy>(doc, "a", 100)).has_value());
    REQUIRE(stack.push(std::make_unique<Heavy>(doc, "b", 100)).has_value());
    CHECK(stack.memory_used() == 200);
    REQUIRE(stack.push(std::make_unique<Heavy>(doc, "c", 100)).has_value());
    // 300 over 250: the OLDEST step falls out; the doc keeps "a".
    CHECK(stack.undo_count() == 2);
    CHECK(stack.memory_used() == 200);
    CHECK(doc.text == "abc");
    // Undo trims from the END: "c" then "b" - the evicted "a" is
    // the permanent floor, never the first thing to go.
    REQUIRE(stack.undo().has_value());
    CHECK(doc.text == "ab");
    REQUIRE(stack.undo().has_value());
    CHECK(doc.text == "a");

    // A single over-limit step survives on its own: an undoable
    // present beats a faithful bound.
    Doc solo;
    ws::app::UndoStack alone{0, 50};
    REQUIRE(alone.push(std::make_unique<Heavy>(solo, "z", 100)).has_value());
    CHECK(alone.undo_count() == 1);
    CHECK(alone.memory_used() == 100);
    REQUIRE(alone.undo().has_value());
    CHECK(solo.text == "");

    // set_limits enforces the new bounds immediately, oldest first.
    Doc shrinking;
    ws::app::UndoStack shrinkable;
    for (const char chunk : {'a', 'b', 'c'}) {
        REQUIRE(shrinkable
                    .push(std::make_unique<Heavy>(
                        shrinking, std::string{chunk}, 100))
                    .has_value());
    }
    CHECK(shrinkable.memory_used() == 300);
    REQUIRE(shrinkable.set_limits(0, 150).has_value());
    CHECK(shrinkable.undo_count() == 1);
    CHECK(shrinkable.memory_used() == 100);
    CHECK(shrinking.text == "abc");  // only the history shrank
}

TEST_CASE("listeners see every change, in registration order") {
    Doc doc;
    ws::app::UndoStack stack;

    // Both record into one vector: the order they appear in IS the
    // registration order.
    std::vector<std::string> runs;
    const auto first = stack.add_listener([&runs] { runs.push_back("one"); });
    REQUIRE(first.has_value());
    REQUIRE(stack.add_listener([&runs] { runs.push_back("two"); }).has_value());

    // A fire per state-changing call - including a push inside an
    // open transaction, before its step has even landed (memory
    // grew; the redo branch may have dropped). begin() itself and
    // an empty end() change nothing and stay silent.
    REQUIRE(stack.push(std::make_unique<SetField>(doc, "one")).has_value());
    REQUIRE(stack.begin_transaction("Batch").has_value());
    REQUIRE(stack.push(std::make_unique<SetField>(doc, "two")).has_value());
    REQUIRE(stack.end_transaction().has_value());
    REQUIRE(stack.undo().has_value());
    REQUIRE(stack.redo().has_value());
    REQUIRE(stack.mark_saved().has_value());
    REQUIRE(stack.set_limits(10, 0).has_value());
    std::vector<std::string> expected;
    for (int i = 0; i < 7; ++i) {  // push, txn push, end, undo, redo,
        expected.push_back("one");  // mark_saved, set_limits -
        expected.push_back("two");   // begin never fires
    }
    CHECK(runs == expected);

    const std::size_t quiet = runs.size();
    REQUIRE(stack.begin_transaction("No-op").has_value());
    REQUIRE(stack.end_transaction().has_value());
    CHECK(runs.size() == quiet);  // the empty transaction: no fire

    REQUIRE(stack.clear().has_value());
    CHECK(runs.size() == quiet + 2);  // clear is a change

    // Removal answers by id; the empty callback refuses.
    CHECK(stack.remove_listener(*first));
    CHECK_FALSE(stack.remove_listener(*first));  // already out
    CHECK_FALSE(stack.remove_listener(999));
    const auto empty = stack.add_listener(nullptr);
    REQUIRE_FALSE(empty.has_value());
    CHECK(code_of(empty.error()) == "app/1");

    // A removal mid-notify takes effect immediately: the walk
    // liveness-checks each id, so the removed listener never runs
    // even though the snapshot holds it.
    Doc mid_doc;
    ws::app::UndoStack mid;
    std::vector<std::string> mid_runs;
    ws::app::UndoStack::ListenerId doomed = 0;
    REQUIRE(mid.add_listener([&mid_runs] { mid_runs.push_back("1"); })
                .has_value());
    REQUIRE(mid.add_listener([&mid, &doomed, &mid_runs] {
        mid_runs.push_back("2");
        mid.remove_listener(doomed);
    }).has_value());
    const auto third_added =
        mid.add_listener([&mid_runs] { mid_runs.push_back("3"); });
    REQUIRE(third_added.has_value());
    doomed = *third_added;
    REQUIRE(mid.push(std::make_unique<SetField>(mid_doc, "x")).has_value());
    CHECK(mid_runs == std::vector<std::string>{"1", "2"});
    REQUIRE(mid.push(std::make_unique<SetField>(mid_doc, "y")).has_value());
    CHECK(mid_runs == std::vector<std::string>{"1", "2", "1", "2"});

    // An addition mid-notify waits for the next change: the new
    // listener is not in the snapshot.
    Doc late_doc;
    ws::app::UndoStack late;
    std::vector<std::string> late_runs;
    bool installed = false;
    REQUIRE(late.add_listener([&late_runs] { late_runs.push_back("a"); })
                .has_value());
    REQUIRE(late.add_listener([&late, &late_runs, &installed] {
        late_runs.push_back("b");
        if (!installed) {
            installed = true;
            CHECK(late.add_listener([&late_runs] { late_runs.push_back("c"); })
                      .has_value());
        }
    }).has_value());
    REQUIRE(late.push(std::make_unique<SetField>(late_doc, "x")).has_value());
    CHECK(late_runs == std::vector<std::string>{"a", "b"});
    REQUIRE(late.push(std::make_unique<SetField>(late_doc, "y")).has_value());
    CHECK(late_runs == std::vector<std::string>{"a", "b", "a", "b", "c"});
}

TEST_CASE("clear drops the history and the save point") {
    Doc doc;
    ws::app::UndoStack stack;

    REQUIRE(stack.push(std::make_unique<SetField>(doc, "one")).has_value());
    REQUIRE(stack.push(std::make_unique<SetField>(doc, "two")).has_value());
    REQUIRE(stack.mark_saved().has_value());
    CHECK_FALSE(stack.is_dirty());

    // The history goes, the doc keeps its text, and the save
    // position no longer exists: clean needs a fresh mark_saved.
    REQUIRE(stack.clear().has_value());
    CHECK(doc.text == "two");
    CHECK(stack.undo_count() == 0);
    CHECK(stack.redo_count() == 0);
    CHECK(stack.memory_used() == 0);
    CHECK(stack.is_dirty());
    const ws::core::Status nothing = stack.undo();
    REQUIRE_FALSE(nothing.has_value());
    CHECK(code_of(nothing.error()) == "app/2");
    REQUIRE(stack.mark_saved().has_value());
    CHECK_FALSE(stack.is_dirty());
    REQUIRE(stack.push(std::make_unique<SetField>(doc, "three")).has_value());
    CHECK(stack.is_dirty());
}
