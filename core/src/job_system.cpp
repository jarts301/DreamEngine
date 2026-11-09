#include "dream/core/job_system.h"

namespace dream::core {

std::uint32_t JobSystem::default_worker_count() noexcept {
    return std::max(std::thread::hardware_concurrency(), 2u) - 1;
}

JobSystem::JobSystem(std::uint32_t worker_count) {
    m_workers.reserve(worker_count);
    for (std::uint32_t i = 0; i < worker_count; ++i) {
        m_workers.emplace_back([this] { worker_loop(); });
    }
}

JobSystem::~JobSystem() {
    {
        const std::lock_guard lock(m_mutex);
        m_stopping = true;
    }
    m_wake.notify_all();

    for (std::thread& worker : m_workers) {
        worker.join();
    }

    // Workers empty the queue before exiting, so this only finds jobs when there are no workers.
    while (!m_queue.empty()) {
        Job job = m_queue.front();
        m_queue.pop_front();
        execute(job);
    }
}

void JobSystem::enqueue(const Job& job) {
    job.counter->m_pending.fetch_add(1, std::memory_order_relaxed);
    {
        const std::lock_guard lock(m_mutex);
        m_queue.push_back(job);
    }
    m_wake.notify_one();
}

void JobSystem::execute(Job& job) {
    job.invoke(job.storage);

    if (job.counter->m_pending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        // Taking the lock ensures a thread inside wait() is either already asleep and receives
        // this notification, or has not yet checked the counter and will see it at zero.
        { const std::lock_guard lock(m_mutex); }
        m_wake.notify_all();
    }
}

void JobSystem::wait(JobCounter& counter) {
    std::unique_lock lock(m_mutex);
    while (!counter.is_done()) {
        if (m_queue.empty()) {
            m_wake.wait(lock);
            continue;
        }

        Job job = m_queue.front();
        m_queue.pop_front();
        lock.unlock();
        execute(job);
        lock.lock();
    }
}

void JobSystem::worker_loop() {
    std::unique_lock lock(m_mutex);
    while (true) {
        m_wake.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
        if (m_queue.empty()) {
            return;
        }

        Job job = m_queue.front();
        m_queue.pop_front();
        lock.unlock();
        execute(job);
        lock.lock();
    }
}

} // namespace dream::core
