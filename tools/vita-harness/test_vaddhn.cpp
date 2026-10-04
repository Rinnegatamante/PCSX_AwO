/* Standalone A32 NEON VADDHN conformance checks for the Vita Dynarmic patch.
 * Copyright (c) 2026. Test code; no upstream Dynarmic source copied here.
 */
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <vector>

#include "dynarmic/interface/A32/a32.h"

namespace {
using namespace Dynarmic;

struct Callbacks final : A32::UserCallbacks {
    std::vector<uint32_t> code;
    uint64_t ticks = 2;
    bool exception = false;
    A32::Exception exception_type = A32::Exception::DecodeError;

    std::optional<uint32_t> MemoryReadCode(uint32_t address) override {
        if ((address & 3) || address / 4 >= code.size()) return 0xeafffffe;
        return code[address / 4];
    }
    uint8_t MemoryRead8(uint32_t) override { return 0; }
    uint16_t MemoryRead16(uint32_t) override { return 0; }
    uint32_t MemoryRead32(uint32_t address) override {
        auto value = MemoryReadCode(address);
        return value.value_or(0xeafffffe);
    }
    uint64_t MemoryRead64(uint32_t) override { return 0; }
    void MemoryWrite8(uint32_t, uint8_t) override {}
    void MemoryWrite16(uint32_t, uint16_t) override {}
    void MemoryWrite32(uint32_t, uint32_t) override {}
    void MemoryWrite64(uint32_t, uint64_t) override {}
    void InterpreterFallback(uint32_t pc, size_t count) override {
        std::fprintf(stderr, "unexpected A32 interpreter fallback pc=%08x n=%zu\n", pc, count);
        std::abort();
    }
    void CallSVC(uint32_t) override { std::abort(); }
    void ExceptionRaised(uint32_t, A32::Exception type) override {
        exception = true;
        exception_type = type;
    }
    void AddTicks(uint64_t count) override { ticks = count >= ticks ? 0 : ticks - count; }
    uint64_t GetTicksRemaining() override { return ticks; }
};

[[noreturn]] void fail(const char *what) {
    std::fprintf(stderr, "VADDHN test failed: %s\n", what);
    std::exit(1);
}
void expect(bool ok, const char *what) { if (!ok) fail(what); }

void set_q(A32::Jit &jit, unsigned q, const std::array<uint32_t, 4> &words) {
    for (unsigned i = 0; i < 4; ++i) jit.ExtRegs()[q * 4 + i] = words[i];
}
uint64_t d_value(const A32::Jit &jit, unsigned d) {
    return uint64_t(jit.ExtRegs()[d * 2]) | (uint64_t(jit.ExtRegs()[d * 2 + 1]) << 32);
}

template<typename T, size_t N>
std::array<uint32_t, 4> pack_q(const std::array<T, N> &lanes) {
    static_assert(sizeof(T) * N == 16, "Q register must be 128 bits");
    std::array<uint32_t, 4> words{};
    for (size_t i = 0; i < N; ++i) {
        const uint64_t lane = static_cast<uint64_t>(lanes[i]);
        for (size_t byte = 0; byte < sizeof(T); ++byte) {
            const size_t offset = i * sizeof(T) + byte;
            words[offset / 4] |= static_cast<uint32_t>((lane >> (byte * 8)) & 0xffU)
                                 << ((offset % 4) * 8);
        }
    }
    return words;
}

template<typename Wide, typename Narrow, size_t N>
uint64_t expected_high_add(const std::array<Wide, N> &a, const std::array<Wide, N> &b) {
    uint64_t result = 0;
    constexpr unsigned wide_bits = sizeof(Wide) * 8;
    constexpr unsigned narrow_bits = wide_bits / 2;
    const Wide mask = std::numeric_limits<Wide>::max();
    for (size_t i = 0; i < N; ++i) {
        const Wide sum = Wide((a[i] + b[i]) & mask);
        const Narrow high = Narrow(sum >> narrow_bits);
        result |= uint64_t(high) << (i * narrow_bits);
    }
    return result;
}

void run_one(uint32_t instruction, A32::Jit &jit, Callbacks &cb) {
    cb.code = {instruction, 0xeafffffe};
    cb.ticks = 2;
    jit.ClearCache();
    jit.Regs()[15] = 0;
    jit.SetCpsr(0x000001d0);
    jit.Run();
    expect(!cb.exception, "valid encoding raised an exception");
}

void valid_widths_and_registers() {
    Callbacks cb;
    A32::UserConfig config;
    config.callbacks = &cb;
    A32::Jit jit{config};

    const std::array<uint16_t, 8> a16{0x0001,0x1000,0x7fff,0xffff,0xff00,0xaaaa,0x8000,0x1234};
    const std::array<uint16_t, 8> b16{0x00ff,0x2345,0x0001,0x0102,0x0100,0x5556,0x8000,0xedcc};
    set_q(jit,1,pack_q(a16));set_q(jit,2,pack_q(b16));
    const uint64_t expected16=expected_high_add<uint16_t,uint8_t>(a16,b16);
    run_one(0xf2820404,jit,cb); // vaddhn.i16 d0, q1, q2
    expect(d_value(jit,0)==expected16,"I16 lane sums/wrap/high bytes");

    const std::array<uint32_t,4> a32{0x00000001,0x12345678,0xffffffff,0x80000000};
    const std::array<uint32_t,4> b32{0x0000ffff,0x11111111,0x00000002,0x80000000};
    set_q(jit,9,pack_q(a32));set_q(jit,10,pack_q(b32));
    jit.ExtRegs()[32]=0xfeedface;jit.ExtRegs()[33]=0x01234567; // D16 sibling
    const uint64_t expected32=expected_high_add<uint32_t,uint16_t>(a32,b32);
    run_one(0xf2d214a4,jit,cb); // vaddhn.i32 d17, q9, q10
    expect(d_value(jit,17)==expected32,"I32 lanes/high half or high destination D bit");
    expect(jit.ExtRegs()[32]==0xfeedface&&jit.ExtRegs()[33]==0x01234567,
           "D17 write damaged its Q8 sibling D16");

    const std::array<uint32_t,4> c32{0xaaaa0001,0x12340000,0xffffffff,0x00008000};
    const std::array<uint32_t,4> d32{0x5555ffff,0x0001ffff,0x00000002,0x00008000};
    set_q(jit,6,pack_q(c32));set_q(jit,9,pack_q(d32));
    const uint64_t expected32_high=expected_high_add<uint32_t,uint16_t>(c32,d32);
    run_one(0xf2dc1422,jit,cb); // vaddhn.i32 d17, q6, q9
    expect(d_value(jit,17)==expected32_high,"I32 operand N/M extension and D17 encoding");
    expect(jit.ExtRegs()[32]==0xfeedface&&jit.ExtRegs()[33]==0x01234567,
           "second D17 write damaged sibling D16");

    const std::array<uint64_t,2> a64{0x123456789abcdef0ULL,0xffffffffffffffffULL};
    const std::array<uint64_t,2> b64{0x1111111122222222ULL,0x0000000000000002ULL};
    set_q(jit,15,pack_q(a64));set_q(jit,14,pack_q(b64));
    const uint64_t expected64=expected_high_add<uint64_t,uint32_t>(a64,b64);
    run_one(0xf2eef4ac,jit,cb); // vaddhn.i64 d31, q15, q14
    expect(d_value(jit,31)==expected64,"I64 lane sums/wrap/high d31 register");

    const std::array<uint16_t,8> alias_a{0xffff,0x1000,0x2000,0x3000,0x4000,0x5000,0x6000,0x7000};
    const std::array<uint16_t,8> alias_b{1,0x0100,0x0200,0x0300,0x0400,0x0500,0x0600,0x0700};
    set_q(jit,0,pack_q(alias_a));set_q(jit,1,pack_q(alias_b));
    const uint32_t sibling0=jit.ExtRegs()[2],sibling1=jit.ExtRegs()[3];
    const uint64_t alias_expected=expected_high_add<uint16_t,uint8_t>(alias_a,alias_b);
    run_one(0xf2800402,jit,cb); // vaddhn.i16 d0, q0, q1 (dest aliases source)
    expect(d_value(jit,0)==alias_expected,"destination/source aliasing");
    expect(jit.ExtRegs()[2]==sibling0&&jit.ExtRegs()[3]==sibling1,"D0 write damaged sibling D1");
}

void invalid_encodings() {
    Callbacks cb;
    A32::UserConfig config;config.callbacks=&cb;
    A32::Jit jit{config};
    // 0xf2b20404 has size=11 but overlaps the more-specific VEXT encoding;
    // Dynarmic correctly routes it through the normal ASIMD decode priority.
    for (uint32_t op : {0xf2830404, 0xf2820405}) {
        cb.code={op,0xeafffffe};cb.ticks=2;cb.exception=false;jit.ClearCache();jit.Regs()[15]=0;jit.SetCpsr(0x1d0);jit.Run();
        expect(cb.exception,"odd Q source encoding was not rejected");
        expect(cb.exception_type==A32::Exception::UndefinedInstruction,"odd Q source did not raise UndefinedInstruction");
    }
}
} // namespace

int main() {
    valid_widths_and_registers();
    invalid_encodings();
    std::puts("A32 VADDHN lane/alias/decode tests PASS");
}
