#include "dream/runtime/asset_manager.h"

#include "fake_platform.h"

#include <doctest/doctest.h>

#include <ostream> // Needed for doctest to print std::string.
#include <string>

using namespace dream;
using namespace dream::runtime;
using dream::runtime::tests::FakePlatform;
using dream::runtime::tests::FakeState;

namespace {

struct TextAsset {
    std::string text;
};

// A different asset type that reads the same files.
struct LengthAsset {
    std::size_t length = 0;
};

// Depends on the TextAsset named by its own file's contents.
struct LinkAsset {
    AssetRef<TextAsset> target;
};

// Depends on itself through the LinkLoop named by its file.
struct LinkLoop {
    int unused = 0;
};

std::vector<std::byte> bytes(std::string_view text) {
    const auto view = std::as_bytes(std::span(text));
    return {view.begin(), view.end()};
}

std::string read_text(AssetLoadContext& context, core::Error& error, bool& ok) {
    auto file = context.files.read(context.path);
    ok = file.has_value();
    if (!ok) {
        error = file.error();
        return {};
    }
    return std::string(reinterpret_cast<const char*>(file.value().data()), file.value().size());
}

struct Fixture {
    FakeState state;
    FakePlatform platform{state};
    VirtualFileSystem files{platform};
    AssetManager assets{files};
    int text_loads = 0;

    Fixture() {
        assets.register_loader<TextAsset>(
            [this](AssetLoadContext& context) -> core::Result<TextAsset> {
                ++text_loads;
                core::Error error;
                bool ok = false;
                std::string text = read_text(context, error, ok);
                if (!ok) {
                    return error;
                }
                return TextAsset{std::move(text)};
            });
    }
};

} // namespace

TEST_CASE_FIXTURE(Fixture, "AssetManager loads through the registered loader") {
    state.assets["a.txt"] = bytes("hello");

    const auto asset = assets.load<TextAsset>("assets:/a.txt");

    REQUIRE(asset.has_value());
    CHECK(asset.value()->text == "hello");
}

TEST_CASE_FIXTURE(Fixture, "AssetManager shares one copy while it is in use") {
    state.assets["a.txt"] = bytes("hello");

    const auto first = assets.load<TextAsset>("assets:/a.txt");
    const auto second = assets.load<TextAsset>("assets:/a.txt");

    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(first.value().get() == second.value().get());
    CHECK(text_loads == 1);
}

TEST_CASE_FIXTURE(Fixture, "AssetManager frees an asset nobody is using and can load it again") {
    state.assets["a.txt"] = bytes("hello");

    {
        const auto asset = assets.load<TextAsset>("assets:/a.txt");
        REQUIRE(asset.has_value());
        CHECK(assets.is_loaded<TextAsset>("assets:/a.txt"));
    }
    CHECK_FALSE(assets.is_loaded<TextAsset>("assets:/a.txt"));

    const auto reloaded = assets.load<TextAsset>("assets:/a.txt");
    REQUIRE(reloaded.has_value());
    CHECK(text_loads == 2);
}

TEST_CASE_FIXTURE(Fixture, "AssetManager reports a failed load and retries it next time") {
    const auto missing = assets.load<TextAsset>("assets:/late.txt");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == core::ErrorCode::NotFound);
    CHECK(missing.error().message.find("late.txt") != std::string::npos);

    state.assets["late.txt"] = bytes("here now");
    CHECK(assets.load<TextAsset>("assets:/late.txt").has_value());
}

TEST_CASE_FIXTURE(Fixture, "AssetManager needs a loader for the type") {
    const auto asset = assets.load<LengthAsset>("assets:/a.txt");
    REQUIRE_FALSE(asset.has_value());
    CHECK(asset.error().code == core::ErrorCode::InvalidArgument);
}

TEST_CASE_FIXTURE(Fixture, "AssetManager keeps different types of one file apart") {
    state.assets["a.txt"] = bytes("hello");
    assets.register_loader<LengthAsset>(
        [](AssetLoadContext& context) -> core::Result<LengthAsset> {
            const auto file = context.files.read(context.path);
            if (!file.has_value()) {
                return file.error();
            }
            return LengthAsset{file.value().size()};
        });

    const auto text = assets.load<TextAsset>("assets:/a.txt");
    const auto length = assets.load<LengthAsset>("assets:/a.txt");

    REQUIRE(text.has_value());
    REQUIRE(length.has_value());
    CHECK(text.value()->text == "hello");
    CHECK(length.value()->length == 5);
}

TEST_CASE_FIXTURE(Fixture, "AssetManager lets one asset load another and keep it alive") {
    state.assets["link"] = bytes("assets:/target.txt");
    state.assets["target.txt"] = bytes("the target");
    assets.register_loader<LinkAsset>([](AssetLoadContext& context) -> core::Result<LinkAsset> {
        core::Error error;
        bool ok = false;
        const std::string target = read_text(context, error, ok);
        if (!ok) {
            return error;
        }
        auto loaded = context.assets.load<TextAsset>(target);
        if (!loaded.has_value()) {
            return loaded.error();
        }
        return LinkAsset{loaded.value()};
    });

    const auto link = assets.load<LinkAsset>("assets:/link");

    REQUIRE(link.has_value());
    CHECK(link.value()->target->text == "the target");
    CHECK(assets.is_loaded<TextAsset>("assets:/target.txt"));
}

TEST_CASE_FIXTURE(Fixture, "AssetManager stops an asset that depends on itself") {
    state.assets["a"] = bytes("assets:/b");
    state.assets["b"] = bytes("assets:/a");
    assets.register_loader<LinkLoop>([](AssetLoadContext& context) -> core::Result<LinkLoop> {
        core::Error error;
        bool ok = false;
        const std::string next = read_text(context, error, ok);
        if (!ok) {
            return error;
        }
        const auto loaded = context.assets.load<LinkLoop>(next);
        if (!loaded.has_value()) {
            return loaded.error();
        }
        return LinkLoop{};
    });

    const auto looped = assets.load<LinkLoop>("assets:/a");

    REQUIRE_FALSE(looped.has_value());
    CHECK(looped.error().message.find("depends on itself") != std::string::npos);

    // The failure leaves no trace, so an unrelated load still works.
    state.assets["ok.txt"] = bytes("fine");
    CHECK(assets.load<TextAsset>("assets:/ok.txt").has_value());
}

TEST_CASE_FIXTURE(Fixture, "AssetManager rejects unsafe paths through the file system") {
    const auto asset = assets.load<TextAsset>("assets:/../secret");
    REQUIRE_FALSE(asset.has_value());
    CHECK(asset.error().code == core::ErrorCode::InvalidArgument);
}
