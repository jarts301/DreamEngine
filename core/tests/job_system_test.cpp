#include "dream/core/job_system.h"

#include <doctest/doctest.h>

#include <atomic>
#include <memory>

using dream::core::JobCounter;
using dream::core::JobSystem;

TEST_CASE("every job runs before wait returns") {
    JobSystem jobs(4);
    JobCounter counter;
    std::atomic<int> runs{0};

    for (int i = 0; i < 10000; ++i) {
        jobs.run(counter, [&runs] { runs.fetch_add(1, std::memory_order_relaxed); });
    }
    jobs.wait(counter);

    CHECK(runs.load() == 10000);
    CHECK(counter.is_done());
}

TEST_CASE("waiting on a counter with no jobs returns immediately") {
    JobSystem jobs(1);
    JobCounter counter;
    jobs.wait(counter);
    CHECK(counter.is_done());
}

TEST_CASE("with no workers, jobs run inside wait") {
    JobSystem jobs(0);
    JobCounter counter;
    int value = 0;

    jobs.run(counter, [&value] { value = 7; });
    CHECK(value == 0);
    CHECK_FALSE(counter.is_done());

    jobs.wait(counter);
    CHECK(value == 7);
}

TEST_CASE("jobs can start and wait for other jobs") {
    for (const std::uint32_t workers : {0u, 1u, 4u}) {
        JobSystem jobs(workers);
        JobCounter outer;
        std::atomic<int> runs{0};

        for (int i = 0; i < 100; ++i) {
            jobs.run(outer, [&jobs, &runs] {
                JobCounter inner;
                for (int j = 0; j < 10; ++j) {
                    jobs.run(inner, [&runs] { runs.fetch_add(1, std::memory_order_relaxed); });
                }
                jobs.wait(inner);
            });
        }
        jobs.wait(outer);

        CHECK(runs.load() == 1000);
    }
}

TEST_CASE("destroying the system runs jobs that are still queued") {
    for (const std::uint32_t workers : {0u, 2u}) {
        JobCounter counter;
        std::atomic<int> runs{0};
        {
            JobSystem jobs(workers);
            for (int i = 0; i < 1000; ++i) {
                jobs.run(counter, [&runs] { runs.fetch_add(1, std::memory_order_relaxed); });
            }
        }
        CHECK(runs.load() == 1000);
        CHECK(counter.is_done());
    }
}

TEST_CASE("parallel_for visits each index exactly once") {
    JobSystem jobs(4);
    constexpr std::size_t k_count = 10007;
    const auto visits = std::make_unique<std::atomic<int>[]>(k_count);

    jobs.parallel_for(k_count, 64, [&visits](std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) {
            visits[i].fetch_add(1, std::memory_order_relaxed);
        }
    });

    int wrong = 0;
    for (std::size_t i = 0; i < k_count; ++i) {
        wrong += visits[i].load() == 1 ? 0 : 1;
    }
    CHECK(wrong == 0);
}

TEST_CASE("parallel_for handles empty ranges and oversized batches") {
    JobSystem jobs(2);
    std::atomic<int> batches{0};
    const auto count_batch = [&batches](std::size_t, std::size_t) { batches.fetch_add(1); };

    jobs.parallel_for(0, 8, count_batch);
    CHECK(batches.load() == 0);

    jobs.parallel_for(3, 100, count_batch);
    CHECK(batches.load() == 1);
}
