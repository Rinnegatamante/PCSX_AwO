#include "namcos10_io.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct Fixture {
    uint16_t system;
    uint32_t inputs;
    unsigned irq_calls;
    unsigned irq_asserts;
    unsigned input_reads;
} Fixture;

static uint16_t read_system(void *opaque)
{
    return ((Fixture *)opaque)->system;
}

static uint32_t read_inputs(void *opaque)
{
    Fixture *fixture = (Fixture *)opaque;
    fixture->input_reads++;
    return fixture->inputs;
}

static void irq10(void *opaque, int asserted)
{
    Fixture *fixture = (Fixture *)opaque;
    fixture->irq_calls++;
    fixture->irq_asserts += !!asserted;
}

static uint32_t readv(NS10IO *io, uint32_t address, unsigned width)
{
    uint32_t value = 0xdeadbeef;
    assert(ns10_io_read(io, address, width, &value) == NS10_IO_OK);
    return value;
}

static void write16(NS10IO *io, uint32_t address, uint16_t value)
{
    assert(ns10_io_write(io, address, 2, value, 0xffff) == NS10_IO_OK);
}

static void test_inputs_ident_and_bus_width(void)
{
    Fixture fixture = {0xa55a, 0x89abcdefu, 0, 0, 0};
    NS10IOConfig config;
    NS10IO *io = NULL;
    memset(&config, 0, sizeof(config));
    config.read_system = read_system;
    config.read_in1 = read_inputs;
    config.irq10 = irq10;
    config.opaque = &fixture;
    config.exio_ident_code = 0x30;
    assert(ns10_io_create(&config, &io) == NS10_IO_OK && io);

    assert(readv(io, 0x1fba0000, 2) == 0xa55a);
    assert(readv(io, 0xbfba0000, 2) == 0xa55a); /* KSEG1 mirror */
    assert(readv(io, 0x9fba0000, 2) == 0xa55a); /* MAME's 0x90000000 mirror */
    assert(readv(io, 0x1fba0004, 4) == 0x89abcdefu);
    assert(fixture.input_reads == 1);
    assert(readv(io, 0x1fba0006, 2) == 0x89ab);
    assert(readv(io, 0x1fba0005, 1) == 0xcd); /* high byte of low IN1 half */

    write16(io, 0x1fba0002, 0); /* MAME resets serial ID to bit 7 */
    assert(readv(io, 0x1fba0002, 2) == 0x0000); /* ID bit 7 */
    assert(readv(io, 0x1fba0002, 2) == 0x0000); /* ID bit 6 */
    assert(readv(io, 0x1fba0002, 2) == 0x0003); /* ID bit 5 */
    assert(readv(io, 0x1fba0002, 2) == 0x0003); /* ID bit 4 */
    assert(readv(io, 0x1fba0002, 2) == 0x0000);
    assert(readv(io, 0x1fba0002, 2) == 0x0000);
    assert(readv(io, 0x1fba0002, 2) == 0x0000);
    assert(readv(io, 0x1fba0002, 2) == 0x0000);

    assert(ns10_io_write(io, 0x1fba0000, 2, 0, 0xffff) == NS10_IO_UNMAPPED);
    {
        uint32_t value;
        assert(ns10_io_read(io, 0x1fba000c, 2, &value) == NS10_IO_UNMAPPED);
        assert(ns10_io_read(io, 0x1fba0012, 2, &value) == NS10_IO_UNMAPPED);
        assert(ns10_io_read(io, 0x1fba0000, 3, &value) == NS10_IO_ARGUMENT);
    }
    ns10_io_destroy(io);

    /* Absent EXIO is the upstream ident code 0xff, not a synthetic device. */
    memset(&config, 0, sizeof(config));
    config.exio_ident_code = 0xff;
    assert(ns10_io_create(&config, &io) == NS10_IO_OK && io);
    write16(io, 0x1fba0002, 0xffff);
    for (unsigned i = 0; i < 40; i++)
        assert(readv(io, 0x1fba0002, 2) == 3);
    ns10_io_destroy(io);
}

static void i2c_set_data(NS10IO *io, unsigned value)
{
    write16(io, 0x1fba000a, (uint16_t)(value & 1));
}

static void i2c_set_clock(NS10IO *io, unsigned value)
{
    write16(io, 0x1fba0008, (uint16_t)(value & 1));
}

static void test_i2c_mame_ack_only_trace(void)
{
    NS10IO *io = NULL;
    uint8_t sent = 0xa5;
    assert(ns10_io_create(NULL, &io) == NS10_IO_OK && io);

    assert(readv(io, 0x1fba0008, 2) == 1);
    assert(readv(io, 0x1fba000a, 2) == 1);
    assert(ns10_io_write(io, 0x1fba0008, 2, 0, 0xff00) == NS10_IO_OK);
    assert(readv(io, 0x1fba0008, 2) == 1); /* high lane does not alter SCL bit */
    assert(ns10_io_write(io, 0x1fba000a, 2, 0, 0) == NS10_IO_OK);
    assert(readv(io, 0x1fba000a, 2) == 1); /* zero mem_mask changes nothing */
    assert(ns10_io_write(io, 0x1fba0008, 1, 0, 0xfe) == NS10_IO_OK);
    assert(readv(io, 0x1fba0008, 1) == 1); /* SCL bit 0 was not on the bus. */
    assert(ns10_io_write(io, 0x1fba000a, 1, 0, 0) == NS10_IO_OK);
    assert(readv(io, 0x1fba000a, 1) == 1);
    /* Start: SDA falls while SCL is high. */
    i2c_set_data(io, 0);
    for (unsigned bit = 0; bit < 8; bit++) {
        unsigned value = (sent >> (7 - bit)) & 1;
        i2c_set_clock(io, 0);
        i2c_set_data(io, value);
        i2c_set_clock(io, 1);
    }
    assert(readv(io, 0x1fba000a, 2) == 0); /* MAME drives ACK low */
    i2c_set_clock(io, 0);
    i2c_set_data(io, 1); /* host releases SDA for ACK */
    i2c_set_clock(io, 1);
    assert(readv(io, 0x1fba000a, 2) == 0);
    i2c_set_clock(io, 0);
    assert(readv(io, 0x1fba000a, 2) == 1); /* MAME releases ACK; no data store */

    /* Stop: SDA rises while SCL is high. The following start is accepted. */
    i2c_set_clock(io, 1);
    i2c_set_data(io, 0);
    i2c_set_clock(io, 1);
    i2c_set_data(io, 1);
    assert(readv(io, 0x1fba000a, 2) == 1);
    i2c_set_data(io, 0);
    assert(readv(io, 0x1fba000a, 2) == 0);
    ns10_io_reset(io);
    assert(readv(io, 0x1fba0008, 2) == 1);
    assert(readv(io, 0x1fba000a, 2) == 1);
    ns10_io_destroy(io);
}

static void test_interrupt_timer_ack_and_nop_windows(void)
{
    Fixture fixture = {0xffff, 0xffffffffu, 0, 0, 0};
    NS10IOConfig config;
    NS10IO *io = NULL;
    memset(&config, 0, sizeof(config));
    config.opaque = &fixture;
    config.irq10 = irq10;
    config.exio_ident_code = 0xff;
    assert(ns10_io_create(&config, &io) == NS10_IO_OK && io);
    assert(readv(io, 0x1fba001a, 2) == 0);
    ns10_io_advance_us(io, 0); /* MAME timer_device has zero start delay. */
    assert(readv(io, 0x1fba001a, 2) == 8 && fixture.irq_calls == 1);
    write16(io, 0x1fba0018, 0);
    ns10_io_advance_us(io, 9999);
    assert(readv(io, 0x1fba001a, 2) == 0);
    assert(fixture.irq_calls == 1);
    ns10_io_advance_us(io, 1);
    assert(readv(io, 0x1fba001a, 2) == 8);

    assert(fixture.irq_calls == 2 && fixture.irq_asserts == 2);
    ns10_io_advance_us(io, 19999);
    assert(fixture.irq_calls == 3 && readv(io, 0x1fba001a, 2) == 8);
    ns10_io_advance_us(io, 1);
    assert(fixture.irq_calls == 4);
    assert(ns10_io_write(io, 0x1fba0019, 1, 0, 0) == NS10_IO_OK);
    assert(readv(io, 0x1fba001a, 2) == 8);

    /* Each halfword in the four-offset ACK range clears its corresponding bit. */
    assert(ns10_io_write(io, 0x1fba0019, 1, 0, 0xff) == NS10_IO_OK);
    assert(readv(io, 0x1fba001a, 2) == 0);
    assert(ns10_io_write(io, 0x1fba0018, 2, 0, 0) == NS10_IO_OK);
    ns10_io_advance_us(io, 10000);
    assert(readv(io, 0x1fba001a, 2) == 8);
    ns10_io_reset(io);
    assert(readv(io, 0x1fba001a, 2) == 0);
    ns10_io_advance_us(io, 0); /* Reset reanchors and schedules an immediate tick. */
    assert(readv(io, 0x1fba001a, 2) == 8);
    write16(io, 0x1fba0018, 0);
    ns10_io_advance_us(io, 9999);
    assert(readv(io, 0x1fba001a, 2) == 0);
    ns10_io_advance_us(io, 1);
    assert(readv(io, 0x1fba001a, 2) == 8);

    ns10_io_cdrom_interrupt(io, 1);
    assert(readv(io, 0x1fba001a, 2) == 12);
    {
        unsigned asserted = fixture.irq_asserts, calls = fixture.irq_calls;
        ns10_io_cdrom_interrupt(io, 0);
        assert(readv(io, 0x1fba001a, 2) == 8);
        assert(fixture.irq_calls == calls + 1 && fixture.irq_asserts == asserted);
    }
    ns10_io_cdrom_interrupt(io, 1);
    write16(io, 0x1fba0016, 0); /* CD pending acknowledgement is independent of I/O. */
    assert(readv(io, 0x1fba001a, 2) == 8);

    assert(ns10_io_write(io, 0x1fb20000, 4, 0xffffffffu, 0xffffffffu) == NS10_IO_OK);
    assert(ns10_io_write(io, 0x1fb60000, 4, 0xffffffffu, 0xffffffffu) == NS10_IO_OK);
    assert(readv(io, 0x1fb60000, 4) == 0);
    ns10_io_destroy(io);
}

int main(void)
{
    test_inputs_ident_and_bus_width();
    test_i2c_mame_ack_only_trace();
    test_interrupt_timer_ack_and_nop_windows();
    puts("Namco System 10 I/O map, EXIO serial ID, I2C ACK trace, 100 Hz IRQ and lanes PASS");
    return 0;
}
