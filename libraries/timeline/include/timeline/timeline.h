#pragma once

// Records timed events from many threads, and writes them as a JSON document in the Chrome trace
// event format, which Perfetto (https://ui.perfetto.dev), chrome://tracing and Speedscope open.
//
// Events are zones (a named span of time on one thread, nested in the order they began), values
// drawn as graphs, messages, and frame marks. Each thread records into its own buffer without
// locking, except once per block of events, so recording costs little in the code it measures.
// Memory is bounded: events beyond the limit are dropped and counted.
//
// Thread safety: every member function of Recorder may be called from any thread at any time,
// including write_json while other threads record.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

namespace timeline {

struct Settings {
    // Events beyond this are dropped and counted. Memory is handed out in blocks of
    // Recorder::k_block_events, so the limit is rounded up to whole blocks.
    std::size_t max_events = 1'000'000;

    // How many frames, counted by mark_frame, pass before recording starts.
    std::uint64_t start_frame = 0;

    // How many frames are recorded from start_frame. Zero means no limit.
    std::uint64_t frame_count = 0;

    // Returns the time in nanoseconds since any fixed point. Defaults to std::chrono::steady_clock.
    std::function<std::int64_t()> clock = nullptr;
};

struct Stats {
    std::uint64_t recorded_events = 0;
    std::uint64_t dropped_events = 0;
    std::uint64_t frames = 0;
    std::size_t threads = 0;
};

class Recorder {
public:
    static constexpr std::size_t k_block_events = 1024;

    explicit Recorder(Settings settings = {});
    ~Recorder();

    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // Strings passed as `const char*` are kept by pointer, so they must outlive the recorder, as
    // string literals do. Strings passed as std::string_view are copied.

    // Begins a zone on the calling thread. `file` and `line` say where it is in the code, and
    // `detail` describes this occurrence; both are optional.
    void begin_zone(const char* name, const char* file = nullptr, std::uint32_t line = 0,
                    std::string_view detail = {});

    // Ends the calling thread's innermost zone, if it has one. A zone is kept if recording is on
    // when it ends.
    void end_zone();

    // Ends a frame, which moves the window set by start_frame and frame_count.
    void mark_frame();

    // Names the calling thread. Threads without a name are called "Thread <n>".
    void set_thread_name(std::string_view name);

    // Values that aren't finite are ignored, because JSON can't hold them.
    void record_value(const char* name, double value);

    void record_message(std::string_view text);

    // Whether an event happening now is kept, according to the frame window.
    [[nodiscard]] bool is_recording() const noexcept {
        return m_recording.load(std::memory_order_relaxed);
    }

    [[nodiscard]] Stats stats() const;

    // Writes everything recorded so far as one JSON document, handed to `output` in pieces of
    // about 64 KB. Returns false, having stopped, if `output` returns false. Zones that are still
    // open are left out. `process_name` names the trace's only process.
    bool write_json(const std::function<bool(std::string_view)>& output,
                    std::string_view process_name = {}) const;

private:
    struct ThreadBuffer;
    struct Event;

    [[nodiscard]] std::int64_t now() const;
    [[nodiscard]] ThreadBuffer& this_thread_buffer();
    [[nodiscard]] Event* next_slot(ThreadBuffer& buffer);
    [[nodiscard]] bool in_window(std::uint64_t frame) const noexcept;

    Settings m_settings;
    // Identifies this recorder in each thread's cache. Unlike an address, it is never reused.
    std::uint64_t m_id;
    std::int64_t m_origin;
    std::size_t m_max_blocks;
    std::atomic<std::size_t> m_blocks_used{0};
    std::atomic<bool> m_full{false};
    std::atomic<bool> m_recording{true};
    std::atomic<std::uint64_t> m_frames{0};

    mutable std::mutex m_threads_mutex;
    std::vector<std::unique_ptr<ThreadBuffer>> m_threads;
};

} // namespace timeline
