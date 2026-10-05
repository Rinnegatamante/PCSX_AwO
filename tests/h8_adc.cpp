/* SPDX-License-Identifier: BSD-3-Clause */
#include "../libpcsxcore/arcade/h8_adc.h"
#include <cassert>
#include <cstdio>

struct Inputs {
    uint16_t value[8];
    unsigned reads[8];
    unsigned irqs;
    Inputs() : value(), reads(), irqs(0) {}
};

static uint16_t read_analog(void *opaque, unsigned channel) {
    Inputs &inputs = *static_cast<Inputs *>(opaque);
    assert(channel < 8);
    ++inputs.reads[channel];
    return inputs.value[channel];
}

static void request_irq(void *opaque) {
    ++static_cast<Inputs *>(opaque)->irqs;
}

static void default_construction_and_rebind() {
    Inputs inputs;
    inputs.value[0] = 0x2aa;
    arcade::H8Adc3337 adc;
    adc.setCallbacks(&inputs, read_analog, request_irq);
    adc.reset();
    adc.write(8, 0x20, 10);
    assert(inputs.reads[0] == 1 && adc.next_event() == 276);
    adc.update(276);
    assert(adc.read(0) == 0xaa && adc.read(1) == 0x80);
}

static void reset_and_single_conversion() {
    Inputs inputs;
    inputs.value[3] = 0x155;
    arcade::H8Adc3337 adc(&inputs, read_analog, request_irq);
    assert(adc.read(8) == 0 && adc.read(9) == 0);
    for (unsigned i = 0; i < 8; ++i)
        assert(adc.read(static_cast<uint8_t>(i)) == 0);

    adc.write(8, 0x63, 100); // ADIE, ADST, channel 3
    assert(inputs.reads[3] == 1); // Sampling is immediate.
    assert(adc.next_event() == 366); // First 10-bit conversion takes 266 cycles.
    assert(adc.update(365) == 366);
    assert(!(adc.read(8) & 0x80));
    assert(adc.update(366) == 0);
    assert(adc.read(6) == 0x55 && adc.read(7) == 0x40);
    assert((adc.read(8) & 0xe0) == 0xc0); // ADF and ADIE set, ADST cleared.
    assert(inputs.irqs == 1);

    // ADF is write-zero-to-clear. Writing one keeps it set.
    adc.write(8, 0xc3, 500);
    assert(adc.read(8) & 0x80);
    adc.write(8, 0x43, 501); // Clear ADF while keeping ADST high.
    assert(!(adc.read(8) & 0x80));
}

static void high_speed_and_external_trigger() {
    Inputs inputs;
    inputs.value[2] = 0x3ff;
    arcade::H8Adc3337 adc(&inputs, read_analog, request_irq);
    adc.write(9, 0x80, 0); // ADCR selects external trigger.
    adc.write(8, 0x2a, 10); // ADST, channel 2, high-speed mode
    assert(adc.next_event() == 144); // Software ADST still starts directly.
    assert(inputs.reads[2] == 1);
    adc.update(144);
    assert(adc.read(4) == 0xff && adc.read(5) == 0xc0);
    assert(!(adc.read(8) & 0x20));

    adc.write(8, 0x0a, 200); // Select channel 2 without starting.
    adc.trigger(false, 201); // Reset pin state is high, so this falling edge starts.
    assert(adc.read(8) & 0x20);
    assert(adc.next_event() == 335);
    adc.update(335);
    assert(!(adc.read(8) & 0x20));

    // No conversion on a stable-low pin; another falling edge is required.
    adc.write(9, 0, 400);
    adc.write(9, 0x80, 401);
    adc.trigger(false, 402);
    assert(adc.next_event() == 0);
    adc.trigger(true, 403);
    adc.trigger(false, 404);
    assert(adc.next_event() == 538);
}

static void scanning_channels_and_repeats() {
    Inputs inputs;
    for (unsigned channel = 0; channel < 8; ++channel)
        inputs.value[channel] = static_cast<uint16_t>(0x100 + channel);
    arcade::H8Adc3337 adc(&inputs, read_analog, request_irq);

    // Start at channel 4 and rotate through 4,5. MAME's 3337 scan mode
    // repeats continuously after reaching its selected endpoint.
    adc.write(8, 0x75, 0); // ADIE, scan, ADST, endpoint 5; start channel 4
    assert(inputs.reads[4] == 1);
    assert(adc.next_event() == 266);
    adc.update(266);
    assert(inputs.reads[5] == 1 && adc.next_event() == 522);
    adc.update(522);
    assert(adc.read(0) == 0x41 && adc.read(1) == 0x00);
    assert(adc.read(2) == 0x41 && adc.read(3) == 0x40);
    assert(adc.read(8) & 0x80);
    assert(adc.read(8) & 0x20); // Scan remains active and repeats.
    assert(inputs.irqs == 1);
    assert(adc.next_event() == 778);
    assert(inputs.reads[4] == 2); // Next scan immediately samples its first channel.
}

int main() {
    default_construction_and_rebind();
    reset_and_single_conversion();
    high_speed_and_external_trigger();
    scanning_channels_and_repeats();
    std::puts("H8/3337 ADC source-model tests passed");
    return 0;
}
