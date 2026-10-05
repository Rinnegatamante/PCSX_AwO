#include <mem/functions.h>
#include <mem/state.h>

#include <cstdio>
#include <cstdlib>

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

    std::puts("host-page protection regression PASS");
    return 0;
}
