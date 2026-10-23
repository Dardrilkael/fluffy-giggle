// The settings store's phase-1 suite: the schema, the layered store,
// validation and observers - all headless, no files yet (the json,
// migration and FileSystem layers grow their own cases later).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <ws/settings/errors.hpp>
#include <ws/settings/schema.hpp>
#include <ws/settings/settings.hpp>
#include <ws/settings/value.hpp>

namespace {

using ws::settings::ConstraintAction;
using ws::settings::EnumConstraint;
using ws::settings::Layer;
using ws::settings::RangeConstraint;
using ws::settings::RegexConstraint;
using ws::settings::Schema;
using ws::settings::SettingChange;
using ws::settings::SettingDesc;
using ws::settings::SettingsError;
using ws::settings::SettingsStore;
using ws::settings::SettingType;
using ws::settings::SettingValue;

// Every error code the suite checks, as the raw int the Error carries.
[[nodiscard]] constexpr std::int32_t code_of(SettingsError error) {
    return static_cast<std::int32_t>(error);
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
