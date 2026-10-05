#include <dlmalloc.h>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

size_t mspace_set_footprint_limit(mspace space, size_t bytes);
size_t mspace_footprint_limit(mspace space);

struct GuestStats {
    uint32_t capacity;
    uint32_t unk;
    uint32_t peak_in_use;
    uint32_t current_in_use;
};
static_assert(sizeof(GuestStats) == 16);

static size_t bounded_base_footprint(mspace space) {
    const size_t granularity = mspace_set_footprint_limit(space, 1);
    const size_t footprint = mspace_footprint(space);
    const size_t limit = footprint >= granularity
        ? footprint - footprint % granularity
        : granularity;
    mspace_set_footprint_limit(space, limit);
    return limit;
}

static void exercise_small_base(size_t capacity) {
    constexpr size_t guard_size = 16;
    void *allocation = nullptr;
    assert(posix_memalign(&allocation, 16, capacity + 2 * guard_size) == 0);
    auto *raw = static_cast<uint8_t *>(allocation);
    std::memset(raw, 0x5A, capacity + 2 * guard_size);
    auto *base = raw + guard_size;
    mspace space = create_mspace_with_base(base, capacity, 0);
    assert(space);

    const size_t initial_footprint = mspace_footprint(space);
    const size_t limit = bounded_base_footprint(space);
    assert(mspace_footprint_limit(space) == limit);

    std::vector<void *> chunks;
    const uintptr_t first = reinterpret_cast<uintptr_t>(base);
    const uintptr_t end = first + capacity;
    while (chunks.size() < capacity / 512 + 2) {
        void *chunk = mspace_malloc(space, 512);
        if (!chunk)
            break;
        const uintptr_t address = reinterpret_cast<uintptr_t>(chunk);
        assert(address >= first && address <= end && 512 <= end - address);
        chunks.push_back(chunk);
    }
    assert(!chunks.empty());
    assert(chunks.size() < capacity / 512 + 2);
    assert(mspace_footprint(space) == initial_footprint);

    for (void *chunk : chunks)
        mspace_free(space, chunk);
    void *reused = mspace_malloc(space, 512);
    assert(reused);
    const uintptr_t reused_address = reinterpret_cast<uintptr_t>(reused);
    assert(reused_address >= first && reused_address <= end && 512 <= end - reused_address);
    mspace_free(space, reused);

    for (size_t i = 0; i < guard_size; ++i) {
        assert(raw[i] == 0x5A);
        assert(raw[guard_size + capacity + i] == 0x5A);
    }
    destroy_mspace(space);
    std::free(allocation);
}

int main() {
    exercise_small_base(4 * 1024);
    exercise_small_base(8 * 1024);
    exercise_small_base(32 * 1024);

    constexpr uint32_t capacity = 1024 * 1024 + 123;
    constexpr size_t guard_size = 16;
    void *allocation = nullptr;
    assert(posix_memalign(&allocation, 16, capacity + 2 * guard_size) == 0);
    auto *raw = static_cast<uint8_t *>(allocation);
    std::memset(raw, 0xA5, capacity + 2 * guard_size);
    auto *base = raw + guard_size;
    mspace space = create_mspace_with_base(base, capacity, 0);
    assert(space);

    const size_t limit = bounded_base_footprint(space);
    assert(limit <= mspace_footprint(space));
    assert(mspace_footprint_limit(space) == limit);

    const size_t baseline = mspace_mallinfo(space).uordblks;
    uint32_t peak_in_use = 0;
    auto current_in_use = [&]() -> uint32_t {
        const size_t allocated = mspace_mallinfo(space).uordblks;
        const size_t current = allocated > baseline ? allocated - baseline : 0;
        assert(current <= UINT32_MAX);
        return static_cast<uint32_t>(current);
    };
    auto sample_peak = [&]() {
        const uint32_t current = current_in_use();
        if (current > peak_in_use)
            peak_in_use = current;
        assert(peak_in_use <= capacity);
    };

    std::vector<void *> chunks;
    while (chunks.size() < capacity / 4096) {
        void *chunk = mspace_malloc(space, 4096);
        if (!chunk)
            break;
        const auto address = reinterpret_cast<uintptr_t>(chunk);
        const auto first = reinterpret_cast<uintptr_t>(base);
        assert(address >= first && address <= first + capacity && 4096 <= first + capacity - address);
        chunks.push_back(chunk);
        sample_peak();
    }
    assert(!chunks.empty());
    assert(chunks.size() < capacity / 4096);
    assert(current_in_use() > 0);
    const uint32_t peak_after_fill = peak_in_use;

    for (void *chunk : chunks)
        mspace_free(space, chunk);
    assert(current_in_use() == 0);
    assert(peak_in_use == peak_after_fill);

    void *reused = mspace_malloc(space, 64 * 1024);
    assert(reused);
    const auto reused_address = reinterpret_cast<uintptr_t>(reused);
    const auto first = reinterpret_cast<uintptr_t>(base);
    assert(reused_address >= first && reused_address <= first + capacity &&
           64 * 1024 <= first + capacity - reused_address);
    sample_peak();
    mspace_free(space, reused);
    assert(current_in_use() == 0);

    for (size_t i = 0; i < guard_size; i++) {
        assert(raw[i] == 0xA5);
        assert(raw[guard_size + capacity + i] == 0xA5);
    }

    const GuestStats stats {capacity, capacity, peak_in_use, current_in_use()};
    assert(stats.capacity == capacity && stats.unk == capacity);
    assert(stats.peak_in_use >= peak_after_fill && stats.current_in_use == 0);
    assert(stats.peak_in_use <= stats.capacity);
    std::printf("mspace stats test passed: cap=%u peak=%u chunks=%zu limit=%zu\n",
                stats.capacity, stats.peak_in_use, chunks.size(), limit);

    destroy_mspace(space);
    std::free(allocation);
}
