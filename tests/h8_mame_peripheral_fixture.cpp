// SPDX-License-Identifier: BSD-3-Clause
// Execute the exact logical-BE firmware injected by check_mame_h8_peripherals.py.
#include "../libpcsxcore/arcade/h8.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

namespace {
struct Bus {
    H8CPU *cpu = nullptr;
    std::vector<uint8_t> memory = std::vector<uint8_t>(0x1000000, 0xff);
    unsigned observations = 0;
    unsigned observation_limit = 0;
};

uint8_t read8(void *opaque, uint32_t address) {
    return static_cast<Bus *>(opaque)->memory[address & 0xffffff];
}

void write8(void *opaque, uint32_t address, uint8_t value) {
    auto &bus = *static_cast<Bus *>(opaque);
    address &= 0xffffff;
    bus.memory[address] = value;
    if (address >= 0x080000 && address < 0x080003) {
        ++bus.observations;
        if (bus.observations > bus.observation_limit) return;
        std::printf("NATIVE_OBS address=%06x value=%02x cycle=%llu\n", address, value,
                    static_cast<unsigned long long>(h8_current_cycle(bus.cpu)));
    }
}

uint16_t adc_read(void *, unsigned) { return 0x155; }
}

int main(int argc, char **argv) {
    if (argc != 3 && argc != 4) {
        std::fprintf(stderr, "usage: h8_mame_peripheral_fixture logical-h8-rom.bin observations [slice-cycles]\n");
        return 2;
    }
    const unsigned expected_observations = static_cast<unsigned>(std::strtoul(argv[2], nullptr, 10));
    const uint64_t slice = argc == 4 ? std::strtoull(argv[3], nullptr, 10) : 1;
    if (!expected_observations || !slice || slice > 50000) return 2;
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) {
        std::fprintf(stderr, "cannot read fixture ROM %s\n", argv[1]);
        return 2;
    }
    std::vector<uint8_t> rom((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
    if (rom.size() != 0x80000) {
        std::fprintf(stderr, "fixture must be a logical big-endian 512 KiB H8 ROM\n");
        return 2;
    }
    Bus bus;
    bus.observation_limit = expected_observations;
    std::copy(rom.begin(), rom.end(), bus.memory.begin());
    H8Config config{};
    config.opaque = &bus;
    config.read8 = read8;
    config.write8 = write8;
    config.adc_read = adc_read;
    config.serial_external_period_as[0] = UINT64_C(542534722222);
    bus.cpu = h8_create(&config);
    if (!bus.cpu) return 2;

    // Short and long absolute slices exercise partial generated states and
    // internal deadlines while MAME runs the same instruction stream.
    for (uint64_t deadline = slice; deadline <= 50000 && bus.observations < expected_observations; deadline += slice) {
        if (h8_run_until(bus.cpu, deadline) != 0) {
            std::fprintf(stderr, "h8_run_until failed at cycle %llu\n",
                         static_cast<unsigned long long>(deadline));
            h8_destroy(bus.cpu);
            return 1;
        }
    }
    h8_destroy(bus.cpu);
    if (bus.observations < expected_observations) {
        std::fprintf(stderr, "firmware produced %u native observation writes; expected %u\n",
                     bus.observations, expected_observations);
        return 1;
    }
    return 0;
}
