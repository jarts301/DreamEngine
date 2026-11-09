#include "dream/core/log.h"
#include "dream/core/module_project.h"

#include <doctest/doctest.h>

#include <memory>
#include <ostream> // Needed for doctest to print std::string_view.
#include <string>
#include <vector>

using namespace dream::core;

namespace {

// These tests make backends fail on purpose, which would otherwise print warnings to the console
// and look like real problems in the test output.
struct QuietConsole {
    QuietConsole() {
        set_console_log_enabled(false);
    }
    ~QuietConsole() {
        set_console_log_enabled(true);
    }
    QuietConsole(const QuietConsole&) = delete;
    QuietConsole& operator=(const QuietConsole&) = delete;
};

class Widget {
public:
    static constexpr std::string_view k_module_interface = "widget";

    virtual ~Widget() = default;
    [[nodiscard]] virtual int id() const = 0;
};

template <int Id> class FixedWidget final : public Widget {
public:
    [[nodiscard]] int id() const override {
        return Id;
    }
};

template <int Id> Result<std::unique_ptr<Widget>> create_fixed(const Config& /*config*/) {
    return std::unique_ptr<Widget>{std::make_unique<FixedWidget<Id>>()};
}

Result<std::unique_ptr<Widget>> create_failing(const Config& /*config*/) {
    return Error{ErrorCode::Unknown, "the device was lost"};
}

Result<std::unique_ptr<Widget>> create_null(const Config& /*config*/) {
    return std::unique_ptr<Widget>{};
}

Result<std::unique_ptr<Widget>> create_from_config(const Config& config) {
    return config.get_bool("widget.enabled").value_or(false)
               ? create_fixed<7>(config)
               : Result<std::unique_ptr<Widget>>{Error{ErrorCode::Unknown, "disabled"}};
}

bool never_supported() {
    return false;
}

// A second interface, to check that the two are kept apart.
class Gadget {
public:
    static constexpr std::string_view k_module_interface = "gadget";

    virtual ~Gadget() = default;
};

class PlainGadget final : public Gadget {};

Result<std::unique_ptr<Gadget>> create_gadget(const Config& /*config*/) {
    return std::unique_ptr<Gadget>{std::make_unique<PlainGadget>()};
}

// Uses the same interface name as Widget, which is a programming error.
class ImpostorWidget {
public:
    static constexpr std::string_view k_module_interface = "widget";

    virtual ~ImpostorWidget() = default;
};

Result<std::unique_ptr<ImpostorWidget>> create_impostor(const Config& /*config*/) {
    return Error{ErrorCode::Unknown, "not used"};
}

using Names = std::vector<std::string>;

Result<void> register_two_widgets(ModuleRegistry& registry) {
    Result<void> first = registry.add(ModuleDescriptor<Widget>{"one", &create_fixed<1>});
    if (!first.has_value()) {
        return first;
    }
    return registry.add(ModuleDescriptor<Widget>{"two", &create_fixed<2>});
}

ModuleRegistry two_widgets() {
    ModuleRegistry registry;
    REQUIRE(register_two_widgets(registry).has_value());
    return registry;
}

Result<void> register_failing_widgets(ModuleRegistry& /*registry*/) {
    return Error{ErrorCode::InvalidArgument, "registration failed"};
}

const Config k_empty_config = Config::parse("").value();

} // namespace

static_assert(ModuleInterface<Widget>);
static_assert(!ModuleInterface<int>);

TEST_CASE_FIXTURE(QuietConsole, "ModuleRegistry creates the first module in the preference list") {
    const ModuleRegistry registry = two_widgets();

    const Names preference{"two", "one"};
    const Result<CreatedModule<Widget>> created = registry.create<Widget>(preference, k_empty_config);

    REQUIRE(created.has_value());
    CHECK(created.value().name == "two");
    CHECK(created.value().instance->id() == 2);
}

TEST_CASE_FIXTURE(QuietConsole, "ModuleRegistry skips a module that is not linked") {
    const ModuleRegistry registry = two_widgets();

    const Names preference{"missing", "one"};
    const auto created = registry.create<Widget>(preference, k_empty_config);

    REQUIRE(created.has_value());
    CHECK(created.value().name == "one");
}

TEST_CASE_FIXTURE(QuietConsole, "ModuleRegistry skips a module this machine does not support") {
    ModuleRegistry registry;
    REQUIRE(registry
                .add(ModuleDescriptor<Widget>{
                    .name = "future", .create = &create_fixed<1>, .is_supported = &never_supported})
                .has_value());
    REQUIRE(registry.add(ModuleDescriptor<Widget>{"basic", &create_fixed<2>}).has_value());

    const Names preference{"future", "basic"};
    const auto created = registry.create<Widget>(preference, k_empty_config);

    REQUIRE(created.has_value());
    CHECK(created.value().name == "basic");
}

TEST_CASE_FIXTURE(QuietConsole, "ModuleRegistry falls back when a module fails to start") {
    ModuleRegistry registry;
    REQUIRE(registry.add(ModuleDescriptor<Widget>{"failing", &create_failing}).has_value());
    REQUIRE(registry.add(ModuleDescriptor<Widget>{"null", &create_null}).has_value());
    REQUIRE(registry.add(ModuleDescriptor<Widget>{"good", &create_fixed<3>}).has_value());

    const Names preference{"failing", "null", "good"};
    const auto created = registry.create<Widget>(preference, k_empty_config);

    REQUIRE(created.has_value());
    CHECK(created.value().instance->id() == 3);
}

TEST_CASE_FIXTURE(QuietConsole, "ModuleRegistry explains why no module could be used") {
    ModuleRegistry registry;
    REQUIRE(registry.add(ModuleDescriptor<Widget>{"failing", &create_failing}).has_value());

    const Names preference{"missing", "failing"};
    const auto created = registry.create<Widget>(preference, k_empty_config);

    REQUIRE_FALSE(created.has_value());
    const Error& error = created.error();
    CHECK(error.code == ErrorCode::NotFound);
    CHECK(error.message.find("widget") != std::string::npos);
    CHECK(error.message.find("'missing': it is not linked") != std::string::npos);
    CHECK(error.message.find("'failing': the device was lost") != std::string::npos);
}

TEST_CASE_FIXTURE(QuietConsole, "ModuleRegistry reports an empty preference list") {
    const ModuleRegistry registry = two_widgets();

    const auto created = registry.create<Widget>(Names{}, k_empty_config);

    REQUIRE_FALSE(created.has_value());
    CHECK(created.error().message.find("No modules are listed") != std::string::npos);
}

TEST_CASE_FIXTURE(QuietConsole, "ModuleRegistry passes the configuration to the factory") {
    ModuleRegistry registry;
    REQUIRE(registry.add(ModuleDescriptor<Widget>{"configured", &create_from_config}).has_value());
    const Names preference{"configured"};

    const Result<Config> enabled = Config::parse("[widget]\nenabled = true");
    REQUIRE(enabled.has_value());
    CHECK(registry.create<Widget>(preference, enabled.value()).has_value());

    CHECK_FALSE(registry.create<Widget>(preference, k_empty_config).has_value());
}

TEST_CASE_FIXTURE(QuietConsole, "ModuleRegistry rejects invalid registrations") {
    ModuleRegistry registry = two_widgets();

    SUBCASE("a duplicate name") {
        CHECK_FALSE(registry.add(ModuleDescriptor<Widget>{"one", &create_fixed<9>}).has_value());
    }
    SUBCASE("an empty name") {
        CHECK_FALSE(registry.add(ModuleDescriptor<Widget>{"", &create_fixed<9>}).has_value());
    }
    SUBCASE("a missing factory") {
        CHECK_FALSE(registry.add(ModuleDescriptor<Widget>{"empty", nullptr}).has_value());
    }
    SUBCASE("a different interface with the same name") {
        CHECK_FALSE(registry.add(ModuleDescriptor<ImpostorWidget>{"other", &create_impostor})
                        .has_value());
    }

    CHECK(registry.modules().size() == 2);
}

TEST_CASE_FIXTURE(QuietConsole, "ModuleRegistry keeps interfaces apart") {
    ModuleRegistry registry = two_widgets();
    REQUIRE(registry.add(ModuleDescriptor<Gadget>{"one", &create_gadget}).has_value());

    CHECK(registry.contains("widget", "one"));
    CHECK(registry.contains("gadget", "one"));
    CHECK_FALSE(registry.contains("gadget", "two"));

    const Names preference{"two"};
    CHECK_FALSE(registry.create<Gadget>(preference, k_empty_config).has_value());

    const std::vector<ModuleId> modules = registry.modules();
    REQUIRE(modules.size() == 3);
    CHECK(modules[2].interface_name == "gadget");
    CHECK(modules[2].name == "one");
}

TEST_CASE_FIXTURE(QuietConsole, "ModuleProject picks the list for its platform") {
    constexpr const char* k_config = R"(
[modules.pc]
widget = ["two", "one"]

[modules.phone]
widget = ["one"]
)";

    const auto created_on = [&](std::string_view platform) {
        const Result<ModuleProject> project =
            ModuleProject::load({platform, k_config, &register_two_widgets});
        REQUIRE(project.has_value());
        return project.value().create<Widget>();
    };

    const auto on_pc = created_on("pc");
    REQUIRE(on_pc.has_value());
    CHECK(on_pc.value().name == "two");

    const auto on_phone = created_on("phone");
    REQUIRE(on_phone.has_value());
    CHECK(on_phone.value().name == "one");
}

TEST_CASE_FIXTURE(QuietConsole, "ModuleProject reports problems loading or selecting") {
    SUBCASE("a syntax error") {
        const auto project = ModuleProject::load({"pc", "broken =", &register_two_widgets});
        REQUIRE_FALSE(project.has_value());
        CHECK(project.error().code == ErrorCode::ParseError);
    }
    SUBCASE("a failed registration") {
        const auto project = ModuleProject::load({"pc", "", &register_failing_widgets});
        REQUIRE_FALSE(project.has_value());
        CHECK(project.error().message == "registration failed");
    }
    SUBCASE("no list for the interface") {
        const auto project = ModuleProject::load({"pc", "", &register_two_widgets});
        REQUIRE(project.has_value());

        const auto created = project.value().create<Widget>();
        REQUIRE_FALSE(created.has_value());
        CHECK(created.error().message.find("modules.pc.widget") != std::string::npos);
    }
}
