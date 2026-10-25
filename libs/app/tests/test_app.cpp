// ws::app's suite: the command registry - the registration refusals
// (empty id/label/callback, the duplicate id), the dispatch round trip
// with the argument payload, the not-found refusal, the enabled/
// checked predicates read live per call, the registration-order
// contract, and the Mod helpers. The shortcut resolver and the undo
// stack grow their own sections as the arc's later phases land.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ws/app/command.hpp>
#include <ws/app/errors.hpp>

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
