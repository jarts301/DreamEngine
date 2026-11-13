#include "dream/runtime/memory_budget.h"

#include "dream/runtime/app.h"
#include "dream/runtime/asset_manager.h"

#include "fake_platform.h"

#include <doctest/doctest.h>

#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <ostream> // Needed for doctest to print std::string.
#include <string>
#include <string_view>
#include <vector>

using namespace dream;
using namespace dream::runtime;
using namespace dream::runtime::tests;

namespace {

constexpr std::uint64_t k_mb = 1024 * 1024;

MemorySettings settings_from(std::string_view text) {
    const core::Config config = core::Config::parse(text).value();
    core::Result<MemorySettings> settings = MemorySettings::from_config(config);
    REQUIRE_MESSAGE(settings.has_value(), settings.error().message);
    return settings.value();
}

// Settings with only a budget, so the machine's real memory doesn't matter.
MemorySettings budget_only(std::uint64_t budget_mb) {
    MemorySettings settings;
    settings.budget_bytes = budget_mb * k_mb;
    settings.low_system_bytes = 0;
    settings.critical_system_bytes = 0;
    return settings;
}

MemoryReading process_using(std::uint64_t megabytes) {
    return MemoryReading{.process_bytes = megabytes * k_mb};
}

MemoryReading system_with(std::uint64_t ram_mb, std::uint64_t commit_mb) {
    return MemoryReading{.available_physical_bytes = ram_mb * k_mb,
                         .available_commit_bytes = commit_mb * k_mb};
}

} // namespace

TEST_CASE("Memory settings have sensible defaults and read every key") {
    const MemorySettings defaults = settings_from("");
    CHECK(defaults.budget_bytes == 0);
    CHECK(defaults.warning_at == doctest::Approx(0.85));
    CHECK(defaults.low_system_bytes == 1024 * k_mb);
    CHECK(defaults.critical_system_bytes == 256 * k_mb);
    CHECK(defaults.reserve_bytes == 32 * k_mb);
    CHECK(defaults.check_rate == 4);
    CHECK(defaults.refuse_loads);

    const MemorySettings read = settings_from(R"(
[memory]
enabled = false
budget_mb = 2048
warning_at = 0.9
low_system_mb = 512
critical_system_mb = 128.5
reserve_mb = 64
check_rate = 10
refuse_loads = false
)");
    CHECK_FALSE(read.enabled);
    CHECK(read.budget_bytes == 2048 * k_mb);
    CHECK(read.warning_at == doctest::Approx(0.9));
    CHECK(read.low_system_bytes == 512 * k_mb);
    CHECK(read.critical_system_bytes == 128 * k_mb + k_mb / 2);
    CHECK(read.reserve_bytes == 64 * k_mb);
    CHECK(read.check_rate == 10);
    CHECK_FALSE(read.refuse_loads);
}

TEST_CASE("Memory settings refuse values they can't use") {
    const auto rejects = [](std::string_view line, std::string_view key) {
        const core::Config config =
            core::Config::parse(std::string("[memory]\n") + std::string(line)).value();
        const core::Result<MemorySettings> settings = MemorySettings::from_config(config);
        REQUIRE_FALSE(settings.has_value());
        CHECK_MESSAGE(settings.error().message.find(key) != std::string::npos,
                      settings.error().message);
    };
    rejects("budget_mb = -1", "budget_mb");
    rejects("budget_mb = \"lots\"", "budget_mb");
    rejects("warning_at = 0", "warning_at");
    rejects("warning_at = 1.5", "warning_at");
    rejects("check_rate = 0", "check_rate");
    rejects("check_rate = 2.5", "check_rate");
    rejects("reserve_mb = 5000", "reserve_mb");
    rejects("enabled = 1", "enabled");
    rejects("refuse_loads = \"yes\"", "refuse_loads");
}

TEST_CASE("A memory budget warns near its limit and is critical over it") {
    MemoryBudget budget(budget_only(1000));
    CHECK(budget.evaluate(MemoryReading{}) == MemoryPressure::Normal);
    CHECK(budget.evaluate(process_using(800)) == MemoryPressure::Normal);
    CHECK(budget.evaluate(process_using(850)) == MemoryPressure::Warning);
    CHECK(budget.evaluate(process_using(1000)) == MemoryPressure::Critical);
    CHECK(budget.pressure() == MemoryPressure::Critical);
}

TEST_CASE("A memory budget only eases once a reading clears its threshold by 10%") {
    MemoryBudget budget(budget_only(1000));
    CHECK(budget.evaluate(process_using(1000)) == MemoryPressure::Critical);
    // Critical until under 90% of the budget.
    CHECK(budget.evaluate(process_using(950)) == MemoryPressure::Critical);
    CHECK(budget.evaluate(process_using(899)) == MemoryPressure::Warning);
    // Warning until under 90% of the warning's 850 MB, which is 765 MB.
    CHECK(budget.evaluate(process_using(800)) == MemoryPressure::Warning);
    CHECK(budget.evaluate(process_using(760)) == MemoryPressure::Normal);
    // Rising again needs the full threshold.
    CHECK(budget.evaluate(process_using(849)) == MemoryPressure::Normal);
}

TEST_CASE("A memory budget watches the system's free RAM and its room to allocate") {
    MemorySettings settings;
    settings.low_system_bytes = 1000 * k_mb;
    settings.critical_system_bytes = 200 * k_mb;
    MemoryBudget budget(settings);

    CHECK(budget.evaluate(system_with(4000, 8000)) == MemoryPressure::Normal);
    // Little free RAM means paging ahead: a warning, while there is still room to allocate.
    CHECK(budget.evaluate(system_with(900, 8000)) == MemoryPressure::Warning);
    CHECK(budget.evaluate(system_with(900, 150)) == MemoryPressure::Critical);
    // Critical until the room is over 220 MB, and a warning until the RAM is over 1100 MB.
    CHECK(budget.evaluate(system_with(900, 210)) == MemoryPressure::Critical);
    CHECK(budget.evaluate(system_with(1050, 230)) == MemoryPressure::Warning);
    CHECK(budget.evaluate(system_with(1150, 230)) == MemoryPressure::Normal);
}

TEST_CASE("A failed allocation is critical whatever the readings, and zero thresholds are off") {
    MemorySettings off;
    off.low_system_bytes = 0;
    off.critical_system_bytes = 0;
    MemoryBudget budget(off);
    CHECK(budget.evaluate(system_with(1, 1)) == MemoryPressure::Normal);
    CHECK(budget.evaluate(process_using(1'000'000)) == MemoryPressure::Normal);
    CHECK(budget.evaluate(MemoryReading{.allocation_failed = true}) == MemoryPressure::Critical);
    CHECK(budget.evaluate(MemoryReading{}) == MemoryPressure::Normal);
}

TEST_CASE("The AssetManager refuses new assets while told to, but still hands out loaded ones") {
    FakeState state;
    state.assets.emplace("a.txt", std::vector<std::byte>{std::byte{'a'}});
    state.assets.emplace("b.txt", std::vector<std::byte>{std::byte{'b'}});
    FakePlatform platform(state);
    VirtualFileSystem files(platform);
    AssetManager assets(files);
    int loads = 0;
    assets.register_loader<std::string>(
        [&loads](AssetLoadContext& context) -> core::Result<std::string> {
            ++loads;
            return std::string(context.path);
        });

    const auto loaded = assets.load<std::string>("assets:/a.txt");
    REQUIRE(loaded.has_value());

    assets.set_refusing_loads(true);
    CHECK(assets.is_refusing_loads());
    const auto refused = assets.load<std::string>("assets:/b.txt");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == core::ErrorCode::OutOfMemory);
    CHECK(assets.load<std::string>("assets:/a.txt").has_value());
    CHECK(loads == 1);

    assets.set_refusing_loads(false);
    CHECK(assets.load<std::string>("assets:/b.txt").has_value());
    CHECK(loads == 2);
}

namespace {

// Records what the App tells it about memory.
class MemoryGame final : public Game {
public:
    core::Result<void> on_start(App& app) override {
        pressure_at_start = app.memory_pressure();
        refusing_at_start = app.assets().is_refusing_loads();
        m_subscription = app.events().subscribe<MemoryPressureChanged>(
            [this](const MemoryPressureChanged& change) { changes.push_back(change); });
        return {};
    }

    void on_update(App& app, const core::FrameTime& /*time*/) override {
        ++frames;
        pressures.push_back(app.memory_pressure());
        refusing.push_back(app.assets().is_refusing_loads());
        handlers.push_back(std::get_new_handler());
        if (frames == 1 && fail_on_first_frame) {
            // What operator new does when it can't allocate.
            std::get_new_handler()();
        }
    }

    void on_low_memory(App& /*app*/) override {
        ++low_memory_calls;
    }

    bool fail_on_first_frame = false;
    int frames = 0;
    int low_memory_calls = 0;
    std::optional<MemoryPressure> pressure_at_start;
    bool refusing_at_start = false;
    std::vector<MemoryPressure> pressures;
    std::vector<bool> refusing;
    std::vector<std::new_handler> handlers;
    std::vector<MemoryPressureChanged> changes;

private:
    Subscription m_subscription;
};

core::Result<void> run_game(MemoryGame& game, std::string_view memory_settings, int frames) {
    FakeState state;
    for (int frame = 0; frame < frames; ++frame) {
        state.script.push_back(k_end_of_frame);
    }
    const core::Config config =
        core::Config::parse(std::string("[app]\nmax_fps = 100\n\n[memory]\n") +
                            std::string(memory_settings))
            .value();
    App app(std::make_unique<FakePlatform>(state), config);
    return app.run(game);
}

} // namespace

TEST_CASE("A game over its memory budget knows it from the start, and new assets are refused") {
    MemoryGame game;
    // 10 KB, which any process is over.
    REQUIRE(run_game(game, "budget_mb = 0.01\nlow_system_mb = 0\ncritical_system_mb = 0\n", 2)
                .has_value());
    CHECK(game.pressure_at_start == MemoryPressure::Critical);
    CHECK(game.refusing_at_start);
    // Still critical, and no change to report, so no call.
    CHECK(game.pressures == std::vector<MemoryPressure>{MemoryPressure::Critical,
                                                        MemoryPressure::Critical});
    CHECK(game.low_memory_calls == 0);
}

TEST_CASE("A failed allocation makes the pressure critical until the reserve is taken back") {
    MemoryGame game;
    game.fail_on_first_frame = true;
    constexpr std::string_view k_settings =
        "low_system_mb = 0\ncritical_system_mb = 0\nreserve_mb = 1\ncheck_rate = 1000\n";
    REQUIRE(run_game(game, k_settings, 4).has_value());

    CHECK(game.pressure_at_start == MemoryPressure::Normal);
    REQUIRE(game.pressures.size() == 4);
    CHECK(game.pressures[0] == MemoryPressure::Normal);
    // The frame after the failure is critical, and refuses new assets.
    CHECK(game.pressures[1] == MemoryPressure::Critical);
    CHECK(game.refusing[1]);
    // The next check takes the reserve back, as memory is plentiful, and all is normal again.
    CHECK(game.pressures[2] == MemoryPressure::Normal);
    CHECK_FALSE(game.refusing[2]);
    CHECK(game.low_memory_calls == 1);

    REQUIRE(game.changes.size() == 2);
    CHECK(game.changes[0].pressure == MemoryPressure::Critical);
    CHECK(game.changes[0].previous == MemoryPressure::Normal);
    CHECK(game.changes[0].reading.allocation_failed);
    CHECK(game.changes[1].pressure == MemoryPressure::Normal);
    CHECK_FALSE(game.changes[1].reading.allocation_failed);

    // The reserve's handler was in place during the game, and is gone after it.
    CHECK(game.handlers[0] != nullptr);
    CHECK(std::get_new_handler() == nullptr);
}

TEST_CASE("Memory watching can be turned off") {
    MemoryGame game;
    REQUIRE(run_game(game, "enabled = false\nbudget_mb = 0.01\n", 1).has_value());
    CHECK(game.pressure_at_start == MemoryPressure::Normal);
    CHECK(game.pressures == std::vector<MemoryPressure>{MemoryPressure::Normal});
    CHECK(game.handlers == std::vector<std::new_handler>{nullptr});
}

TEST_CASE("The App refuses memory settings it can't use") {
    MemoryGame game;
    const core::Result<void> result = run_game(game, "check_rate = 0\n", 1);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().message.find("memory.check_rate") != std::string::npos);
}
