/* GPL-2.0-or-later. Native host wiring for the MAME G-Net / Sony ZN board.
 * Source: MAME deb2f54fe7300e7a192690318beb860159bfd52a, sony/taitogn.cpp.
 * Device implementations retain their upstream BSD-3-Clause notices.
 */
#include "systemgnet.h"
#include "system10.h"
#include "system11.h"
#include "system12.h"
#include "systemtaito.h"
#include "systemzn.h"
#include "psxmem.h"
#include "r3000a.h"
#include "psxhw.h"
#include "arcade/gnet_zoom.h"
#include "arcade/zn_board.h"
#include "arcade/zn_mcu.h"
#include "arcade/psx_sio.h"
#include "gpu_vram.h"
#include <CommonCrypto/CommonDigest.h>
#ifndef DRC_DISABLE
#include "new_dynarec/new_dynarec.h"
#endif
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define GNET_SOURCE_COMMIT "deb2f54fe7300e7a192690318beb860159bfd52a"
#define PSX_CLOCK_HZ 33868800u
#define MN_CLOCK_HZ 6250000u
#define BIOS_BYTES 0x80000u
#define FIRM_BYTES 0x200000u
#define EPROM_BYTES 0x100000u
#define KEY_BYTES 8u
#define EEPROM_BYTES 2080u
#define PATH_BYTES 4096

typedef struct GnetProfile {
    const char *set, *model, *cpu, *cpu_hz, *gpu;
    const char *bios_sha1, *key0_sha1, *key1_sha1, *firm_sha1, *eprom_sha1;
} GnetProfile;

static const GnetProfile profiles[] = {
    {"coh1002t", "ZN1", "CXD8530CQ", "67737600", "CXD8561Q",
     "e18907cf8c6ba54d96edba0a9a00487a90219e0d",
     "96d94f2b06fb7ecd91454969f4959a583ffb9d5a",
     "89981ec68d30729c27d31f820cbfd0c23ed14a03",
     "9ce888bd2a7e575c8ca71d978cb85de22afb5660",
     "bf0b121670df23f2cc64302d9f215e7c81187bbb"},
    {"coh3002t", "ZN2", "CXD8661R", "100000000", "CXD8654Q",
     "0e17fec2286e4e25deb23d40e41ce0986f373d49",
     "2cc02113207a8f0b078152d31ce6503411750e70",
     "9dcba08f10775a9adf2b1f382c947460edd3d239",
     "c6dad60266ce2ff635696bc0d91903c543273559",
     "78361f46fa7186d5058937c86c66247a86b1257f"}
};

typedef struct GnetBoard {
    const GnetProfile *profile;
    uint8_t *bios, *key0, *key1, *firm, *eprom;
    uint8_t *zoomprog, *waves[3];
    gnet_zoom_board *zoom;
    zn_board_t zn;
    ZNMCU *mcu;
    psx_sio_t *sio[2];
    uint8_t inputs[6], dsw;
    int cat_out[2], mcu_out, active, fault, zn_initialized;
    int memory_initialized;
    uint32_t old_ram_size, old_rom_size, last_cycle;
    uint64_t total_cycles, mn_cycles, mn_fraction;
    uint64_t reads, writes, unsupported;
} GnetBoard;
static GnetBoard board;

#ifndef DRC_DISABLE
static int gnet_code_span(unsigned int pc, const unsigned int **words,
                          unsigned int *limit)
{
    (void)pc; (void)words; (void)limit;
    return 0;
}
#endif

static uint32_t physical_address(uint32_t address)
{
    return address >= 0x80000000u && address < 0xc0000000u ?
        address & 0x1fffffffu : address;
}

static const GnetProfile *find_profile(const char *set)
{
    size_t i;
    for (i = 0; i < sizeof(profiles) / sizeof(profiles[0]); ++i)
        if (!strcmp(set, profiles[i].set)) return &profiles[i];
    return NULL;
}

static const char *const config_keys[] = {
    "format", "mame_set", "mame_source_commit", "board_model",
    "cpu_device", "cpu_clock_hz", "gpu_device", "gpu_clock_hz",
    "main_ram_size", "vram_size", "game_execution_verified"
};

static int read_config(const char *directory, const GnetProfile **selected)
{
    char path[PATH_BYTES], line[512], values[11][128] = {{0}};
    uint32_t seen = 0;
    size_t i;
    FILE *file;
    const GnetProfile *p;
    if (snprintf(path, sizeof(path), "%s/board.ini", directory) >= (int)sizeof(path)) return -1;
    file = fopen(path, "r");
    if (!file) { perror(path); return -1; }
    while (fgets(line, sizeof(line), file)) {
        char *equal;
        if (!strchr(line, '\n') && !feof(file)) goto bad;
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0]) continue;
        equal = strchr(line, '=');
        if (!equal || strchr(equal + 1, '=')) goto bad;
        *equal++ = 0;
        for (i = 0; i < sizeof(config_keys) / sizeof(config_keys[0]); ++i)
            if (!strcmp(line, config_keys[i])) break;
        if (i == sizeof(config_keys) / sizeof(config_keys[0]) ||
            (seen & (1u << i)) || !*equal || strlen(equal) >= sizeof(values[i])) goto bad;
        strcpy(values[i], equal);
        seen |= 1u << i;
    }
    if (ferror(file) || seen != (1u << (sizeof(config_keys) / sizeof(config_keys[0]))) - 1) goto bad;
    p = find_profile(values[1]);
    if (!p || strcmp(values[0], "pcsx-aow-gnet-v1") ||
        strcmp(values[2], GNET_SOURCE_COMMIT) || strcmp(values[3], p->model) ||
        strcmp(values[4], p->cpu) || strcmp(values[5], p->cpu_hz) ||
        strcmp(values[6], p->gpu) || strcmp(values[7], "53693175") ||
        strcmp(values[8], "4194304") || strcmp(values[9], "2097152") ||
        strcmp(values[10], "false")) goto bad;
    *selected = p;
    fclose(file);
    return 0;
bad:
    fprintf(stderr, "%s: invalid or unsupported G-Net board.ini\n", path);
    fclose(file);
    return -1;
}

static int read_region(const char *directory, const char *filename, size_t size,
                       const char *sha1, uint8_t **result)
{
    char path[PATH_BYTES];
    unsigned char digest[CC_SHA1_DIGEST_LENGTH];
    char hex[CC_SHA1_DIGEST_LENGTH * 2 + 1];
    static const char digits[] = "0123456789abcdef";
    FILE *file;
    long length;
    size_t i;
    if (snprintf(path, sizeof(path), "%s/%s", directory, filename) >= (int)sizeof(path)) return -1;
    file = fopen(path, "rb");
    if (!file) { perror(path); return -1; }
    if (fseek(file, 0, SEEK_END) || (length = ftell(file)) < 0 ||
        (size_t)length != size || fseek(file, 0, SEEK_SET)) {
        fprintf(stderr, "%s: expected exactly %zu bytes\n", path, size);
        fclose(file); return -1;
    }
    *result = (uint8_t *)malloc(size);
    if (!*result) { fclose(file); return -1; }
    {
        int read_failed = fread(*result, 1, size, file) != size;
        int close_failed = fclose(file) != 0;
        if (read_failed || close_failed) return -1;
    }
    if (!CC_SHA1(*result, (CC_LONG)size, digest)) return -1;
    for (i = 0; i < sizeof(digest); ++i) {
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 15];
    }
    hex[sizeof(hex) - 1] = 0;
    if (strcmp(hex, sha1)) {
        fprintf(stderr, "%s: SHA1 does not match pinned MAME region\n", path);
        return -1;
    }
    return 0;
}

static uint8_t input_read(void *opaque, zn_board_input_t input)
{
    (void)opaque;
    return input >= ZN_INPUT_P1 && input <= ZN_INPUT_P4 ? board.inputs[input] : board.inputs[2 + (input == ZN_INPUT_SERVICE ? 0 : 1)];
}
static uint8_t mcu_dsw(void *opaque) { (void)opaque; return board.dsw; }
static uint8_t mcu_analog(void *opaque) { (void)opaque; return 0xff; }
static uint16_t mcu_trackball(void *opaque) { (void)opaque; return 0; }
static void update_rxd(void)
{
    if (board.sio[0]) psx_sio_write_rxd(board.sio[0], board.cat_out[0] && board.cat_out[1] && board.mcu_out);
}
static void cat_dataout(void *opaque, unsigned chip, int state)
{
    (void)opaque;
    if (chip < 2) board.cat_out[chip] = !!state;
    update_rxd();
}
static void mcu_txd(void *opaque, int state)
{
    (void)opaque; board.mcu_out = !!state; update_rxd();
}
static void mcu_dsr(void *opaque, int state)
{
    (void)opaque;
    if (board.sio[0]) psx_sio_write_dsr(board.sio[0], state);
}
static int advance_zn_at(uint64_t cycles)
{
    uint64_t us = cycles / PSX_CLOCK_HZ * 1000000u +
                  cycles % PSX_CLOCK_HZ * 1000000u / PSX_CLOCK_HZ;
    return zn_board_advance_to_us(&board.zn, us);
}
static void sio_sck(void *opaque, int state)
{
    (void)opaque;
    if (advance_zn_at(psx_sio_elapsed_cycles(board.sio[0])) ||
        zn_board_sio_clock(&board.zn, state)) board.fault = 1;
}
static void sio_txd(void *opaque, int state)
{
    (void)opaque;
    if (advance_zn_at(psx_sio_elapsed_cycles(board.sio[0]))) { board.fault = 1; return; }
    zn_board_sio_tx(&board.zn, state);
}
static void sio_irq(void *opaque, int state)
{
    unsigned port = (unsigned)(uintptr_t)opaque;
    if (state && psxRegs.ptrs.psxH) psxHu32ref(0x1070) |= SWAPu32(1u << (7 + port));
}

static void destroy_devices(void)
{
    unsigned i;
    gnet_zoom_destroy(board.zoom);
    if (board.zn_initialized) zn_board_destroy(&board.zn);
    zn_mcu_destroy(board.mcu);
    for (i = 0; i < 2; ++i) psx_sio_destroy(board.sio[i]);
    free(board.bios); free(board.key0); free(board.key1); free(board.firm); free(board.eprom);
    free(board.zoomprog);
    for (i = 0; i < 3; ++i) free(board.waves[i]);
    memset(&board, 0, sizeof(board));
}

static int create_devices(void)
{
    zn_board_config_t zcfg = {0};
    zn_board_callbacks_t zcb = {0};
    ZNMCUConfig mcfg = {0};
    psx_sio_callbacks_t sio_cb = {0};
    gnet_zoom_config_t gcfg = {0};
    uint8_t keys[2][8];
    unsigned i;
    memset(board.inputs, 0xff, sizeof(board.inputs));
    board.dsw = 0x0f; board.cat_out[0] = board.cat_out[1] = board.mcu_out = 1;
    for (i = 0; i < 2; ++i) {
        sio_cb.opaque = (void *)(uintptr_t)i; sio_cb.irq = sio_irq;
        if (i == 0) { sio_cb.sck = sio_sck; sio_cb.txd = sio_txd; }
        if (psx_sio_create(i ? PSX_SIO1 : PSX_SIO0, &sio_cb, &board.sio[i])) return -1;
    }
    for (i = 0; i < 8; ++i) mcfg.analog[i] = mcu_analog;
    for (i = 0; i < 4; ++i) mcfg.trackball[i] = mcu_trackball;
    mcfg.dsw = mcu_dsw; mcfg.dsr = mcu_dsr; mcfg.txd = mcu_txd;
    if (zn_mcu_create(&mcfg, &board.mcu) != ZN_MCU_OK) return -1;
    memcpy(keys[0], board.key0, 8); memcpy(keys[1], board.key1, 8);
    zcfg.model = board.profile && !strcmp(board.profile->model, "ZN1") ? ZN_BOARD_ZN1 : ZN_BOARD_ZN2;
    zcfg.ram_bytes = 0x400000; zcfg.vram_bytes = 0x200000;
    zcfg.cat702_count = 2; zcfg.cat702_keys = (const uint8_t (*)[8])keys; zcfg.mcu = board.mcu;
    zcb.read_input = input_read; zcb.cat702_dataout = cat_dataout;
    if (!zn_board_init(&board.zn, &zcfg, &zcb)) return -1;
    board.zn_initialized = 1;
    gcfg.firm_flash = board.firm; gcfg.firm_flash_size = FIRM_BYTES;
    gcfg.zoom_program_flash = board.zoomprog; gcfg.zoom_program_flash_size = GNET_ZOOM_PROGRAM_FLASH_BYTES;
    for (i = 0; i < 3; ++i) { gcfg.sample_flash[i] = board.waves[i]; gcfg.sample_flash_size[i] = GNET_FIRM_FLASH_BYTES; }
    gcfg.eprom = board.eprom; gcfg.eprom_size = EPROM_BYTES;
    if (gnet_zoom_create(&gcfg, &board.zoom) != GNET_ZOOM_OK) return -1;
    if (gpu_set_model(2) || gpu_set_vram_height(1024)) return -1;
    board.old_ram_size = psxRamSize; board.old_rom_size = psxRomSize;
    psxRamSize = 0x400000; psxRomSize = BIOS_BYTES;
    board.last_cycle = psxRegs.cycle;
    board.active = 1;
#ifndef DRC_DISABLE
    new_dynarec_set_code_span_provider(gnet_code_span);
#endif
    update_rxd();
    return 0;
}

static int load_region(const char *directory, const char *name, size_t size,
                       const char *hash, uint8_t **data)
{ return read_region(directory, name, size, hash, data); }

int systemgnet_load(const char *directory)
{
    if (!directory || board.active || psxRegs.ptrs.psxM ||
        systemtaito_active() || systemzn_active() || system10_active() ||
        system11_active() || system12_active()) return -1;
    if (read_config(directory, &board.profile)) goto bad;
    if (load_region(directory, "maincpu.bin", BIOS_BYTES, board.profile->bios_sha1, &board.bios) ||
        load_region(directory, "cat702_0.bin", KEY_BYTES, board.profile->key0_sha1, &board.key0) ||
        load_region(directory, "cat702_1.bin", KEY_BYTES, board.profile->key1_sha1, &board.key1) ||
        load_region(directory, "firm.bin", FIRM_BYTES, board.profile->firm_sha1, &board.firm) ||
        load_region(directory, "eprom.bin", EPROM_BYTES, board.profile->eprom_sha1, &board.eprom)) goto bad;
    board.zoomprog = (uint8_t *)malloc(GNET_ZOOM_PROGRAM_FLASH_BYTES);
    for (unsigned i = 0; i < 3; ++i) board.waves[i] = (uint8_t *)malloc(GNET_FIRM_FLASH_BYTES);
    if (!board.zoomprog || !board.waves[0] || !board.waves[1] || !board.waves[2]) goto bad;
    memset(board.zoomprog, 0xff, GNET_ZOOM_PROGRAM_FLASH_BYTES);
    for (unsigned i = 0; i < 3; ++i) memset(board.waves[i], 0xff, GNET_FIRM_FLASH_BYTES);
    if (create_devices()) goto bad;
    return 0;
bad:
    fprintf(stderr, "G-Net: profile load failed; only coh1002t/coh3002t base ROM regions are accepted\n");
    systemgnet_unload();
    return -1;
}

int systemgnet_test_load(void)
{
    static const GnetProfile *unused;
    (void)unused;
    if (board.active || psxRegs.ptrs.psxM || systemtaito_active() || systemzn_active() ||
        system10_active() || system11_active() || system12_active()) return -1;
    board.profile = &profiles[1];
    board.bios = (uint8_t *)malloc(BIOS_BYTES);
    board.key0 = (uint8_t *)malloc(KEY_BYTES); board.key1 = (uint8_t *)malloc(KEY_BYTES);
    board.firm = (uint8_t *)malloc(FIRM_BYTES); board.eprom = (uint8_t *)malloc(EPROM_BYTES);
    board.zoomprog = (uint8_t *)malloc(GNET_ZOOM_PROGRAM_FLASH_BYTES);
    for (unsigned i = 0; i < 3; ++i) board.waves[i] = (uint8_t *)malloc(GNET_FIRM_FLASH_BYTES);
    if (!board.bios || !board.key0 || !board.key1 || !board.firm || !board.eprom ||
        !board.zoomprog || !board.waves[0] || !board.waves[1] || !board.waves[2]) goto bad;
    memset(board.bios, 0xff, BIOS_BYTES);
    /* Real guest code: select G-Net bank 1, read MAME's fixed ID register,
     * and publish the result to PSX RAM. */
    {
        const uint32_t program[] = {
            0x3c081fb4, 0x24090004, 0xa1090000, 0x3c0a1f00,
            0x954b0000, 0x3c0c8000, 0xad8b0104, 0x3c0a1fb7,
            0x954b0000, 0x00000000, 0xad8b0100, 0x1000ffff, 0x00000000
        };
        for (unsigned i = 0; i < sizeof(program) / sizeof(program[0]); ++i) {
            board.bios[i * 4 + 0] = (uint8_t)program[i];
            board.bios[i * 4 + 1] = (uint8_t)(program[i] >> 8);
            board.bios[i * 4 + 2] = (uint8_t)(program[i] >> 16);
            board.bios[i * 4 + 3] = (uint8_t)(program[i] >> 24);
        }
    }
    for (unsigned i = 0; i < KEY_BYTES; ++i) { board.key0[i] = (uint8_t)(i * 17 + 3); board.key1[i] = (uint8_t)(i * 29 + 7); }
    memset(board.firm, 0xff, FIRM_BYTES); memset(board.eprom, 0xff, EPROM_BYTES);
    memset(board.zoomprog, 0xff, GNET_ZOOM_PROGRAM_FLASH_BYTES);
    for (unsigned i = 0; i < 3; ++i) memset(board.waves[i], 0xff, GNET_FIRM_FLASH_BYTES);
    board.waves[0][0] = 0x34; board.waves[0][1] = 0x12;
    if (create_devices()) goto bad;
    return 0;
bad:
    systemgnet_unload();
    return -1;
}

void systemgnet_unload(void)
{
#ifndef DRC_DISABLE
    if (board.active) new_dynarec_set_code_span_provider(NULL);
#endif
    if (board.active) { psxRamSize = board.old_ram_size; psxRomSize = board.old_rom_size; }
    destroy_devices();
    if (!psxRegs.ptrs.psxM && !systemtaito_active() && !systemzn_active() &&
        !system10_active() && !system11_active() && !system12_active()) {
        psxRamSize = 0x200000; psxRomSize = 0x80000;
        gpu_set_model(2); gpu_set_vram_height(512);
    }
}
int systemgnet_active(void) { return board.active; }

void systemgnet_mem_reset(void)
{
    if (!board.active || !psxRegs.ptrs.psxM || !psxRegs.ptrs.psxR) return;
    memcpy(psxRegs.ptrs.psxR, board.bios, BIOS_BYTES);
    if (!board.memory_initialized) memset(psxRegs.ptrs.psxM, 0, PSX_RAM_SIZE);
    memset(psxRegs.ptrs.psxP, 0xff, 0x10000);
    board.memory_initialized = 1;
}
void systemgnet_reset(void)
{
    if (!board.active) return;
    board.last_cycle = psxRegs.cycle; board.fault = 0;
}

int systemgnet_sync(uint32_t cycle)
{
    uint32_t delta;
    uint64_t scaled;
    if (!board.active) return 0;
    if (cycle < board.last_cycle && board.last_cycle - cycle < 0x80000000u) {
        board.fault = 1; return -1;
    }
    delta = cycle - board.last_cycle; board.last_cycle = cycle;
    if (delta > UINT64_MAX - board.total_cycles) { board.fault = 1; return -1; }
    board.total_cycles += delta;
    for (unsigned i = 0; i < 2; ++i)
        if (psx_sio_advance_to_cycle(board.sio[i], board.total_cycles)) board.fault = 1;
    if (advance_zn_at(board.total_cycles)) board.fault = 1;
    scaled = (uint64_t)delta * MN_CLOCK_HZ + board.mn_fraction;
    if (scaled / PSX_CLOCK_HZ > UINT64_MAX - board.mn_cycles) { board.fault = 1; return -1; }
    board.mn_cycles += scaled / PSX_CLOCK_HZ;
    board.mn_fraction = scaled % PSX_CLOCK_HZ;
    if (gnet_zoom_sync_to_cycle(board.zoom, board.mn_cycles) != GNET_ZOOM_OK) board.fault = 1;
    return board.fault ? -1 : 0;
}

static int sio_bus(uint32_t address, unsigned width, uint32_t *value, int write)
{
    uint32_t raw, shift, mask; unsigned port;
    if (address < 0x1f801040u || address >= 0x1f801060u) return 0;
    port = (address - 0x1f801040u) >> 4; shift = (address & 3u) * 8;
    mask = width == 4 ? UINT32_MAX : ((1u << (width * 8)) - 1) << shift;
    if (write) {
        if (psx_sio_write(board.sio[port], (address >> 2) & 3, *value << shift, mask)) board.fault = 1;
    } else {
        if (psx_sio_read(board.sio[port], (address >> 2) & 3, mask, &raw)) { board.fault = 1; raw = UINT32_MAX; }
        *value = (raw & mask) >> shift;
    }
    return 1;
}
static int gnet_map(uint32_t address)
{
    return (address >= 0x1f000000u && address < 0x1f800000u) ||
        address == 0x1fa30000u || address == 0x1fb40000u || address == 0x1fb60000u ||
        address == 0x1fb70000u || (address >= 0x1fb00000u && address < 0x1fb10000u) ||
        address == 0x1fb80000u || address == 0x1fb80002u || address == 0x1fba0000u ||
        address == 0x1fbc0000u || (address >= 0x1fbe0000u && address < 0x1fbe0200u);
}
static int zn_common_map(uint32_t address)
{
    return (address >= 0x1fa00000u && address < 0x1fa00400u) ||
        (address >= 0x1fa10000u && address < 0x1fa10400u) ||
        address == 0x1fa20000u || (address >= 0x1fa30000u && address < 0x1fa30004u) ||
        (address >= 0x1fa40000u && address < 0x1fa40004u) ||
        (address >= 0x1fa51c00u && address < 0x1fa51e00u) ||
        (address >= 0x1fa60000u && address < 0x1fa60002u) ||
        (address >= 0x1faf0000u && address < 0x1faf0800u) ||
        (address >= 0x1fb20000u && address < 0x1fb20008u);
}
int systemgnet_read(uint32_t address, unsigned width, uint32_t *value)
{
    int result; uint32_t physical = physical_address(address);
    if (!board.active || !value || (width != 1 && width != 2 && width != 4) || (physical & (width - 1))) return 0;
    if (!gnet_map(physical) && !zn_common_map(physical) &&
        !(physical >= 0x1f801040u && physical < 0x1f801060u)) return 0;
    if (systemgnet_sync(psxRegs.cycle)) { *value = UINT32_MAX; return -1; }
    if (sio_bus(physical, width, value, 0)) return 1;
    result = gnet_zoom_bus_read(board.zoom, physical, width, value);
    if (result == GNET_ZOOM_UNMAPPED) result = zn_board_read(&board.zn, physical, width, value) ? GNET_ZOOM_OK : GNET_ZOOM_UNMAPPED;
    if (result == GNET_ZOOM_UNSUPPORTED) { board.unsupported++; board.fault = 1; *value = UINT32_MAX; return 1; }
    if (result < 0) { board.fault = 1; *value = UINT32_MAX; return -1; }
    if (result == GNET_ZOOM_OK) board.reads++;
    return result == GNET_ZOOM_OK;
}
int systemgnet_write(uint32_t address, unsigned width, uint32_t value)
{
    int result; uint32_t physical = physical_address(address);
    if (!board.active || (width != 1 && width != 2 && width != 4) || (physical & (width - 1))) return 0;
    if (!gnet_map(physical) && !zn_common_map(physical) &&
        !(physical >= 0x1f801040u && physical < 0x1f801060u)) return 0;
    if (systemgnet_sync(psxRegs.cycle)) return -1;
    if (sio_bus(physical, width, &value, 1)) return 1;
    result = gnet_zoom_bus_write(board.zoom, physical, width, value);
    if (result == GNET_ZOOM_UNMAPPED) result = zn_board_write(&board.zn, physical, width, value) ? GNET_ZOOM_OK : GNET_ZOOM_UNMAPPED;
    if (result == GNET_ZOOM_UNSUPPORTED) { board.unsupported++; board.fault = 1; return 1; }
    if (result < 0) { board.fault = 1; return -1; }
    if (result == GNET_ZOOM_OK) board.writes++;
    return result == GNET_ZOOM_OK;
}

int systemgnet_set_input(const char *name, int pressed)
{
    static const struct { const char *name; unsigned port, mask; } map[] = {
        {"p1_up",0,1},{"p1_down",0,2},{"p1_left",0,4},{"p1_right",0,8},
        {"p1_button1",0,16},{"p1_button2",0,32},{"p1_button3",0,64},
        {"p2_up",1,1},{"p2_down",1,2},{"p2_left",1,4},{"p2_right",1,8},
        {"p2_button1",1,16},{"p2_button2",1,32},{"p2_button3",1,64},
        {"test",2,1},{"service",2,2},{"tilt",2,4},{"p1_start",3,1},
        {"p2_start",3,2},{"coin1",3,16},{"coin2",3,32}
    };
    if (!board.active || !name || (pressed != 0 && pressed != 1)) return -1;
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); ++i) if (!strcmp(name, map[i].name)) {
        if (pressed) board.inputs[map[i].port] &= (uint8_t)~map[i].mask;
        else board.inputs[map[i].port] |= (uint8_t)map[i].mask;
        return 0;
    }
    return -1;
}
int systemgnet_eeprom_import(const unsigned char *image, size_t size)
{ return board.active ? zn_board_eeprom_import(&board.zn, image, size) : -1; }
int systemgnet_eeprom_export(unsigned char *image, size_t size)
{ return board.active ? zn_board_eeprom_export(&board.zn, image, size) : -1; }
int systemgnet_selftest(void) { return board.active ? 0 : -1; }
void systemgnet_report(FILE *out)
{
    if (!out || !board.active) return;
    fprintf(out, "SystemGNet set=%s model=%s ram=4194304 vram=2097152 gpu=type2 BIOS=512KiB "
        "reads=%" PRIu64 " writes=%" PRIu64 " unsupported=%" PRIu64 " bank=%u fault=%u execution_verified=false\n",
        board.profile ? board.profile->set : "fixture", board.profile ? board.profile->model : "ZN2",
        board.reads, board.writes, board.unsupported, gnet_zoom_selected_bank(board.zoom), (unsigned)board.fault);
}
