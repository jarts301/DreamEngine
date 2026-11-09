#include "dream/runtime/event_bus.h"

#include <doctest/doctest.h>

#include <optional>
#include <utility>
#include <vector>

using namespace dream::runtime;

namespace {

struct Hit {
    int damage = 0;
};

struct Heal {
    int amount = 0;
};

} // namespace

TEST_CASE("EventBus delivers an event to every listener of its type") {
    EventBus bus;
    std::vector<int> received;

    const Subscription first = bus.subscribe<Hit>([&](const Hit& hit) { received.push_back(hit.damage); });
    const Subscription second =
        bus.subscribe<Hit>([&](const Hit& hit) { received.push_back(hit.damage * 10); });
    const Subscription other = bus.subscribe<Heal>([&](const Heal&) { received.push_back(-1); });

    bus.publish(Hit{3});

    CHECK(received == std::vector<int>{3, 30});
}

TEST_CASE("EventBus ignores an event nobody listens to") {
    EventBus bus;
    bus.publish(Hit{1});
    bus.enqueue(Hit{1});
    bus.flush();
}

TEST_CASE("EventBus stops calling a listener once its subscription ends") {
    EventBus bus;
    int calls = 0;

    {
        const Subscription subscription = bus.subscribe<Hit>([&](const Hit&) { ++calls; });
        bus.publish(Hit{});
    }
    bus.publish(Hit{});

    CHECK(calls == 1);
}

TEST_CASE("EventBus subscriptions can be moved and reset") {
    EventBus bus;
    int calls = 0;

    Subscription first = bus.subscribe<Hit>([&](const Hit&) { ++calls; });
    Subscription second = std::move(first);
    bus.publish(Hit{});
    CHECK(calls == 1);

    first.reset();
    bus.publish(Hit{});
    CHECK(calls == 2);

    second.reset();
    bus.publish(Hit{});
    CHECK(calls == 2);
}

TEST_CASE("EventBus can be destroyed before its subscriptions") {
    Subscription subscription;
    {
        EventBus bus;
        subscription = bus.subscribe<Hit>([](const Hit&) {});
    }
    subscription.reset();
}

TEST_CASE("EventBus lets a listener remove itself while it runs") {
    EventBus bus;
    int calls = 0;
    Subscription subscription;
    subscription = bus.subscribe<Hit>([&](const Hit&) {
        ++calls;
        subscription.reset();
    });

    bus.publish(Hit{});
    bus.publish(Hit{});

    CHECK(calls == 1);
}

TEST_CASE("EventBus skips a listener removed by an earlier listener in the same publish") {
    EventBus bus;
    int later_calls = 0;
    Subscription later;
    const Subscription earlier = bus.subscribe<Hit>([&](const Hit&) { later.reset(); });
    later = bus.subscribe<Hit>([&](const Hit&) { ++later_calls; });

    bus.publish(Hit{});

    CHECK(later_calls == 0);
}

TEST_CASE("EventBus delays a listener added during a publish until the next event") {
    EventBus bus;
    int new_calls = 0;
    std::vector<Subscription> added;
    const Subscription adder = bus.subscribe<Hit>([&](const Hit&) {
        if (added.empty()) {
            added.push_back(bus.subscribe<Hit>([&](const Hit&) { ++new_calls; }));
        }
    });

    bus.publish(Hit{});
    CHECK(new_calls == 0);

    bus.publish(Hit{});
    CHECK(new_calls == 1);
}

TEST_CASE("EventBus allows publishing from inside a listener") {
    EventBus bus;
    std::vector<int> order;
    const Subscription relay = bus.subscribe<Hit>([&](const Hit& hit) {
        order.push_back(hit.damage);
        if (hit.damage > 0) {
            bus.publish(Hit{hit.damage - 1});
        }
    });

    bus.publish(Hit{2});

    CHECK(order == std::vector<int>{2, 1, 0});
}

TEST_CASE("EventBus holds enqueued events until a flush, in order") {
    EventBus bus;
    std::vector<int> received;
    const Subscription subscription =
        bus.subscribe<Hit>([&](const Hit& hit) { received.push_back(hit.damage); });

    bus.enqueue(Hit{1});
    bus.enqueue(Hit{2});
    CHECK(received.empty());

    bus.flush();
    CHECK(received == std::vector<int>{1, 2});

    bus.flush();
    CHECK(received.size() == 2);
}

TEST_CASE("EventBus keeps events enqueued during a flush for the next flush") {
    EventBus bus;
    int calls = 0;
    const Subscription subscription = bus.subscribe<Hit>([&](const Hit&) {
        ++calls;
        bus.enqueue(Hit{});
    });

    bus.enqueue(Hit{});
    bus.flush();
    CHECK(calls == 1);

    bus.flush();
    CHECK(calls == 2);
}
