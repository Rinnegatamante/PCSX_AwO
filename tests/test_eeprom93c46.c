#include "eeprom93c46.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t time_ns;
static EEPROM93C46 *chip;
static void begin(void);
static void end(void);

static void tick(void)
{
    ++time_ns;
    assert(eeprom93c46_advance_to_ns(chip, time_ns) == 0);
}

static void line_cs(int state)
{
    tick();
    assert(eeprom93c46_set_cs(chip, state) == 0);
}

static void bit_out(int bit)
{
    tick();
    assert(eeprom93c46_set_di(chip, bit) == 0);
    tick();
    assert(eeprom93c46_set_clk(chip, 1) == 0);
    tick();
    assert(eeprom93c46_set_clk(chip, 0) == 0);
}

static void command_prefix(unsigned op, unsigned address)
{
    unsigned command_address = (op << 6) | (address & 63u);
    int bit;
    begin();
    for (bit = 7; bit > 0; --bit)
        bit_out((command_address >> bit) & 1u);
}

static int final_bit(int bit)
{
    tick();
    assert(eeprom93c46_set_di(chip, bit) == 0);
    tick();
    return eeprom93c46_set_clk(chip, 1);
}

static void close_and_lower_clock(void)
{
    end();
    tick();
    assert(eeprom93c46_set_clk(chip, 0) == 0);
}

static void begin(void)
{
    line_cs(1);
    bit_out(1); /* start bit is separate from the command/address word */
}

static void command(unsigned op, unsigned address)
{
    unsigned command_address = (op << 6) | (address & 63u);
    int bit;
    begin();
    for (bit = 7; bit >= 0; --bit)
        bit_out((command_address >> bit) & 1u);
}

static void end(void)
{
    line_cs(0);
}

static void send_word(uint16_t value)
{
    int bit;
    for (bit = 15; bit >= 0; --bit)
        bit_out((value >> bit) & 1u);
}

static uint16_t read_word(unsigned address)
{
    uint16_t value = 0;
    unsigned bit;
    command(2, address);
    for (bit = 0; bit < 16; ++bit) {
        bit_out(0);
        value = (uint16_t)((value << 1) | (unsigned)eeprom93c46_do(chip));
    }
    end();
    return value;
}

static void advance_us(uint64_t duration)
{
    time_ns += duration * 1000u;
    assert(eeprom93c46_advance_to_ns(chip, time_ns) == 0);
}

static void advance_ns(uint64_t duration)
{
    time_ns += duration;
    assert(eeprom93c46_advance_to_ns(chip, time_ns) == 0);
}

static void unlock(void)
{
    command(0, 0x30); /* opcode 00, address high bits 11: EWEN */
    end();
}

static void test_start_timestamp_and_busy(void)
{
    /* An initial CLK edge at the same emulated timestamp as CS rising is
       deliberately ignored by MAME's CS-edge-time guard. */
    tick();
    assert(eeprom93c46_set_cs(chip, 1) == 0);
    assert(eeprom93c46_set_di(chip, 1) == 0);
    assert(eeprom93c46_set_clk(chip, 1) == 0);
    assert(eeprom93c46_set_clk(chip, 0) == 0);
    tick(); /* Same serial start bit, now on a later nanosecond edge. */
    assert(eeprom93c46_set_clk(chip, 1) == 0);
    tick();
    assert(eeprom93c46_set_clk(chip, 0) == 0);
    {
        unsigned command_address = 2u << 6 | 37u;
        int bit;
        uint16_t actual = 0;
        for (bit = 7; bit >= 0; --bit)
            bit_out((command_address >> bit) & 1u);
        for (bit = 0; bit < 16; ++bit) {
            bit_out(0);
            actual = (uint16_t)((actual << 1) | (unsigned)eeprom93c46_do(chip));
        }
        assert(actual == (uint16_t)(((75u ^ 0x5au) << 8) | (74u ^ 0x5au)));
    }
    end();

    /* Locked writes are discarded without entering the busy state. */
    command(1, 37);
    send_word(0xa65c);
    assert(!eeprom93c46_busy(chip));
    end();
    assert(read_word(37) == (uint16_t)(((75u ^ 0x5au) << 8) | (74u ^ 0x5au)));

    unlock();
    command(1, 37);
    send_word(0xa65c);
    assert(eeprom93c46_busy(chip));
    assert(eeprom93c46_do(chip) == 1); /* state is completion, DO pull-up */
    end();

    /* Writes land in storage when the serial command completes; READY stays
       low for the source-configured 1.75 ms write cycle. */
    {
        uint8_t image[EEPROM93C46_IMAGE_BYTES];
        assert(eeprom93c46_export_image(chip, image, sizeof(image)) == 0);
        assert(image[74] == 0x5c && image[75] == 0xa6);
    }
    line_cs(1);
    assert(eeprom93c46_do(chip) == 0);
    advance_ns(EEPROM93C46_WRITE_NS - 4);
    assert(eeprom93c46_do(chip) == 0);
    advance_ns(1);
    assert(!eeprom93c46_busy(chip));
    assert(eeprom93c46_do(chip) == 1);
    /* Start a READ while CS remains high, now that the ready gate is open. */
    bit_out(1);
    {
        unsigned command_address = 2u << 6 | 37u;
        int bit;
        for (bit = 7; bit >= 0; --bit)
            bit_out((command_address >> bit) & 1u);
    }
    {
        uint16_t value = 0;
        unsigned bit;
        for (bit = 0; bit < 16; ++bit) {
            bit_out(0);
            value = (uint16_t)((value << 1) | (unsigned)eeprom93c46_do(chip));
        }
        assert(value == 0xa65c);
    }
    end();

    /* EWDS prevents later writes. */
    command(0, 0x00);
    end();
    command(1, 37);
    send_word(0x1234);
    end();
    assert(!eeprom93c46_busy(chip));
    assert(read_word(37) == 0xa65c);
}

static void test_erase_and_all_commands(void)
{
    unlock();
    command(3, 37); /* ERASE */
    assert(eeprom93c46_busy(chip));
    end();
    advance_us(EEPROM93C46_ERASE_US);
    assert(!eeprom93c46_busy(chip));
    assert(read_word(37) == 0xffff);

    command(0, 0x10); /* WRAL */
    send_word(0x0f3c);
    assert(eeprom93c46_busy(chip));
    end();
    advance_us(EEPROM93C46_WRITE_ALL_US);
    assert(read_word(0) == 0x0b18 && read_word(63) == 0x0524);
    assert(read_word(37) == 0x0f3c); /* ERAL address was FF before WRAL */

    command(0, 0x20); /* ERAL */
    assert(eeprom93c46_busy(chip));
    end();
    advance_us(EEPROM93C46_ERASE_ALL_US);
    assert(read_word(0) == 0xffff && read_word(63) == 0xffff);

    /* Reset matches MAME's eeprom_base reset: data survives, busy is cleared. */
    command(1, 2);
    send_word(0x55aa);
    assert(eeprom93c46_busy(chip));
    eeprom93c46_reset(chip);
    assert(!eeprom93c46_busy(chip));
    end();
    assert(read_word(2) == 0x55aa);
}

static void test_storage_and_time_guards(void)
{
    uint8_t image[EEPROM93C46_IMAGE_BYTES];
    uint8_t imported[EEPROM93C46_IMAGE_BYTES];
    memset(imported, 0, sizeof(imported));
    imported[0] = 0x34;
    imported[1] = 0x12;
    assert(eeprom93c46_import_image(chip, imported, sizeof(imported)) == 0);
    assert(read_word(0) == 0x1234);
    assert(eeprom93c46_export_image(chip, image, sizeof(image)) == 0);
    assert(memcmp(image, imported, sizeof(image)) == 0);
    command(2, 0);
    {
        unsigned bit;
        uint16_t serial = 0;
        for (bit = 0; bit < 16; ++bit) {
            bit_out(0);
            serial = (uint16_t)((serial << 1) | (unsigned)eeprom93c46_do(chip));
        }
        assert(serial == 0x1234);
        for (bit = 0; bit < 600; ++bit) {
            bit_out(0);
            if (bit >= 16)
                assert(eeprom93c46_do(chip) == 1);
        }
    }
    end();
    assert(eeprom93c46_import_image(chip, imported, sizeof(imported) - 1) == -1);
    assert(eeprom93c46_export_image(chip, image, sizeof(image) - 1) == -1);
    assert(eeprom93c46_advance_to_ns(chip, time_ns - 1) == -1);
    assert(eeprom93c46_now_ns(chip) == time_ns);
    {
        uint64_t next_us = time_ns / 1000u + 1u;
        assert(eeprom93c46_advance_to_us(chip, next_us) == 0);
        time_ns = next_us * 1000u;
        assert(eeprom93c46_now_ns(chip) == time_ns);
        assert(eeprom93c46_now_us(chip) == next_us);
        assert(eeprom93c46_advance_to_us(chip, UINT64_MAX) == -1);
        assert(eeprom93c46_now_ns(chip) == time_ns);
    }
    assert(eeprom93c46_set_clk(NULL, 1) == -1);
}

static void test_reset_preserves_protocol_and_lock(void)
{
    unsigned bit;
    uint16_t actual = 0;
    unlock();
    eeprom93c46_reset(chip); /* EWEN state is retained across device_reset. */
    command(1, 4);
    send_word(0x8042);
    assert(eeprom93c46_busy(chip));
    end();
    advance_us(EEPROM93C46_WRITE_US);

    command(2, 4);
    for (bit = 0; bit < 5; ++bit) {
        bit_out(0);
        actual = (uint16_t)((actual << 1) | (unsigned)eeprom93c46_do(chip));
    }
    tick();
    assert(eeprom93c46_set_clk(chip, 1) == 0); /* leave CLK high across reset */
    actual = (uint16_t)((actual << 1) | (unsigned)eeprom93c46_do(chip));
    eeprom93c46_reset(chip); /* Read state and active CS/CLK pins are retained. */
    {
        int held_do = eeprom93c46_do(chip);
        assert(eeprom93c46_set_clk(chip, 1) == 0); /* no synthetic edge */
        assert(eeprom93c46_do(chip) == held_do);
        tick();
        assert(eeprom93c46_set_clk(chip, 0) == 0);
    }
    for (bit = 6; bit < 16; ++bit) {
        bit_out(0);
        actual = (uint16_t)((actual << 1) | (unsigned)eeprom93c46_do(chip));
    }
    assert(actual == 0x8042);
    end();
    assert(read_word(4) == 0x8042);
}

static void test_deadline_overflow_is_nonmutating(void)
{
    uint8_t before[EEPROM93C46_IMAGE_BYTES];
    uint8_t after[EEPROM93C46_IMAGE_BYTES];
    int bit;
    unlock();
    assert(eeprom93c46_export_image(chip, before, sizeof(before)) == 0);
    time_ns = UINT64_MAX - 1000u;
    assert(eeprom93c46_advance_to_ns(chip, time_ns) == 0);
    command(1, 12);
    for (bit = 15; bit > 0; --bit)
        bit_out((0x5aa5u >> bit) & 1u);
    tick();
    assert(eeprom93c46_set_di(chip, 1) == 0);
    tick();
    assert(eeprom93c46_set_clk(chip, 1) == -2);
    assert(!eeprom93c46_busy(chip));
    assert(eeprom93c46_export_image(chip, after, sizeof(after)) == 0);
    assert(memcmp(before, after, sizeof(before)) == 0);
    close_and_lower_clock();

    /* The other three mutating commands must reject the overflowing deadline
       before touching any EEPROM bytes too. */
    command_prefix(3, 0); /* ERASE */
    assert(final_bit(0) == -2);
    assert(eeprom93c46_export_image(chip, after, sizeof(after)) == 0);
    assert(memcmp(before, after, sizeof(before)) == 0);
    close_and_lower_clock();

    command_prefix(0, 0x10); /* WRAL */
    assert(final_bit(0) == 0);
    tick();
    assert(eeprom93c46_set_clk(chip, 0) == 0);
    for (bit = 15; bit > 0; --bit)
        bit_out((0x5aa5u >> bit) & 1u);
    assert(final_bit(1) == -2);
    assert(eeprom93c46_export_image(chip, after, sizeof(after)) == 0);
    assert(memcmp(before, after, sizeof(before)) == 0);
    close_and_lower_clock();

    command_prefix(0, 0x20); /* ERAL */
    assert(final_bit(0) == -2);
    assert(eeprom93c46_export_image(chip, after, sizeof(after)) == 0);
    assert(memcmp(before, after, sizeof(before)) == 0);
    assert(eeprom93c46_advance_to_ns(chip, UINT64_MAX) == 0);
    assert(eeprom93c46_advance_to_ns(chip, 0) == -1);
}

int main(void)
{
    uint8_t initial[EEPROM93C46_IMAGE_BYTES];
    uint8_t erased[EEPROM93C46_IMAGE_BYTES];
    EEPROM93C46 *blank;
    unsigned i;
    for (i = 0; i < sizeof(initial); ++i)
        initial[i] = (uint8_t)(i ^ 0x5a);
    assert(eeprom93c46_create(initial, sizeof(initial) - 1) == NULL);
    chip = eeprom93c46_create(initial, sizeof(initial));
    assert(chip != NULL);
    blank = eeprom93c46_create(NULL, 0);
    assert(blank != NULL);
    assert(eeprom93c46_export_image(blank, erased, sizeof(erased)) == 0);
    for (i = 0; i < sizeof(erased); ++i)
        assert(erased[i] == 0xff);
    eeprom93c46_destroy(blank);
    time_ns = 0;
    test_start_timestamp_and_busy();
    test_erase_and_all_commands();
    test_storage_and_time_guards();
    test_reset_preserves_protocol_and_lock();
    test_deadline_overflow_is_nonmutating();
    eeprom93c46_destroy(chip);
    assert(eeprom93c46_create(NULL, 1) == NULL);
    puts("93C46 16-bit serial protocol/timing/storage tests PASS");
    return 0;
}
