// ws::app's suite: the command registry - the registration refusals
// (empty id/label/callback, the duplicate id), the dispatch round trip
// with the argument payload, the not-found refusal, the enabled/
// checked predicates read live per call, the registration-order
// contract, and the Mod helpers; the shortcut resolver - the bind and
// collision rules, the context ladder with the text rule, the two-step
// sequences driven on a fake clock, rebinding, and the defaults seed
// with its whole-or-nothing clash refusal. The undo stack grows its
// own section as the arc's later phases land.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ws/app/command.hpp>
#include <ws/app/errors.hpp>
#include <ws/app/shortcut.hpp>

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
