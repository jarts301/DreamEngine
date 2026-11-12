#include "timeline/timeline.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <format>
#include <iterator>
#include <string>
#include <tuple>
#include <utility>

namespace timeline {
namespace {

constexpr std::uint32_t k_no_text = 0xffffffffu;
constexpr std::size_t k_output_piece = 64 * 1024;

enum class Kind : std::uint8_t { Zone, Value, Message, Frame };

std::atomic<std::uint64_t> g_next_recorder_id{1};

// Each thread remembers its buffer in the last few recorders it used, so finding it takes no lock.
// Entries of destroyed recorders are never matched, because recorder ids aren't reused. A thread
// that uses more recorders than this in turn starts a new buffer in one it returns to, losing the
// zones it had open there; programs normally have one recorder.
struct CacheEntry {
    std::uint64_t recorder = 0;
    void* buffer = nullptr;
};
thread_local std::array<CacheEntry, 8> t_cache{};
thread_local std::size_t t_cache_next = 0;

std::int64_t steady_nanoseconds() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string_view file_name(const char* path) {
    if (path == nullptr) {
        return {};
    }
    const std::string_view full(path);
    const std::size_t slash = full.find_last_of("/\\");
    return slash == std::string_view::npos ? full : full.substr(slash + 1);
}

// Builds the document in a buffer that is handed over whenever it grows past k_output_piece.
class JsonWriter {
public:
    explicit JsonWriter(const std::function<bool(std::string_view)>& output) : m_output(output) {
        m_buffer.reserve(k_output_piece + 1024);
    }

    [[nodiscard]] bool ok() const noexcept {
        return m_ok;
    }

    void raw(std::string_view text) {
        m_buffer += text;
        if (m_buffer.size() >= k_output_piece) {
            hand_over();
        }
    }

    void string(std::string_view text) {
        m_buffer += '"';
        for (const char c : text) {
            switch (c) {
            case '"':
                m_buffer += "\\\"";
                break;
            case '\\':
                m_buffer += "\\\\";
                break;
            case '\n':
                m_buffer += "\\n";
                break;
            case '\r':
                m_buffer += "\\r";
                break;
            case '\t':
                m_buffer += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    std::format_to(std::back_inserter(m_buffer), "\\u{:04x}",
                                   static_cast<unsigned>(static_cast<unsigned char>(c)));
                } else {
                    m_buffer += c;
                }
            }
        }
        m_buffer += '"';
    }

    // Trace timestamps are in microseconds. Nanoseconds become three decimals, without rounding.
    void microseconds(std::int64_t nanoseconds) {
        nanoseconds = std::max<std::int64_t>(nanoseconds, 0);
        std::format_to(std::back_inserter(m_buffer), "{}.{:03}", nanoseconds / 1000,
                       nanoseconds % 1000);
    }

    template <typename... Args> void format(std::format_string<Args...> text, Args&&... args) {
        std::format_to(std::back_inserter(m_buffer), text, std::forward<Args>(args)...);
    }

    bool finish() {
        hand_over();
        return m_ok;
    }

private:
    void hand_over() {
        if (m_ok && !m_buffer.empty()) {
            m_ok = m_output(m_buffer);
        }
        m_buffer.clear();
    }

    const std::function<bool(std::string_view)>& m_output;
    std::string m_buffer;
    bool m_ok = true;
};

} // namespace

struct Recorder::Event {
    // Nanoseconds since the recorder was created.
    std::int64_t start = 0;
    // A zone's end, a frame's number, or a value's bits.
    std::int64_t end = 0;
    const char* name = nullptr;
    const char* file = nullptr;
    std::uint32_t line = 0;
    // A zone's detail or a message's text, as an index into the thread's texts.
    std::uint32_t text = k_no_text;
    Kind kind = Kind::Zone;
};

struct Recorder::ThreadBuffer {
    struct OpenZone {
        const char* name = nullptr;
        const char* file = nullptr;
        std::uint32_t line = 0;
        std::int64_t start = 0;
        std::string detail;
    };

    std::uint32_t id = 0;

    // Used only by the thread that owns the buffer.
    std::vector<OpenZone> open_zones;

    // The owning thread adds events to `blocks` without locking, and publishes each one by raising
    // `count` with release ordering. Readers take `mutex`, and read only the events below `count`.
    // The mutex guards the list of blocks, the texts and the name.
    std::mutex mutex;
    std::vector<std::unique_ptr<Event[]>> blocks;
    std::vector<std::string> texts;
    std::string name;
    std::atomic<std::size_t> count{0};
    std::atomic<std::uint64_t> dropped{0};

    // Only the owning thread calls this.
    std::uint32_t add_text(std::string text) {
        const std::lock_guard lock(mutex);
        texts.push_back(std::move(text));
        return static_cast<std::uint32_t>(texts.size() - 1);
    }

    // Only the owning thread calls this, after filling the slot from next_slot.
    void publish() {
        count.store(count.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }
};

Recorder::Recorder(Settings settings)
    : m_settings(std::move(settings)),
      m_id(g_next_recorder_id.fetch_add(1, std::memory_order_relaxed)),
      m_max_blocks((m_settings.max_events + k_block_events - 1) / k_block_events) {
    if (!m_settings.clock) {
        m_settings.clock = &steady_nanoseconds;
    }
    m_origin = m_settings.clock();
    m_recording.store(in_window(0), std::memory_order_relaxed);
}

Recorder::~Recorder() = default;

std::int64_t Recorder::now() const {
    return m_settings.clock() - m_origin;
}

bool Recorder::in_window(std::uint64_t frame) const noexcept {
    if (frame < m_settings.start_frame) {
        return false;
    }
    return m_settings.frame_count == 0 || frame - m_settings.start_frame < m_settings.frame_count;
}

Recorder::ThreadBuffer& Recorder::this_thread_buffer() {
    for (const CacheEntry& entry : t_cache) {
        if (entry.recorder == m_id) {
            return *static_cast<ThreadBuffer*>(entry.buffer);
        }
    }

    auto created = std::make_unique<ThreadBuffer>();
    ThreadBuffer* const buffer = created.get();
    {
        const std::lock_guard lock(m_threads_mutex);
        buffer->id = static_cast<std::uint32_t>(m_threads.size() + 1);
        m_threads.push_back(std::move(created));
    }
    t_cache[t_cache_next] = CacheEntry{m_id, buffer};
    t_cache_next = (t_cache_next + 1) % t_cache.size();
    return *buffer;
}

Recorder::Event* Recorder::next_slot(ThreadBuffer& buffer) {
    const std::size_t index = buffer.count.load(std::memory_order_relaxed);
    const std::size_t block = index / k_block_events;

    // Only this thread changes the list of blocks, so reading it here needs no lock.
    if (block == buffer.blocks.size()) {
        const bool granted = !m_full.load(std::memory_order_relaxed) &&
                             m_blocks_used.fetch_add(1, std::memory_order_relaxed) < m_max_blocks;
        if (!granted) {
            m_full.store(true, std::memory_order_relaxed);
            buffer.dropped.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        auto events = std::make_unique<Event[]>(k_block_events);
        const std::lock_guard lock(buffer.mutex);
        buffer.blocks.push_back(std::move(events));
    }
    return &buffer.blocks[block][index % k_block_events];
}

void Recorder::begin_zone(const char* name, const char* file, std::uint32_t line,
                          std::string_view detail) {
    ThreadBuffer& buffer = this_thread_buffer();
    buffer.open_zones.push_back(ThreadBuffer::OpenZone{
        .name = name,
        .file = file,
        .line = line,
        .start = 0,
        .detail = std::string(detail),
    });
    // Read last, so the bookkeeping above isn't counted in the zone.
    buffer.open_zones.back().start = now();
}

void Recorder::end_zone() {
    // Read first, for the same reason.
    const std::int64_t end = now();

    ThreadBuffer& buffer = this_thread_buffer();
    if (buffer.open_zones.empty()) {
        return;
    }
    ThreadBuffer::OpenZone zone = std::move(buffer.open_zones.back());
    buffer.open_zones.pop_back();

    if (!is_recording()) {
        return;
    }
    Event* const event = next_slot(buffer);
    if (event == nullptr) {
        return;
    }
    *event = Event{
        .start = zone.start,
        .end = end,
        .name = zone.name,
        .file = zone.file,
        .line = zone.line,
        .text = zone.detail.empty() ? k_no_text : buffer.add_text(std::move(zone.detail)),
        .kind = Kind::Zone,
    };
    buffer.publish();
}

void Recorder::mark_frame() {
    const std::int64_t time = now();
    const std::uint64_t frame = m_frames.fetch_add(1, std::memory_order_relaxed);

    if (is_recording()) {
        ThreadBuffer& buffer = this_thread_buffer();
        if (Event* const event = next_slot(buffer)) {
            *event = Event{.start = time,
                           .end = static_cast<std::int64_t>(frame),
                           .name = "Frame",
                           .kind = Kind::Frame};
            buffer.publish();
        }
    }
    m_recording.store(in_window(frame + 1), std::memory_order_relaxed);
}

void Recorder::set_thread_name(std::string_view name) {
    ThreadBuffer& buffer = this_thread_buffer();
    const std::lock_guard lock(buffer.mutex);
    buffer.name = name;
}

void Recorder::record_value(const char* name, double value) {
    if (!std::isfinite(value) || !is_recording()) {
        return;
    }
    const std::int64_t time = now();
    ThreadBuffer& buffer = this_thread_buffer();
    if (Event* const event = next_slot(buffer)) {
        *event = Event{.start = time,
                       .end = std::bit_cast<std::int64_t>(value),
                       .name = name,
                       .kind = Kind::Value};
        buffer.publish();
    }
}

void Recorder::record_message(std::string_view text) {
    if (!is_recording()) {
        return;
    }
    const std::int64_t time = now();
    ThreadBuffer& buffer = this_thread_buffer();
    if (Event* const event = next_slot(buffer)) {
        *event = Event{.start = time,
                       .text = buffer.add_text(std::string(text)),
                       .kind = Kind::Message};
        buffer.publish();
    }
}

Stats Recorder::stats() const {
    Stats stats;
    stats.frames = m_frames.load(std::memory_order_relaxed);
    const std::lock_guard lock(m_threads_mutex);
    stats.threads = m_threads.size();
    for (const std::unique_ptr<ThreadBuffer>& buffer : m_threads) {
        stats.recorded_events += buffer->count.load(std::memory_order_acquire);
        stats.dropped_events += buffer->dropped.load(std::memory_order_relaxed);
    }
    return stats;
}

bool Recorder::write_json(const std::function<bool(std::string_view)>& output,
                          std::string_view process_name) const {
    // A copy of each thread's published events, so writing doesn't hold up the threads recording.
    struct Snapshot {
        std::uint32_t id = 0;
        std::string name;
        std::vector<Event> events;
        std::vector<std::string> texts;
    };
    std::vector<Snapshot> threads;
    std::uint64_t dropped = 0;
    {
        const std::lock_guard lock(m_threads_mutex);
        threads.reserve(m_threads.size());
        for (const std::unique_ptr<ThreadBuffer>& buffer : m_threads) {
            Snapshot& snapshot = threads.emplace_back();
            snapshot.id = buffer->id;
            dropped += buffer->dropped.load(std::memory_order_relaxed);

            const std::lock_guard buffer_lock(buffer->mutex);
            snapshot.name = buffer->name;
            snapshot.texts = buffer->texts;
            const std::size_t count = buffer->count.load(std::memory_order_acquire);
            snapshot.events.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                snapshot.events.push_back(buffer->blocks[i / k_block_events][i % k_block_events]);
            }
        }
    }

    std::uint64_t recorded = 0;
    for (Snapshot& thread : threads) {
        recorded += thread.events.size();
        // Zones are stored as they end, so inner ones come first. Viewers expect them in the order
        // they began, with an outer zone before the zones inside it.
        const auto key = [](const Event& event) {
            return std::tuple(event.start, event.kind, event.kind == Kind::Zone ? -event.end : 0);
        };
        std::stable_sort(thread.events.begin(), thread.events.end(),
                         [&key](const Event& a, const Event& b) { return key(a) < key(b); });
    }

    JsonWriter json(output);
    json.raw("{\"traceEvents\":[\n");
    bool first = true;
    const auto begin_event = [&](std::string_view phase, std::uint32_t thread) {
        json.raw(first ? "{\"ph\":" : ",\n{\"ph\":");
        first = false;
        json.string(phase);
        json.format(",\"pid\":1,\"tid\":{}", thread);
    };

    if (!process_name.empty()) {
        begin_event("M", 0);
        json.raw(",\"name\":\"process_name\",\"args\":{\"name\":");
        json.string(process_name);
        json.raw("}}");
    }

    for (const Snapshot& thread : threads) {
        begin_event("M", thread.id);
        json.raw(",\"name\":\"thread_name\",\"args\":{\"name\":");
        json.string(thread.name.empty() ? std::format("Thread {}", thread.id) : thread.name);
        json.raw("}}");
        begin_event("M", thread.id);
        json.format(",\"name\":\"thread_sort_index\",\"args\":{{\"sort_index\":{}}}}}", thread.id);

        for (const Event& event : thread.events) {
            if (!json.ok()) {
                return false;
            }
            const bool has_text = event.text != k_no_text && event.text < thread.texts.size();
            const std::string_view text =
                has_text ? std::string_view(thread.texts[event.text]) : std::string_view{};
            switch (event.kind) {
            case Kind::Zone: {
                begin_event("X", thread.id);
                json.raw(",\"cat\":\"zone\",\"name\":");
                json.string(event.name != nullptr ? event.name : "");
                json.raw(",\"ts\":");
                json.microseconds(event.start);
                json.raw(",\"dur\":");
                json.microseconds(event.end - event.start);
                const std::string_view file = file_name(event.file);
                if (!file.empty() || !text.empty()) {
                    json.raw(",\"args\":{");
                    if (!file.empty()) {
                        json.raw("\"source\":");
                        json.string(std::format("{}:{}", file, event.line));
                    }
                    if (!text.empty()) {
                        json.raw(file.empty() ? "\"detail\":" : ",\"detail\":");
                        json.string(text);
                    }
                    json.raw("}");
                }
                json.raw("}");
                break;
            }
            case Kind::Value:
                begin_event("C", thread.id);
                json.raw(",\"name\":");
                json.string(event.name != nullptr ? event.name : "");
                json.raw(",\"ts\":");
                json.microseconds(event.start);
                json.format(",\"args\":{{\"value\":{}}}}}", std::bit_cast<double>(event.end));
                break;
            case Kind::Message:
                begin_event("i", thread.id);
                json.raw(",\"s\":\"t\",\"cat\":\"message\",\"name\":");
                json.string(text);
                json.raw(",\"ts\":");
                json.microseconds(event.start);
                json.raw("}");
                break;
            case Kind::Frame:
                begin_event("i", thread.id);
                json.raw(",\"s\":\"p\",\"cat\":\"frame\",\"name\":\"Frame\",\"ts\":");
                json.microseconds(event.start);
                json.format(",\"args\":{{\"frame\":{}}}}}", event.end);
                break;
            }
        }
    }

    json.format("\n],\n\"displayTimeUnit\":\"ms\",\n\"otherData\":{{\"recorded_events\":{},"
                "\"dropped_events\":{},\"frames\":{}}}}}\n",
                recorded, dropped, m_frames.load(std::memory_order_relaxed));
    return json.finish();
}

} // namespace timeline
