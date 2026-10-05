#include <mem/functions.h>
#include <mem/state.h>

#include <cstdio>
#include <cstdlib>

#ifdef VITA3K_MEM_STALE_FAULT_TEST
#include <atomic>
#include <chrono>
#include <thread>

void mem_test_arm_stale_fault_gate() noexcept;
uint32_t mem_test_stale_fault_arrivals() noexcept;
void mem_test_release_stale_fault_gate() noexcept;
#endif

namespace {
constexpr Address kBase = 0x40000000;
int watched_a = 0;
int watched_c = 0;

void require(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

ProtectCallback counter(int &count) {
    return [&count](Address, bool) {
        ++count;
        return true;
    };
}
}

int main() {
    MemState mem;
    require(init(mem, false), "init without page table");
    require(mem.host_page_size == 16384, "run this regression on the 16 KiB M1 host-page profile");
    require(alloc_at(mem, kBase, 5 * mem.host_page_size, "host-page-test") == kBase,
            "allocate at fixed guest address");

    const Address host_page = static_cast<Address>(mem.host_page_size);
    const Address a = kBase + 0x100;
    const Address b = kBase + 4096 + 0x100;
    const Address c = kBase + 8192 + 0x100;
    require(c < kBase + host_page, "A, B, and C fit in one host page");

    require(add_protect(mem, a, 64, MemPerm::ReadOnly, counter(watched_a)), "watch A");
    require(add_protect(mem, c, 64, MemPerm::ReadOnly, counter(watched_c)), "watch C");
    require(mem.protect_tree.size() == 1, "same host page shares one protection segment");
    require(mem.protect_tree.begin()->second.blocks.size() == 2,
            "logical watcher keys A and C remain distinct");

    // B has no logical watcher, but host-page protection makes its access
    // fault. Both callbacks must be conservatively invalidated before opening
    // the host page.
    mem.memory[b] = 0x5a;
    require(watched_a == 1 && watched_c == 1,
            "neighbor access conservatively fires A and C callbacks");
    // Re-arm each watcher independently. Their callbacks must still be
    // reachable after the conservative neighbor fault.
    require(add_protect(mem, a, 64, MemPerm::ReadOnly, counter(watched_a)), "re-arm A");
    mem.memory[a] = 0x6b;
    require(watched_a == 2, "A callback fires after re-arm");
    require(add_protect(mem, c, 64, MemPerm::ReadOnly, counter(watched_c)), "re-arm C");
    mem.memory[c] = 0x7c;
    require(watched_c == 2, "C callback fires after re-arm");

    // A/C straddle a host-page boundary. Each side needs its own host
    // protection segment; accesses on one side must not consume the other.
    const Address edge_a = kBase + host_page - 0x40;
    const Address edge_c = kBase + host_page + 0x40;
    int edge_a_count = 0;
    int edge_c_count = 0;
    require(add_protect(mem, edge_a, 16, MemPerm::ReadOnly, counter(edge_a_count)), "watch before boundary");
    require(add_protect(mem, edge_c, 16, MemPerm::ReadOnly, counter(edge_c_count)), "watch after boundary");
    require(mem.protect_tree.size() == 2,
            "host-page boundary retains separate protection segments");
    mem.memory[edge_c] = 0x2d;
    require(edge_c_count == 1 && edge_a_count == 0,
            "access after boundary invokes only its own callback");
    mem.memory[edge_a] = 0x3e;
    require(edge_a_count == 1, "access before boundary still invokes its callback");

    // Grow a new low-address range across two existing, disjoint segments.
    // The descending-key map must walk from the lower segment to the higher
    // one, and its merge bound must use the host-rounded effective range.
    int lower_count = 0;
    int upper_count = 0;
    int wide_count = 0;
    const Address lower_watch = kBase + host_page + 0x100;
    const Address upper_watch = kBase + 3 * host_page + 0x100;
    require(add_protect(mem, lower_watch, 16, MemPerm::ReadOnly, counter(lower_count)),
            "install lower existing segment");
    require(add_protect(mem, upper_watch, 16, MemPerm::ReadOnly, counter(upper_count)),
            "install upper existing segment");
    require(mem.protect_tree.size() == 2, "disjoint host pages start as two segments");
    require(add_protect(mem, kBase + 0x100, 3 * host_page + 0x200,
                        MemPerm::ReadOnly, counter(wide_count)),
            "grow wide segment across both existing segments");
    require(mem.protect_tree.size() == 1, "wide range merges both intersected segments");
    require(mem.protect_tree.begin()->second.blocks.size() == 3,
            "wide merge retains all three logical callback blocks");
    mem.memory[kBase + 2 * host_page + 0x100] = 0x4f;
    require(lower_count == 1 && upper_count == 1 && wide_count == 1,
            "effective grown range invokes all merged callbacks");

#ifdef VITA3K_MEM_STALE_FAULT_TEST
    // Both threads must have taken the synchronous host fault before either
    // handler can acquire protect_mutex and open/erase this segment.
    const Address lower_neighbor = kBase + 3 * host_page + 0x200;
    const Address race_watch = kBase + 4 * host_page + 0x200;
    int lower_neighbor_callbacks = 0;
    require(add_protect(mem, lower_neighbor, 64, MemPerm::ReadOnly,
                        counter(lower_neighbor_callbacks)),
            "retain a lower neighboring protection segment");
    const auto run_race = [&] {
        mem.memory[race_watch] = 0x81;
        int race_callbacks = 0;
        require(add_protect(mem, race_watch, 64, MemPerm::None, counter(race_callbacks)),
                "protect the concurrent-fault page");
        const volatile uint8_t *race_ptr = &mem.memory[race_watch];
        uint8_t result_a = 0;
        uint8_t result_b = 0;
        mem_test_arm_stale_fault_gate();
        std::thread reader_a([&] { result_a = race_ptr[0]; });
        std::thread reader_b([&] { result_b = race_ptr[0]; });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (mem_test_stale_fault_arrivals() < 2 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        const bool both_faulted = mem_test_stale_fault_arrivals() == 2;
        mem_test_release_stale_fault_gate();
        reader_a.join();
        reader_b.join();
        require(both_faulted, "both readers reached the fault gate");
        require(result_a == 0x81 && result_b == 0x81, "both readers retry successfully");
        require(race_callbacks == 1, "concurrent faults invoke the callback once");
        require(lower_neighbor_callbacks == 0,
                "neighboring segment callback remains untouched after stale-fault recovery");
        std::puts("STALE_FAULT_RACE_PASS");
    };
    run_race();
    std::atomic<int> unrelated_callbacks { 0 };
    require(add_protect(mem, kBase + 0x100, 64, MemPerm::None, [&](Address, bool) {
                unrelated_callbacks.fetch_add(1, std::memory_order_relaxed);
                return true;
            }),
            "keep an unrelated lower-address segment during the duplicate fault");
    run_race();
    require(unrelated_callbacks.load(std::memory_order_relaxed) == 0, "duplicate fault preserves the unrelated watcher");
    mem.memory[kBase + 0x100] = 0x42;
    require(unrelated_callbacks.load(std::memory_order_relaxed) == 1, "unrelated watcher remains armed after duplicate fault");
    const Address race_missing = race_watch + 0x100;

    // A real metadata miss must retain the old critical fallback. The page is
    // protected directly, with no matching protect_tree entry.
    protect_inner(mem, race_missing, 16, MemPerm::None);
    require(handle_access_violation(mem, &mem.memory[race_missing], false),
            "true metadata miss retains fallback recovery");
    require(mem.memory[race_missing] == 0,
            "fallback recovery makes a true-miss page accessible");
    std::puts("STALE_FAULT_TRUE_MISS_PASS");
#endif

    std::puts("host-page protection regression PASS");
    return 0;
}
