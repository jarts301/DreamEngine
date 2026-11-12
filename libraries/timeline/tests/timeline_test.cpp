#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "timeline/timeline.h"

#include "json.h"

#include <doctest/doctest.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <ostream> // Needed for doctest to print std::string.
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace timeline;

namespace {

// A clock the test moves by hand, in nanoseconds.
struct FakeClock {
    std::shared_ptr<std::int64_t> time = std::make_shared<std::int64_t>(0);

    void set(std::int64_t nanoseconds) const {
        *time = nanoseconds;
    }

    [[nodiscard]] std::function<std::int64_t()> function() const {
        return [time = time] { return *time; };
    }
};

std::string write(const Recorder& recorder, std::string_view process_name = {}) {
    std::string text;
    const bool written = recorder.write_json(
        [&text](std::string_view piece) {
            text += piece;
            return true;
        },
        process_name);
    REQUIRE(written);
    return text;
}

// The trace as parsed JSON, which must be valid.
json::Value parse(const Recorder& recorder, std::string_view process_name = {}) {
    const std::string text = write(recorder, process_name);
    std::optional<json::Value> document = json::parse(text);
    REQUIRE_MESSAGE(document.has_value(), text);
    REQUIRE(document->type == json::Value::Type::Object);
    const json::Value* const events = document->find("traceEvents");
    REQUIRE(events != nullptr);
    REQUIRE(events->type == json::Value::Type::Array);
    return std::move(*document);
}

// The events with phase `phase`, in the order they were written.
std::vector<json::Value> events_of(const json::Value& document, std::string_view phase) {
    std::vector<json::Value> found;
    for (const json::Value& event : document.find("traceEvents")->items) {
        if (event.text("ph") == phase) {
            found.push_back(event);
        }
    }
    return found;
}

std::vector<std::string> zone_names(const json::Value& document) {
    std::vector<std::string> names;
    for (const json::Value& zone : events_of(document, "X")) {
        names.emplace_back(zone.text("name"));
    }
    return names;
}

double other_data(const json::Value& document, std::string_view key) {
    const json::Value* const data = document.find("otherData");
    REQUIRE(data != nullptr);
    return data->number_of(key);
}

} // namespace

TEST_CASE("an empty recorder writes a valid trace") {
    const Recorder recorder;
    const json::Value document = parse(recorder);
    CHECK(document.find("traceEvents")->items.empty());
    CHECK(other_data(document, "recorded_events") == 0.0);
    CHECK(document.text("displayTimeUnit") == "ms");
}

TEST_CASE("a zone records its name, source, start and duration in microseconds") {
    const FakeClock clock;
    clock.set(5'000);
    Recorder recorder(Settings{.clock = clock.function()});

    clock.set(6'000);
    recorder.begin_zone("Update", "/home/dev/game/src/update.cpp", 42);
    clock.set(8'500);
    recorder.end_zone();

    const json::Value document = parse(recorder);
    const std::vector<json::Value> zones = events_of(document, "X");
    REQUIRE(zones.size() == 1);
    CHECK(zones[0].text("name") == "Update");
    CHECK(zones[0].text("cat") == "zone");
    CHECK(zones[0].number_of("ts") == doctest::Approx(1.0));
    CHECK(zones[0].number_of("dur") == doctest::Approx(2.5));
    CHECK(zones[0].number_of("pid") == 1.0);
    CHECK(zones[0].number_of("tid") == 1.0);
    // Only the file's name is kept, which is enough to find it and keeps traces small.
    CHECK(zones[0].find("args")->text("source") == "update.cpp:42");
    CHECK(zones[0].find("args")->find("detail") == nullptr);
}

TEST_CASE("Windows paths are shortened too, and zones without a source have no arguments") {
    Recorder recorder;
    recorder.begin_zone("Windows", "C:\\dev\\game\\render.cpp", 7);
    recorder.end_zone();
    recorder.begin_zone("Anonymous");
    recorder.end_zone();

    const std::vector<json::Value> zones = events_of(parse(recorder), "X");
    REQUIRE(zones.size() == 2);
    CHECK(zones[0].find("args")->text("source") == "render.cpp:7");
    CHECK(zones[1].find("args") == nullptr);
}

TEST_CASE("nested zones are written outer first, in the order they began") {
    const FakeClock clock;
    Recorder recorder(Settings{.clock = clock.function()});

    clock.set(1'000);
    recorder.begin_zone("Frame");
    recorder.begin_zone("Update"); // Begins at the same time as its parent.
    clock.set(2'000);
    recorder.begin_zone("Physics");
    clock.set(3'000);
    recorder.end_zone();
    recorder.end_zone();
    recorder.begin_zone("Render");
    clock.set(4'000);
    recorder.end_zone();
    recorder.end_zone();

    CHECK(zone_names(parse(recorder)) ==
          std::vector<std::string>{"Frame", "Update", "Physics", "Render"});
}

TEST_CASE("details and messages keep any text, escaped as JSON needs") {
    Recorder recorder;
    const std::string awkward = "quote \" backslash \\ newline \n tab \t bell \x07 caf\xc3\xa9";

    recorder.begin_zone("Load", nullptr, 0, awkward);
    recorder.end_zone();
    recorder.record_message(awkward);

    const json::Value document = parse(recorder);
    const std::vector<json::Value> zones = events_of(document, "X");
    REQUIRE(zones.size() == 1);
    CHECK(zones[0].find("args")->text("detail") == awkward);

    const std::vector<json::Value> messages = events_of(document, "i");
    REQUIRE(messages.size() == 1);
    CHECK(messages[0].text("name") == awkward);
    CHECK(messages[0].text("s") == "t");
}

TEST_CASE("values become counters, and values JSON can't hold are ignored") {
    Recorder recorder;
    recorder.record_value("Bodies", 12.0);
    recorder.record_value("Load", 0.25);
    recorder.record_value("Broken", std::numeric_limits<double>::quiet_NaN());
    recorder.record_value("Broken", std::numeric_limits<double>::infinity());

    const std::vector<json::Value> counters = events_of(parse(recorder), "C");
    REQUIRE(counters.size() == 2);
    CHECK(counters[0].text("name") == "Bodies");
    CHECK(counters[0].find("args")->number_of("value") == 12.0);
    CHECK(counters[1].find("args")->number_of("value") == 0.25);
}

TEST_CASE("frames are marked and counted") {
    Recorder recorder;
    recorder.mark_frame();
    recorder.mark_frame();

    const json::Value document = parse(recorder);
    const std::vector<json::Value> frames = events_of(document, "i");
    REQUIRE(frames.size() == 2);
    CHECK(frames[0].text("name") == "Frame");
    CHECK(frames[0].text("s") == "p");
    CHECK(frames[0].find("args")->number_of("frame") == 0.0);
    CHECK(frames[1].find("args")->number_of("frame") == 1.0);
    CHECK(other_data(document, "frames") == 2.0);
    CHECK(recorder.stats().frames == 2);
}

TEST_CASE("the process and threads are named, and each thread has its own track") {
    Recorder recorder;
    recorder.set_thread_name("Main");
    recorder.begin_zone("Main zone");
    recorder.end_zone();

    std::thread named([&recorder] {
        recorder.set_thread_name("Loader");
        recorder.begin_zone("Loader zone");
        recorder.end_zone();
    });
    named.join();
    std::thread unnamed([&recorder] {
        recorder.begin_zone("Unnamed zone");
        recorder.end_zone();
    });
    unnamed.join();

    const json::Value document = parse(recorder, "My Game");
    std::vector<std::string> thread_names;
    std::string process_name;
    for (const json::Value& metadata : events_of(document, "M")) {
        if (metadata.text("name") == "thread_name") {
            thread_names.emplace_back(metadata.find("args")->text("name"));
        } else if (metadata.text("name") == "process_name") {
            process_name = metadata.find("args")->text("name");
        }
    }
    CHECK(process_name == "My Game");
    CHECK(thread_names == std::vector<std::string>{"Main", "Loader", "Thread 3"});

    const std::vector<json::Value> zones = events_of(document, "X");
    REQUIRE(zones.size() == 3);
    CHECK(zones[0].number_of("tid") == 1.0);
    CHECK(zones[1].number_of("tid") == 2.0);
    CHECK(zones[2].number_of("tid") == 3.0);
    CHECK(recorder.stats().threads == 3);
}

TEST_CASE("zones still open are left out, and ending a zone that never began does nothing") {
    Recorder recorder;
    recorder.end_zone();
    recorder.begin_zone("Finished");
    recorder.end_zone();
    recorder.begin_zone("Still open");

    CHECK(zone_names(parse(recorder)) == std::vector<std::string>{"Finished"});

    recorder.end_zone();
    CHECK(zone_names(parse(recorder)) == std::vector<std::string>{"Finished", "Still open"});
}

TEST_CASE("the frame window keeps only the frames asked for") {
    Recorder recorder(Settings{.start_frame = 2, .frame_count = 2});
    CHECK_FALSE(recorder.is_recording());

    for (int frame = 0; frame < 6; ++frame) {
        recorder.begin_zone("Frame", nullptr, 0, std::to_string(frame));
        recorder.record_value("Frame number", frame);
        recorder.end_zone();
        recorder.mark_frame();
    }
    CHECK_FALSE(recorder.is_recording());

    const json::Value document = parse(recorder);
    const std::vector<json::Value> zones = events_of(document, "X");
    REQUIRE(zones.size() == 2);
    CHECK(zones[0].find("args")->text("detail") == "2");
    CHECK(zones[1].find("args")->text("detail") == "3");
    CHECK(events_of(document, "C").size() == 2);

    const std::vector<json::Value> frames = events_of(document, "i");
    REQUIRE(frames.size() == 2);
    CHECK(frames[0].find("args")->number_of("frame") == 2.0);
    CHECK(other_data(document, "frames") == 6.0);
}

TEST_CASE("a zone that ends inside the window is kept whole, even if it began before") {
    Recorder recorder(Settings{.start_frame = 1});
    recorder.begin_zone("Loading");
    recorder.mark_frame();
    CHECK(recorder.is_recording());
    recorder.end_zone();

    CHECK(zone_names(parse(recorder)) == std::vector<std::string>{"Loading"});
}

TEST_CASE("events beyond the limit are dropped and counted") {
    // The limit is rounded up to a whole block.
    Recorder recorder(Settings{.max_events = 10});
    const std::size_t total = Recorder::k_block_events + 100;
    for (std::size_t i = 0; i < total; ++i) {
        recorder.record_value("Index", static_cast<double>(i));
    }

    const Stats stats = recorder.stats();
    CHECK(stats.recorded_events == Recorder::k_block_events);
    CHECK(stats.dropped_events == 100);

    const json::Value document = parse(recorder);
    CHECK(events_of(document, "C").size() == Recorder::k_block_events);
    CHECK(other_data(document, "recorded_events") == static_cast<double>(Recorder::k_block_events));
    CHECK(other_data(document, "dropped_events") == 100.0);
}

TEST_CASE("with no room at all, everything is dropped but zones still pair up") {
    Recorder recorder(Settings{.max_events = 0});
    recorder.begin_zone("Outer");
    recorder.begin_zone("Inner");
    recorder.end_zone();
    recorder.end_zone();

    CHECK(recorder.stats().recorded_events == 0);
    CHECK(recorder.stats().dropped_events == 2);
    CHECK(zone_names(parse(recorder)).empty());
}

TEST_CASE("large traces arrive in pieces, and writing stops when the output refuses") {
    Recorder recorder;
    for (int i = 0; i < 5000; ++i) {
        recorder.begin_zone("Zone with a fairly long name, to fill the output");
        recorder.end_zone();
    }

    int pieces = 0;
    std::string text;
    REQUIRE(recorder.write_json([&](std::string_view piece) {
        ++pieces;
        text += piece;
        return true;
    }));
    CHECK(pieces > 1);
    CHECK(json::parse(text).has_value());

    int refused = 0;
    CHECK_FALSE(recorder.write_json([&refused](std::string_view) {
        ++refused;
        return false;
    }));
    CHECK(refused == 1);
}

TEST_CASE("recorders used by the same thread keep their events apart") {
    auto first = std::make_unique<Recorder>();
    Recorder second;

    first->begin_zone("First outer");
    second.begin_zone("Second");
    first->begin_zone("First inner");
    first->end_zone();
    second.end_zone();
    first->end_zone();

    CHECK(zone_names(parse(*first)) == std::vector<std::string>{"First outer", "First inner"});
    CHECK(zone_names(parse(second)) == std::vector<std::string>{"Second"});

    // A new recorder, possibly at the same address, starts with nothing from the old one.
    first->begin_zone("Left open");
    first.reset();
    first = std::make_unique<Recorder>();
    first->end_zone();
    CHECK(zone_names(parse(*first)).empty());
    CHECK(first->stats().recorded_events == 0);
}

TEST_CASE("a trace can be written while other threads record") {
    // Small enough that the threads fill it, so dropping happens while writing too.
    Recorder recorder(Settings{.max_events = 8'000});
    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&recorder, &stop] {
            recorder.set_thread_name("Worker");
            while (!stop.load()) {
                recorder.begin_zone("Outer", __FILE__, __LINE__, "detail");
                recorder.begin_zone("Inner");
                recorder.record_value("Value", 1.0);
                recorder.end_zone();
                recorder.record_message("Message");
                recorder.end_zone();
            }
        });
    }

    // Writes overlap the recording only once it has begun.
    while (recorder.stats().recorded_events < 1000) {
        std::this_thread::yield();
    }
    for (int write = 0; write < 5; ++write) {
        // Each write is a consistent snapshot: its count matches the events it contains.
        const json::Value document = parse(recorder);
        CHECK(other_data(document, "recorded_events") ==
              static_cast<double>(document.find("traceEvents")->items.size() -
                                  events_of(document, "M").size()));
    }
    stop = true;
    for (std::thread& thread : threads) {
        thread.join();
    }

    const Stats stats = recorder.stats();
    CHECK(stats.threads == 4);
    CHECK(stats.recorded_events > 0);
    CHECK(stats.recorded_events <= 8 * Recorder::k_block_events);

    const json::Value document = parse(recorder);
    for (const json::Value& zone : events_of(document, "X")) {
        CHECK(zone.number_of("dur") >= 0.0);
    }
    CHECK(other_data(document, "recorded_events") == static_cast<double>(stats.recorded_events));
}
