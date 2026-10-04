#include "../libpcsxcore/arcade/nor_flash.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void wr(nor_flash_t *f, uint32_t address, uint16_t value)
{
    assert(nor_flash_write(f, address, value) == NOR_FLASH_OK);
}

static uint16_t rd(nor_flash_t *f, uint32_t address)
{
    uint16_t value = 0;
    assert(nor_flash_read(f, address, &value) == NOR_FLASH_OK);
    return value;
}

static void amd_unlock(nor_flash_t *f, uint32_t command_address)
{
    wr(f, 0x555, 0xaa);
    wr(f, 0x2aa, 0x55);
    wr(f, command_address, 0x90);
}

static void amd_reset(nor_flash_t *f)
{
    wr(f, 0, 0xf0);
}

static void amd_program(nor_flash_t *f, uint32_t address, uint8_t value)
{
    wr(f, 0x555, 0xaa);
    wr(f, 0x2aa, 0x55);
    wr(f, 0x555, 0xa0);
    wr(f, address, value);
}

static void amd_sector_erase(nor_flash_t *f, uint32_t address)
{
    wr(f, 0x555, 0xaa); wr(f, 0x2aa, 0x55); wr(f, 0x555, 0x80);
    wr(f, 0x555, 0xaa); wr(f, 0x2aa, 0x55); wr(f, address, 0x30);
}

static void test_geometry_and_intel(void)
{
    const size_t size = 0x800000;
    uint8_t *image = (uint8_t *)malloc(size);
    nor_flash_t *f;
    uint16_t value;
    assert(image);
    memset(image, 0x5a, size);
    assert(nor_flash_create(NOR_FLASH_INTEL_28F640J5_X16, image, size - 1) == NULL);
    assert(nor_flash_create(NOR_FLASH_INTEL_28F640J5_X16, NULL, size) == NULL);
    f = nor_flash_create(NOR_FLASH_INTEL_28F640J5_X16, image, size);
    assert(f && nor_flash_bus_width(f) == 16);
    assert(nor_flash_image(f) == image && nor_flash_image_size(f) == size);
    assert(image[0] == 0x5a); /* constructor preserves borrowed image data */
    assert(nor_flash_read(f, (uint32_t)(size / 2), &value) == NOR_FLASH_ERR_RANGE);
    assert(nor_flash_write(f, (uint32_t)(size / 2), 0) == NOR_FLASH_ERR_RANGE);

    wr(f, 0, 0x90);
    assert(rd(f, 0) == 0x89 && rd(f, 1) == 0x15);
    assert(rd(f, 2) == 0 && rd(f, 3) == 0); /* MAME reports lock config clear */
    wr(f, 0, 0xff);
    wr(f, 0, 0x60); wr(f, 0, 0xf1);
    wr(f, 0, 0x90); assert(rd(f, 3) == 1);
    wr(f, 0, 0xff);
    wr(f, 0, 0x60); wr(f, 0, 0xd0);
    wr(f, 0, 0x90); assert(rd(f, 3) == 0);
    wr(f, 0, 0xff);

    /* Intel x16 program writes the following word and then reports ready. */
    image[0x200] = image[0x201] = 0xff;
    wr(f, 0x100, 0x40); wr(f, 0x100, 0x12f0);
    assert(rd(f, 0x100) == 0x80);
    wr(f, 0, 0xff);
    assert(rd(f, 0x100) == 0x12f0);
    wr(f, 0x100, 0x40); wr(f, 0x100, 0xffff);
    wr(f, 0, 0xff);
    assert(rd(f, 0x100) == 0x12f0); /* programming cannot set a zero bit */

    /* 0x20 / 0xd0 erases one 64 KiB block, with a one-second busy window. */
    wr(f, 0x20, 0x20); wr(f, 0x20, 0xd0);
    assert(image[0x40] == 0xff && image[0x41] == 0xff);
    assert(!nor_flash_ready(f));
    wr(f, 0, 0x70); assert(rd(f, 0) == 0);
    nor_flash_advance_us(f, 999999);
    assert(!nor_flash_ready(f) && rd(f, 0) == 0);
    nor_flash_advance_us(f, 1);
    assert(nor_flash_ready(f) && rd(f, 0) == 0x80);
    wr(f, 0, 0xff);

    wr(f, 0, 0x20);
    assert(nor_flash_write(f, 0, 0) == NOR_FLASH_ERR_SEQUENCE);
    assert(nor_flash_write(f, 0, 0x98) == NOR_FLASH_ERR_UNSUPPORTED);
    nor_flash_destroy(f);
    free(image);
}

static void test_amd_profile(nor_flash_profile_t profile, size_t size,
                             uint8_t maker, uint8_t device,
                             uint32_t sector, uint64_t sector_us,
                             int fujitsu_busy_all)
{
    uint8_t *image = (uint8_t *)malloc(size);
    nor_flash_t *f;
    uint16_t value;
    uint32_t target = sector * 2 + 17;
    uint8_t start_status, next_status;
    assert(image);
    memset(image, 0x00, size);
    f = nor_flash_create(profile, image, size);
    assert(f && nor_flash_bus_width(f) == 8);
    assert(nor_flash_manufacturer_id(f) == maker && nor_flash_device_id(f) == device);
    assert(nor_flash_sector_size(f, target) == sector);

    amd_unlock(f, 0x555);
    assert(rd(f, 0) == maker && rd(f, 1) == device && rd(f, 2) == 0);
    amd_reset(f);
    amd_program(f, target, 0x0f);
    assert(image[target] == 0x00); /* initial image had zero bits already */
    image[target] = 0xff;
    amd_program(f, target, 0x0f);
    assert(image[target] == 0x0f);
    amd_program(f, target, 0xff);
    assert(image[target] == 0x0f);

    image[target] = 0;
    image[target + 1] = 0x7e;
    image[0] = 0x11;
    amd_sector_erase(f, target);
    assert(image[target] == 0xff && image[target + 1] == 0xff);
    assert(!nor_flash_ready(f));
    start_status = (uint8_t)rd(f, target);
    next_status = (uint8_t)rd(f, target);
    assert((start_status ^ next_status) == 0x44); /* DQ6/DQ2 toggle */
    assert((rd(f, target - sector) == 0x4c) == !!fujitsu_busy_all);
    nor_flash_advance_us(f, sector_us - 1);
    assert(!nor_flash_ready(f));
    nor_flash_advance_us(f, 1);
    assert(nor_flash_ready(f) && rd(f, target) == 0xff);

    assert(nor_flash_read(f, (uint32_t)size, &value) == NOR_FLASH_ERR_RANGE);
    assert(nor_flash_write(f, (uint32_t)size, 0) == NOR_FLASH_ERR_RANGE);
    assert(nor_flash_write(f, 0, 0x98) == NOR_FLASH_ERR_UNSUPPORTED);

    /* Bad unlock order aborts the sequence and cannot program data. */
    wr(f, 0x555, 0xaa);
    assert(nor_flash_write(f, 0x123, 0x00) == NOR_FLASH_ERR_SEQUENCE);
    assert(image[0x123] == 0x00);

    if (profile == NOR_FLASH_AMD_29F040) {
        image[0] = 0;
        wr(f, 0x555, 0xaa); wr(f, 0x2aa, 0x55); wr(f, 0x555, 0x80);
        wr(f, 0x555, 0xaa); wr(f, 0x2aa, 0x55); wr(f, 0x555, 0x10);
        assert(image[0] == 0xff && !nor_flash_ready(f));
        nor_flash_advance_us(f, 15999999);
        assert(!nor_flash_ready(f));
        nor_flash_advance_us(f, 1);
        assert(nor_flash_ready(f));
    }
    nor_flash_destroy(f);
    free(image);
}

int main(void)
{
    test_geometry_and_intel();
    test_amd_profile(NOR_FLASH_FUJITSU_MBM29F016A, 0x200000,
                     0x04, 0xad, 0x10000, 1000000, 1);
    test_amd_profile(NOR_FLASH_AMD_29F040, 0x80000,
                     0x01, 0xa4, 0x10000, 1000000, 0);
    test_amd_profile(NOR_FLASH_SST_39SF040, 0x80000,
                     0xbf, 0xb7, 0x1000, 125000, 0);
    puts("NOR flash synthetic tests passed");
    return 0;
}
