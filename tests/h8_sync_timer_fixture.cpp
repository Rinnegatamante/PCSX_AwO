// Real H8 firmware probe; the host consumes empty global SCI timer callbacks.
// Include the core like h8_irq_filter.cpp to observe its public SCI next_event,
// without adding a production API solely for diagnostic state.
#include "../libpcsxcore/arcade/h8.cpp"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

struct Bus {
    H8CPU *cpu = nullptr;
    std::vector<uint8_t> rom;
    unsigned channel = 0, count = 0, limit = 0;
};

static uint8_t rom_read(void *opaque, uint32_t address) {
    Bus &bus = *static_cast<Bus *>(opaque);
    return address < bus.rom.size() ? bus.rom[address] : 0xff;
}

static void marker(void *opaque, uint32_t address, uint8_t) {
    Bus &bus = *static_cast<Bus *>(opaque);
    if (address != 0x080000 || bus.count >= bus.limit) return;
    H8SyncTimer timer{};
    if (h8_get_sync_timer(bus.cpu, bus.channel, &timer)) std::abort();
    std::printf("NATIVE_SYNC n=%u cycle=%llu armed=%u expire=%llu:%018llu clock_event=%llu\n",
        ++bus.count, static_cast<unsigned long long>(h8_current_cycle(bus.cpu)),
        unsigned(timer.armed), static_cast<unsigned long long>(timer.expires.seconds),
        static_cast<unsigned long long>(timer.expires.attoseconds),
        static_cast<unsigned long long>(bus.cpu->sci[bus.channel].next_event()));
}

static bool due(H8Time expiry, arcade::H8CycleClock::Time now) {
    return expiry.seconds < now.seconds ||
        (expiry.seconds == now.seconds && expiry.attoseconds <= now.attoseconds);
}

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    Bus bus;
    bus.channel = std::strtoul(argv[2], nullptr, 10);
    bus.limit = std::strtoul(argv[3], nullptr, 10);
    if (bus.channel > 1 || !bus.limit || bus.limit > 1000) return 2;
    std::ifstream stream(argv[1], std::ios::binary);
    bus.rom.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    if (bus.rom.size() != 0x80000) return 2;
    H8Config config{};
    config.opaque = &bus;
    config.read8 = rom_read;
    config.write8 = marker;
    config.serial_external_period_as[0] = UINT64_C(542534722222);
    bus.cpu = h8_create(&config);
    if (!bus.cpu) return 2;
    for (uint64_t cycle = 1; cycle <= 50000 && bus.count < bus.limit; ++cycle) {
        if (h8_run_until(bus.cpu, cycle)) return 1;
        // Scheduling control for this isolated, one-cycle fixture only. A bus
        // operation may be observed beyond the budget before the timer fires,
        // exactly as generated partial H8 states carry source cycle debt.
        H8SyncTimer timer{};
        unsigned channel;
        const auto now = arcade::H8CycleClock::fromTicks(cycle);
        while (h8_peek_next_sync_timer(bus.cpu, &channel, &timer) == 0 &&
               due(timer.expires, now)) {
            if (h8_fire_sync_timer(bus.cpu, channel, timer.generation) != 1) return 1;
        }
    }
    H8Status status{};
    h8_get_status(bus.cpu, &status);
    h8_destroy(bus.cpu);
    return bus.count == bus.limit && !status.halted && !status.illegal_opcode &&
        !status.unsupported_peripherals ? 0 : 1;
}
