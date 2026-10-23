// The settings store's suite: the schema, the layered store,
// validation, observers, and the json facility - still headless,
// no files yet (the migration and FileSystem layers grow their own
// cases later).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ws/settings/errors.hpp>
#include <ws/settings/json.hpp>
#include <ws/settings/schema.hpp>
#include <ws/settings/settings.hpp>
#include <ws/settings/value.hpp>

namespace {

using ws::settings::ConstraintAction;
using ws::settings::EnumConstraint;
using ws::settings::Layer;
using ws::settings::MigrationStep;
using ws::settings::Migrations;
using ws::settings::RangeConstraint;
using ws::settings::RegexConstraint;
using ws::settings::Schema;
using ws::settings::SettingChange;
using ws::settings::SettingDesc;
using ws::settings::SettingsError;
using ws::settings::SettingsStore;
using ws::settings::SettingType;
using ws::settings::SettingValue;

using ws::settings::json::Array;
using ws::settings::json::Kind;
using ws::settings::json::Object;
using ws::settings::json::Value;
using ws::settings::json::parse;
using ws::settings::json::write;

// Every error code the suite checks, as the raw int the Error carries.
[[nodiscard]] constexpr std::int32_t code_of(SettingsError error) {
    return static_cast<std::int32_t>(error);
}

// A document that must have parsed, for the happy paths.
[[nodiscard]] Value parsed(std::string_view text) {
    const auto v = parse(text);
    REQUIRE(v);
    return *v;
}

// The error a malformed document must have answered.
[[nodiscard]] ws::core::Error parse_error(std::string_view text) {
    const auto v = parse(text);
    REQUIRE_FALSE(v);
    return v.error();
}

// The ten-key schema every store case builds on: one key per constraint
// flavor and type, two categories for introspection.
Schema demo_schema() {
    Schema schema;

    REQUIRE(schema.declare(SettingDesc{
        .key = "editor.font.size",
        .type = SettingType::integer,
        .default_value = SettingValue::integer(14),
        .constraints = {RangeConstraint{
            .min = 8.0, .max = 72.0, .action = ConstraintAction::clamp}},
        .description = "Editor font size in points.",
        .category = "Editor",
    }));
    REQUIRE(schema.declare(SettingDesc{
        .key = "editor.font.name",
        .type = SettingType::text,
        .default_value = SettingValue::text("Mono"),
        .constraints = {RegexConstraint{.pattern = "^\\w+$"}},
        .description = "Editor font family.",
        .category = "Editor",
    }));
    REQUIRE(schema.declare(SettingDesc{
        .key = "editor.theme",
        .type = SettingType::text,
        .default_value = SettingValue::text("dark"),
        .constraints = {EnumConstraint{
            .allowed = {SettingValue::text("dark"),
                        SettingValue::text("light")}}},
        .description = "Editor color theme.",
        .category = "Editor",
    }));
    REQUIRE(schema.declare(SettingDesc{
        .key = "editor.tab_width",
        .type = SettingType::integer,
        .default_value = SettingValue::integer(4),
        .constraints = {RangeConstraint{.min = 1.0, .max = 8.0}},
        .description = "Spaces per tab.",
        .category = "Editor",
    }));
    REQUIRE(schema.declare(SettingDesc{
        .key = "app.max_recent",
        .type = SettingType::integer,
        .default_value = SettingValue::integer(10),
        .constraints = {RangeConstraint{.min = 0.0, .max = 20.0}},
        .description = "Recent-file list length.",
        .category = "Application",
    }));
    REQUIRE(schema.declare(SettingDesc{
        .key = "app.telemetry",
        .type = SettingType::boolean,
        .default_value = SettingValue::boolean(false),
        .description = "Anonymous usage reporting.",
        .category = "Application",
    }));
    REQUIRE(schema.declare(SettingDesc{
        .key = "app.scale",
        .type = SettingType::real,
        .default_value = SettingValue::real(1.0),
        .constraints = {RangeConstraint{.min = 0.5, .max = 3.0}},
        .description = "UI zoom factor.",
        .category = "Application",
    }));
    REQUIRE(schema.declare(SettingDesc{
        .key = "app.session_limit",
        .type = SettingType::integer,
        .default_value = SettingValue::integer(0),
        .description = "Concurrent sessions allowed (0 = unlimited).",
        .category = "Application",
    }));
    REQUIRE(schema.declare(SettingDesc{
        .key = "audio.volume",
        .type = SettingType::real,
        .default_value = SettingValue::real(0.8),
        .constraints = {RangeConstraint{
            .min = 0.0, .max = 1.0, .action = ConstraintAction::clamp}},
        .description = "Master volume.",
        .category = "Application",
    }));
    REQUIRE(schema.declare(SettingDesc{
        .key = "ui.language",
        .type = SettingType::text,
        .default_value = SettingValue::text("en"),
        .description = "Interface language tag.",
        .category = "Editor",
    }));

    return schema;
}

}  // namespace

TEST_CASE("schema: declare validation") {
    Schema schema;

    SUBCASE("a well-formed declare lands") {
        REQUIRE(schema.declare(SettingDesc{
            .key = "a.good.key",
            .type = SettingType::text,
            .default_value = SettingValue::text("ok"),
        }));
        CHECK(schema.contains("a.good.key"));
        CHECK(schema.size() == 1);
    }
    SUBCASE("empty key") {
        const auto r = schema.declare(SettingDesc{
            .key = "",
            .type = SettingType::text,
            .default_value = SettingValue::text("x"),
        });
        REQUIRE_FALSE(r);
        CHECK(r.error().code == code_of(SettingsError::bad_schema));
    }
    SUBCASE("duplicate key") {
        REQUIRE(schema.declare(SettingDesc{
            .key = "dup",
            .type = SettingType::integer,
            .default_value = SettingValue::integer(1),
        }));
        const auto r = schema.declare(SettingDesc{
            .key = "dup",
            .type = SettingType::integer,
            .default_value = SettingValue::integer(2),
        });
        REQUIRE_FALSE(r);
        CHECK(r.error().code == code_of(SettingsError::bad_schema));
    }
    SUBCASE("default type differs from the declared type") {
        const auto r = schema.declare(SettingDesc{
            .key = "bad.default",
            .type = SettingType::integer,
            .default_value = SettingValue::text("14"),
        });
        REQUIRE_FALSE(r);
        CHECK(r.error().code == code_of(SettingsError::bad_schema));
    }
    SUBCASE("range on a bool key") {
        const auto r = schema.declare(SettingDesc{
            .key = "bad.bool_range",
            .type = SettingType::boolean,
            .default_value = SettingValue::boolean(true),
            .constraints = {RangeConstraint{.min = 0.0, .max = 1.0}},
        });
        REQUIRE_FALSE(r);
        CHECK(r.error().code == code_of(SettingsError::bad_schema));
    }
    SUBCASE("range on a text key") {
        const auto r = schema.declare(SettingDesc{
            .key = "bad.text_range",
            .type = SettingType::text,
            .default_value = SettingValue::text("x"),
            .constraints = {RangeConstraint{.min = 0.0, .max = 9.0}},
        });
        REQUIRE_FALSE(r);
        CHECK(r.error().code == code_of(SettingsError::bad_schema));
    }
    SUBCASE("inside-out range") {
        const auto r = schema.declare(SettingDesc{
            .key = "bad.range",
            .type = SettingType::integer,
            .default_value = SettingValue::integer(5),
            .constraints = {RangeConstraint{.min = 10.0, .max = 2.0}},
        });
        REQUIRE_FALSE(r);
        CHECK(r.error().code == code_of(SettingsError::bad_schema));
    }
    SUBCASE("enum with a wrong-typed entry") {
        const auto r = schema.declare(SettingDesc{
            .key = "bad.enum",
            .type = SettingType::text,
            .default_value = SettingValue::text("dark"),
            .constraints = {EnumConstraint{
                .allowed = {SettingValue::text("dark"),
                            SettingValue::integer(2)}}},
        });
        REQUIRE_FALSE(r);
        CHECK(r.error().code == code_of(SettingsError::bad_schema));
    }
    SUBCASE("empty enum") {
        const auto r = schema.declare(SettingDesc{
            .key = "bad.empty_enum",
            .type = SettingType::text,
            .default_value = SettingValue::text("dark"),
            .constraints = {EnumConstraint{}},
        });
        REQUIRE_FALSE(r);
        CHECK(r.error().code == code_of(SettingsError::bad_schema));
    }
    SUBCASE("regex on an integer key") {
        const auto r = schema.declare(SettingDesc{
            .key = "bad.regex_type",
            .type = SettingType::integer,
            .default_value = SettingValue::integer(1),
            .constraints = {RegexConstraint{.pattern = "^\\d+$"}},
        });
        REQUIRE_FALSE(r);
        CHECK(r.error().code == code_of(SettingsError::bad_schema));
    }
    SUBCASE("unparsable regex") {
        const auto r = schema.declare(SettingDesc{
            .key = "bad.regex",
            .type = SettingType::text,
            .default_value = SettingValue::text("x"),
            .constraints = {RegexConstraint{.pattern = "[unclosed"}},
        });
        REQUIRE_FALSE(r);
        CHECK(r.error().code == code_of(SettingsError::bad_schema));
    }
}

TEST_CASE("store: the schema's defaults, served with provenance") {
    const SettingsStore store{demo_schema()};

    const auto font = store.get<std::int64_t>("editor.font.size");
    REQUIRE(font);
    CHECK(font->value == 14);
    CHECK(font->layer == Layer::defaults);

    const auto name = store.get<std::string>("editor.font.name");
    REQUIRE(name);
    CHECK(name->value == "Mono");
    CHECK(name->layer == Layer::defaults);

    const auto telemetry = store.get<bool>("app.telemetry");
    REQUIRE(telemetry);
    CHECK(telemetry->value == false);
    CHECK(telemetry->layer == Layer::defaults);

    const auto volume = store.get<double>("audio.volume");
    REQUIRE(volume);
    CHECK(volume->value == doctest::Approx(0.8));
    CHECK(volume->layer == Layer::defaults);

    const auto layer = store.layer_of("app.scale");
    REQUIRE(layer);
    CHECK(*layer == Layer::defaults);
}

TEST_CASE("store: layer precedence walks rank, clear falls back down") {
    SettingsStore store{demo_schema()};

    REQUIRE(store.set(Layer::system, "editor.font.size", 20));
    REQUIRE(store.set(Layer::user, "editor.font.size", 28));
    REQUIRE(store.set(Layer::project, "editor.font.size", 32));
    REQUIRE(store.set(Layer::command_line, "editor.font.size", 40));

    const auto font = store.get<std::int64_t>("editor.font.size");
    REQUIRE(font);
    CHECK(font->value == 40);
    CHECK(font->layer == Layer::command_line);
    const auto layer = store.layer_of("editor.font.size");
    REQUIRE(layer);
    CHECK(*layer == Layer::command_line);
    CHECK(store.has_value(Layer::project, "editor.font.size"));
    CHECK(store.has_value(Layer::user, "editor.font.size"));

    REQUIRE(store.clear(Layer::command_line, "editor.font.size"));
    CHECK(*store.layer_of("editor.font.size") == Layer::project);
    CHECK(store.get<std::int64_t>("editor.font.size")->value == 32);

    REQUIRE(store.clear(Layer::project, "editor.font.size"));
    CHECK(*store.layer_of("editor.font.size") == Layer::user);

    REQUIRE(store.clear(Layer::user, "editor.font.size"));
    CHECK(*store.layer_of("editor.font.size") == Layer::system);
    CHECK(store.get<std::int64_t>("editor.font.size")->value == 20);

    REQUIRE(store.clear(Layer::system, "editor.font.size"));
    CHECK(*store.layer_of("editor.font.size") == Layer::defaults);
    CHECK(store.get<std::int64_t>("editor.font.size")->value == 14);

    // Nothing left in any explicit slot.
    CHECK_FALSE(store.clear(Layer::system, "editor.font.size"));
    CHECK_FALSE(store.has_value(Layer::system, "editor.font.size"));
}

TEST_CASE("store: the defaults layer takes no explicit values") {
    SettingsStore store{demo_schema()};

    const auto r = store.set(Layer::defaults, "editor.font.size", 20);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == code_of(SettingsError::invalid_argument));
    CHECK(store.get<std::int64_t>("editor.font.size")->value == 14);
}

TEST_CASE("store: unknown keys refuse everywhere") {
    SettingsStore store{demo_schema()};

    const auto got = store.get<std::int64_t>("editor.font.weight");
    REQUIRE_FALSE(got);
    CHECK(got.error().code == code_of(SettingsError::unknown_key));

    const auto set = store.set(Layer::user, "editor.font.weight", 3);
    REQUIRE_FALSE(set);
    CHECK(set.error().code == code_of(SettingsError::unknown_key));

    const auto layer = store.layer_of("editor.font.weight");
    REQUIRE_FALSE(layer);
    CHECK(layer.error().code == code_of(SettingsError::unknown_key));

    // clear() stays bool: an unknown key simply has nothing to drop.
    CHECK_FALSE(store.clear(Layer::user, "editor.font.weight"));
}

TEST_CASE("store: strict type matching, both directions") {
    SettingsStore store{demo_schema()};

    // Write side: no silent conversions, whatever set() accepted.
    CHECK_FALSE(store.set(Layer::user, "app.telemetry", 1));
    CHECK_FALSE(store.set(Layer::user, "app.telemetry", "true"));
    CHECK_FALSE(store.set(Layer::user, "app.scale", 3));
    CHECK_FALSE(store.set(Layer::user, "editor.font.size", 2.5));
    CHECK_FALSE(store.set(Layer::user, "editor.font.size", "14"));
    CHECK_FALSE(store.set(Layer::user, "editor.font.size", true));

    const auto mixed = store.set(Layer::user, "editor.font.size", 2.5);
    REQUIRE_FALSE(mixed);
    CHECK(mixed.error().code == code_of(SettingsError::type_mismatch));

    // A refused write stores nothing.
    CHECK_FALSE(store.has_value(Layer::user, "editor.font.size"));

    // Read side: the getter's T must be the key's own type.
    CHECK_FALSE(store.get<std::string>("editor.font.size"));
    CHECK_FALSE(store.get<double>("editor.font.size"));
    CHECK_FALSE(store.get<bool>("editor.theme"));
    CHECK_FALSE(store.get<std::int64_t>("audio.volume"));
    const auto read = store.get<std::int64_t>("audio.volume");
    REQUIRE_FALSE(read);
    CHECK(read.error().code == code_of(SettingsError::type_mismatch));
}

TEST_CASE("store: range constraints reject or clamp") {
    SettingsStore store{demo_schema()};

    // reject: the write answers, nothing lands, the default holds.
    CHECK_FALSE(store.set(Layer::user, "editor.tab_width", 0));
    CHECK_FALSE(store.set(Layer::user, "editor.tab_width", 9));
    CHECK_FALSE(store.set(Layer::user, "app.scale", 4.0));
    CHECK_FALSE(store.set(Layer::user, "app.scale", 0.25));
    const auto refused = store.set(Layer::user, "app.scale", 4.0);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == code_of(SettingsError::constraint_rejected));
    CHECK(store.get<std::int64_t>("editor.tab_width")->value == 4);
    CHECK(store.get<std::int64_t>("editor.tab_width")->layer ==
          Layer::defaults);
    CHECK_FALSE(store.has_value(Layer::user, "app.scale"));

    // clamp: the nearest boundary lands.
    REQUIRE(store.set(Layer::user, "editor.font.size", 100));
    CHECK(store.get<std::int64_t>("editor.font.size")->value == 72);
    REQUIRE(store.set(Layer::user, "editor.font.size", -5));
    CHECK(store.get<std::int64_t>("editor.font.size")->value == 8);
    REQUIRE(store.set(Layer::user, "audio.volume", 1.5));
    CHECK(store.get<double>("audio.volume")->value == doctest::Approx(1.0));
    REQUIRE(store.set(Layer::user, "audio.volume", -0.2));
    CHECK(store.get<double>("audio.volume")->value == doctest::Approx(0.0));
}

TEST_CASE("store: enum constraints accept or reject") {
    SettingsStore store{demo_schema()};

    const auto r = store.set(Layer::user, "editor.theme", "solarized");
    REQUIRE_FALSE(r);
    CHECK(r.error().code == code_of(SettingsError::constraint_rejected));
    CHECK_FALSE(store.has_value(Layer::user, "editor.theme"));

    REQUIRE(store.set(Layer::user, "editor.theme", "light"));
    CHECK(store.get<std::string>("editor.theme")->value == "light");
}

TEST_CASE("store: regex constraints gate text") {
    SettingsStore store{demo_schema()};

    const auto r = store.set(Layer::user, "editor.font.name", "Roboto Sans");
    REQUIRE_FALSE(r);
    CHECK(r.error().code == code_of(SettingsError::constraint_rejected));

    REQUIRE(store.set(Layer::user, "editor.font.name", "Roboto_Sans"));
    CHECK(store.get<std::string>("editor.font.name")->value == "Roboto_Sans");

    // ui.language carries no constraint: any text lands.
    REQUIRE(store.set(Layer::user, "ui.language", "pt-BR, com acentos"));
    CHECK(store.get<std::string>("ui.language")->value == "pt-BR, com acentos");
}

TEST_CASE("store: every supported type round-trips") {
    SettingsStore store{demo_schema()};

    REQUIRE(store.set(Layer::user, "app.telemetry", true));
    CHECK(store.get<bool>("app.telemetry")->value == true);

    REQUIRE(store.set(Layer::user, "app.telemetry", false));
    CHECK(store.get<bool>("app.telemetry")->value == false);

    // The int64 edges, through the unconstrained integer key.
    REQUIRE(store.set(Layer::user, "app.session_limit",
                      std::numeric_limits<std::int64_t>::min()));
    CHECK(store.get<std::int64_t>("app.session_limit")->value ==
          std::numeric_limits<std::int64_t>::min());
    REQUIRE(store.set(Layer::user, "app.session_limit",
                      std::numeric_limits<std::int64_t>::max()));
    CHECK(store.get<std::int64_t>("app.session_limit")->value ==
          std::numeric_limits<std::int64_t>::max());

    REQUIRE(store.set(Layer::user, "audio.volume", 0.25));
    CHECK(store.get<double>("audio.volume")->value == doctest::Approx(0.25));

    const std::string text_with_spaces{"hello world\ttab \"quoted\""};
    REQUIRE(store.set(Layer::user, "ui.language", text_with_spaces));
    CHECK(store.get<std::string>("ui.language")->value == text_with_spaces);

    // set also accepts the exact-width types via the integral branch.
    REQUIRE(store.set(Layer::user, "editor.tab_width", 4));
    CHECK(store.get<std::int64_t>("editor.tab_width")->value == 4);
}

TEST_CASE("observers: patterns, order, payload, equal-value sets") {
    SettingsStore store{demo_schema()};
    std::vector<std::string> hits;

    (void)store.observe("editor.font.size", [&](const SettingChange& c) {
        hits.push_back("exact");
        // The payload carries the STORED value - the clamped one, not
        // what was passed to set().
        CHECK(c.value.as_integer().value_or(-1) == 72);
        CHECK(c.layer == Layer::user);
    });
    const auto prefix_id = store.observe("editor.", [&](const SettingChange&) {
        hits.push_back("prefix");
    });
    (void)store.observe("audio.volume", [&](const SettingChange&) {
        hits.push_back("audio");
    });

    // 100 clamps to 72; both the exact observer and the prefix see it,
    // in registration order.
    REQUIRE(store.set(Layer::user, "editor.font.size", 100));
    REQUIRE(hits.size() == 2);
    CHECK(hits[0] == "exact");
    CHECK(hits[1] == "prefix");
    hits.clear();

    // A set on a key only one observer matches runs just that one.
    REQUIRE(store.set(Layer::user, "audio.volume", 0.5));
    REQUIRE(hits.size() == 1);
    CHECK(hits[0] == "audio");
    hits.clear();

    // Removal stops delivery.
    store.remove_observer(prefix_id);
    REQUIRE(store.set(Layer::user, "editor.theme", "light"));
    CHECK(hits.empty());

    // A set that lands the same value the key already answers still
    // notifies: every successful write is a change event.
    (void)store.observe("editor.theme", [&](const SettingChange&) {
        hits.push_back("theme");
    });
    REQUIRE(store.set(Layer::user, "editor.theme", "dark"));
    REQUIRE(hits.size() == 1);
    CHECK(hits[0] == "theme");
}

TEST_CASE("observers: removal and registration mid-notification") {
    SettingsStore store{demo_schema()};
    std::vector<std::string> log;

    // A removes B before B's turn; C registers D from inside the round.
    auto b_id = std::make_shared<ws::settings::ObserverId>(0);
    (void)store.observe("editor.", [&](const SettingChange&) {
        log.push_back("a");
        store.remove_observer(*b_id);
    });
    *b_id = store.observe("editor.", [&](const SettingChange&) {
        log.push_back("b");
    });
    (void)store.observe("editor.", [&](const SettingChange&) {
        log.push_back("c");
        (void)store.observe("editor.", [&](const SettingChange&) {
            log.push_back("d");
        });
    });

    REQUIRE(store.set(Layer::user, "editor.font.size", 16));
    REQUIRE(log.size() == 2);
    CHECK(log[0] == "a");  // ran, removed B
    CHECK(log[1] == "c"); // B skipped live, D not in the snapshot yet

    log.clear();
    REQUIRE(store.set(Layer::user, "editor.font.size", 17));
    REQUIRE(log.size() == 3);
    CHECK(log[0] == "a");
    CHECK(log[1] == "c");
    CHECK(log[2] == "d"); // D runs from the next change onward

    // An observer removing itself still runs this round, gone the next.
    log.clear();
    auto self_id = std::make_shared<ws::settings::ObserverId>(0);
    *self_id = store.observe("audio.volume", [&](const SettingChange&) {
        log.push_back("self");
        store.remove_observer(*self_id);
    });
    REQUIRE(store.set(Layer::user, "audio.volume", 0.4));
    REQUIRE(log.size() == 1);
    CHECK(log[0] == "self");
    log.clear();
    REQUIRE(store.set(Layer::user, "audio.volume", 0.5));
    CHECK(log.empty());
}

TEST_CASE("observers: a re-entrant set runs the other key's observers") {
    SettingsStore store{demo_schema()};
    std::vector<std::string> order;

    (void)store.observe("editor.font.size", [&](const SettingChange&) {
        order.push_back("font.size");
        REQUIRE(store.set(Layer::user, "audio.volume", 0.3));
    });
    (void)store.observe("audio.volume", [&](const SettingChange&) {
        order.push_back("volume");
    });

    REQUIRE(store.set(Layer::user, "editor.font.size", 18));
    REQUIRE(order.size() == 2);
    CHECK(order[0] == "font.size");
    CHECK(order[1] == "volume");  // ran recursively inside the font set
    CHECK(store.get<double>("audio.volume")->value == doctest::Approx(0.3));
}

TEST_CASE("schema: introspection") {
    const Schema schema = demo_schema();

    CHECK(schema.size() == 10);
    CHECK(schema.contains("editor.theme"));
    CHECK_FALSE(schema.contains("editor.color"));

    const SettingDesc* font = schema.find("editor.font.size");
    REQUIRE(font != nullptr);
    CHECK(font->category == "Editor");
    CHECK(font->description == "Editor font size in points.");
    CHECK(schema.find("editor.font.weight") == nullptr);

    const auto categories = schema.categories();
    REQUIRE(categories.size() == 2);
    CHECK(categories[0] == "Editor");        // first-declared order
    CHECK(categories[1] == "Application");

    const auto editor = schema.in_category("Editor");
    REQUIRE(editor.size() == 5);
    CHECK(editor[0]->key == "editor.font.size");
    CHECK(editor[1]->key == "editor.font.name");
    CHECK(editor[2]->key == "editor.theme");
    CHECK(editor[3]->key == "editor.tab_width");
    CHECK(editor[4]->key == "ui.language");

    const auto& descs = schema.descriptors();
    REQUIRE(descs.size() == 10);
    CHECK(descs[0].key == "editor.font.size");
}

TEST_CASE("store: clear and has_value edge behavior") {
    SettingsStore store{demo_schema()};

    CHECK_FALSE(store.clear(Layer::user, "editor.font.size"));
    CHECK_FALSE(store.has_value(Layer::user, "editor.font.size"));
    CHECK_FALSE(store.has_value(Layer::defaults, "editor.font.size"));

    REQUIRE(store.set(Layer::user, "editor.font.size", 20));
    CHECK(store.has_value(Layer::user, "editor.font.size"));
    REQUIRE(store.clear(Layer::user, "editor.font.size"));
    CHECK_FALSE(store.has_value(Layer::user, "editor.font.size"));
    CHECK(*store.layer_of("editor.font.size") == Layer::defaults);
}

TEST_CASE("value: SettingValue typed getters and to_text") {
    const auto v = SettingValue::integer(7);
    CHECK(v.type() == SettingType::integer);
    CHECK(v.as_integer().value_or(0) == 7);
    const auto wrong = v.as_bool();
    REQUIRE_FALSE(wrong);
    CHECK(wrong.error().code == code_of(SettingsError::type_mismatch));
    CHECK(v == SettingValue::integer(7));
    CHECK(v != SettingValue::integer(8));

    CHECK(SettingValue::boolean(true).to_text() == "true");
    CHECK(SettingValue::boolean(false).to_text() == "false");
    CHECK(SettingValue::integer(-3).to_text() == "-3");
    CHECK(SettingValue::real(1.5).to_text() == "1.5");
    CHECK(SettingValue::real(0.1).to_text() == "0.1");
    CHECK(SettingValue::text("hi").to_text() == "\"hi\"");

    CHECK(SettingValue::text("same").type() == SettingType::text);
    CHECK(SettingValue::text(std::string{"owned"}).as_text().value_or("") ==
          "owned");
}

// ---- json -------------------------------------------------------------

TEST_CASE("json: scalars parse") {
    CHECK(parsed("null").is_null());
    CHECK(parsed("true").get_bool().value_or(false));
    CHECK_FALSE(parsed("false").get_bool().value_or(true));
    CHECK(parsed("42").get_integer().value_or(0) == 42);
    CHECK(parsed("-7").get_integer().value_or(0) == -7);
    CHECK(parsed(std::to_string(std::numeric_limits<std::int64_t>::min()))
              .get_integer()
              .value_or(0) == std::numeric_limits<std::int64_t>::min());
    CHECK(parsed(std::to_string(std::numeric_limits<std::int64_t>::max()))
              .get_integer()
              .value_or(0) == std::numeric_limits<std::int64_t>::max());
    CHECK(parsed("1.5").get_real().value_or(0.0) == 1.5);
    CHECK(parsed("-0.25").get_real().value_or(0.0) == -0.25);
    CHECK(parsed("2.5e-3").get_real().value_or(0.0) == 2.5e-3);
    CHECK(parsed("1E+2").get_real().value_or(0.0) == 100.0);
    CHECK(parsed("-1e3").get_real().value_or(0.0) == -1000.0);
    CHECK(parsed("-0").is_integer());

    // Whitespace everywhere it is legal.
    CHECK(parsed(" \t\r\n42 \n").get_integer().value_or(0) == 42);

    CHECK(parsed("true").kind() == Kind::boolean);
    CHECK(parsed("42").kind() == Kind::integer);
    CHECK(parsed("4.5").kind() == Kind::real);
    CHECK(parsed("\"x\"").kind() == Kind::text);
}

TEST_CASE("json: strings and escapes") {
    CHECK(parsed("\"\"").get_text().value_or("") == "");
    CHECK(parsed(R"( "\/\b\f\n\r\t\" " )").get_text().value_or("") ==
          "/\b\f\n\r\t\" ");
    CHECK(parsed(R"( "a\\b" )").get_text().value_or("") == "a\\b");
    CHECK(parsed(R"( "A" )").get_text().value_or("") == "A");
    CHECK(parsed(R"( "é" )").get_text().value_or("") == "\xc3\xa9");
    CHECK(parsed(R"( "😀" )").get_text().value_or("") ==
          "\xf0\x9f\x98\x80");
    CHECK(parsed("\"caf\xc3\xa9\"").get_text().value_or("") == "caf\xc3\xa9");
}

TEST_CASE("json: arrays, objects, nesting") {
    const Value v =
        parsed(R"( {"a": [1, 2.5, "x", null, true], "b": {"c": {"d": [false]}}} )");
    REQUIRE(v.is_object());
    const Object& root = *v.as_object();
    CHECK(root.size() == 2);

    const Value* a = root.find("a");
    REQUIRE(a != nullptr);
    REQUIRE(a->is_array());
    CHECK(a->as_array()->size() == 5);
    CHECK((*a->as_array())[0].get_integer().value_or(0) == 1);
    CHECK((*a->as_array())[1].get_real().value_or(0.0) == 2.5);
    CHECK((*a->as_array())[2].get_text().value_or("") == "x");
    CHECK((*a->as_array())[3].is_null());
    CHECK((*a->as_array())[4].get_bool().value_or(false));

    const Value* b = root.find("b");
    REQUIRE(b != nullptr);
    const Value* c = b->as_object()->find("c");
    REQUIRE(c != nullptr);
    const Value* d = c->as_object()->find("d");
    REQUIRE(d != nullptr);
    REQUIRE(d->is_array());
    CHECK(d->as_array()->size() == 1);
    CHECK_FALSE(d->as_array()->front().get_bool().value_or(true));

    CHECK(parsed("[]").as_array() != nullptr);
    CHECK(parsed("[]").as_array()->empty());
    CHECK(parsed("{}").as_object() != nullptr);
    CHECK(parsed("{}").as_object()->empty());
}

TEST_CASE("json: the depth cap") {
    // A value inside 128 arrays is at the limit; inside 129 it is
    // refused. (The innermost needs a value - an empty array never
    // asks for one, so a bare "[[[" ... "]]]" never reaches the cap.)
    const std::string at_limit =
        std::string(128, '[') + "0" + std::string(128, ']');
    CHECK(parse(at_limit));

    const std::string past =
        std::string(129, '[') + "0" + std::string(129, ']');
    const auto r = parse(past);
    REQUIRE_FALSE(r);
    CHECK(r.error().code == code_of(SettingsError::parse_failed));
    CHECK(r.error().message.find("nesting deeper than 128") !=
          std::string::npos);
}

TEST_CASE("json: comments and a leading BOM") {
    const Value v = parsed(
        R"( // the one true config
        {
            /* the block form */ "a": 1, "b": /* inline */ 2
        }
        // trailing, after the document
        )");
    REQUIRE(v.is_object());
    const Object& obj = *v.as_object();
    CHECK(obj.size() == 2);
    CHECK(obj.find("a")->get_integer().value_or(0) == 1);
    CHECK(obj.find("b")->get_integer().value_or(0) == 2);

    CHECK(parsed("\xEF\xBB\xBF 42").get_integer().value_or(0) == 42);

    const auto unterminated = parse_error(R"( /* never closed )");
    CHECK(unterminated.code == code_of(SettingsError::parse_failed));
    CHECK(unterminated.message.find("unterminated block comment") !=
          std::string::npos);
}

TEST_CASE("json: malformed inputs answer parse_failed") {
    const std::pair<std::string_view, std::string_view> bad[] = {
        {"", "unexpected end of input"},
        {"   ", "unexpected end of input"},
        {"{", "expected a key"},
        {R"({"a")", "expected ':'"},
        {R"({"a":)", "unexpected end of input"},
        {R"({"a" 1})", "expected ':'"},
        {R"({"a":1 "b":2})", "expected ',' or '}'"},
        {R"({"a":1,})", "expected a key"},
        {R"({"a":1])", "expected ',' or '}'"},
        {"[1,]", "unexpected character"},
        {"[1 2]", "expected ',' or ']'"},
        {"tru", "invalid literal"},
        {"nul", "invalid literal"},
        {"falsx", "invalid literal"},
        {"01", "invalid number"},
        {"1.", "invalid number"},
        {"-", "unexpected end of input"},
        {"1e", "invalid number"},
        {"1e+", "invalid number"},
        {"1.2.3", "invalid number"},
        {"9223372036854775808", "integer literal out of range"},
        {"1e999", "invalid number"},
        {R"("unterminated)", "unterminated string"},
        {R"("bad \x escape")", "invalid escape"},
        {R"("\u00")", "invalid \\u escape"},
        {R"("\ud83d")", "unpaired high surrogate"},
        {R"("\udc00")", "lone low surrogate"},
        {R"("\ud83dX")", "unpaired high surrogate"},
        {R"({} trailing)", "trailing characters"},
        {R"(/* no value */)", "unexpected end of input"},
    };
    for (const auto& [document, want] : bad) {
        CAPTURE(document);
        const auto e = parse_error(document);
        CHECK(e.code == code_of(SettingsError::parse_failed));
        CHECK(e.message.find(want) != std::string::npos);
    }

    // A raw newline inside a string is reported on the line it sits on.
    const auto nl = parse_error("{\n \"a\": \"b\nc\"\n}");
    CAPTURE(nl.message);
    CHECK(nl.message.find("line 2") != std::string::npos);

    // Every parse failure carries its position.
    CHECK(parse_error("x").message.find("at line 1, column 1") !=
          std::string::npos);
}

TEST_CASE("json: write is readable and escapes") {
    Object theme;
    theme.set("editor.theme", Value::text("dark"));
    theme.set("editor.font.size", Value::integer(14));

    Array list;
    list.push_back(Value::integer(1));
    list.push_back(Value::text("two"));
    list.push_back(Value::boolean(false));

    Object root;
    root.set("theme", Value::object(std::move(theme)));
    root.set("list", Value::array(std::move(list)));
    root.set("empty", Value::object(Object{}));
    root.set("scale", Value::real(1.5));

    const std::string expected =
        "{\n"
        "    \"theme\": {\n"
        "        \"editor.theme\": \"dark\",\n"
        "        \"editor.font.size\": 14\n"
        "    },\n"
        "    \"list\": [\n"
        "        1,\n"
        "        \"two\",\n"
        "        false\n"
        "    ],\n"
        "    \"empty\": {},\n"
        "    \"scale\": 1.5\n"
        "}\n";
    CHECK(write(Value::object(std::move(root))) == expected);

    CHECK(write(Value::null()) == "null\n");
    CHECK(write(Value::array()) == "[]\n");
    CHECK(write(Value::boolean(true)) == "true\n");
    CHECK(write(Value::integer(-3)) == "-3\n");
    CHECK(write(Value::real(0.5)) == "0.5\n");
    CHECK(write(Value::real(1200.0)) == "1200.0\n");
    CHECK(write(Value::real(-0.0)) == "-0.0\n");
    CHECK(write(Value::real(2.5e-3)) == "0.0025\n");
    CHECK(write(Value::real(1e300)) == "1e+300\n");

    CHECK(write(Value::text("a\"b\\c\nd\te")) == "\"a\\\"b\\\\c\\nd\\te\"\n");
    const std::string control = std::string{'x', '\x01', 'y'};
    CHECK(write(Value::text(control)) == "\"x\\u0001y\"\n");

    // A non-finite real has no json form; the writer refuses to guess.
    CHECK(write(Value::real(std::nan(""))) == "null\n");
    CHECK(write(Value::real(HUGE_VAL)) == "null\n");
}

TEST_CASE("json: parse -> write -> parse stability") {
    const char* source = R"( // hand-maintained
{
    "editor.theme": "dark",  /* the one that ships */
    "editor.font.name": "Café Mono",
    "scale": 1.25,
    "limits": {
        "low": -2.5e-3,
        "high": 1e+2
    },
    "emoji": "😀",
    "list": [1, 2.0, "three", null, true, []]
}
)";
    const Value first = parsed(source);
    const std::string written = write(first);
    const Value second = parsed(written);
    CHECK(second == first);

    // The written form is the readable shape: comments are gone, one
    // entry per line, 4-space steps.
    CHECK(written.find("//") == std::string::npos);
    CHECK(written.find("/*") == std::string::npos);
}

TEST_CASE("json: insertion order, duplicates, and Object::set") {
    const Value v = parsed(R"( {"z": 1, "a": 2, "m": 3} )");
    const Object& obj = *v.as_object();
    REQUIRE(obj.size() == 3);
    CHECK(obj.begin()->first == "z");
    CHECK(std::next(obj.begin())->first == "a");
    CHECK(std::next(obj.begin(), 2)->first == "m");

    // A duplicate key keeps the FIRST position and takes the LAST
    // value: hand-edited files override at the bottom.
    const Value dup = parsed(R"( {"a": 1, "b": 9, "a": 2} )");
    const Object& d = *dup.as_object();
    REQUIRE(d.size() == 2);
    CHECK(d.begin()->first == "a");
    CHECK(d.begin()->second.get_integer().value_or(0) == 2);
    CHECK(d.find("b")->get_integer().value_or(0) == 9);

    // set() overwrites in place: position kept, value replaced.
    Object hand;
    hand.set("k", Value::integer(1));
    hand.set("k", Value::integer(5));
    REQUIRE(hand.size() == 1);
    CHECK(hand.begin()->second.get_integer().value_or(0) == 5);

    CHECK(obj == *v.as_object());
    CHECK(obj.find("missing") == nullptr);
}

TEST_CASE("json: integer and real stay apart; typed pulls answer Result") {
    CHECK(parsed("3").is_integer());
    CHECK(parsed("3.0").is_real());
    CHECK(parsed("3e0").is_real());

    CHECK(write(parsed("3")) == "3\n");
    CHECK(write(parsed("3.0")) == "3.0\n");

    // A round trip never re-types a value.
    CHECK(parse(write(parsed("3")))->is_integer());
    CHECK(parse(write(parsed("-12e2")))->is_real());
    const Value weird = parsed("-12e2");
    CHECK(parse(write(weird))->get_real().value_or(0.0) ==
          weird.get_real().value_or(0.0));

    const auto wrong = parsed("3").get_text();
    REQUIRE_FALSE(wrong);
    CHECK(wrong.error().code == code_of(SettingsError::type_mismatch));

    CHECK(parsed("3").as_array() == nullptr);
    CHECK(parsed("[1]").as_array() != nullptr);
    CHECK(parsed("3").as_object() == nullptr);
    CHECK(parsed("{}").as_object() != nullptr);
}

// ---- the shape version + the migration chain ---------------------------

TEST_CASE("schema: the shape version and the reserved key") {
    Schema schema;
    CHECK(schema.version() == 1);
    REQUIRE(schema.set_version(3));
    CHECK(schema.version() == 3);

    const auto low = schema.set_version(0);
    REQUIRE_FALSE(low);
    CHECK(low.error().code == code_of(SettingsError::invalid_argument));
    CHECK(schema.version() == 3);

    const auto reserved = schema.declare(SettingDesc{
        .key = "version",
        .type = SettingType::integer,
        .default_value = SettingValue::integer(1),
    });
    REQUIRE_FALSE(reserved);
    CHECK(reserved.error().code == code_of(SettingsError::bad_schema));
}

TEST_CASE("migrations: the chain walks step by step") {
    Migrations chain;
    std::vector<std::string> walked;
    REQUIRE(chain.add(1, [&](Value& doc) {
        walked.push_back("1->2");
        // v2 renamed editor.size to editor.font.size.
        Object* root = doc.as_object();
        if (const Value* old = root->find("editor.size")) {
            const auto size = old->get_integer();
            root->erase("editor.size");
            root->set("editor.font.size",
                      Value::integer(size.value_or(14)));
        }
        return ws::core::Status{};
    }));
    REQUIRE(chain.add(2, [&](Value& doc) {
        walked.push_back("2->3");
        Object* root = doc.as_object();
        if (const Value* size = root->find("editor.font.size")) {
            root->set("editor.font.size",
                      Value::integer(size->get_integer().value_or(0) * 2));
        }
        return ws::core::Status{};
    }));

    // v1 -> v3: both steps, in order, then the version stamped.
    Value doc = parsed(R"( {"version": 1, "editor.size": 11} )");
    REQUIRE(chain.run(doc, 3));
    CHECK(walked == std::vector<std::string>{"1->2", "2->3"});
    CHECK(doc.as_object()->find("version")->get_integer().value_or(0) == 3);
    CHECK(doc.as_object()->find("editor.size") == nullptr);
    CHECK(doc.as_object()->find("editor.font.size")->get_integer()
              .value_or(0) == 22);

    // Already current: the fast path runs nothing.
    walked.clear();
    Value fresh = parsed(R"( {"version": 3, "editor.font.size": 8} )");
    REQUIRE(chain.run(fresh, 3));
    CHECK(walked.empty());
    CHECK(fresh.as_object()->find("editor.font.size")->get_integer()
              .value_or(0) == 8);

    // A missing step names the gap.
    Migrations short_chain;
    REQUIRE(short_chain.add(1, [](Value&) { return ws::core::Status{}; }));
    Value gap = parsed(R"( {"version": 1} )");
    const auto missing = short_chain.run(gap, 3);
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == code_of(SettingsError::migrate_failed));
    CHECK(missing.error().message.find("from version 2") !=
          std::string::npos);

    // A file from the future is refused, not downgraded.
    Value future = parsed(R"( {"version": 9} )");
    const auto newer = chain.run(future, 3);
    REQUIRE_FALSE(newer);
    CHECK(newer.error().code == code_of(SettingsError::migrate_failed));
    CHECK(newer.error().message.find("newer") != std::string::npos);

    // A pre-versioning file reads as shape 1 and walks the chain.
    walked.clear();
    Value pre = parsed(R"( {"editor.size": 5} )");
    REQUIRE(chain.run(pre, 3));
    CHECK(walked == std::vector<std::string>{"1->2", "2->3"});
    CHECK(pre.as_object()->find("version")->get_integer().value_or(0) == 3);

    // The chain refuses ambiguity and nonsense at add().
    const auto duplicate =
        chain.add(2, [](Value&) { return ws::core::Status{}; });
    REQUIRE_FALSE(duplicate);
    CHECK(duplicate.error().code == code_of(SettingsError::invalid_argument));
    Migrations empty_chain;
    CHECK_FALSE(empty_chain.add(0, [](Value&) { return ws::core::Status{}; }));
    CHECK_FALSE(empty_chain.add(1, MigrationStep{}));

    // A non-object root and a non-integer version answer corrupt_file.
    Value list = parsed("[1]");
    const auto not_object = chain.run(list, 3);
    REQUIRE_FALSE(not_object);
    CHECK(not_object.error().code == code_of(SettingsError::corrupt_file));

    Value wordy = parsed(R"( {"version": "two"} )");
    const auto bad_version = chain.run(wordy, 3);
    REQUIRE_FALSE(bad_version);
    CHECK(bad_version.error().code == code_of(SettingsError::corrupt_file));

    // A step's own failure is the walk's failure, message intact.
    Migrations failing;
    REQUIRE(failing.add(1, [](Value&) {
        return ws::core::Status{
            fail(SettingsError::migrate_failed, "the step said no")};
    }));
    Value victim = parsed(R"( {"version": 1} )");
    const auto step_failed = failing.run(victim, 2);
    REQUIRE_FALSE(step_failed);
    CHECK(step_failed.error().message.find("the step said no") !=
          std::string::npos);

    // A step that rebuilds the root as a non-object answers corrupt_file.
    Migrations rebuilding;
    REQUIRE(rebuilding.add(1, [](Value& doc) {
        doc = Value::array();
        return ws::core::Status{};
    }));
    Value wrecked = parsed(R"( {"version": 1} )");
    const auto not_root = rebuilding.run(wrecked, 2);
    REQUIRE_FALSE(not_root);
    CHECK(not_root.error().code == code_of(SettingsError::corrupt_file));
}

// ---- the layer-file bridge ---------------------------------------------

TEST_CASE("store: load_layer folds a file, remembering unknowns") {
    SettingsStore store{demo_schema()};
    Migrations chain;

    Value file = parsed(R"(
    {
        "version": 1,
        "editor.font.size": 200,  // clamps to 72 on the way in
        "editor.theme": "light",
        "app.telemetry": true,
        "some.future.key": [1, {"nested": true}],
        "hand.written": "kept"
    })");
    REQUIRE(store.load_layer(Layer::user, file, chain));

    const auto size = store.get<std::int64_t>("editor.font.size");
    REQUIRE(size);
    CHECK(size->value == 72);
    CHECK(size->layer == Layer::user);
    CHECK(store.get<std::string>("editor.theme")->value == "light");
    CHECK(store.get<bool>("app.telemetry")->value == true);

    // Unknown keys never became settings.
    const auto no = store.layer_of("hand.written");
    REQUIRE_FALSE(no);
    CHECK(no.error().code == code_of(SettingsError::unknown_key));

    // The defaults layer takes no file.
    const auto refused = store.load_layer(Layer::defaults, file, chain);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == code_of(SettingsError::invalid_argument));
}

TEST_CASE("store: a failing load names its key and leaves the layer "
          "untouched") {
    SettingsStore store{demo_schema()};
    Migrations chain;
    REQUIRE(store.set(Layer::user, "editor.font.size", 20));

    // A valid key before the bad one still never applies: the load is
    // all-or-nothing.
    const auto rejected = store.load_layer(
        Layer::user, parsed(R"( {"app.max_recent": 5, "editor.tab_width": 99} )"),
        chain);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code ==
          code_of(SettingsError::constraint_rejected));
    CHECK(rejected.error().message.find("editor.tab_width") !=
          std::string::npos);
    CHECK(store.get<std::int64_t>("app.max_recent")->layer ==
          Layer::defaults);
    CHECK(store.get<std::int64_t>("editor.font.size")->value == 20);

    // A json kind with no scalar counterpart.
    const auto not_scalar = store.load_layer(
        Layer::user, parsed(R"( {"editor.theme": ["a", "b"]} )"), chain);
    REQUIRE_FALSE(not_scalar);
    CHECK(not_scalar.error().code == code_of(SettingsError::type_mismatch));
    CHECK(not_scalar.error().message.find("editor.theme") !=
          std::string::npos);

    // Strict types: a json integer at a real key is a mismatch, the
    // same as set().
    const auto strict = store.load_layer(
        Layer::user, parsed(R"( {"app.scale": 2} )"), chain);
    REQUIRE_FALSE(strict);
    CHECK(strict.error().code == code_of(SettingsError::type_mismatch));

    // A non-object root is corrupt.
    const auto no_root =
        store.load_layer(Layer::user, parsed("[1, 2]"), chain);
    REQUIRE_FALSE(no_root);
    CHECK(no_root.error().code == code_of(SettingsError::corrupt_file));
}

TEST_CASE("store: the file is the layer - a reload drops what the file "
          "dropped") {
    SettingsStore store{demo_schema()};
    Migrations chain;

    Value file = parsed(R"( {"editor.font.size": 18} )");
    REQUIRE(store.load_layer(Layer::user, file, chain));
    REQUIRE(store.set(Layer::user, "app.telemetry", true));
    CHECK(store.has_value(Layer::user, "app.telemetry"));

    // Reload: the file carries only font.size, so telemetry reverts to
    // the layer below - silently, the phase-1 clear() contract.
    REQUIRE(store.load_layer(Layer::user, file, chain));
    CHECK(store.get<bool>("app.telemetry")->layer == Layer::defaults);
    CHECK(store.has_value(Layer::user, "editor.font.size"));
    CHECK_FALSE(store.has_value(Layer::user, "app.telemetry"));
}

TEST_CASE("store: a load notifies observers per applied key, in file order") {
    SettingsStore store{demo_schema()};
    Migrations chain;
    std::vector<SettingChange> seen;
    (void)store.observe("editor.", [&](const SettingChange& c) {
        seen.push_back(c);
    });

    REQUIRE(store.load_layer(
        Layer::user,
        parsed(R"( {"editor.theme": "light", "editor.font.size": 12, )"
               R"( "app.max_recent": 3} )"),
        chain));

    REQUIRE(seen.size() == 2);
    CHECK(seen[0].key == "editor.theme");
    CHECK(seen[0].layer == Layer::user);
    CHECK(seen[1].key == "editor.font.size");
    CHECK(seen[1].value == SettingValue::integer(12));
}

TEST_CASE("store: save_layer writes the version, the layer, the unknowns") {
    Schema bumped = demo_schema();
    REQUIRE(bumped.set_version(7));
    SettingsStore store{std::move(bumped)};
    Migrations chain;

    Value file = parsed(R"(
    {
        "version": 7,
        "editor.font.size": 16,
        "hand.written": {"kept": [1, 2]},
        "zz.another": null
    })");
    REQUIRE(store.load_layer(Layer::user, file, chain));
    REQUIRE(store.set(Layer::user, "editor.theme", "dark"));

    const auto saved = store.save_layer(Layer::user);
    REQUIRE(saved);
    const Object& root = *saved->as_object();
    CHECK(root.size() == 5);
    CHECK(root.find("version")->get_integer().value_or(0) == 7);
    CHECK(root.find("editor.font.size")->get_integer().value_or(0) == 16);
    CHECK(root.find("editor.theme")->get_text().value_or("") == "dark");
    CHECK(root.find("hand.written")->as_object() != nullptr);
    CHECK(root.find("hand.written")->as_object()->find("kept")
              ->as_array()
              ->size() == 2);
    CHECK(root.find("zz.another")->is_null());

    // Order: version first, the layer's own keys alphabetical, the
    // unknowns in the order the file carried them.
    auto it = root.begin();
    CHECK(it->first == "version");
    CHECK(std::next(it)->first == "editor.font.size");
    CHECK(std::next(it, 2)->first == "editor.theme");
    CHECK(std::next(it, 3)->first == "hand.written");
    CHECK(std::next(it, 4)->first == "zz.another");

    // The unknown subtrees round-trip verbatim.
    CHECK(*root.find("hand.written") == parsed(R"( {"kept": [1, 2]} )"));

    // The defaults layer saves nothing.
    const auto refused = store.save_layer(Layer::defaults);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == code_of(SettingsError::invalid_argument));
}

TEST_CASE("store: load -> save -> load round-trips") {
    SettingsStore store{demo_schema()};
    Migrations chain;

    REQUIRE(store.load_layer(Layer::user, parsed(R"(
    {
        "editor.font.size": 16,
        "audio.volume": 0.25,
        "editor.font.name": "Inconsolata",
        "mystery": {"a": [1, 2.5, "x"]}
    })"), chain));
    const auto saved = store.save_layer(Layer::user);
    REQUIRE(saved);
    const std::string text = write(*saved);

    SettingsStore reloaded{demo_schema()};
    REQUIRE(reloaded.load_layer(Layer::user, parsed(text), chain));
    CHECK(reloaded.get<std::int64_t>("editor.font.size")->value == 16);
    CHECK(reloaded.get<double>("audio.volume")->value ==
          doctest::Approx(0.25));
    CHECK(reloaded.get<std::string>("editor.font.name")->value ==
          "Inconsolata");

    // The unknown subtree survived two saves, and the second save
    // reproduces the first byte for byte.
    const auto again = reloaded.save_layer(Layer::user);
    REQUIRE(again);
    CHECK(*again == *saved);
    CHECK(write(*again) == text);
}

TEST_CASE("store: load_layer runs the migration chain first") {
    Schema schema;
    REQUIRE(schema.set_version(2));
    REQUIRE(schema.declare(SettingDesc{
        .key = "editor.font.size",
        .type = SettingType::integer,
        .default_value = SettingValue::integer(14),
        .constraints = {RangeConstraint{
            .min = 8.0, .max = 72.0, .action = ConstraintAction::clamp}},
    }));
    SettingsStore store{std::move(schema)};

    Migrations chain;
    REQUIRE(chain.add(1, [](Value& doc) {
        // v2 renamed editor.size -> editor.font.size.
        Object* root = doc.as_object();
        if (const Value* old = root->find("editor.size")) {
            const auto size = old->get_integer();
            root->erase("editor.size");
            root->set("editor.font.size",
                      Value::integer(size.value_or(14)));
        }
        return ws::core::Status{};
    }));

    Value v1 = parsed(R"( {"version": 1, "editor.size": 22} )");
    REQUIRE(store.load_layer(Layer::user, v1, chain));
    const auto size = store.get<std::int64_t>("editor.font.size");
    REQUIRE(size);
    CHECK(size->value == 22);
    CHECK(size->layer == Layer::user);

    // The caller's document stayed intact - the walk ran on a copy.
    CHECK(v1.as_object()->find("editor.size") != nullptr);

    // The retired key did not survive as an unknown.
    const auto saved = store.save_layer(Layer::user);
    REQUIRE(saved);
    CHECK(saved->as_object()->find("editor.size") == nullptr);
    CHECK(saved->as_object()->find("version")->get_integer().value_or(0) ==
          2);
    CHECK(saved->as_object()->find("editor.font.size")
              ->get_integer()
              .value_or(0) == 22);

    // Without the step, the v1 file cannot load.
    Schema again_schema;
    REQUIRE(again_schema.set_version(2));
    REQUIRE(again_schema.declare(SettingDesc{
        .key = "editor.font.size",
        .type = SettingType::integer,
        .default_value = SettingValue::integer(14),
    }));
    SettingsStore plain{std::move(again_schema)};

    const auto no_step = plain.load_layer(Layer::user, v1, Migrations{});
    REQUIRE_FALSE(no_step);
    CHECK(no_step.error().code == code_of(SettingsError::migrate_failed));
}
