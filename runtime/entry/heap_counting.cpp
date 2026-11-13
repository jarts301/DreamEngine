// Replaces the global operator new and delete with versions that count every allocation, so the
// profiler can show allocations per frame and the heap in use (core::heap_stats). dream_add_game
// compiles this directly into a game whose configuration lists a profiler, because a replacement
// inside a static library might not be linked. See docs/adr/0016-profiling-resources.md.
//
// It is empty on Android, which profiling doesn't cover yet, and when DREAM_PROFILING is off.

#include "dream/core/process_stats.h"

#if !defined(DREAM_DISABLE_PROFILING) && (defined(_WIN32) || defined(__linux__)) &&                \
    !defined(__ANDROID__)

#include <cstddef>
#include <cstdlib>
#include <malloc.h>
#include <new>

namespace {

// malloc already returns memory this aligned. Larger alignments need their own functions, and on
// Windows their own free, so allocation and release both decide by the same rule.
constexpr std::size_t k_default_alignment = __STDCPP_DEFAULT_NEW_ALIGNMENT__;

bool is_over_aligned(std::size_t alignment) noexcept {
    return alignment > k_default_alignment;
}

// What the allocator really set aside, so that a block's release subtracts exactly what its
// allocation added, even when delete isn't told the size.
std::size_t block_size(void* pointer, std::size_t alignment) noexcept {
#if defined(_WIN32)
    return is_over_aligned(alignment) ? _aligned_msize(pointer, alignment, 0) : _msize(pointer);
#else
    static_cast<void>(alignment);
    return malloc_usable_size(pointer);
#endif
}

void* try_allocate(std::size_t size, std::size_t alignment) noexcept {
    // Each allocation must return a distinct pointer, even for zero bytes.
    if (size == 0) {
        size = 1;
    }
    void* pointer = nullptr;
    if (!is_over_aligned(alignment)) {
        pointer = std::malloc(size);
    } else {
#if defined(_WIN32)
        pointer = _aligned_malloc(size, alignment);
#else
        if (posix_memalign(&pointer, alignment, size) != 0) {
            pointer = nullptr;
        }
#endif
    }
    if (pointer != nullptr) {
        dream::core::detail::count_heap_allocation(size, block_size(pointer, alignment));
    }
    return pointer;
}

void* allocate(std::size_t size, std::size_t alignment) {
    while (true) {
        if (void* const pointer = try_allocate(size, alignment)) {
            return pointer;
        }
        // The engine is built without exceptions, so there is no std::bad_alloc to throw.
        const std::new_handler handler = std::get_new_handler();
        if (handler == nullptr) {
            std::abort();
        }
        handler();
    }
}

void release(void* pointer, std::size_t alignment) noexcept {
    if (pointer == nullptr) {
        return;
    }
    dream::core::detail::count_heap_free(block_size(pointer, alignment));
#if defined(_WIN32)
    if (is_over_aligned(alignment)) {
        _aligned_free(pointer);
        return;
    }
#endif
    std::free(pointer);
}

std::size_t to_size(std::align_val_t alignment) noexcept {
    return static_cast<std::size_t>(alignment);
}

} // namespace

void* operator new(std::size_t size) {
    return allocate(size, k_default_alignment);
}
void* operator new[](std::size_t size) {
    return allocate(size, k_default_alignment);
}
void* operator new(std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
    return try_allocate(size, k_default_alignment);
}
void* operator new[](std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
    return try_allocate(size, k_default_alignment);
}
void* operator new(std::size_t size, std::align_val_t alignment) {
    return allocate(size, to_size(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return allocate(size, to_size(alignment));
}
void* operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t& /*tag*/) noexcept {
    return try_allocate(size, to_size(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t& /*tag*/) noexcept {
    return try_allocate(size, to_size(alignment));
}

void operator delete(void* pointer) noexcept {
    release(pointer, k_default_alignment);
}
void operator delete[](void* pointer) noexcept {
    release(pointer, k_default_alignment);
}
void operator delete(void* pointer, std::size_t /*size*/) noexcept {
    release(pointer, k_default_alignment);
}
void operator delete[](void* pointer, std::size_t /*size*/) noexcept {
    release(pointer, k_default_alignment);
}
void operator delete(void* pointer, const std::nothrow_t& /*tag*/) noexcept {
    release(pointer, k_default_alignment);
}
void operator delete[](void* pointer, const std::nothrow_t& /*tag*/) noexcept {
    release(pointer, k_default_alignment);
}
void operator delete(void* pointer, std::align_val_t alignment) noexcept {
    release(pointer, to_size(alignment));
}
void operator delete[](void* pointer, std::align_val_t alignment) noexcept {
    release(pointer, to_size(alignment));
}
void operator delete(void* pointer, std::size_t /*size*/, std::align_val_t alignment) noexcept {
    release(pointer, to_size(alignment));
}
void operator delete[](void* pointer, std::size_t /*size*/, std::align_val_t alignment) noexcept {
    release(pointer, to_size(alignment));
}
void operator delete(void* pointer, std::align_val_t alignment,
                     const std::nothrow_t& /*tag*/) noexcept {
    release(pointer, to_size(alignment));
}
void operator delete[](void* pointer, std::align_val_t alignment,
                       const std::nothrow_t& /*tag*/) noexcept {
    release(pointer, to_size(alignment));
}

#endif
