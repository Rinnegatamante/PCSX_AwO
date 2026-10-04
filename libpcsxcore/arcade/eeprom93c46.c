/* license:BSD-3-Clause; copyright-holders:Aaron Giles */
/* 93C46 16-bit command/state/timing behavior ported from MAME eepromser.cpp
 * and eeprom.cpp. See eeprom93c46_LICENSE.txt for the full license. */
#include "eeprom93c46.h"

#include <stdlib.h>
#include <string.h>

typedef enum {
    STATE_RESET,
    STATE_WAIT_START,
    STATE_WAIT_COMMAND,
    STATE_READ,
    STATE_WAIT_DATA,
    STATE_WAIT_COMPLETION
} EEPROMState;

typedef enum {
    COMMAND_INVALID,
    COMMAND_READ,
    COMMAND_WRITE,
    COMMAND_ERASE,
    COMMAND_LOCK,
    COMMAND_UNLOCK,
    COMMAND_WRITE_ALL,
    COMMAND_ERASE_ALL
} EEPROMCommand;

struct EEPROM93C46 {
    uint8_t image[EEPROM93C46_IMAGE_BYTES];
    uint8_t cs, clk, di, locked;
    EEPROMState state;
    EEPROMCommand command;
    uint32_t bits;
    uint16_t command_address;
    uint8_t address;
    uint32_t shift;
    uint64_t now_ns, cs_rising_ns, busy_until_ns;
};

static uint16_t read_word(const EEPROM93C46 *chip, unsigned address)
{
    unsigned at = (address & 63u) * 2u;
    return (uint16_t)(chip->image[at] | ((uint16_t)chip->image[at + 1] << 8));
}

static void write_word(EEPROM93C46 *chip, unsigned address, uint16_t value)
{
    unsigned at = (address & 63u) * 2u;
    chip->image[at] = (uint8_t)value;
    chip->image[at + 1] = (uint8_t)(value >> 8);
}

static int ready(const EEPROM93C46 *chip)
{
    return chip->now_ns >= chip->busy_until_ns;
}

static void set_state(EEPROM93C46 *chip, EEPROMState state)
{
    chip->state = state;
}

static int deadline_fits(const EEPROM93C46 *chip, uint64_t duration_ns)
{
    return chip->now_ns <= UINT64_MAX - duration_ns;
}

static void start_busy(EEPROM93C46 *chip, uint64_t duration_ns)
{
    chip->busy_until_ns = chip->now_ns + duration_ns;
    set_state(chip, STATE_WAIT_COMPLETION);
}

static int execute_write(EEPROM93C46 *chip)
{
    if (chip->locked) {
        set_state(chip, STATE_RESET);
        return 0;
    }
    if (!deadline_fits(chip, EEPROM93C46_WRITE_NS)) {
        set_state(chip, STATE_RESET);
        return -2;
    }
    write_word(chip, chip->address, (uint16_t)chip->shift);
    start_busy(chip, EEPROM93C46_WRITE_NS);
    return 0;
}

static int execute_write_all(EEPROM93C46 *chip)
{
    unsigned address;
    if (chip->locked) {
        set_state(chip, STATE_RESET);
        return 0;
    }
    if (!deadline_fits(chip, EEPROM93C46_WRITE_ALL_NS)) {
        set_state(chip, STATE_RESET);
        return -2;
    }
    for (address = 0; address < EEPROM93C46_WORDS; ++address)
        write_word(chip, address,
                   (uint16_t)(read_word(chip, address) & (uint16_t)chip->shift));
    start_busy(chip, EEPROM93C46_WRITE_ALL_NS);
    return 0;
}

static void parse_command(EEPROM93C46 *chip)
{
    unsigned op = chip->command_address >> 6;
    unsigned address = chip->command_address & 63u;
    chip->command = COMMAND_INVALID;
    chip->address = (uint8_t)address;
    if (op == 0) {
        switch (address >> 4) {
        case 0: chip->command = COMMAND_LOCK; break;
        case 1: chip->command = COMMAND_WRITE_ALL; break;
        case 2: chip->command = COMMAND_ERASE_ALL; break;
        case 3: chip->command = COMMAND_UNLOCK; break;
        default: break;
        }
        chip->address = 0;
    } else if (op == 1) {
        chip->command = COMMAND_WRITE;
    } else if (op == 2) {
        chip->command = COMMAND_READ;
    } else {
        chip->command = COMMAND_ERASE;
    }
}

static int execute_command(EEPROM93C46 *chip)
{
    parse_command(chip);
    chip->bits = 0;
    switch (chip->command) {
    case COMMAND_READ:
        chip->shift = 0;
        set_state(chip, STATE_READ);
        return 0;
    case COMMAND_WRITE:
    case COMMAND_WRITE_ALL:
        chip->shift = 0;
        set_state(chip, STATE_WAIT_DATA);
        return 0;
    case COMMAND_ERASE:
        if (chip->locked) {
            set_state(chip, STATE_RESET);
            return 0;
        } else {
            if (!deadline_fits(chip, EEPROM93C46_ERASE_NS)) {
                set_state(chip, STATE_RESET);
                return -2;
            }
            write_word(chip, chip->address, 0xffff);
            start_busy(chip, EEPROM93C46_ERASE_NS);
            return 0;
        }
    case COMMAND_LOCK:
        chip->locked = 1;
        set_state(chip, STATE_RESET);
        return 0;
    case COMMAND_UNLOCK:
        chip->locked = 0;
        set_state(chip, STATE_RESET);
        return 0;
    case COMMAND_ERASE_ALL:
        if (chip->locked) {
            set_state(chip, STATE_RESET);
            return 0;
        } else {
            if (!deadline_fits(chip, EEPROM93C46_ERASE_ALL_NS)) {
                set_state(chip, STATE_RESET);
                return -2;
            }
            memset(chip->image, 0xff, sizeof(chip->image));
            start_busy(chip, EEPROM93C46_ERASE_ALL_NS);
            return 0;
        }
    default:
        set_state(chip, STATE_RESET);
        return 0;
    }
}

static int clock_rising(EEPROM93C46 *chip)
{
    switch (chip->state) {
    case STATE_WAIT_START:
        if (chip->di && ready(chip) && chip->now_ns > chip->cs_rising_ns) {
            chip->command_address = 0;
            chip->bits = 0;
            set_state(chip, STATE_WAIT_COMMAND);
        }
        return 0;
    case STATE_WAIT_COMMAND:
        chip->command_address = (uint16_t)((chip->command_address << 1) | chip->di);
        if (++chip->bits == 8) /* two opcode plus six address bits */
            return execute_command(chip);
        return 0;
    case STATE_READ: {
        unsigned bit_index = chip->bits++;
        if (bit_index == 0) /* non-streaming 93C46: load exactly one word */
            chip->shift = (uint32_t)read_word(chip, chip->address) << 16;
        else
            chip->shift = (chip->shift << 1) | 1u;
        return 0;
    }
    case STATE_WAIT_DATA:
        chip->shift = (chip->shift << 1) | chip->di;
        if (++chip->bits == 16) {
            if (chip->command == COMMAND_WRITE)
                return execute_write(chip);
            else if (chip->command == COMMAND_WRITE_ALL)
                return execute_write_all(chip);
            else {
                set_state(chip, STATE_RESET);
                return 0;
            }
        }
        return 0;
    default:
        return 0;
    }
}

static void clock_falling(EEPROM93C46 *chip)
{
    (void)chip;
}

EEPROM93C46 *eeprom93c46_create(const uint8_t *image, size_t image_size)
{
    EEPROM93C46 *chip;
    if ((image == NULL && image_size != 0) ||
        (image != NULL && image_size != EEPROM93C46_IMAGE_BYTES))
        return NULL;
    chip = (EEPROM93C46 *)calloc(1, sizeof(*chip));
    if (chip == NULL)
        return NULL;
    memset(chip->image, 0xff, sizeof(chip->image));
    if (image != NULL)
        memcpy(chip->image, image, sizeof(chip->image));
    chip->locked = 1;
    chip->state = STATE_RESET;
    return chip;
}

void eeprom93c46_destroy(EEPROM93C46 *chip)
{
    free(chip);
}

int eeprom93c46_advance_to_ns(EEPROM93C46 *chip, uint64_t now_ns)
{
    if (chip == NULL || now_ns < chip->now_ns)
        return -1;
    chip->now_ns = now_ns;
    return 0;
}

int eeprom93c46_advance_to_us(EEPROM93C46 *chip, uint64_t now_us)
{
    if (now_us > UINT64_MAX / 1000u)
        return -1;
    return eeprom93c46_advance_to_ns(chip, now_us * 1000u);
}

uint64_t eeprom93c46_now_ns(const EEPROM93C46 *chip)
{
    return chip == NULL ? 0 : chip->now_ns;
}

uint64_t eeprom93c46_now_us(const EEPROM93C46 *chip)
{
    return chip == NULL ? 0 : chip->now_ns / 1000u;
}

int eeprom93c46_set_cs(EEPROM93C46 *chip, int state)
{
    if (chip == NULL)
        return -1;
    state = !!state;
    if (state == chip->cs)
        return 0;
    chip->cs = (uint8_t)state;
    if (state) {
        chip->cs_rising_ns = chip->now_ns;
        if (chip->state == STATE_RESET)
            set_state(chip, STATE_WAIT_START);
    } else if (chip->state == STATE_WAIT_START ||
               chip->state == STATE_WAIT_COMMAND ||
               chip->state == STATE_READ ||
               chip->state == STATE_WAIT_DATA ||
               chip->state == STATE_WAIT_COMPLETION) {
        set_state(chip, STATE_RESET);
    }
    return 0;
}

int eeprom93c46_set_clk(EEPROM93C46 *chip, int state)
{
    uint8_t new_state;
    if (chip == NULL)
        return -1;
    new_state = (uint8_t)!!state;
    if (new_state == chip->clk)
        return 0;
    chip->clk = new_state;
    if (new_state) {
        int result = clock_rising(chip);
        if (result != 0)
            return result;
    } else {
        clock_falling(chip);
    }
    return 0;
}

int eeprom93c46_set_di(EEPROM93C46 *chip, int state)
{
    if (chip == NULL)
        return -1;
    chip->di = (uint8_t)!!state;
    return 0;
}

int eeprom93c46_do(const EEPROM93C46 *chip)
{
    if (chip == NULL)
        return 1;
    if (chip->state == STATE_WAIT_START)
        return ready(chip);
    if (chip->state == STATE_READ)
        return (int)((chip->shift >> 31) & 1u);
    return 1;
}

int eeprom93c46_busy(const EEPROM93C46 *chip)
{
    return chip != NULL && !ready(chip);
}

void eeprom93c46_reset(EEPROM93C46 *chip)
{
    if (chip != NULL)
        chip->busy_until_ns = chip->now_ns;
}

int eeprom93c46_export_image(const EEPROM93C46 *chip, uint8_t *image,
                             size_t image_size)
{
    if (chip == NULL || image == NULL || image_size != EEPROM93C46_IMAGE_BYTES)
        return -1;
    memcpy(image, chip->image, sizeof(chip->image));
    return 0;
}

int eeprom93c46_import_image(EEPROM93C46 *chip, const uint8_t *image,
                             size_t image_size)
{
    if (chip == NULL || image == NULL || image_size != EEPROM93C46_IMAGE_BYTES)
        return -1;
    memcpy(chip->image, image, sizeof(chip->image));
    return 0;
}
