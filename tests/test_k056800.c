#include "k056800.h"
#include <assert.h>
#include <stdio.h>

typedef struct Probe { K056800 *chip; unsigned count; int last; } Probe;
static void irq(void *opaque, int state)
{
    Probe *probe = opaque;
    probe->count++;
    probe->last = state;
    assert(probe->chip->interrupt_line == state);
    /* Inspection is allowed, but the host cannot mutate this transaction. */
    assert(k056800_sound_read(probe->chip, 0) == probe->chip->host_to_sound[0]);
    assert(k056800_reset(probe->chip) == -1);
    assert(k056800_host_write(probe->chip, 0, 0xee) == -1);
    assert(k056800_sound_write(probe->chip, 4, 0) == -1);
}
int main(void)
{
    K056800 chip;
    Probe probe = { &chip, 0, 0 };
    unsigned reg, mirror;
    assert(k056800_init(&chip, irq, &probe) == 0);
    assert(!probe.count && !chip.interrupt_pending && !chip.interrupt_enabled);
    for (reg = 0; reg < 4; reg++) {
        assert(k056800_host_write(&chip, reg, 0x40 + reg) == 0);
        for (mirror = 0; mirror < 32; mirror += 8)
            assert(k056800_sound_read(&chip, reg + mirror) == 0x40 + reg);
    }
    for (reg = 0; reg < 2; reg++) {
        assert(k056800_sound_write(&chip, reg + 8, 0x80 + reg) == 0);
        assert(k056800_host_read(&chip, reg) == 0x80 + reg);
    }
    /* Source volume busy reads zero; these controls do not alter audio. */
    for (reg = 2; reg < 8; reg++) assert(k056800_host_read(&chip, reg) == 0);
    for (reg = 4; reg < 8; reg++) assert(k056800_sound_read(&chip, reg) == 0);
    for (reg = 4; reg < 7; reg++) assert(k056800_host_write(&chip, reg, 0xff) == 0);
    for (reg = 2; reg < 8; reg++)
        if (reg != 4) assert(k056800_sound_write(&chip, reg, 0xff) == 0);
    assert(k056800_host_write(&chip, 7, 0xff) == 0);
    assert(!chip.interrupt_pending && !probe.count);
    assert(k056800_sound_write(&chip, 4, 3) == 0);
    assert(chip.interrupt_enabled && !probe.count);
    assert(k056800_host_write(&chip, 15, 0) == 0);
    assert(chip.interrupt_pending && probe.count == 1 && probe.last == 1);
    /* Literal repeated callbacks, even with the physical line already high. */
    assert(k056800_host_write(&chip, 7, 0) == 0);
    assert(k056800_sound_write(&chip, 4, 1) == 0);
    assert(probe.count == 3 && probe.last == 1);
    assert(k056800_sound_write(&chip, 4, 2) == 0);
    assert(!chip.interrupt_pending && !chip.interrupt_enabled);
    assert(probe.count == 4 && probe.last == 0);
    assert(k056800_sound_write(&chip, 4, 1) == 0);
    assert(probe.count == 4);
    assert(k056800_host_write(&chip, 7, 0) == 0);
    assert(k056800_reset(&chip) == 0);
    assert(probe.count == 5 && probe.last == 1 && chip.interrupt_line == 1);
    assert(!chip.interrupt_pending && !chip.interrupt_enabled);
    for (reg = 0; reg < 8; reg++) {
        assert(k056800_host_read(&chip, reg) == 0);
        assert(k056800_sound_read(&chip, reg) == 0);
    }
    assert(k056800_sound_write(&chip, 4, 0) == 0);
    assert(probe.count == 6 && probe.last == 0);
    assert(k056800_init(NULL, NULL, NULL) == -1);
    assert(k056800_host_write(NULL, 0, 0) == -1);
    assert(k056800_sound_write(NULL, 0, 0) == -1);
    puts("K056800 source registers, IRQ callbacks, mirrors and reset PASS");
    return 0;
}
