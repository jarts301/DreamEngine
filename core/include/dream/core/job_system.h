#pragma once

#include "dream/core/assert.h"

#include <algorithm>
#include <atomic>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace dream::core {

class JobSystem;

// Tracks a group of jobs. It must outlive every job started with it.
class JobCounter {
public:
    JobCounter() = default;
    JobCounter(const JobCounter&) = delete;
    JobCounter& operator=(const JobCounter&) = delete;

    [[nodiscard]] bool is_done() const noexcept {
        return m_pending.load(std::memory_order_acquire) == 0;
    }

private:
    friend class JobSystem;
    std::atomic<std::uint32_t> m_pending{0};
};

// Runs jobs on worker threads. All member functions are thread-safe and may be called from inside
// a job. Jobs still queued when the system is destroyed are run before destruction finishes.
class JobSystem {
public:
    static constexpr std::size_t k_max_job_size = 48;

    // One thread per CPU core, leaving one for the thread that created the system.
    [[nodiscard]] static std::uint32_t default_worker_count() noexcept;

    // With zero workers, jobs run only inside wait().
    explicit JobSystem(std::uint32_t worker_count = default_worker_count());
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    // Queues a job without allocating for it. A job is usually a lambda that captures a few
    // pointers, references or numbers; captures must be trivially copyable and fit k_max_job_size.
    template <std::invocable F> void run(JobCounter& counter, F job) {
        static_assert(std::is_trivially_copyable_v<F>, "Job captures must be trivially copyable");
        static_assert(sizeof(F) <= k_max_job_size, "Job captures are too large");
        static_assert(alignof(F) <= alignof(std::max_align_t), "Job is over-aligned");

        Job entry;
        entry.invoke = [](void* storage) { (*std::launder(static_cast<F*>(storage)))(); };
        entry.counter = &counter;
        ::new (static_cast<void*>(entry.storage)) F(job);
        enqueue(entry);
    }

    // Blocks until the counter's jobs finish, running queued jobs in the meantime.
    void wait(JobCounter& counter);

    // Splits [0, count) into batches, calls body(begin, end) for each, and waits for all of them.
    template <typename F>
        requires std::invocable<const F&, std::size_t, std::size_t>
    void parallel_for(std::size_t count, std::size_t batch_size, const F& body) {
        DREAM_ASSERT(batch_size > 0, "Batch size must not be zero");

        JobCounter counter;
        for (std::size_t begin = 0; begin < count; begin += batch_size) {
            const std::size_t end = std::min(begin + batch_size, count);
            run(counter, [&body, begin, end] { body(begin, end); });
        }
        wait(counter);
    }

    [[nodiscard]] std::uint32_t worker_count() const noexcept {
        return static_cast<std::uint32_t>(m_workers.size());
    }

private:
    struct Job {
        void (*invoke)(void*);
        JobCounter* counter;
        alignas(std::max_align_t) std::byte storage[k_max_job_size];
    };

    void enqueue(const Job& job);
    void execute(Job& job);
    void worker_loop(std::uint32_t index);

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Job> m_queue;
    std::vector<std::thread> m_workers;
    bool m_stopping = false;
};

} // namespace dream::core
