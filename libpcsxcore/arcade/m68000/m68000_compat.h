/* Private fixed-width and bus shim for the upstream MAME 68000 handlers. */
#ifndef PCSX_ARCADE_M68000_COMPAT_H
#define PCSX_ARCADE_M68000_COMPAT_H
#include <cstdint>
#include <functional>
#include <utility>

using u8 = uint8_t;
using s8 = int8_t;
using u16 = uint16_t;
using s16 = int16_t;
using u32 = uint32_t;
using s32 = int32_t;
using u64 = uint64_t;
using offs_t = uint32_t;

enum class M68kSpace : uint8_t { Program, Opcode, Cpu };
struct M68000Bus {
    void *opaque = nullptr;
    bool (*read_word)(void *opaque, M68kSpace space, uint32_t address,
                      uint16_t mem_mask, uint16_t *value) = nullptr;
    bool (*write_word)(void *opaque, M68kSpace space, uint32_t address,
                       uint16_t data, uint16_t mem_mask) = nullptr;
    bool use_default_autovectors = true;
    uint64_t cycle_origin = 0;
};

template<typename... Args>
class M68kDelegate {
public:
    void set(std::function<void(Args...)> callback) { callback_ = std::move(callback); }
    bool isnull() const { return !callback_; }
    void operator()(Args... args) const { if (callback_) callback_(args...); }
private:
    std::function<void(Args...)> callback_;
};

class m68000_device;
class M68kBusAccess {
public:
    M68kBusAccess() = default;
    M68kBusAccess(m68000_device *owner, M68kSpace space);
    uint16_t read_interruptible(uint32_t address, uint16_t mem_mask = 0xffff);
    void write_interruptible(uint32_t address, uint16_t data,
                             uint16_t mem_mask = 0xffff);
private:
    m68000_device *owner_ = nullptr;
    M68kSpace space_ = M68kSpace::Program;
};
#endif
