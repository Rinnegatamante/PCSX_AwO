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
#include "arcade/gnet_storage.h"
#include "psxcommon.h"
#include "arcade/zn_board.h"
#include "arcade/zn_mcu.h"
#include "arcade/psx_sio.h"
#include "gpu_vram.h"
#if defined(VITA) || defined(__vita__)
#include <openssl/sha.h>
#else
#include <CommonCrypto/CommonDigest.h>
#endif
#ifndef DRC_DISABLE
#include "new_dynarec/new_dynarec.h"
#endif
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define GNET_SOURCE_COMMIT "deb2f54fe7300e7a192690318beb860159bfd52a"
#define PSX_CLOCK_HZ PSXCLK
#define MN_CLOCK_HZ 6250000u
#define BIOS_BYTES 0x80000u
#define FIRM_BYTES 0x200000u
#define EPROM_BYTES 0x100000u
#define KEY_BYTES 8u
#define EEPROM_BYTES 2080u
#define PATH_BYTES 4096
enum { GNET_SHA1_BYTES = 20 };

static int gnet_sha1(const void *data, size_t size,
                     unsigned char digest[GNET_SHA1_BYTES])
{
#if defined(VITA) || defined(__vita__)
    return SHA1((const unsigned char *)data, size, digest) ? 0 : -1;
#else
    if (size > UINT32_MAX) return -1;
    return CC_SHA1(data, (CC_LONG)size, digest) ? 0 : -1;
#endif
}

typedef struct GnetBiosRoot {
    const char *set, *model, *macro, *cpu_file, *cpu_crc, *cpu_sha1;
    const char *firm_file, *firm_crc, *firm_sha1;
    const char *key0_file, *key0_crc, *key0_sha1;
    const char *key1_file, *key1_crc, *key1_sha1;
} GnetBiosRoot;
typedef struct GnetBiosOption {
    const char *root, *name, *file, *crc, *sha1;
} GnetBiosOption;
typedef struct GnetProfile {
    const char *set;
    int year;
    const char *description, *parent, *bios_root, *bios_model;
    const char *bios_default, *machine, *input, *state, *init, *rotation;
    const char *manufacturer, *flags, *card, *card_kind, *chd, *chd_sha1;
    const char *metadata_policy;
} GnetProfile;

static const GnetBiosRoot bios_roots[] = {
#define GNET_BIOS_ROOT(...) { __VA_ARGS__ },
#define GNET_BIOS_OPTION(...)
#define GNET_METADATA_TAGS(...)
#define GNET_PROFILE(...)
#include "arcade/gnet_profiles.inc"
#undef GNET_BIOS_ROOT
#undef GNET_BIOS_OPTION
#undef GNET_METADATA_TAGS
#undef GNET_PROFILE
};
static const GnetBiosOption bios_options[] = {
#define GNET_BIOS_ROOT(...)
#define GNET_BIOS_OPTION(...) { __VA_ARGS__ },
#define GNET_METADATA_TAGS(...)
#define GNET_PROFILE(...)
#include "arcade/gnet_profiles.inc"
#undef GNET_BIOS_ROOT
#undef GNET_BIOS_OPTION
#undef GNET_METADATA_TAGS
#undef GNET_PROFILE
};
static const GnetProfile profiles[] = {
#define GNET_BIOS_ROOT(...)
#define GNET_BIOS_OPTION(...)
#define GNET_METADATA_TAGS(...)
#define MACHINE_IS_BIOS_ROOT "MACHINE_IS_BIOS_ROOT"
#define MACHINE_SUPPORTS_SAVE "MACHINE_SUPPORTS_SAVE"
#define GNET_PROFILE(...) { __VA_ARGS__ },
#include "arcade/gnet_profiles.inc"
#undef GNET_BIOS_ROOT
#undef GNET_BIOS_OPTION
#undef GNET_METADATA_TAGS
#undef MACHINE_IS_BIOS_ROOT
#undef MACHINE_SUPPORTS_SAVE
#undef GNET_PROFILE
};

typedef struct GnetBoard {
    const GnetProfile *profile;
    const GnetBiosRoot *bios_root;
    const GnetBiosOption *bios_option;
    uint8_t *bios, *key0, *key1, *firm, *eprom;
    uint8_t *zoomprog, *waves[3];
    gnet_zoom_board *zoom;
    gnet_storage_t *storage;
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
static systemgnet_audio_frame_fn audio_output;
static void *audio_output_opaque;

static void zoom_audio_frame(void *opaque, float left, float right,
                             uint64_t zsg_cycle)
{
    (void)opaque;
    if (audio_output) audio_output(audio_output_opaque, left, right, zsg_cycle);
}

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

static const GnetBiosRoot *find_bios_root(const char *set)
{
    size_t i;
    for (i = 0; i < sizeof(bios_roots) / sizeof(bios_roots[0]); ++i)
        if (!strcmp(set, bios_roots[i].set)) return &bios_roots[i];
    return NULL;
}

static const GnetBiosOption *find_bios_option(const char *root,
                                              const char *name)
{
    size_t i;
    for (i = 0; i < sizeof(bios_options) / sizeof(bios_options[0]); ++i)
        if (!strcmp(root, bios_options[i].root) &&
            !strcmp(name, bios_options[i].name)) return &bios_options[i];
    return NULL;
}

static void hardware_for_root(const char *root, const char **model,
                              const char **cpu, const char **cpu_hz,
                              const char **gpu)
{
    if (!strcmp(root, "coh1002t")) {
        *model = "ZN1"; *cpu = "CXD8530CQ"; *cpu_hz = "67737600";
        *gpu = "CXD8561Q";
    } else {
        *model = "ZN2"; *cpu = "CXD8661R"; *cpu_hz = "100000000";
        *gpu = "CXD8654Q";
    }
}

static const char *const config_keys[] = {
    "format", "mame_set", "mame_source_commit", "board_model",
    "cpu_device", "cpu_clock_hz", "gpu_device", "gpu_clock_hz",
    "main_ram_size", "vram_size", "game_execution_verified",
    "bios_option", "chd_file", "overlay_file"
};

static int safe_media_filename(const char *name)
{
    return name && *name && !strchr(name, '/') && !strchr(name, '\\') &&
           strcmp(name, ".") && strcmp(name, "..") && !strstr(name, "..");
}

static int read_config(const char *directory, const GnetProfile **selected,
                       const GnetBiosRoot **selected_root,
                       const GnetBiosOption **selected_option,
                       char overlay_file[PATH_BYTES])
{
    char path[PATH_BYTES], line[512], values[14][PATH_BYTES] = {{0}};
    uint32_t seen = 0;
    size_t i;
    FILE *file;
    const GnetProfile *p;
    const GnetBiosRoot *root;
    const GnetBiosOption *option = NULL;
    const char *model, *cpu, *cpu_hz, *gpu;
    const char *bios_name;
    {
        int length = snprintf(path, sizeof(path), "%s/board.ini", directory);
        if (length < 0 || length >= (int)sizeof(path)) return -1;
    }
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
    if (ferror(file) || (seen & 0x7ffu) != 0x7ffu) goto bad;
    p = find_profile(values[1]);
    root = p ? find_bios_root(p->bios_root) : NULL;
    if (!p || !root || strcmp(values[0], "pcsx-aow-gnet-v1") ||
        strcmp(values[2], GNET_SOURCE_COMMIT)) goto bad;
    hardware_for_root(root->set, &model, &cpu, &cpu_hz, &gpu);
    if (strcmp(values[3], model) || strcmp(values[4], cpu) ||
        strcmp(values[5], cpu_hz) || strcmp(values[6], gpu) ||
        strcmp(values[7], "53693175") ||
        strcmp(values[8], "4194304") || strcmp(values[9], "2097152") ||
        strcmp(values[10], "false")) goto bad;
    bios_name = (seen & (1u << 11)) ? values[11] : p->bios_default;
    if (!strcmp(root->set, "coh1002t")) {
        if (strcmp(bios_name, "fixed")) goto bad;
    } else {
        option = find_bios_option(root->set, bios_name);
        if (!option) goto bad;
    }
    if (p->card_kind && strcmp(p->card_kind, "NONE")) {
        char expected_chd[PATH_BYTES];
        if (!p->chd || !p->chd_sha1) goto bad;
        {
            int length = snprintf(expected_chd, sizeof(expected_chd),
                                  "%s.chd", p->chd);
            if (length < 0 || length >= (int)sizeof(expected_chd)) goto bad;
        }
        if ((seen & (1u << 12)) &&
            (!safe_media_filename(values[12]) ||
             strcmp(values[12], expected_chd))) goto bad;
        if ((seen & (1u << 13)) && !safe_media_filename(values[13])) goto bad;
        if (seen & (1u << 13))
            strcpy(overlay_file, values[13]);
        else {
            int length = snprintf(overlay_file, PATH_BYTES, "%s.overlay", p->set);
            if (length < 0 || length >= PATH_BYTES) goto bad;
        }
    } else if ((seen & ((1u << 12) | (1u << 13))) != 0) {
        goto bad;
    } else {
        overlay_file[0] = 0;
    }
    *selected = p;
    *selected_root = root;
    *selected_option = option;
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
    unsigned char digest[GNET_SHA1_BYTES];
    char hex[GNET_SHA1_BYTES * 2 + 1];
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
    if (gnet_sha1(*result, size, digest)) return -1;
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

static int destroy_devices(void)
{
    unsigned i;
    if (board.zoom) {
        if (gnet_zoom_try_destroy(board.zoom) != GNET_ZOOM_OK) {
            fprintf(stderr, "G-Net: Zoom board is still in a callback; unload deferred\n");
            return -1;
        }
        board.zoom = NULL;
    }
    if (board.storage) {
        if (gnet_storage_flush(board.storage) != GNET_STORAGE_OK)
            fprintf(stderr, "G-Net: storage overlay flush failed; writes may be lost\n");
        if (gnet_storage_try_destroy(board.storage) != GNET_STORAGE_OK) {
            fprintf(stderr, "G-Net: storage is still in a callback; unload deferred\n");
            return -1;
        }
        board.storage = NULL;
    }
    if (board.zn_initialized) zn_board_destroy(&board.zn);
    zn_mcu_destroy(board.mcu);
    for (i = 0; i < 2; ++i) psx_sio_destroy(board.sio[i]);
    free(board.bios); free(board.key0); free(board.key1); free(board.firm); free(board.eprom);
    free(board.zoomprog);
    for (i = 0; i < 3; ++i) free(board.waves[i]);
    memset(&board, 0, sizeof(board));
    return 0;
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
    zcfg.model = board.bios_root && !strcmp(board.bios_root->set, "coh1002t")
        ? ZN_BOARD_ZN1 : ZN_BOARD_ZN2;
    zcfg.ram_bytes = 0x400000; zcfg.vram_bytes = 0x200000;
    zcfg.cat702_count = 2; zcfg.cat702_keys = (const uint8_t (*)[8])keys; zcfg.mcu = board.mcu;
    zcb.read_input = input_read; zcb.cat702_dataout = cat_dataout;
    if (!zn_board_init(&board.zn, &zcfg, &zcb)) return -1;
    board.zn_initialized = 1;
    gcfg.firm_flash = board.firm; gcfg.firm_flash_size = FIRM_BYTES;
    gcfg.zoom_program_flash = board.zoomprog; gcfg.zoom_program_flash_size = GNET_ZOOM_PROGRAM_FLASH_BYTES;
    for (i = 0; i < 3; ++i) { gcfg.sample_flash[i] = board.waves[i]; gcfg.sample_flash_size[i] = GNET_FIRM_FLASH_BYTES; }
    gcfg.eprom = board.eprom; gcfg.eprom_size = EPROM_BYTES;
    gcfg.audio_frame = zoom_audio_frame;
    gcfg.pc_card = board.storage ? gnet_storage_card_ops(board.storage) : NULL;
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

static int join_path(char output[PATH_BYTES], const char *directory,
                     const char *filename)
{
    int length = snprintf(output, PATH_BYTES, "%s/%s", directory, filename);
    return length < 0 || length >= PATH_BYTES ? -1 : 0;
}

static int decode_sha1(const char *hex, uint8_t output[GNET_SHA1_BYTES])
{
    unsigned i;
    if (!hex || strlen(hex) != GNET_SHA1_BYTES * 2) return -1;
    for (i = 0; i < GNET_SHA1_BYTES; ++i) {
        unsigned hi, lo;
        char a = hex[i * 2], b = hex[i * 2 + 1];
        if (a >= '0' && a <= '9') hi = (unsigned)(a - '0');
        else if (a >= 'a' && a <= 'f') hi = (unsigned)(a - 'a' + 10);
        else return -1;
        if (b >= '0' && b <= '9') lo = (unsigned)(b - '0');
        else if (b >= 'a' && b <= 'f') lo = (unsigned)(b - 'a' + 10);
        else return -1;
        output[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

static gnet_pccard_variant_t card_variant_for_profile(const GnetProfile *profile)
{
    if (!strcmp(profile->card_kind, "TYPE1")) return GNET_PCCARD_TAITO_TYPE1;
    if (!strcmp(profile->card_kind, "TYPE2")) return GNET_PCCARD_TAITO_TYPE2;
    if (!strcmp(profile->card_kind, "CF")) return GNET_PCCARD_TAITO_COMPACT_FLASH;
    return GNET_PCCARD_ATA_FLASH;
}

static int open_profile_storage(const char *directory,
                                const GnetProfile *profile,
                                const char *overlay_file)
{
    char chd_name[PATH_BYTES], chd_path[PATH_BYTES], overlay_path[PATH_BYTES];
    uint8_t expected_sha1[GNET_SHA1_BYTES];
    gnet_storage_config_t config;
    char error[256];
    int result;
    if (!profile || !profile->card_kind || !strcmp(profile->card_kind, "NONE"))
        return 0;
    if (!profile->chd || !profile->chd_sha1 || !overlay_file ||
        snprintf(chd_name, sizeof(chd_name), "%s.chd", profile->chd) < 0 ||
        strlen(profile->chd) + 4 >= sizeof(chd_name) ||
        join_path(chd_path, directory, chd_name) ||
        join_path(overlay_path, directory, overlay_file) ||
        decode_sha1(profile->chd_sha1, expected_sha1))
        return -1;
    memset(&config, 0, sizeof(config));
    config.chd_path = chd_path;
    config.overlay_path = overlay_path;
    config.card_variant = card_variant_for_profile(profile);
    config.expected_sha1 = expected_sha1;
    config.expected_sha1_size = sizeof(expected_sha1);
    result = gnet_storage_open(&config, &board.storage, error, sizeof(error));
    if (result != GNET_STORAGE_OK) {
        fprintf(stderr, "G-Net: cannot open %s PC Card media %s (%d): %s\n",
                profile->set, chd_path, result, error);
        return -1;
    }
    return 0;
}

int systemgnet_load(const char *directory)
{
    char overlay_file[PATH_BYTES];
    if (!directory || board.active || psxRegs.ptrs.psxM ||
        systemtaito_active() || systemzn_active() || system10_active() ||
        system11_active() || system12_active()) return -1;
    if (read_config(directory, &board.profile, &board.bios_root,
                    &board.bios_option, overlay_file)) goto bad;
    if (load_region(directory, "maincpu.bin", BIOS_BYTES, board.bios_root->cpu_sha1, &board.bios) ||
        load_region(directory, "cat702_0.bin", KEY_BYTES, board.bios_root->key0_sha1, &board.key0) ||
        load_region(directory, "cat702_1.bin", KEY_BYTES, board.bios_root->key1_sha1, &board.key1) ||
        load_region(directory, "firm.bin", FIRM_BYTES, board.bios_root->firm_sha1, &board.firm)) goto bad;
    if (board.bios_option) {
        if (load_region(directory, "eprom.bin", EPROM_BYTES,
                        board.bios_option->sha1, &board.eprom)) goto bad;
    } else {
        board.eprom = (uint8_t *)malloc(EPROM_BYTES);
        if (!board.eprom) goto bad;
        memset(board.eprom, 0xff, EPROM_BYTES);
    }
    if (board.profile->card_kind && strcmp(board.profile->card_kind, "NONE") &&
        open_profile_storage(directory, board.profile, overlay_file)) goto bad;
    board.zoomprog = (uint8_t *)malloc(GNET_ZOOM_PROGRAM_FLASH_BYTES);
    for (unsigned i = 0; i < 3; ++i) board.waves[i] = (uint8_t *)malloc(GNET_FIRM_FLASH_BYTES);
    if (!board.zoomprog || !board.waves[0] || !board.waves[1] || !board.waves[2]) goto bad;
    memset(board.zoomprog, 0xff, GNET_ZOOM_PROGRAM_FLASH_BYTES);
    for (unsigned i = 0; i < 3; ++i) memset(board.waves[i], 0xff, GNET_FIRM_FLASH_BYTES);
    if (create_devices()) goto bad;
    return 0;
bad:
    fprintf(stderr, "G-Net: profile load failed; media must match the selected MAME source profile\n");
    systemgnet_unload();
    return -1;
}

int systemgnet_test_load(void)
{
    static const GnetProfile *unused;
    (void)unused;
    if (board.active || psxRegs.ptrs.psxM || systemtaito_active() || systemzn_active() ||
        system10_active() || system11_active() || system12_active()) return -1;
    board.profile = find_profile("coh3002t");
    board.bios_root = find_bios_root("coh3002t");
    board.bios_option = find_bios_option("coh3002t", "v1");
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
    for (unsigned i = 0; i < 3; ++i) memset(board.waves[i], 0, GNET_FIRM_FLASH_BYTES);
    /* Test-only executable MN10200 program which uploads a real TMS57002
     * tone routine through Port 1. Production media loading remains strict
     * and continues to leave the absent program/sample regions erased. */
    {
        size_t p = 0;
        static const uint8_t tms_program[] = {
            0,0,0, 0,0,0, 0xcc,0x05,0x10, 0x01,0x10,0x00, 0xfc,0x40,0x00
        };
        static const uint8_t tms_coefficients[] = {0x10,0x12,0x34,0x56,0x78};
        unsigned i;
#define GNET_TEST_STORE8(v, a) do { \
            const uint8_t instruction[] = {0xf8, (uint8_t)(v), 0, 0xf4, 0x44, \
                (uint8_t)(a), (uint8_t)((a) >> 8), (uint8_t)((a) >> 16)}; \
            memcpy(board.zoomprog + p, instruction, sizeof(instruction)); \
            p += sizeof(instruction); \
        } while (0)
        GNET_TEST_STORE8(0xff, 0xffe1); /* P1 direction = output */
        GNET_TEST_STORE8(3, 0xfe64);    /* release PLOAD and CLOAD */
        GNET_TEST_STORE8(2, 0xfe64);    /* assert PLOAD */
        for (i = 0; i < sizeof(tms_program); ++i) GNET_TEST_STORE8(tms_program[i], 0xc00000);
        GNET_TEST_STORE8(3, 0xfe64);    /* release PLOAD */
        GNET_TEST_STORE8(1, 0xfe64);    /* assert CLOAD */
        for (i = 0; i < sizeof(tms_coefficients); ++i) GNET_TEST_STORE8(tms_coefficients[i], 0xc00000);
        GNET_TEST_STORE8(3, 0xfe64);    /* complete CLOAD */
        board.zoomprog[p++] = 0xea; board.zoomprog[p++] = 0xfe; /* BRA -2 */
#undef GNET_TEST_STORE8
    }
    board.waves[0][0] = 0x34; board.waves[0][1] = 0x12;
    if (create_devices()) goto bad;
    return 0;
bad:
    systemgnet_unload();
    return -1;
}

void systemgnet_unload(void)
{
    const int was_active = board.active;
    const uint32_t old_ram_size = board.old_ram_size;
    const uint32_t old_rom_size = board.old_rom_size;
    if (destroy_devices()) return;
#ifndef DRC_DISABLE
    if (was_active) new_dynarec_set_code_span_provider(NULL);
#endif
    if (was_active) { psxRamSize = old_ram_size; psxRomSize = old_rom_size; }
    if (!psxRegs.ptrs.psxM && !systemtaito_active() && !systemzn_active() &&
        !system10_active() && !system11_active() && !system12_active()) {
        psxRamSize = 0x200000; psxRomSize = 0x80000;
        gpu_set_model(2); gpu_set_vram_height(512);
    }
}
int systemgnet_active(void) { return board.active; }
void systemgnet_set_audio_output(systemgnet_audio_frame_fn callback, void *opaque)
{
    audio_output = callback;
    audio_output_opaque = callback ? opaque : NULL;
}

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
    if (board.storage && gnet_storage_reset(board.storage) != GNET_STORAGE_OK)
        board.fault = 1;
}

static int cycles_to_ns(uint64_t cycles, uint64_t *result)
{
    const uint64_t clock = PSXCLK;
    const uint64_t whole = cycles / clock;
    const uint64_t remainder = cycles % clock;
    if (whole > UINT64_MAX / 1000000000u) return -1;
    *result = whole * 1000000000u + remainder * 1000000000u / clock;
    return 0;
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
    if (board.storage) {
        uint64_t now_ns;
        if (cycles_to_ns(board.total_cycles, &now_ns) ||
            gnet_storage_run_until(board.storage, now_ns) != GNET_STORAGE_OK)
            board.fault = 1;
    }
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
    const char *model, *cpu, *cpu_hz, *gpu;
    if (!out || !board.active) return;
    hardware_for_root(board.bios_root ? board.bios_root->set : "coh3002t",
                      &model, &cpu, &cpu_hz, &gpu);
    (void)cpu; (void)cpu_hz; (void)gpu;
    fprintf(out, "SystemGNet set=%s model=%s ram=4194304 vram=2097152 gpu=type2 BIOS=512KiB "
        "reads=%" PRIu64 " writes=%" PRIu64 " unsupported=%" PRIu64 " bank=%u fault=%u execution_verified=false\n",
        board.profile ? board.profile->set : "fixture", model,
        board.reads, board.writes, board.unsupported, gnet_zoom_selected_bank(board.zoom), (unsigned)board.fault);
    fprintf(out, "SystemGNet media bios=%s option=%s card=%s chd=%s auth_boot=false\n",
        board.bios_root ? board.bios_root->set : "fixture",
        board.bios_option ? board.bios_option->name : "fixed",
        board.profile && board.profile->card_kind ? board.profile->card_kind : "NONE",
        board.profile && board.profile->chd ? board.profile->chd : "none");
    if (board.profile)
        fprintf(out, "SystemGNet source parent=%s bios_model=%s machine=%s input=%s init=%s rotation=%s flags=%s\n",
            board.profile->parent ? board.profile->parent : "none",
            board.profile->bios_model, board.profile->machine,
            board.profile->input, board.profile->init,
            board.profile->rotation, board.profile->flags);
}
