#ifndef PSRAMSTLALLOCATOR_H
#define PSRAMSTLALLOCATOR_H

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <utility>
#include <esp_heap_caps.h>

// Minimal C++11 STL allocator that places storage in PSRAM, falling back to
// internal heap when PSRAM is full or unavailable. Intended for large,
// CPU-only containers (e.g. WebSocket reassembly buffers up to 64 KB) so they
// stay off the scarce internal SRAM. Do not use for DMA- or ISR-accessed data.
//
// On total allocation failure it aborts (matching the default operator new),
// so callers don't need exceptions enabled.
template <typename T> struct PsramStlAllocator {
    using value_type = T;

    PsramStlAllocator() noexcept = default;
    template <typename U> PsramStlAllocator(const PsramStlAllocator<U> &) noexcept {}

    T *allocate(std::size_t n) {
        const std::size_t bytes = n * sizeof(T);
        void *p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p)
            p = heap_caps_malloc(bytes, MALLOC_CAP_DEFAULT);
        if (!p)
            abort();
        return static_cast<T *>(p);
    }

    void deallocate(T *p, std::size_t) noexcept { heap_caps_free(p); }
};

template <typename T, typename U> bool operator==(const PsramStlAllocator<T> &, const PsramStlAllocator<U> &) noexcept {
    return true;
}
template <typename T, typename U> bool operator!=(const PsramStlAllocator<T> &, const PsramStlAllocator<U> &) noexcept {
    return false;
}

template <typename T> struct PsramDeleter {
    void operator()(T *value) const noexcept {
        value->~T();
        PsramStlAllocator<T>{}.deallocate(value, 1);
    }
};

// Large adapter scratch objects must not accumulate on the small firmware task
// stacks. Construct directly in allocated storage, without a stack temporary.
template <typename T, typename... Args> std::unique_ptr<T, PsramDeleter<T>> makePsramUnique(Args &&...args) {
    T *storage = PsramStlAllocator<T>{}.allocate(1);
    return std::unique_ptr<T, PsramDeleter<T>>(new (storage) T(std::forward<Args>(args)...));
}

#endif // PSRAMSTLALLOCATOR_H
