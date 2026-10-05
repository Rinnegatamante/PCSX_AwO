/*
 * Namco System 12 base-board profiles for the interpreter harness.
 * Board mapping/banking/DMA derived from MAME namcos12.cpp by smf,
 * BSD-3-Clause. See THIRD_PARTY_NOTICES.md for the retained license.
 * New integration code: GPL-2.0-or-later, as the surrounding PCSX core.
 *
 * RTC/settings serial wiring and JAMMA conversion are adapted from MAME;
 * shared RAM exchanges are produced by the actual H8 ROM in the interpreter.
 */
#include "system12.h"
#include "system12_profile.h"
#include "system12_audio_clock.h"
#include "system10.h"
#include "system11.h"
#include "systemtaito.h"
#include "systemzn.h"
#include "psxmem.h"
#include "psxhw.h"
#include "psxdma.h"
#include "psxinterpreter.h"
#include "plugins.h"
#include <inttypes.h>
#include "arcade/h8.h"
#include "arcade/c352.h"
#include "gpu_vram.h"
#include "arcade/serial_nvram.h"
#include "arcade/at28c16.h"
#include <errno.h>

#ifndef DRC_DISABLE
#include "new_dynarec/new_dynarec.h"
#endif
#if DISABLE_MEM_LUTS
#error System12 requires memory LUTs for banked instruction fetch
#endif

enum { DEFAULT_MAIN_SIZE = 0x400000, DEFAULT_BANK_SIZE = 0x2000000,
       DEFAULT_SUB_SIZE = 0x80000, DEFAULT_WAVE_SIZE = 0x1000000,
       DEFAULT_RAM_SIZE = 0x400000, WINDOW_SIZE = 0x200000 };
static struct {
    u8 *main, *banked, *sub, *wave;
    size_t main_size, banked_size, sub_size, wave_size;
    u32 ram_size;
    u8 shared[0x10000];
    AT28C16 *eeprom;
    H8CPU *h8;
    C352 *pcm;
    rtc4543_serial_t rtc;
    namco_settings_serial_t settings;
    u64 rtc_fraction, eeprom_time_us, eeprom_cycle_fraction;
    int eeprom_clock_error;
    u32 last_cycle;
    u64 h8_fraction, h8_requested, pcm_fraction, pcm_due, pcm_frames;
    u64 pcm_epoch_frames, sub_reads, sub_writes;
    System12AudioClock pcm_h8_clock;
    unsigned pcm_peak;
    FILE *audio_file;
    system12_audio_cb audio_cb;
    void *audio_opaque;
    u64 audio_bytes;
    int audio_error;
    u32 jvs_player[2], jvs_system;
    u8 jvs_coin[2];
    u8 port8, porta, portb;
    u32 dma_offset;
    u32 bank;
    u32 alt_bank_offset, tektagt_dma_offset;
    const char *profile, *machine, *state, *unsupported;
    int boot_hack, alt_bank, tektagt_dma, has_tektagt_dma;
    int profile_owned;
    u64 shared_reads, shared_writes, bank_writes, dma_transfers, dma_bytes;
    u64 unknown_reads, unknown_writes, dma_truncations;
    u32 last_shared_address, last_shared_pc, last_shared_value;
    u32 last_unknown_address, last_unknown_pc;
    int active, booted;
} board;
static void pcm_sync_h8(void);
static system12_bus_trace_cb bus_trace_cb;
static void *bus_trace_opaque;

void system12_set_bus_trace(system12_bus_trace_cb callback, void *opaque)
{
    bus_trace_cb = callback;
    bus_trace_opaque = opaque;
}

static void trace_bus(u32 address, unsigned width, int is_write, u32 value)
{
    if (bus_trace_cb)
        bus_trace_cb(bus_trace_opaque, address, width, is_write, value,
            psxRegs.pc, psxRegs.cycle);
}

static const struct { const char *name; unsigned player, mask; } player_inputs[] = {
    {"p1start", 0, 0x00000080}, {"p1up", 0, 0x00000020},
    {"p1down", 0, 0x00000010}, {"p1left", 0, 0x00000008},
    {"p1right", 0, 0x00000004}, {"p1button1", 0, 0x00000002},
    {"p1button2", 0, 0x00000001}, {"p1button3", 0, 0x00008000},
    {"p1button4", 0, 0x00004000}, {"p1button5", 0, 0x00002000},
    {"p1button6", 0, 0x00001000}, {"p1button7", 0, 0x00000800},
    {"p1button8", 0, 0x00000400},
    {"p2start", 1, 0x00000080}, {"p2up", 1, 0x00000020},
    {"p2down", 1, 0x00000010}, {"p2left", 1, 0x00000008},
    {"p2right", 1, 0x00000004}, {"p2button1", 1, 0x00000002},
    {"p2button2", 1, 0x00000001}, {"p2button3", 1, 0x00008000},
    {"p2button4", 1, 0x00004000}, {"p2button5", 1, 0x00002000},
    {"p2button6", 1, 0x00001000}, {"p2button7", 1, 0x00000800},
    {"p2button8", 1, 0x00000400}
};

static void put_le16(u8 *p, u16 value)
{
    p[0] = value; p[1] = value >> 8;
}

static void put_le32(u8 *p, u32 value)
{
    p[0] = value; p[1] = value >> 8; p[2] = value >> 16; p[3] = value >> 24;
}

static int audio_write_header(FILE *f, u32 data_bytes)
{
    u8 h[44] = {0};
    memcpy(h, "RIFF", 4); put_le32(h + 4, 36 + data_bytes);
    memcpy(h + 8, "WAVEfmt ", 8); put_le32(h + 16, 16);
    put_le16(h + 20, 1); put_le16(h + 22, 2);
    put_le32(h + 24, 88200); put_le32(h + 28, 88200 * 4);
    put_le16(h + 32, 4); put_le16(h + 34, 16);
    memcpy(h + 36, "data", 4); put_le32(h + 40, data_bytes);
    return fseek(f, 0, SEEK_SET) || fwrite(h, 1, sizeof(h), f) != sizeof(h) ? -1 : 0;
}

static int audio_write_samples(const int16_t *samples, unsigned frames)
{
    u8 bytes[256 * 4];
    unsigned i;
    if (!board.audio_file || board.audio_error) return board.audio_error ? -1 : 0;
    if (board.audio_bytes + (u64)frames * 4 > 0xffffffc3u) {
        board.audio_error = 1; return -1;
    }
    for (i = 0; i < frames; i++) {
        put_le16(bytes + i * 4, (u16)samples[i * 4]);
        put_le16(bytes + i * 4 + 2, (u16)samples[i * 4 + 1]);
    }
    if (fwrite(bytes, 4, frames, board.audio_file) != frames) {
        board.audio_error = 1; return -1;
    }
    board.audio_bytes += (u64)frames * 4;
    return 0;
}

int system12_open_audio(const char *path)
{
    FILE *f;
    if (!board.active || !path || board.audio_file) return -1;
    f = fopen(path, "wb+");
    if (!f) return -1;
    board.audio_file = f; board.audio_bytes = 0; board.audio_error = 0;
    if (audio_write_header(f, 0)) {
        fclose(f); board.audio_file = NULL; board.audio_error = 1;
        return -1;
    }
    if (fseek(f, 44, SEEK_SET)) {
        fclose(f); board.audio_file = NULL; board.audio_error = 1;
        return -1;
    }
    return 0;
}

int system12_close_audio(void)
{
    FILE *f = board.audio_file;
    int failed;
    if (!f) return board.audio_error ? -1 : 0;
    failed = board.audio_error || board.audio_bytes > 0xffffffc3u;
    if (!failed && audio_write_header(f, (u32)board.audio_bytes)) failed = 1;
    if (fclose(f)) failed = 1;
    board.audio_file = NULL;
    board.audio_error = failed;
    return failed ? -1 : 0;
}

void system12_set_audio_output(system12_audio_cb callback, void *opaque)
{
    board.audio_cb = callback;
    board.audio_opaque = opaque;
}

/* Deterministic nonzero waveform fixture used only by harness validation. */
void system12_audio_test_fixture(void)
{
    unsigned i;
    if (!board.active || !board.pcm) return;
    memset(board.wave, 0, 64);
    for (i = 0; i < 64; i++) board.wave[i] = 0x40;
    c352_reset(board.pcm);
    c352_write(board.pcm, 0, 0x8080, 0xffff); /* front L/R volume */
    c352_write(board.pcm, 2, 0x8000, 0xffff); /* half sample per frame */
    c352_write(board.pcm, 3, 0x4006, 0xffff); /* key on, filtered loop */
    c352_write(board.pcm, 6, 63, 0xffff);     /* inclusive wave end */
    c352_write(board.pcm, 0x202, 0, 0xffff);  /* execute-key */
}

int system12_set_input(const char *control, int pressed)
{
    unsigned i;
    if (!board.active || !control || (pressed != 0 && pressed != 1)) return -1;
    if (!strcmp(control, "coin1")) board.jvs_coin[0] = pressed;
    else if (!strcmp(control, "coin2")) board.jvs_coin[1] = pressed;
    else if (!strcmp(control, "test")) board.jvs_system = (board.jvs_system & ~0x80u) | ((u32)pressed << 7);
    else if (!strcmp(control, "service")) board.jvs_player[0] = (board.jvs_player[0] & ~0x40u) | ((u32)pressed << 6);
    else for (i = 0; i < sizeof(player_inputs) / sizeof(player_inputs[0]); i++)
        if (!strcmp(control, player_inputs[i].name)) {
            u32 *p = &board.jvs_player[player_inputs[i].player];
            *p = (*p & ~player_inputs[i].mask) | (pressed ? player_inputs[i].mask : 0);
            return 0;
        }
    return (!strcmp(control, "coin1") || !strcmp(control, "coin2") ||
            !strcmp(control, "test") || !strcmp(control, "service")) ? 0 : -1;
}

int system12_eeprom_export(u8 *image, size_t size)
{
    if (!board.active || !board.eeprom) return -1;
    return at28c16_get_image(board.eeprom, image, size);
}

int system12_eeprom_import(const u8 *image, size_t size)
{
    if (!board.active || !board.eeprom) return -1;
    return at28c16_import_image(board.eeprom, image, size);
}

unsigned char system12_jamma_read(unsigned offset)
{
    u32 p1 = board.jvs_player[0], p2 = board.jvs_player[1];
    u32 value;
    switch (offset & 3) {
    case 0: value = ((p2 >> 7 & 1) << 7) | ((p2 >> 15 & 1) << 6) |
            ((p2 >> 0 & 1) << 5) | ((p2 >> 1 & 1) << 4) |
            ((p2 >> 5 & 1) << 3) | ((p2 >> 4 & 1) << 2) |
            ((p2 >> 3 & 1) << 1) | ((p2 >> 2 & 1) << 0); break;
    case 1: value = ((p1 >> 7 & 1) << 7) | ((p1 >> 15 & 1) << 6) |
            ((p1 >> 0 & 1) << 5) | ((p1 >> 1 & 1) << 4) |
            ((p1 >> 5 & 1) << 3) | ((p1 >> 4 & 1) << 2) |
            ((p1 >> 3 & 1) << 1) | ((p1 >> 2 & 1) << 0); break;
    case 2: value = ((p1 >> 6 & 1) << 7) | ((board.jvs_system >> 7 & 1) << 6) |
            ((board.jvs_coin[0] & 1) << 5) | ((board.jvs_coin[1] & 1) << 4) |
            ((p1 >> 11 & 1) << 3) | ((p2 >> 11 & 1) << 2) |
            ((p1 >> 10 & 1) << 1) | ((p2 >> 10 & 1) << 0); break;
    default: value = ((p2 >> 12 & 1) << 7) | ((p2 >> 13 & 1) << 6) |
            ((p2 >> 14 & 1) << 5) | ((p1 >> 12 & 1) << 3) |
            ((p1 >> 13 & 1) << 2) | ((p1 >> 14 & 1) << 1); break;
    }
    return (u8)~value;
}

int system12_active(void) { return board.active; }

static u32 physical(u32 address)
{
    return address >= 0x80000000 && address < 0xc0000000
        ? address & 0x1fffffff : address;
}

#ifndef DRC_DISABLE
static int system12_code_span(unsigned int pc, const unsigned int **words,
    unsigned int *limit)
{
    const u32 address = physical(pc);
    if (!board.active || address < 0x1fa00000 || address >= 0x1fc00000 ||
        (pc & 3) || board.bank >= board.banked_size / WINDOW_SIZE)
        return 0;
    *words = (const unsigned int *)(board.banked + board.bank * WINDOW_SIZE +
        address - 0x1fa00000);
    *limit = pc + (0x1fc00000 - address);
    return 1;
}
#endif

/* The H8 sees big-endian 16-bit words, while the main CPU sees the same
 * shared backing as little-endian halfwords. MAME bus oracle verifies this. */
static u8 sub_read8(void *opaque, u32 address)
{
    (void)opaque;
    if (address < board.sub_size) return board.sub[address];
    if (address >= 0x080000 && address < 0x090000) {
        board.sub_reads++;
        return board.shared[(address - 0x080000) ^ 1];
    }
    if (address >= 0x280000 && address < 0x288000) {
        pcm_sync_h8();
        u16 word = c352_read(board.pcm, (address - 0x280000) / 2);
        return address & 1 ? word : word >> 8;
    }
    /* Default JVS reads are active-low with all controls released. */
    if (address >= 0x300000 && address < 0x300004)
        return system12_jamma_read(address - 0x300000);
    return 0xff;
}

static void sub_write8(void *opaque, u32 address, u8 value)
{
    (void)opaque;
    if (address >= 0x080000 && address < 0x090000) {
        board.shared[(address - 0x080000) ^ 1] = value;
        board.sub_writes++;
    }
    else if (address >= 0x280000 && address < 0x288000) {
        unsigned shift = address & 1 ? 0 : 8;
        pcm_sync_h8();
        c352_write(board.pcm, (address - 0x280000) / 2,
            (u16)value << shift, 0xffu << shift);
    }
}

static u16 sub_read16(void *opaque, u32 address)
{
    return ((u16)sub_read8(opaque, address) << 8) | sub_read8(opaque, address + 1);
}

static void sub_write16(void *opaque, u32 address, u16 value)
{
    if (address >= 0x280000 && address < 0x288000) {
        pcm_sync_h8();
        c352_write(board.pcm, (address - 0x280000) / 2, value, 0xffff);
    } else {
        sub_write8(opaque, address, value >> 8);
        sub_write8(opaque, address + 1, value);
    }
}

static u8 sub_port_read(void *opaque, unsigned port)
{
    (void)opaque;
    if (port == 8) return 0xef; /* sense None */
    if (port == 11) return board.portb | 0x7f;
    return 0xff;
}

static void sub_port_write(void *opaque, unsigned port, u8 value, u8 ddr)
{
    (void)opaque; (void)ddr;
    if (port == 8) board.port8 = value;
    if (port == 10) board.porta = value;
    if (port == 11) board.portb = (board.portb & 0x80) | (value & 0x7f);
    rtc4543_ce_w(&board.rtc, (board.portb & 0x20) && (board.porta & 1));
    namco_settings_ce_w(&board.settings, (board.portb & 0x20) && !(board.porta & 1));
}

/* Byte-level SCI scheduling retains the actual bit order and opposite clock
 * polarities of the board. Pin transitions are grouped at byte completion. */
static void sub_serial_tx(void *opaque, unsigned channel, u8 value)
{
    unsigned bit;
    u8 received = 0;
    (void)opaque;
    if (channel != 1) return; /* No JVS peripheral connected. */
    for (bit = 0; bit < 8; bit++) {
        namco_settings_data_w(&board.settings, (value >> bit) & 1);
        namco_settings_clk_w(&board.settings, 0);
        rtc4543_clk_w(&board.rtc, 1); /* board inverts the SCI clock */
        received |= rtc4543_data_r(&board.rtc) << bit;
        namco_settings_clk_w(&board.settings, 1);
        rtc4543_clk_w(&board.rtc, 0);
    }
    h8_serial_receive(board.h8, 1, received);
}

/* Preserve sub-microsecond time across scheduler calls. Reset re-anchors the
 * PSX cycle counter, while the EEPROM's absolute device clock keeps advancing. */
static int eeprom_advance_cycles(u32 elapsed)
{
    u64 numerator = board.eeprom_cycle_fraction + (u64)elapsed * 1000000;
    u64 delta = numerator / PSXCLK;
    if (board.eeprom_time_us > UINT64_MAX - delta)
        return -1;
    board.eeprom_time_us += delta;
    board.eeprom_cycle_fraction = numerator % PSXCLK;
    return at28c16_advance_to_us(board.eeprom, board.eeprom_time_us);
}

/* One monotonic stream cursor for register catch-up and board catch-up. */
static void pcm_generate_to(u64 target)
{
    while (board.pcm_frames < target) {
        int16_t samples[256 * 4];
        int16_t stereo[256 * 2];
        u64 remaining = target - board.pcm_frames;
        unsigned frames = remaining > 256 ? 256 : (unsigned)remaining, i;
        uint64_t pcm_started = pcsx_profile_begin(PCSX_PROF_PCM, 1);
        c352_generate(board.pcm, samples, frames);
        pcsx_profile_end(PCSX_PROF_PCM, pcm_started);
        for (i = 0; i < frames; i++) {
            stereo[i * 2] = samples[i * 4];
            stereo[i * 2 + 1] = samples[i * 4 + 1];
        }
        uint64_t audio_started = pcsx_profile_begin(PCSX_PROF_BOARD_AUDIO, 1);
        if (board.audio_cb)
            board.audio_cb(board.audio_opaque, stereo, frames);
        pcsx_profile_end(PCSX_PROF_BOARD_AUDIO, audio_started);
        for (i = 0; i < frames * 4; i++) {
            unsigned amplitude = samples[i] < 0 ? -(int)samples[i] : samples[i];
            if (amplitude > board.pcm_peak) board.pcm_peak = amplitude;
        }
        if (audio_write_samples(samples, frames))
            SysPrintf("System12: WAV audio write failed\n");
        board.pcm_frames += frames;
    }
}

static void pcm_sync_h8(void)
{
    u64 target;
    if (system12_audio_clock_advance(&board.pcm_h8_clock,
            h8_current_cycle(board.h8), &target) == 0)
        pcm_generate_to(target);
}

static void sync_to(u32 psx_cycle, int force)
{
    u32 elapsed;
    if (!board.active || !board.h8) return;
    elapsed = psx_cycle - board.last_cycle;
    if (!elapsed || (!force && elapsed < 256)) return;
    board.last_cycle = psx_cycle;
    if (eeprom_advance_cycles(elapsed) && !board.eeprom_clock_error) {
        board.eeprom_clock_error = 1;
        SysPrintf("System12: EEPROM clock overflow or rewind\n");
    }
    board.rtc_fraction += elapsed;
    if (board.rtc_fraction >= PSXCLK) {
        rtc4543_advance_seconds(&board.rtc, board.rtc_fraction / PSXCLK);
        board.rtc_fraction %= PSXCLK;
    }
    board.h8_fraction += (u64)elapsed * 16934400;
    u32 requested = board.h8_fraction / PSXCLK;
    board.h8_requested += requested;
    uint64_t h8_started = pcsx_profile_begin(PCSX_PROF_H8, 1);
    h8_run_cycles(board.h8, requested);
    pcsx_profile_end(PCSX_PROF_H8, h8_started);
    board.h8_fraction %= PSXCLK;
    board.pcm_fraction += (u64)elapsed * c352_sample_rate(board.pcm);
    board.pcm_due += board.pcm_fraction / PSXCLK;
    board.pcm_fraction %= PSXCLK;
    /* MAME's stream includes the sample at the current nonzero time. */
    pcm_generate_to(board.pcm_due + 1);
}

void system12_sync(u32 psx_cycle)
{
    sync_to(psx_cycle, 0);
}

/* Before the main CPU counter is cleared, retain even a sub-quantum interval.
 * This runs at the same quiescent CPU boundary as psxReset, never from a sink. */
void system12_pre_reset(void)
{
    if (board.booted) sync_to(psxRegs.cycle, 1);
}

#ifdef PCSX_SYSTEM12_PROFILE
void system12_profile_counters(uint64_t *cycles, uint64_t *samples)
{
    H8Status state;
    h8_get_status(board.h8, &state);
    *cycles = state.cycles;
    *samples = board.pcm_frames - board.pcm_epoch_frames;
}
#endif

void system12_vblank(int level)
{
    if (!board.active || !board.h8) return;
    system12_sync(psxRegs.cycle);
    board.portb = (board.portb & 0x7f) | (level ? 0x80 : 0);
    h8_set_irq(board.h8, 1, level);
    h8_adc_trigger(board.h8, level);
}

static int load_region(const char *directory, const char *name, size_t size, u8 **out)
{
    char path[4096];
    FILE *f;
    u8 *data;
    if (snprintf(path, sizeof(path), "%s/%s", directory, name) >= sizeof(path))
        return -1;
    f = fopen(path, "rb");
    if (!f) {
        SysPrintf("System12: cannot open %s\n", path);
        return -1;
    }
    data = malloc(size);
    if (!data || fread(data, 1, size, f) != size || fgetc(f) != EOF || ferror(f)) {
        SysPrintf("System12: %s must contain exactly %zu bytes\n", path, size);
        free(data);
        fclose(f);
        return -1;
    }
    fclose(f);
    *out = data;
    return 0;
}

static int parse_decimal(const char *value, size_t *result)
{
    size_t n = 0;
    const unsigned char *p = (const unsigned char *)value;
    if (!*p) return -1;
    while (*p) {
        if (*p < '0' || *p > '9' || n > (SIZE_MAX - (*p - '0')) / 10) return -1;
        n = n * 10 + (*p++ - '0');
    }
    *result = n;
    return 0;
}

static int load_profile(const char *directory)
{
    static const char *const keys[] = {"schema", "mame_set", "machine", "state",
        "ram_size", "main_size", "bank_size", "sub_size", "wave_size",
        "boot_hack", "alt_bank", "tektagt_dma", "unsupported"};
    char path[4096], line[512], *eq;
    char values[13][256] = {{0}};
    unsigned seen = 0, i;
    FILE *f;
    if (snprintf(path, sizeof(path), "%s/board.cfg", directory) >= (int)sizeof(path)) return -1;
    f = fopen(path, "rb");
    if (!f) {
        if (errno != ENOENT) return -1;
        /* Original Soul Calibur harness regions predate board.cfg. */
        board.main_size = DEFAULT_MAIN_SIZE; board.banked_size = DEFAULT_BANK_SIZE;
        board.sub_size = DEFAULT_SUB_SIZE; board.wave_size = DEFAULT_WAVE_SIZE;
        board.ram_size = DEFAULT_RAM_SIZE;
        board.profile = "coh700"; board.machine = "coh700"; board.state = "namcos12_state";
        board.unsupported = "none";
        return 0;
    }
    while (fgets(line, sizeof(line), f)) {
        char *key = line, *value;
        size_t length = strlen(line);
        if (length && line[length - 1] == '\n') line[--length] = 0;
        if (!length || line[0] == '#') continue;
        eq = strchr(line, '=');
        if (!eq || eq == line || !eq[1]) { fclose(f); return -1; }
        *eq = 0; value = eq + 1;
        for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
            if (!strcmp(key, keys[i])) break;
        if (i == sizeof(keys) / sizeof(keys[0]) || (seen & (1u << i)) || strlen(value) >= sizeof(values[i])) {
            fclose(f); return -1;
        }
        strcpy(values[i], value); seen |= 1u << i;
    }
    if (ferror(f)) { fclose(f); return -1; }
    fclose(f);
    if (seen != (1u << (sizeof(keys) / sizeof(keys[0]))) - 1 || strcmp(values[0], "1")) return -1;
    if (!values[1][0] || !values[2][0] || !values[3][0]) return -1;
    {
        size_t sizes[5];
        for (i = 0; i < 5; i++) if (parse_decimal(values[4 + i], &sizes[i])) return -1;
        if ((sizes[0] != 0x400000 && sizes[0] != 0x1000000) || sizes[1] != DEFAULT_MAIN_SIZE ||
            sizes[2] < 0x400000 || sizes[2] > 0x4000000 || (sizes[2] % WINDOW_SIZE) ||
            sizes[3] != DEFAULT_SUB_SIZE || sizes[4] == 0 || sizes[4] > 0x1000000)
            return -1;
        board.ram_size = (u32)sizes[0]; board.main_size = sizes[1];
        board.banked_size = sizes[2]; board.sub_size = sizes[3]; board.wave_size = sizes[4];
    }
    for (i = 9; i < 12; i++) if (strcmp(values[i], "0") && strcmp(values[i], "1")) return -1;
    board.profile_owned = 1;
    board.profile = strdup(values[1]); board.machine = strdup(values[2]);
    board.state = strdup(values[3]); board.unsupported = strdup(values[12]);
    if (!board.profile || !board.machine || !board.state || !board.unsupported) return -1;
    board.boot_hack = values[9][0] == '1';
    board.alt_bank = values[10][0] == '1';
    board.tektagt_dma = values[11][0] == '1';
    if (strcmp(board.unsupported, "none")) {
        SysPrintf("System12: %s requires unimplemented board device(s): %s\n",
            values[1], board.unsupported);
        return -2;
    }
    if ((strcmp(board.machine, "coh700") && strcmp(board.machine, "coh700b") &&
         strcmp(board.machine, "coh716")) ||
        (strcmp(board.state, "namcos12_state") && strcmp(board.state, "namcos12_altbank_state"))) {
        SysPrintf("System12: profile %s/%s requires an unimplemented game-specific board state\n",
            board.machine, board.state);
        return -2;
    }
    if (board.alt_bank != !strcmp(board.state, "namcos12_altbank_state") ||
        board.tektagt_dma ||
        board.boot_hack != (!strcmp(board.machine, "coh700b") || !strcmp(board.machine, "coh716")) ||
        board.ram_size != (!strcmp(board.machine, "coh716") ? 0x1000000 : 0x400000))
        return -1;
    return 0;
}

int system12_load(const char *directory)
{
    int profile_result;
    /* Layout must be selected before the core allocates/maps memory. */
    if (psxRegs.ptrs.psxM || board.active || systemtaito_active() || systemzn_active() || system10_active() || system11_active())
        return -1;
    profile_result = load_profile(directory);
    if (profile_result) {
        system12_unload();
        return profile_result;
    }
    if (load_region(directory, "main.bin", board.main_size, &board.main) ||
        load_region(directory, "banked.bin", board.banked_size, &board.banked) ||
        load_region(directory, "sub.bin", board.sub_size, &board.sub) ||
        load_region(directory, "wave.bin", board.wave_size, &board.wave)) {
        system12_unload();
        return -1;
    }
    H8Config h8config = {0};
    board.pcm = c352_create(board.wave, board.wave_size, 25401600, 288);
    board.eeprom = at28c16_create(NULL, 0);
    h8config.read8 = sub_read8; h8config.write8 = sub_write8;
    h8config.read16 = sub_read16; h8config.write16 = sub_write16;
    h8config.serial_tx = sub_serial_tx;
    h8config.port_read = sub_port_read; h8config.port_write = sub_port_write;
    board.h8 = h8_create(&h8config);
    if (!board.pcm || !board.h8 || !board.eeprom) { system12_unload(); return -1; }
    rtc4543_datetime_t epoch = {2026, 10, 4, 7, 12, 0, 0};
    rtc4543_init(&board.rtc, NULL, NULL, &epoch);
    namco_settings_init(&board.settings, NULL, NULL);
    psxRamSize = board.ram_size;
    psxRomSize = board.main_size;
    if (gpu_set_vram_height(1024)) { system12_unload(); return -1; }
    board.active = 1;
#ifndef DRC_DISABLE
    new_dynarec_set_code_span_provider(system12_code_span);
#endif
    return 0;
}

void system12_unload(void)
{
    /* Call after psxShutdown(): sizes are needed to unmap the core. */
    bus_trace_cb = NULL;
    bus_trace_opaque = NULL;
#ifndef DRC_DISABLE
    new_dynarec_set_code_span_provider(NULL);
#endif
    if (board.audio_file) system12_close_audio();
    h8_destroy(board.h8);
    c352_destroy(board.pcm);
    at28c16_destroy(board.eeprom);
    free(board.main);
    free(board.banked);
    free(board.sub);
    free(board.wave);
    if (board.profile_owned) {
        free((void *)board.profile); free((void *)board.machine);
        free((void *)board.state); free((void *)board.unsupported);
    }
    memset(&board, 0, sizeof(board));
    if (!psxRegs.ptrs.psxM) {
        psxRamSize = 0x200000, psxRomSize = 0x80000;
        gpu_set_vram_height(512);
    }
}

static void map_bank(unsigned bank)
{
    unsigned segment, page;
    const u32 bases[] = { 0x1fa00000, 0x9fa00000, 0xbfa00000 };
#ifndef DRC_DISABLE
    if (board.active && board.bank != bank && psxCpu == &psxRec)
        new_dynarec_invalidate_range_hard(0x1fa00000, 0x1fc00000);
#endif
    board.bank = bank;
    if (DISABLE_MEM_LUTS)
        return;
    for (segment = 0; segment < 3; segment++)
        for (page = 0; page < WINDOW_SIZE; page += 1u << PSXM_SHIFT) {
            u32 address = bases[segment] + page;
            psxRegs.ptrs.memRLUT[address >> PSXM_SHIFT] = bank < board.banked_size / WINDOW_SIZE
                ? (uintptr_t)(board.banked + bank * WINDOW_SIZE) - bases[segment]
                : INVALID_PTR_VAL;
            psxRegs.ptrs.memWLUT[address >> PSXM_SHIFT] = INVALID_PTR_VAL;
        }
}

void system12_mem_reset(void)
{
    memcpy(psxRegs.ptrs.psxR, board.main, board.main_size);
    /* MAME allocates RAM at machine start; driver_reset only repeats the boot
     * copy on the affected boards. A soft reset does not erase either CPU RAM. */
    if (!board.booted) memset(psxRegs.ptrs.psxM, 0, PSX_RAM_SIZE);
    memset(psxRegs.ptrs.psxP, 0xff, 0x10000);
    if (board.boot_hack && board.main_size >= 0x2028c && board.ram_size >= 0x1000c)
        memcpy(psxRegs.ptrs.psxM + 0x10000, board.main + 0x20280, 12);
    map_bank(board.booted ? board.bank : 0);
}

void system12_reset(void)
{
    pcsx_profile_reset();
    if (!board.booted) {
        board.dma_offset = board.alt_bank_offset = board.tektagt_dma_offset = 0;
        board.h8_fraction = board.h8_requested = 0;
        board.pcm_fraction = board.pcm_due = board.pcm_frames = 0;
        memset(&board.pcm_h8_clock, 0, sizeof(board.pcm_h8_clock));
        board.port8 = board.porta = 0; board.portb = 0x50;
        board.jvs_player[0] = board.jvs_player[1] = board.jvs_system = 0;
        board.jvs_coin[0] = board.jvs_coin[1] = 0;
        board.rtc_fraction = 0;
        memset(board.shared, 0, sizeof(board.shared));
        h8_reset(board.h8);
    } else {
        /* MAME clears instruction overshoot at reset, while retaining its
         * budget-based local time. The port's executed-state counter can be
         * ahead, so restart from the absolute requested clock, not that tail. */
        if (h8_soft_reset(board.h8, board.h8_requested))
            SysPrintf("System12: H8 soft reset outside a quiescent boundary\n");
    }
    board.has_tektagt_dma = 0;
    board.last_cycle = psxRegs.cycle;
    board.pcm_epoch_frames = board.pcm_frames;
    board.pcm_peak = 0;
    board.sub_reads = board.sub_writes = 0;
    c352_reset(board.pcm);
    rtc4543_reset(&board.rtc);
    namco_settings_reset(&board.settings);
    board.shared_reads = board.shared_writes = board.bank_writes = 0;
    board.dma_transfers = board.dma_bytes = board.dma_truncations = 0;
    board.unknown_reads = board.unknown_writes = 0;
    board.last_shared_address = board.last_shared_pc = board.last_shared_value = 0;
    board.last_unknown_address = board.last_unknown_pc = 0;
    map_bank(board.bank);
    board.booted = 1;
    psxRegs.pc = 0xbfc00000;
    psxRegs.CP0.n.SR = 0x10600000;
    psxRegs.CP0.n.PRid = 2;
    psxRegs.biosBranchCheck = ~0u;
    psxRegs.biosFuncsHooked = 0;
}

int system12_read(u32 address, unsigned width, u32 *value)
{
    u32 a = physical(address), v = 0;
    unsigned i;
    if (!board.active)
        return 0;
    if (board.tektagt_dma &&
        ((a >= 0x1f700000 && a < 0x1f700004) ||
         (a >= 0x1ff00000 && a < 0x1ff00004))) {
        board.has_tektagt_dma = 1;
        *value = 0;
        trace_bus(a, width, 0, 0);
        return 1;
    }
    /* Non-LUT fallback also permits execution from valid banked ROM. */
    if (DISABLE_MEM_LUTS && a >= 0x1fa00000 && a < 0x1fc00000) {
        for (i = 0; i < width; i++)
            v |= (u32)(board.bank < board.banked_size / WINDOW_SIZE
                ? board.banked[board.bank * WINDOW_SIZE + ((a + i) & (WINDOW_SIZE - 1))]
                : 0xff) << (i * 8);
        *value = v;
        trace_bus(a, width, 0, v);
        return 1;
    }
    if (a < 0x1f000000 || a >= 0x1f800000)
        return 0;
    if (a >= 0x1f080000 && a + width <= 0x1f084000) {
        for (i = 0; i < width; i++)
            v |= (u32)board.shared[a - 0x1f080000 + i] << (i * 8);
        board.shared_reads++;
        board.last_shared_address = a;
        board.last_shared_pc = psxRegs.pc;
        board.last_shared_value = v;
    }
    else if (a >= 0x1f140000 && a + width <= 0x1f141000) {
        /* AT28C16 has an 8-bit data bus on each low byte of a 16-bit lane. */
        for (i = 0; i < width; i++)
            v |= (u32)((a + i) & 1 ? 0xff
                : at28c16_read(board.eeprom, (a + i - 0x1f140000) / 2)) << (i * 8);
    }
    else if (a < 0x1f000004)
        v = 0; /* MAME nopr bank-select register */
    else {
        v = ~0u;
        board.unknown_reads++;
        board.last_unknown_address = a;
        board.last_unknown_pc = psxRegs.pc;
    }
    *value = v;
    trace_bus(a, width, 0, v);
    return 1;
}

int system12_write(u32 address, unsigned width, u32 value)
{
    u32 a = physical(address);
    unsigned i;
    if (!board.active)
        return 0;
    if (board.tektagt_dma && a >= 0x1fb00000 && a < 0x1fb00004) {
        if (!(a & 2)) board.tektagt_dma_offset = value & 0xffff;
        else board.tektagt_dma_offset |= (value & 0xffff) << 16;
        trace_bus(a, width, 1, value);
        return 1;
    }
    if (a < 0x1f000000 || a >= 0x1f800000)
        return 0;
    if (a < 0x1f000002) {
        u32 mask = width == 1 ? 0xff : 0xffff;
        unsigned shift = (a & 1) * 8;
        u32 written = (board.bank & ~(mask << shift)) | ((value & mask) << shift);
        if (board.alt_bank) {
            u16 data = (u16)written;
            if (data & 8) board.alt_bank_offset = (u16)((data - 8) << 2);
            else board.alt_bank_offset = (board.alt_bank_offset & ~7u) | (data & 7);
            map_bank(board.alt_bank_offset);
        }
        else map_bank(written);
        board.bank_writes++;
    }
    else if (a >= 0x1f080000 && a + width <= 0x1f084000) {
        for (i = 0; i < width; i++)
            board.shared[a - 0x1f080000 + i] = value >> (i * 8);
        board.shared_writes++;
        if (board.shared_writes <= 8)
            SysPrintf("System12 shared w%u %08x=%08x pc=%08x\n",
                width * 8, a, value, psxRegs.pc);
    }
    else if (a >= 0x1f140000 && a + width <= 0x1f141000) {
        for (i = 0; i < width; i++)
            if (!((a + i) & 1))
                if (at28c16_write(board.eeprom,
                        (a + i - 0x1f140000) / 2, value >> (i * 8)) < 0 &&
                    !board.eeprom_clock_error) {
                    board.eeprom_clock_error = 1;
                    SysPrintf("System12: EEPROM write rejected\n");
                }
    }
    else if (a >= 0x1f700000 && a < 0x1f710000) {
        /* The low address bits supply the low half of the DMA source. */
        if (board.tektagt_dma) board.tektagt_dma_offset = ((a - 0x1f700000) & ~1u) | ((value & 0xffff) << 16);
        else board.dma_offset = ((a - 0x1f700000) & ~1u) | ((value & 0xffff) << 16);
    }
    else if ((a >= 0x1f008000 && a < 0x1f008004) ||
             (a >= 0x1f010000 && a < 0x1f010004) ||
             (a >= 0x1f018000 && a < 0x1f018004) ||
             (a >= 0x1f1bff08 && a < 0x1f1bff10) ||
             (a >= 0x1f000002 && a < 0x1f000004)) {
        /* MAME nopw windows */
    }
    else {
        board.unknown_writes++;
        board.last_unknown_address = a;
        board.last_unknown_pc = psxRegs.pc;
    }
    trace_bus(a, width, 1, value);
    return 1;
}

void system12_dma5(u32 madr, u32 bcr, u32 chcr)
{
    u64 requested, bytes;
    int special_dma = board.has_tektagt_dma;
    u64 offset = board.has_tektagt_dma ? board.tektagt_dma_offset : board.dma_offset;
    u32 destination = madr & PSX_RAM_MASK;
    int mainrom = (offset & 0x80000000) || psxHu32(0x1000) == 0x1f300000;
    u8 *source = mainrom ? board.main : board.banked;
    u64 source_size = mainrom ? board.main_size : board.banked_size;
    if (!board.active)
        return;
    board.has_tektagt_dma = 0;
    /* Sync 0 counts words; sync 1 counts blocks of words. Linked lists are
     * unsupported for the ROM channel. Completion is synchronous for now. */
    requested = chcr == 0x01000200
        ? (u64)((bcr >> 16) ? (bcr >> 16) : 0x10000) * (bcr & 0xffff) : bcr;
    requested *= 4;
    offset &= mainrom ? 0xffffff : 0x7fffffff;
    offset &= ~1u;
    if (!mainrom && !special_dma && board.alt_bank)
        offset += board.alt_bank_offset * (u64)WINDOW_SIZE;
    destination &= ~3u;
    bytes = requested;
    if (offset >= source_size ||
        (chcr != 0x01000000 && chcr != 0x01000200))
        bytes = 0;
    else {
        if (bytes > source_size - offset) bytes = source_size - offset;
        if (bytes > PSX_RAM_SIZE - destination) bytes = PSX_RAM_SIZE - destination;
        bytes &= ~3ull;
    }
    if (bytes < requested) board.dma_truncations++;
    if (bytes) {
        memcpy(psxRegs.ptrs.psxM + destination, source + offset, bytes);
        psxCpu->Clear(destination, bytes / 4);
    }
    board.dma_transfers++;
    board.dma_bytes += bytes;
    if (board.dma_transfers <= 8)
        SysPrintf("System12 DMA5 %s %08" PRIx64 " -> %08x bytes=%" PRIu64 "/%" PRIu64 " chcr=%08x pc=%08x\n",
            mainrom ? "main" : "banked", offset, destination, bytes, requested, chcr, psxRegs.pc);
    /* MAME's ROM read callback and DMA completion leave MADR unchanged. */
    psxHu32ref(0x10d8) &= SWAPu32(~0x11000000u);
    DMA_INTERRUPT(5);
}

void system12_report(FILE *out)
{
    fprintf(out, "System12: JAMMA=%02x/%02x/%02x/%02x\n",
        system12_jamma_read(0), system12_jamma_read(1),
        system12_jamma_read(2), system12_jamma_read(3));
    fprintf(out, "System12: profile=%s machine=%s state=%s RAM=%u main=%" PRIu64 " banked=%" PRIu64 " sub=%" PRIu64 " wave=%" PRIu64 " bank=%u alt=%u boot_hack=%u\n",
        board.profile ? board.profile : "inactive", board.machine ? board.machine : "-",
        board.state ? board.state : "-", PSX_RAM_SIZE,
        (uint64_t)board.main_size, (uint64_t)board.banked_size,
        (uint64_t)board.sub_size, (uint64_t)board.wave_size,
        board.bank, board.alt_bank, board.boot_hack);
    fprintf(out, "System12: DMA5=%" PRIu64 " bytes=%" PRIu64 " truncations=%" PRIu64 " bank_writes=%" PRIu64 "\n",
        board.dma_transfers, board.dma_bytes, board.dma_truncations, board.bank_writes);
    fprintf(out, "System12: shared reads=%" PRIu64 " writes=%" PRIu64 " last=%08x value=%08x pc=%08x\n",
        board.shared_reads, board.shared_writes, board.last_shared_address,
        board.last_shared_value, board.last_shared_pc);
    fprintf(out, "System12: unmapped reads=%" PRIu64 " writes=%" PRIu64 " last=%08x pc=%08x\n",
        board.unknown_reads, board.unknown_writes, board.last_unknown_address, board.last_unknown_pc);
    H8Status h8state;
    h8_get_status(board.h8, &h8state);
    fprintf(out, "System12: H8 pc=%06x er7=%08x ccr=%02x cycles=%" PRIu64 " instructions=%" PRIu64 " halted=%u illegal=%06x:%04x unsupported=%x shared r=%" PRIu64 " w=%" PRIu64 "\n",
        h8state.pc, h8state.er[7], h8state.ccr, h8state.cycles, h8state.instructions,
        h8state.halted, h8state.illegal_pc, h8state.illegal_opcode,
        h8state.unsupported_peripherals, board.sub_reads, board.sub_writes);
    fprintf(out, "System12: H8 SCI0=%02x/%02x/%02x SCI1=%02x/%02x/%02x IRQ=%02x/%02x/%02x portB=%02x\n",
        h8_read_byte(board.h8, 0xffffb0), h8_read_byte(board.h8, 0xffffb2), h8_read_byte(board.h8, 0xffffb4),
        h8_read_byte(board.h8, 0xffffb8), h8_read_byte(board.h8, 0xffffba), h8_read_byte(board.h8, 0xffffbc),
        h8_read_byte(board.h8, 0xfffff4), h8_read_byte(board.h8, 0xfffff5), h8_read_byte(board.h8, 0xfffff6), board.portb);
    fprintf(out, "System12: C352 rate=%u frames=%" PRIu64 " peak=%u WAV_bytes=%" PRIu64 "\n",
        c352_sample_rate(board.pcm), board.pcm_frames, board.pcm_peak, board.audio_bytes);
    fprintf(out, "System12: settings frames=%" PRIu64 " last=%02x:%02x RTC wirebits=%u\n",
        board.settings.frames_received, board.settings.last_address, board.settings.last_value,
        board.rtc.bit_count);
    fprintf(out, "System12: CPU pc=%08x opcode=%08x ra=%08x sr=%08x cause=%08x epc=%08x epc_opcode=%08x; clock=%u cycle_multiplier=%d timing=%s\n",
        psxRegs.pc, intFakeFetch(psxRegs.pc), psxRegs.GPR.n.ra,
        psxRegs.CP0.n.SR, psxRegs.CP0.n.Cause, psxRegs.CP0.n.EPC,
        intFakeFetch(psxRegs.CP0.n.EPC),
        PSXCLK, Config.cycle_multiplier,
#ifdef PCSX_SYSTEM12_SOURCE_TIMING
        "source-experiment"
#else
        "console"
#endif
    );
}

int system12_selftest(void)
{
    int failures = 0;
    u32 value, first, last;
    unsigned bank;
    AT28C16State saved_eeprom;
    u64 saved_eeprom_time = board.eeprom_time_us;
    u64 saved_eeprom_fraction = board.eeprom_cycle_fraction;
    int saved_eeprom_error = board.eeprom_clock_error;
    int eeprom_state_valid = board.eeprom &&
        at28c16_get_state(board.eeprom, &saved_eeprom) == 0;
#define CHECK(condition) do { if (!(condition)) { \
    SysPrintf("System12 selftest FAIL line %d: %s\n", __LINE__, #condition); failures++; \
} } while (0)
    /* Exercise both CPU endiannesses and byte lanes through the H8 bus. */
    psxMemWrite32(&psxRegs, 0x1f080000, 0x76543210);
    CHECK(h8_read_byte(board.h8, 0x080000) == 0x32);
    CHECK(h8_read_byte(board.h8, 0x080001) == 0x10);
    CHECK(h8_read_byte(board.h8, 0x080002) == 0x76);
    h8_write_byte(board.h8, 0x080002, 0xcd);
    h8_write_byte(board.h8, 0x080003, 0xef);
    CHECK(psxMemRead32(&psxRegs, 0x1f080000) == 0xcdef3210);
    sub_write16(NULL, 0x280000, 0x1234);
    CHECK(sub_read16(NULL, 0x280000) == 0x1234);
    CHECK(board.active && PSX_RAM_SIZE == board.ram_size && PSX_ROM_SIZE == board.main_size);
    if (board.ram_size > DEFAULT_RAM_SIZE) {
        psxMemWrite32(&psxRegs, board.ram_size - 4, 0x5a12c0de);
        CHECK(psxMemRead32(&psxRegs, board.ram_size - 4) == 0x5a12c0de);
    }
    if (board.boot_hack)
        CHECK(!memcmp(psxRegs.ptrs.psxM + 0x10000, board.main + 0x20280, 12));
    CHECK(system12_jamma_read(0) == 0xff && system12_jamma_read(1) == 0xff &&
          system12_jamma_read(2) == 0xff && system12_jamma_read(3) == 0xff);
    CHECK(!system12_set_input("p1start", 1) && system12_jamma_read(1) == 0x7f);
    CHECK(h8_read_byte(board.h8, 0x300001) == 0x7f);
    CHECK(!system12_set_input("service", 1) && system12_jamma_read(2) == 0x7f);
    CHECK(!system12_set_input("test", 1) && system12_jamma_read(2) == 0x3f);
    CHECK(!system12_set_input("coin1", 1) && system12_jamma_read(2) == 0x1f);
    CHECK(!system12_set_input("p1button7", 1) && system12_jamma_read(2) == 0x17);
    CHECK(!system12_set_input("p2button8", 1) && system12_jamma_read(2) == 0x16);
    CHECK(!system12_set_input("p1button6", 1) && system12_jamma_read(3) == 0xf7);
    CHECK(!system12_set_input("p2button4", 1) && system12_jamma_read(3) == 0xd7);
    CHECK(system12_set_input("unknown", 1) < 0);
    board.jvs_player[0] = board.jvs_player[1] = board.jvs_system = 0;
    board.jvs_coin[0] = board.jvs_coin[1] = 0;
    CHECK(intFakeFetch(0xbfc00000) == SWAPu32(*(u32 *)board.main));
    CHECK(psxMemRead32(&psxRegs, 0x1ffffffc) == SWAPu32(*(u32 *)(board.main + board.main_size - 4)));
    /* Upper 2 MiB must not alias lower RAM; all physical/KSEG mirrors agree. */
    psxMemWrite32(&psxRegs, 0x10000, 0x12345678);
    psxMemWrite32(&psxRegs, 0x210000, 0x87654321);
    CHECK(psxMemRead32(&psxRegs, 0x10000) == 0x12345678);
    CHECK(psxMemRead32(&psxRegs, 0x80210000) == 0x87654321);
    CHECK(psxMemRead32(&psxRegs, 0xa0210000) == 0x87654321);
    CHECK(psxMemRead32(&psxRegs, 0x610000) == (board.ram_size == DEFAULT_RAM_SIZE ? 0x87654321 : 0));
    for (bank = 0; bank < board.banked_size / WINDOW_SIZE; bank++) {
        if (board.alt_bank) {
            psxMemWrite16(&psxRegs, 0xbf000000, 8 + ((bank >> 2) & 7));
            if (bank & 3) psxMemWrite16(&psxRegs, 0xbf000000, bank & 3);
        }
        else psxMemWrite16(&psxRegs, 0xbf000000, bank);
        first = SWAPu32(*(u32 *)(board.banked + bank * WINDOW_SIZE));
        last = SWAPu32(*(u32 *)(board.banked + (bank + 1) * WINDOW_SIZE - 4));
        CHECK(psxMemRead32(&psxRegs, 0x1fa00000) == first);
        CHECK(psxMemRead32(&psxRegs, 0x9fbffffc) == last);
        CHECK(intFakeFetch(0xbfa00000) == first);
    }
    if (!board.alt_bank) {
        psxMemWrite16(&psxRegs, 0x1f000000, board.banked_size / WINDOW_SIZE);
        CHECK(psxMemRead32(&psxRegs, 0x1fa00000) == ~0u);
    }
    else {
        psxMemWrite16(&psxRegs, 0x1f000000, 8); /* bankoffset zero */
    }
    psxMemWrite32(&psxRegs, 0x1f080000, 0x76543210);
    CHECK(psxMemRead16(&psxRegs, 0xbf080002) == 0x7654);
    psxMemWrite8(&psxRegs, 0x9f080001, 0xab);
    CHECK(psxMemRead32(&psxRegs, 0x1f080000) == 0x7654ab10);
    {
        int failures_before_eeprom = failures;
        u64 remaining_199 = 199ull * PSXCLK - board.eeprom_cycle_fraction;
        u64 remaining_200 = 200ull * PSXCLK - board.eeprom_cycle_fraction;
        u32 cycles_to_199us = (u32)((remaining_199 + 999999) / 1000000);
        u32 cycles_to_200us = (u32)((remaining_200 + 999999) / 1000000);
        CHECK(eeprom_state_valid);
        psxMemWrite16(&psxRegs, 0x1f140000, 0x00cd);
        CHECK(psxMemRead16(&psxRegs, 0xbf140000) == 0xff4d); /* D7 polling */
        psxMemWrite16(&psxRegs, 0x1f140002, 0x0012); /* ignored while busy */
        CHECK(eeprom_advance_cycles(cycles_to_199us) == 0);
        CHECK(psxMemRead16(&psxRegs, 0x1f140000) == 0xff4d);
        CHECK(at28c16_busy(board.eeprom));
        CHECK(eeprom_advance_cycles(cycles_to_200us - cycles_to_199us) == 0);
        CHECK(!at28c16_busy(board.eeprom));
        CHECK(psxMemRead16(&psxRegs, 0x1f140000) == 0xffcd);
        psxMemWrite16(&psxRegs, 0x1f140002, 0x0012);
        CHECK(psxMemRead16(&psxRegs, 0x1f140002) == 0xff92);
        remaining_199 = 199ull * PSXCLK - board.eeprom_cycle_fraction;
        remaining_200 = 200ull * PSXCLK - board.eeprom_cycle_fraction;
        cycles_to_199us = (u32)((remaining_199 + 999999) / 1000000);
        cycles_to_200us = (u32)((remaining_200 + 999999) / 1000000);
        CHECK(eeprom_advance_cycles(cycles_to_199us) == 0);
        CHECK(at28c16_busy(board.eeprom));
        CHECK(eeprom_advance_cycles(cycles_to_200us - cycles_to_199us) == 0);
        CHECK(psxMemRead32(&psxRegs, 0xbf140000) == 0xff12ffcd);
        if (failures == failures_before_eeprom)
            SysPrintf("System12 EEPROM timing: PASS\n");
    }
    psxMemWrite32(&psxRegs, 0x1f8010f0, 8u << 20);
    psxMemWrite16(&psxRegs, 0x1f700010, 0x8000);
    psxMemWrite32(&psxRegs, 0x1f8010d0, 0x210000);
    psxMemWrite32(&psxRegs, 0x1f8010d4, 0x10004);
    psxMemWrite32(&psxRegs, 0x1f8010d8, 0x01000200);
    CHECK(!memcmp(psxRegs.ptrs.psxM + 0x210000, board.main + 0x10, 16));
    CHECK(!(psxHu32(0x10d8) & 0x01000000));
    CHECK(psxHu32(0x10d0) == 0x210000);
    /* Starting CHCR before enabling DPCR must defer the transfer. */
    psxMemWrite32(&psxRegs, 0x1f8010f0, 0);
    psxMemWrite16(&psxRegs, 0x1f700000, 0x0010);
    psxMemWrite32(&psxRegs, 0x1f8010d0, 0x210020);
    psxMemWrite32(&psxRegs, 0x1f8010d4, 0x10004);
    psxMemWrite32(&psxRegs, 0x1f8010d8, 0x01000200);
    CHECK(psxHu32(0x10d8) & 0x01000000);
    psxMemWrite32(&psxRegs, 0x1f8010f0, 8u << 20);
    CHECK(!memcmp(psxRegs.ptrs.psxM + 0x210020, board.banked + 0x100000, 16));
    if (board.alt_bank && board.banked_size >= 0x600000) {
        static const u8 alt_dma_pattern[16] = "alt-bank-dma-ok!";
        u8 saved[sizeof(alt_dma_pattern)];
        memcpy(saved, board.banked + 0x500000, sizeof(saved));
        memcpy(board.banked + 0x500000, alt_dma_pattern, sizeof(saved));
        psxMemWrite16(&psxRegs, 0xbf000000, 8);
        psxMemWrite16(&psxRegs, 0xbf000000, 2); /* bankoffset = 2 */
        psxMemWrite16(&psxRegs, 0x1f700000, 0x0010); /* DMA byte offset = 1 MiB */
        psxDma5(0x210040, 0x10004, 0x01000200);
        CHECK(!memcmp(psxRegs.ptrs.psxM + 0x210040, alt_dma_pattern, sizeof(saved)));
        memcpy(board.banked + 0x500000, saved, sizeof(saved));
        psxMemWrite16(&psxRegs, 0xbf000000, 8); /* restore bankoffset = 0 */
    }
    /* Bounds truncation and invalid offsets must not overrun either region. */
    psxMemWrite16(&psxRegs, 0x1f70fffc, 0x803f);
    psxDma5(0x3ffffc, 0x10004, 0x01000200);
    CHECK(!memcmp(psxRegs.ptrs.psxM + 0x3ffffc, board.main + board.main_size - 4, 4));
    psxMemWrite16(&psxRegs, 0x1f700000, 0x7fff);
    psxDma5(0x210000, 0x10004, 0x01000200);
    CHECK(board.dma_truncations == 2);
    CHECK(system12_read(0x1f000000, 4, &value) && value == 0);
    /* DMA masks the base to 4 MiB. A zero block count means 65536 blocks. */
    psxMemWrite16(&psxRegs, 0x1f700000, 0x8000);
    psxDma5(0x610000, 1, 0x01000200);
    CHECK(!memcmp(psxRegs.ptrs.psxM + (board.ram_size == DEFAULT_RAM_SIZE ? 0x210000 : 0x610000),
                  board.main, 0x40000));
    /* DMA IRQ propagation uses the existing PSX interrupt controller. */
    psxMemWrite32(&psxRegs, 0x1f8010f4, (1u << 21) | (1u << 23));
    psxMemWrite16(&psxRegs, 0x1f700000, 0x8000);
    psxDma5(0x210000, 0x10004, 0x01000200);
    CHECK(psxHu32(0x10f4) & (1u << 29));
    CHECK(psxHu32(0x1070) & 8);
    /* A linked-list GPU packet in upper RAM must use the 4 MiB address mask
     * in both the core and the rasterizer, rather than aliasing low RAM. */
    psxMemWrite32(&psxRegs, 0x210100, 0x03ffffff);
    psxMemWrite32(&psxRegs, 0x210104, 0x020000ff); // red fill
    psxMemWrite32(&psxRegs, 0x210108, 0);          // x=0,y=0
    psxMemWrite32(&psxRegs, 0x21010c, 0x00010010);// 16x1 pixels
    psxMemWrite32(&psxRegs, 0x1f8010f0, 8u << 8);
    psxMemWrite32(&psxRegs, 0x1f8010a0, 0x210100);
    psxMemWrite32(&psxRegs, 0x1f8010a8, 0x01000401);
    GPU_readStatus();
    GPU_writeData(0xc0000000); // read back two red pixels from VRAM
    GPU_writeData(0);
    GPU_writeData(0x00010002);
    CHECK(GPU_readData() == 0x001f001f);
    SysPrintf("System12 selftest: %s\n", failures ? "FAIL" : "PASS");
    /* Selftest changes guest state: always restore a fresh board boot. */
    if (eeprom_state_valid)
        (void)at28c16_set_state(board.eeprom, &saved_eeprom);
    board.eeprom_time_us = saved_eeprom_time;
    board.eeprom_cycle_fraction = saved_eeprom_fraction;
    board.eeprom_clock_error = saved_eeprom_error;
    GPU_writeStatus(0);
    /* Validation deliberately dirties RAM/banks: restore a cold boot here,
     * rather than pretending that a normal hardware soft reset erases RAM. */
    board.booted = 0;
    psxReset();
    return failures ? -1 : 0;
#undef CHECK
}
