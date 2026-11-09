#include "dream/core/handle.h"

#include <doctest/doctest.h>

#include <string>

using dream::core::Handle;
using dream::core::HandlePool;

TEST_CASE("a default handle is invalid and resolves to nothing") {
    HandlePool<int> pool;
    (void)pool.create(1);

    const Handle<int> handle;
    CHECK_FALSE(handle.is_valid());
    CHECK(pool.get(handle) == nullptr);
}

TEST_CASE("a created object is reachable through its handle") {
    HandlePool<std::string> pool;
    const Handle<std::string> handle = pool.create("dream");

    CHECK(handle.is_valid());
    REQUIRE(pool.get(handle) != nullptr);
    CHECK(*pool.get(handle) == "dream");
    CHECK(pool.size() == 1);
}

TEST_CASE("a stale handle does not resolve after its slot is reused") {
    HandlePool<int> pool;
    const Handle<int> stale = pool.create(1);
    REQUIRE(pool.destroy(stale));

    const Handle<int> fresh = pool.create(2);

    CHECK(fresh.index == stale.index);
    CHECK(fresh != stale);
    CHECK_FALSE(pool.contains(stale));
    REQUIRE(pool.get(fresh) != nullptr);
    CHECK(*pool.get(fresh) == 2);
}

TEST_CASE("destroying a handle twice fails the second time") {
    HandlePool<int> pool;
    const Handle<int> handle = pool.create(1);

    CHECK(pool.destroy(handle));
    CHECK_FALSE(pool.destroy(handle));
    CHECK(pool.size() == 0);
}

TEST_CASE("destroy runs the object's destructor") {
    struct Tracked {
        explicit Tracked(int* counter) : destroyed(counter) {}
        Tracked(Tracked&& other) noexcept : destroyed(other.destroyed) {
            other.destroyed = nullptr;
        }
        ~Tracked() {
            if (destroyed != nullptr) {
                ++*destroyed;
            }
        }
        int* destroyed;
    };

    int destroyed = 0;
    HandlePool<Tracked> pool;
    const Handle<Tracked> handle = pool.create(&destroyed);

    CHECK(destroyed == 0);
    CHECK(pool.destroy(handle));
    CHECK(destroyed == 1);
}

TEST_CASE("handles survive pool growth") {
    HandlePool<int> pool;
    const Handle<int> first = pool.create(100);
    for (int i = 0; i < 1000; ++i) {
        (void)pool.create(i);
    }

    REQUIRE(pool.get(first) != nullptr);
    CHECK(*pool.get(first) == 100);
    CHECK(pool.size() == 1001);
}
