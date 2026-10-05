/* Exercise the public Vita allocator ABI in an emulated ARM guest. */
#include <psp2/kernel/clib.h>
#include <stdint.h>
#include <stddef.h>

#define CAPACITY (512u * 1024u)
static struct {
    unsigned char before[16];
    unsigned char arena[CAPACITY];
    unsigned char after[16];
} heap __attribute__((aligned(4096)));

#define CHECK(expr) do { if (!(expr)) { \
    sceClibPrintf("pcsx-vita: mspace FAIL line=%d check=%s\n", __LINE__, #expr); \
    return 1; } } while (0)

static int in_arena(const void *pointer, size_t size)
{
    uintptr_t start = (uintptr_t)heap.arena, address = (uintptr_t)pointer;
    return pointer && size <= CAPACITY && address >= start &&
        address - start <= CAPACITY - size;
}

int main(void)
{
    struct {
        uint32_t before;
        SceClibMspaceStats value;
        uint32_t after;
    } output = {0x13579bdf, {0xa5a5a5a5, 0xa5a5a5a5, 0xa5a5a5a5, 0xa5a5a5a5}, 0x2468ace0};
    SceClibMspaceStats initial, full, released, fast;
    void *blocks[256], *aligned;
    unsigned char *zeroed, *grown;
    unsigned i, count = 0;
    SceClibMspace space;
    for (i = 0; i < 16; ++i) heap.before[i] = heap.after[i] = 0x5a;
    space = sceClibMspaceCreate(heap.arena, CAPACITY);
    CHECK(space);
    sceClibMspaceMallocStats(space, &output.value);
    CHECK(output.before == 0x13579bdf && output.after == 0x2468ace0);
    initial = output.value;
    CHECK(initial.capacity == CAPACITY && initial.unk == initial.capacity);
    CHECK(initial.current_in_use <= initial.capacity);
    aligned = sceClibMspaceMemalign(space, 64, 4096);
    CHECK(in_arena(aligned, 4096) && !((uintptr_t)aligned & 63));
    zeroed = sceClibMspaceCalloc(space, 128, 4);
    CHECK(in_arena(zeroed, 512));
    for (i = 0; i < 512; ++i) CHECK(zeroed[i] == 0);
    for (i = 0; i < 512; ++i) zeroed[i] = (unsigned char)i;
    grown = sceClibMspaceRealloc(space, zeroed, 2048);
    CHECK(in_arena(grown, 2048));
    for (i = 0; i < 512; ++i) CHECK(grown[i] == (unsigned char)i);
    while (count < 256) {
        void *p = sceClibMspaceMalloc(space, 4096);
        if (!p) break;
        CHECK(in_arena(p, 4096));
        blocks[count++] = p;
    }
    CHECK(count > 0 && count < 256);
    sceClibMspaceMallocStats(space, &full);
    sceClibMspaceMallocStatsFast(space, &fast);
    CHECK(full.capacity == CAPACITY && full.unk == full.capacity);
    CHECK(full.current_in_use > initial.current_in_use && full.current_in_use <= full.capacity);
    CHECK(full.peak_in_use >= full.current_in_use && full.peak_in_use <= full.capacity);
    CHECK(fast.capacity == full.capacity && fast.unk == full.unk &&
          fast.current_in_use == full.current_in_use && fast.peak_in_use == full.peak_in_use);
    for (i = 0; i < count; ++i) sceClibMspaceFree(space, blocks[i]);
    sceClibMspaceFree(space, aligned);
    sceClibMspaceFree(space, grown);
    sceClibMspaceMallocStats(space, &released);
    CHECK(released.current_in_use == initial.current_in_use);
    CHECK(released.peak_in_use == full.peak_in_use);
    aligned = sceClibMspaceMalloc(space, 4096);
    CHECK(in_arena(aligned, 4096));
    sceClibMspaceFree(space, aligned);
    sceClibMspaceDestroy(space);
    for (i = 0; i < 16; ++i) CHECK(heap.before[i] == 0x5a && heap.after[i] == 0x5a);
    sceClibPrintf("pcsx-vita: mspace PASS capacity=%u blocks=%u peak=%u initial=%u released=%u\n",
        CAPACITY, count, full.peak_in_use, initial.current_in_use, released.current_in_use);
    return 0;
}
