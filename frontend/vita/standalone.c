#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/display.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/dmac.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <kubridge.h>

#include <SDL3/SDL.h>
#include <libretro.h>

#include "rom_selector.h"
#include "livearea_launch.h"
#include "savestates.h"
#include "settings.h"
#include "input_mapping.h"
#include "paths.h"

#define APP_ROOT PCSX_AWO_APP_ROOT
#define BIOS_DIR PCSX_AWO_BIOS_DIR
#define SAVE_DIR PCSX_AWO_SAVES_DIR
#define ROMS_DIR PCSX_AWO_ROMS_DIR
#define COVERS_DIR PCSX_AWO_COVERS_DIR

#define DISPLAY_W 960
#define DISPLAY_H 544
#define DISPLAY_PITCH 960
#define DISPLAY_MEM_SIZE (2 * 1024 * 1024)
#define GAME_W 725
#define GAME_X ((DISPLAY_W - GAME_W) / 2)

#define AUDIO_RATE_IN 44100
#define AUDIO_DEVICE_FRAMES 1024
#define AUDIO_QUEUE_TARGET_FRAMES 2048
#define AUDIO_FRAME_BYTES (sizeof(int16_t) * 2)

static SceUID g_fb_uid[2] = { -1, -1 };
static uint32_t *g_fb[2];
static SceUID g_stage_uid = -1;
static uint32_t *g_stage;
static bool g_stage_alloc_attempted;
static int g_front;
static int g_map_w = -1;
static int g_map_h = -1;
static int g_map_target_w = -1;
static uint16_t g_xmap[DISPLAY_W];
static uint16_t g_ymap[DISPLAY_H];
static uint32_t g_rgb565_lut[65536];
static bool g_rgb565_lut_ready;

static SceCtrlData g_pads[4];
static SceTouchPanelInfo g_rear_panel;
static unsigned g_rear_buttons;
static bool g_rear_touch_enabled;
static bool g_multi_controller_supported;
static unsigned g_connected_players;
static unsigned g_multitap_mode = VITA_MULTITAP_OFF;
static int g_psx_controller_device[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
static bool g_exit_requested;
static bool g_pause_requested;
static bool g_pause_combo_down;
static struct retro_disk_control_ext_callback g_disk_control;
extern int VitaGpu_PresentNeonFrame(const void *data, int width, int height, int pitch);

static SDL_AudioStream *g_audio_stream;
static uint32_t g_audio_put_errors;

static void pacing_wait_tick(SceUInt64 *deadline, SceUInt64 interval)
{
    SceUInt64 now = (SceUInt64)sceKernelGetSystemTimeWide();
    if (!*deadline || now > *deadline + interval || *deadline > now + interval * 2)
        *deadline = now;
    while (now < *deadline) {
        const SceUInt64 remaining = *deadline - now;
        if (remaining > 700) {
            SceUInt64 sleep_us = remaining - 500;
            if (sleep_us > 4000)
                sleep_us = 4000;
            sceKernelDelayThread((SceUInt)sleep_us);
        }
        now = (SceUInt64)sceKernelGetSystemTimeWide();
    }
    *deadline += interval;
}

static SceUInt64 pacing_frame_interval_us(void)
{
    struct retro_system_av_info av;
    retro_get_system_av_info(&av);
    if (av.timing.fps < 1.0)
        return 16667;
    return (SceUInt64)(1000000.0 / av.timing.fps + 0.5);
}

unsigned int _newlib_heap_size_user = 40 * 1024 * 1024;
static SceUID g_vm_block = -1;
extern int _newlib_vm_size_user;

int getVMBlock(void)
{
    int size;
    int ret;
    void *base = NULL;
    SceKernelAllocMemBlockKernelOpt opt;

    if (g_vm_block > 0)
        return g_vm_block;

    size = (_newlib_vm_size_user + 0xfffff) & ~0xfffff;
    memset(&opt, 0, sizeof(opt));
    opt.size = sizeof(opt);
    g_vm_block = kuKernelAllocMemBlock("pcsx-awo-jit", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, size, &opt);
    if (g_vm_block < 0) {
        sceClibPrintf("PCSX AwO: kuKernelAllocMemBlock(JIT) failed: 0x%x\n", g_vm_block);
        return g_vm_block;
    }

    ret = sceKernelGetMemBlockBase(g_vm_block, &base);
    if (ret < 0) {
        sceClibPrintf("PCSX AwO: sceKernelGetMemBlockBase(JIT) failed: 0x%x\n", ret);
        sceKernelFreeMemBlock(g_vm_block);
        g_vm_block = -1;
        return ret;
    }

    ret = kuKernelMemProtect(base, size, KU_KERNEL_PROT_READ | KU_KERNEL_PROT_WRITE | KU_KERNEL_PROT_EXEC);
    if (ret < 0) {
        sceClibPrintf("PCSX AwO: kuKernelMemProtect(JIT RWX) failed: 0x%x\n", ret);
        sceKernelFreeMemBlock(g_vm_block);
        g_vm_block = -1;
        return ret;
    }

    sceClibPrintf("PCSX AwO: JIT kubridge block size=0x%x base=%p uid=0x%x\n", size, base, g_vm_block);
    return g_vm_block;
}

static void vita_log(enum retro_log_level level, const char *fmt, ...)
{
    char buf[768];
    va_list ap;
    (void)level;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    sceClibPrintf("%s", buf);
}

static const char *option_value(const char *key)
{
    return VitaSettings_GetOption(key);
}

static bool disk_control_ready(void)
{
    return g_disk_control.get_num_images && g_disk_control.get_image_index &&
        g_disk_control.get_eject_state && g_disk_control.set_eject_state &&
        g_disk_control.set_image_index;
}

static void read_disc_info(VitaDiscInfo *info)
{
    memset(info, 0, sizeof(*info));
    if (!disk_control_ready())
        return;

    const unsigned count = g_disk_control.get_num_images();
    if (count > VITA_MAX_DISCS)
        return;
    info->count = count;
    info->current = g_disk_control.get_image_index();
    for (unsigned i = 0; i < count; ++i) {
        if (!g_disk_control.get_image_label ||
            !g_disk_control.get_image_label(i, info->labels[i], VITA_DISC_LABEL_SIZE) ||
            !info->labels[i][0])
            snprintf(info->labels[i], VITA_DISC_LABEL_SIZE, "Disc %u", i + 1);
        info->labels[i][VITA_DISC_LABEL_SIZE - 1] = '\0';
    }
}

static bool change_disc(unsigned index)
{
    if (!disk_control_ready() || index >= g_disk_control.get_num_images())
        return false;
    const unsigned previous = g_disk_control.get_image_index();
    if (index == previous)
        return true;

    if (!g_disk_control.get_eject_state() && !g_disk_control.set_eject_state(true))
        return false;
    bool ok = g_disk_control.set_image_index(index);
    if (!ok)
        g_disk_control.set_image_index(previous);
    if (!g_disk_control.set_eject_state(false))
        ok = false;
    sceClibPrintf("PCSX AwO: disc change %u -> %u %s\n", previous + 1, index + 1,
        ok ? "completed" : "failed");
    return ok;
}

static bool environment_cb(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = vita_log;
        return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *(const char **)data = BIOS_DIR;
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = SAVE_DIR;
        return true;
#ifdef RETRO_ENVIRONMENT_GET_CONTENT_DIRECTORY
    case RETRO_ENVIRONMENT_GET_CONTENT_DIRECTORY:
        *(const char **)data = ROMS_DIR;
        return true;
#endif
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = VitaSettings_ConsumeDirty();
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *var = (struct retro_variable *)data;
        var->value = !g_multi_controller_supported &&
            strcmp(var->key, "pcsx_rearmed_multitap") == 0 ?
            "disabled" : option_value(var->key);
        return var->value != NULL;
    }
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return *(const enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;
    case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
        *(unsigned *)data = 0;
        return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        *(unsigned *)data = 0;
        return true;
    case RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION:
        *(unsigned *)data = 1;
        return true;
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE:
        if (!data)
            return false;
        g_disk_control = *(const struct retro_disk_control_ext_callback *)data;
        return disk_control_ready();
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE: {
        const struct retro_disk_control_callback *control =
            (const struct retro_disk_control_callback *)data;
        if (!control)
            return false;
        memset(&g_disk_control, 0, sizeof(g_disk_control));
        g_disk_control.set_eject_state = control->set_eject_state;
        g_disk_control.get_eject_state = control->get_eject_state;
        g_disk_control.get_image_index = control->get_image_index;
        g_disk_control.set_image_index = control->set_image_index;
        g_disk_control.get_num_images = control->get_num_images;
        g_disk_control.replace_image_index = control->replace_image_index;
        g_disk_control.add_image_index = control->add_image_index;
        return disk_control_ready();
    }
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_VARIABLES:
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
    case RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY:
        return true;
    default:
        return false;
    }
}

static uint32_t rgb565_to_abgr8888(uint16_t p)
{
    uint32_t r5 = (p >> 11) & 31;
    uint32_t g6 = (p >> 5) & 63;
    uint32_t b5 = p & 31;
    uint32_t r = (r5 << 3) | (r5 >> 2);
    uint32_t g = (g6 << 2) | (g6 >> 4);
    uint32_t b = (b5 << 3) | (b5 >> 2);
    return 0xff000000u | (b << 16) | (g << 8) | r;
}

static void rebuild_scale_map(unsigned width, unsigned height, int target_width)
{
    int x, y;
    for (x = 0; x < target_width; x++)
        g_xmap[x] = (uint16_t)(((uint64_t)x * width) / target_width);
    for (y = 0; y < DISPLAY_H; y++)
        g_ymap[y] = (uint16_t)(((uint64_t)y * height) / DISPLAY_H);
    g_map_w = (int)width;
    g_map_h = (int)height;
    g_map_target_w = target_width;
}

static void ensure_video_stage(void)
{
    if (g_stage_alloc_attempted)
        return;
    g_stage_alloc_attempted = true;
    g_stage_uid = sceKernelAllocMemBlock("pcsx-awo-cached-stage",
        SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, DISPLAY_MEM_SIZE, NULL);
    if (g_stage_uid >= 0 && sceKernelGetMemBlockBase(g_stage_uid, (void **)&g_stage) >= 0) {
        memset(g_stage, 0, DISPLAY_MEM_SIZE);
        return;
    }
    sceClibPrintf("PCSX AwO: cached video staging unavailable; using direct CDRAM output\n");
    if (g_stage_uid >= 0)
        sceKernelFreeMemBlock(g_stage_uid);
    g_stage_uid = -1;
    g_stage = NULL;
}

static void video_cb(const void *data, unsigned width, unsigned height, size_t pitch)
{
    SceDisplayFrameBuf frame;
    uint32_t *dst;
    int back;
    int y, x;
    const char *aspect = VitaSettings_GetOption("vita_aspect_ratio");
    const int target_width = aspect && strcmp(aspect, "16:9_stretched") == 0 ? DISPLAY_W : GAME_W;
    const int target_x = (DISPLAY_W - target_width) / 2;

    if (!data || width == 0 || height == 0)
        return;

    if (VitaGpu_PresentNeonFrame(data, (int)width, (int)height, (int)pitch))
        return;

    ensure_video_stage();
    if (!g_rgb565_lut_ready) {
        for (int pixel = 0; pixel < 65536; ++pixel)
            g_rgb565_lut[pixel] = rgb565_to_abgr8888((uint16_t)pixel);
        g_rgb565_lut_ready = true;
    }

    if (g_map_w != (int)width || g_map_h != (int)height || g_map_target_w != target_width) {
        if (g_map_target_w != target_width) {
            if (g_stage)
                memset(g_stage, 0, DISPLAY_W * DISPLAY_H * sizeof(uint32_t));
            else {
                memset(g_fb[0], 0, DISPLAY_W * DISPLAY_H * sizeof(uint32_t));
                memset(g_fb[1], 0, DISPLAY_W * DISPLAY_H * sizeof(uint32_t));
            }
        }
        rebuild_scale_map(width, height, target_width);
    }

    back = g_front ^ 1;
    dst = g_stage ? g_stage : g_fb[back];

    for (y = 0; y < DISPLAY_H; y++) {
        uint32_t *out = dst + y * DISPLAY_PITCH + target_x;
        if (y > 0 && g_ymap[y] == g_ymap[y - 1]) {
            memcpy(out, out - DISPLAY_PITCH, target_width * sizeof(*out));
        } else {
            const uint16_t *src = (const uint16_t *)((const uint8_t *)data + (size_t)g_ymap[y] * pitch);
            for (x = 0; x < target_width; x++)
                out[x] = g_rgb565_lut[src[g_xmap[x]]];
        }
    }

    if (g_stage) {
        const size_t frame_bytes = DISPLAY_PITCH * DISPLAY_H * sizeof(uint32_t);
        if (sceDmacMemcpy(g_fb[back], g_stage, frame_bytes) < 0)
            memcpy(g_fb[back], g_stage, frame_bytes);
    }

    frame.size = sizeof(frame);
    frame.base = g_fb[back];
    frame.pitch = DISPLAY_PITCH;
    frame.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    frame.width = DISPLAY_W;
    frame.height = DISPLAY_H;
    sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME);
    sceDisplayWaitVblankStart();
    g_front = back;
}

static size_t audio_batch_cb(const int16_t *data, size_t frames)
{
    if (!g_audio_stream || !data || frames == 0)
        return frames;

    if (!SDL_PutAudioStreamData(g_audio_stream, data, (int)(frames * AUDIO_FRAME_BYTES))) {
        if (g_audio_put_errors < 8u)
            sceClibPrintf("PCSX AwO: SDL_PutAudioStreamData failed: %s\n", SDL_GetError());
        g_audio_put_errors++;
        return frames;
    }

    return frames;
}

static void input_init_rear_touch(void)
{
    SceCtrlPortInfo info;
    if (g_multi_controller_supported &&
        sceCtrlGetControllerPortInfo(&info) >= 0 &&
        info.port[0] != SCE_CTRL_TYPE_PHY)
        return;
    if (sceTouchGetPanelInfo(SCE_TOUCH_PORT_BACK, &g_rear_panel) >= 0 &&
        sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START) >= 0)
        g_rear_touch_enabled = true;
}

static void input_poll_rear_touch(void)
{
    SceTouchData touch;
    g_rear_buttons = 0;
    if (!g_rear_touch_enabled || sceTouchPeek(SCE_TOUCH_PORT_BACK, &touch, 1) <= 0)
        return;
    if (touch.status & 0x00010000u)
        return;
    const unsigned count = touch.reportNum < SCE_TOUCH_MAX_REPORT ?
                           touch.reportNum : SCE_TOUCH_MAX_REPORT;
    for (unsigned i = 0; i < count; ++i) {
        const SceTouchReport *report = &touch.report[i];
        if (report->info & SCE_TOUCH_REPORT_INFO_HIDE_UPPER_LAYER)
            continue;
        // Rear-panel X is mirrored when viewed from the back.
        g_rear_buttons |= vita_rear_trigger_at(report->x, report->y,
            g_rear_panel.minAaX, g_rear_panel.maxAaX,
            g_rear_panel.minAaY, g_rear_panel.maxAaY);
    }
}

static unsigned input_multitap_mode(void)
{
    const char *setting = VitaSettings_GetOption("pcsx_rearmed_multitap");
    if (!g_multi_controller_supported || !setting)
        return VITA_MULTITAP_OFF;
    if (strcmp(setting, "port 1") == 0)
        return VITA_MULTITAP_PORT1;
    if (strcmp(setting, "port 2") == 0)
        return VITA_MULTITAP_PORT2;
    if (strcmp(setting, "ports 1 and 2") == 0)
        return VITA_MULTITAP_BOTH;
    return VITA_MULTITAP_OFF;
}

static void input_neutral_pad(SceCtrlData *pad)
{
    memset(pad, 0, sizeof(*pad));
    pad->lx = pad->ly = pad->rx = pad->ry = 128;
}

static void input_sample_controllers(void)
{
    unsigned connected = 0;
    for (unsigned i = 0; i < 4; ++i)
        input_neutral_pad(&g_pads[i]);

    if (g_multi_controller_supported) {
        SceCtrlPortInfo info;
        if (sceCtrlGetControllerPortInfo(&info) >= 0) {
            for (unsigned i = 0; i < 4; ++i) {
                if (info.port[i + 1] == SCE_CTRL_TYPE_UNPAIRED)
                    continue;
                connected |= 1u << i;
                if (sceCtrlPeekBufferPositiveExt2((int)i + 1, &g_pads[i], 1) <= 0 &&
                    sceCtrlPeekBufferPositive2((int)i + 1, &g_pads[i], 1) <= 0)
                    input_neutral_pad(&g_pads[i]);
            }
            if (!(connected & 1u) && info.port[0] == SCE_CTRL_TYPE_PHY) {
                connected |= 1u;
                if (sceCtrlPeekBufferPositiveExt2(0, &g_pads[0], 1) <= 0 &&
                    sceCtrlPeekBufferPositive(0, &g_pads[0], 1) <= 0)
                    input_neutral_pad(&g_pads[0]);
            }
        } else {
            connected = 1;
            if (sceCtrlPeekBufferPositiveExt2(0, &g_pads[0], 1) <= 0 &&
                sceCtrlPeekBufferPositive(0, &g_pads[0], 1) <= 0)
                input_neutral_pad(&g_pads[0]);
        }
    } else {
        connected = 1;
        if (sceCtrlPeekBufferPositiveExt2(0, &g_pads[0], 1) <= 0 &&
            sceCtrlPeekBufferPositive(0, &g_pads[0], 1) <= 0)
            input_neutral_pad(&g_pads[0]);
    }
    for (unsigned i = 0; i < 4; ++i)
        if (g_pads[i].buttons & SCE_CTRL_INTERCEPTED)
            input_neutral_pad(&g_pads[i]);
    g_connected_players = connected;
}

static void input_configure_psx_controllers(bool force)
{
    static const char *const keys[4] = {
        "vita_controller_type", "vita_controller_p2", "vita_controller_p3", "vita_controller_p4"
    };
    const unsigned multitap = g_multitap_mode = input_multitap_mode();
    for (unsigned port = 0; port < 8; ++port) {
        const int player = vita_player_for_psx_port(port, multitap);
        unsigned device = RETRO_DEVICE_NONE;
        if (player >= 0 && (g_connected_players & (1u << player))) {
            const char *setting = VitaSettings_GetOption(keys[player]);
            device = setting && strcmp(setting, "digital") == 0 ?
                RETRO_DEVICE_JOYPAD : RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 1);
        }
        if (force || g_psx_controller_device[port] != (int)device) {
            retro_set_controller_port_device(port, device);
            g_psx_controller_device[port] = (int)device;
        }
    }
}

static void input_poll_cb(void)
{
    const uint32_t pause_mask = SCE_CTRL_SELECT | SCE_CTRL_START;
    const unsigned previous_connected = g_connected_players;
    input_sample_controllers();
    if (previous_connected != g_connected_players)
        input_configure_psx_controllers(false);
    input_poll_rear_touch();

    const bool combo_down = (g_pads[0].buttons & pause_mask) == pause_mask;
    if (combo_down && !g_pause_combo_down)
        g_pause_requested = true;
    g_pause_combo_down = combo_down;
}

static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
    uint32_t button = 0;
    const int player = vita_player_for_psx_port(port, g_multitap_mode);
    if (player < 0 || !(g_connected_players & (1u << player)))
        return 0;
    const SceCtrlData *pad = &g_pads[player];

    device &= RETRO_DEVICE_MASK;
    if (device == RETRO_DEVICE_ANALOG) {
        if (id != RETRO_DEVICE_ID_ANALOG_X && id != RETRO_DEVICE_ID_ANALOG_Y)
            return 0;
        if (index == RETRO_DEVICE_INDEX_ANALOG_LEFT)
            return id == RETRO_DEVICE_ID_ANALOG_X ? vita_analog_axis(pad->lx) : vita_analog_axis(pad->ly);
        if (index == RETRO_DEVICE_INDEX_ANALOG_RIGHT)
            return id == RETRO_DEVICE_ID_ANALOG_X ? vita_analog_axis(pad->rx) : vita_analog_axis(pad->ry);
        return 0;
    }
    if (device != RETRO_DEVICE_JOYPAD)
        return 0;

    if (player == 0 && (pad->buttons & (SCE_CTRL_START | SCE_CTRL_SELECT)) ==
        (SCE_CTRL_START | SCE_CTRL_SELECT)) {
        if (id == RETRO_DEVICE_ID_JOYPAD_START || id == RETRO_DEVICE_ID_JOYPAD_SELECT)
            return 0;
    }

    switch (id) {
    case RETRO_DEVICE_ID_JOYPAD_B:
    case RETRO_DEVICE_ID_JOYPAD_A: {
        const char *setting = VitaSettings_GetOption("vita_swap_cross_circle");
        const bool swapped = setting && strcmp(setting, "enabled") == 0;
        if (id == RETRO_DEVICE_ID_JOYPAD_B)
            button = swapped ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS;
        else
            button = swapped ? SCE_CTRL_CROSS : SCE_CTRL_CIRCLE;
        break;
    }
    case RETRO_DEVICE_ID_JOYPAD_Y:      button = SCE_CTRL_SQUARE; break;
    case RETRO_DEVICE_ID_JOYPAD_X:      button = SCE_CTRL_TRIANGLE; break;
    case RETRO_DEVICE_ID_JOYPAD_L:      button = SCE_CTRL_L1; break;
    case RETRO_DEVICE_ID_JOYPAD_R:      button = SCE_CTRL_R1; break;
    case RETRO_DEVICE_ID_JOYPAD_L2:
        return !!((pad->buttons & SCE_CTRL_L2) || (player == 0 && (g_rear_buttons & VITA_REAR_L2)));
    case RETRO_DEVICE_ID_JOYPAD_R2:
        return !!((pad->buttons & SCE_CTRL_R2) || (player == 0 && (g_rear_buttons & VITA_REAR_R2)));
    case RETRO_DEVICE_ID_JOYPAD_L3:     button = SCE_CTRL_L3; break;
    case RETRO_DEVICE_ID_JOYPAD_R3:     button = SCE_CTRL_R3; break;
    case RETRO_DEVICE_ID_JOYPAD_UP:     button = SCE_CTRL_UP; break;
    case RETRO_DEVICE_ID_JOYPAD_DOWN:   button = SCE_CTRL_DOWN; break;
    case RETRO_DEVICE_ID_JOYPAD_LEFT:   button = SCE_CTRL_LEFT; break;
    case RETRO_DEVICE_ID_JOYPAD_RIGHT:  button = SCE_CTRL_RIGHT; break;
    case RETRO_DEVICE_ID_JOYPAD_START:  button = SCE_CTRL_START; break;
    case RETRO_DEVICE_ID_JOYPAD_SELECT: button = SCE_CTRL_SELECT; break;
    default: return 0;
    }
    return (pad->buttons & button) != 0;
}

static int display_init(void)
{
    SceDisplayFrameBuf frame;
    int i;

    for (i = 0; i < 2; i++) {
        g_fb_uid[i] = sceKernelAllocMemBlock(i ? "pcsx-awo-fb1" : "pcsx-awo-fb0",
            SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, DISPLAY_MEM_SIZE, NULL);
        if (g_fb_uid[i] < 0)
            return g_fb_uid[i];
        if (sceKernelGetMemBlockBase(g_fb_uid[i], (void **)&g_fb[i]) < 0)
            return -1;
        memset(g_fb[i], 0, DISPLAY_MEM_SIZE);
    }

    frame.size = sizeof(frame);
    frame.base = g_fb[0];
    frame.pitch = DISPLAY_PITCH;
    frame.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    frame.width = DISPLAY_W;
    frame.height = DISPLAY_H;
    g_front = 0;
    return sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME);
}

static void display_finish(void)
{
    int i;
    for (i = 0; i < 2; i++) {
        if (g_fb_uid[i] >= 0)
            sceKernelFreeMemBlock(g_fb_uid[i]);
        g_fb_uid[i] = -1;
        g_fb[i] = NULL;
    }
    if (g_stage_uid >= 0)
        sceKernelFreeMemBlock(g_stage_uid);
    g_stage_uid = -1;
    g_stage = NULL;
}

static int audio_init(void)
{
    SDL_AudioSpec want;
    SDL_AudioSpec src_spec;
    SDL_AudioSpec dst_spec;
    SDL_AudioSpec device_spec;
    int device_frames = 0;

    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "1024");

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        sceClibPrintf("PCSX AwO: SDL audio init failed: %s\n", SDL_GetError());
        return -1;
    }

    memset(&want, 0, sizeof(want));
    want.freq = AUDIO_RATE_IN;
    want.format = SDL_AUDIO_S16;
    want.channels = 2;

    g_audio_stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want, NULL, NULL);
    if (!g_audio_stream) {
        sceClibPrintf("PCSX AwO: SDL_OpenAudioDeviceStream failed: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return -1;
    }

    memset(&src_spec, 0, sizeof(src_spec));
    memset(&dst_spec, 0, sizeof(dst_spec));
    memset(&device_spec, 0, sizeof(device_spec));
    SDL_GetAudioStreamFormat(g_audio_stream, &src_spec, &dst_spec);
    SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(g_audio_stream),
        &device_spec, &device_frames);
    sceClibPrintf("PCSX AwO: audio stream src=%dHz dst=%dHz device=%dHz frames=%d\n",
        src_spec.freq, dst_spec.freq, device_spec.freq, device_frames);

    if (!SDL_ResumeAudioStreamDevice(g_audio_stream)) {
        sceClibPrintf("PCSX AwO: SDL_ResumeAudioStreamDevice failed: %s\n", SDL_GetError());
        SDL_DestroyAudioStream(g_audio_stream);
        g_audio_stream = NULL;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return -1;
    }

    return 0;
}

static void audio_finish(void)
{
    if (g_audio_stream) {
        SDL_DestroyAudioStream(g_audio_stream);
        g_audio_stream = NULL;
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

static void audio_set_paused(bool paused)
{
    if (!g_audio_stream)
        return;
    if (paused)
        SDL_PauseAudioStreamDevice(g_audio_stream);
    else
        SDL_ResumeAudioStreamDevice(g_audio_stream);
}

static void audio_clear(void)
{
    if (g_audio_stream)
        SDL_ClearAudioStream(g_audio_stream);
}

static void audio_prime(void)
{
    static const int16_t silence[AUDIO_QUEUE_TARGET_FRAMES * 2] = {0};
    if (g_audio_stream)
        SDL_PutAudioStreamData(g_audio_stream, silence, sizeof(silence));
}

static void wait_for_menu_input_release(void)
{
    const uint32_t mask =
        SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT |
        SCE_CTRL_TRIANGLE | SCE_CTRL_CIRCLE | SCE_CTRL_CROSS | SCE_CTRL_SQUARE |
        SCE_CTRL_START | SCE_CTRL_SELECT;

    for (;;) {
        SceCtrlData pad;
        memset(&pad, 0, sizeof(pad));
        if (sceCtrlPeekBufferPositiveExt2(0, &pad, 1) < 0)
            sceCtrlPeekBufferPositive(0, &pad, 1);
        if ((pad.buttons & mask) == 0)
            break;
        sceKernelDelayThread(1000);
    }

    for (unsigned i = 0; i < 4; ++i)
        input_neutral_pad(&g_pads[i]);
    g_pause_combo_down = false;
    g_pause_requested = false;
}

static bool has_ext(const char *name, const char *ext)
{
    size_t nl = strlen(name), el = strlen(ext), i;
    if (nl < el)
        return false;
    name += nl - el;
    for (i = 0; i < el; i++) {
        char a = name[i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return false;
    }
    return true;
}

static int extension_rank(const char *name)
{
    static const char *exts[] = { ".cue", ".chd", ".pbp", ".m3u", ".iso", ".img", ".bin", ".exe" };
    int i;
    for (i = 0; i < (int)(sizeof(exts) / sizeof(exts[0])); i++)
        if (has_ext(name, exts[i]))
            return i;
    return 999;
}

static bool is_game_path(const char *path)
{
    const char *base;

    if (path == NULL || path[0] == '\0' || extension_rank(path) == 999)
        return false;

    base = strrchr(path, '/');
    {
        const char *backslash = strrchr(path, '\\');
        if (backslash != NULL && (base == NULL || backslash > base))
            base = backslash;
    }
    base = base != NULL ? base + 1 : path;

    if (strcasecmp(base, "eboot.bin") == 0 ||
        strcasecmp(base, "pcsx-rearmed-vita.elf") == 0 ||
        strcasecmp(base, "pcsx-rearmed-vita.velf") == 0 ||
        strcasecmp(base, "pcsx-awo.elf") == 0 ||
        strcasecmp(base, "pcsx-awo.velf") == 0)
        return false;

    return true;
}
static void make_dirs(void)
{
    sceIoMkdir(APP_ROOT, 0777);
    sceIoMkdir(BIOS_DIR, 0777);
    sceIoMkdir(SAVE_DIR, 0777);
    sceIoMkdir(ROMS_DIR, 0777);
    sceIoMkdir(COVERS_DIR, 0777);
}

int main(int argc, char **argv)
{
    struct retro_game_info game;
    char game_path[1024];
    VitaBootDisc boot_disc;
    bool game_loaded = false;
    int exit_code = 0;
    int ret;

    sceClibPrintf("PCSX AwO: main entered\n");
    make_dirs();
    VitaSettings_Init();
    VitaSaveStates_Init();
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);
    g_multi_controller_supported = sceCtrlIsMultiControllerSupported() > 0;
    input_init_rear_touch();

    ret = display_init();
    if (ret < 0) {
        sceClibPrintf("PCSX AwO: display init failed 0x%x\n", ret);
        return 1;
    }
    sceClibPrintf("PCSX AwO: display initialized\n");

    ret = audio_init();
    if (ret < 0)
        sceClibPrintf("PCSX AwO: audio init failed 0x%x; continuing silent\n", ret);
    else
        sceClibPrintf("PCSX AwO: audio initialized\n");

    sceClibPrintf("PCSX AwO: entering retro_init\n");
    retro_set_environment(environment_cb);
    retro_set_video_refresh(video_cb);
    retro_set_audio_sample(NULL);
    retro_set_audio_sample_batch(audio_batch_cb);
    retro_set_input_poll(input_poll_cb);
    retro_set_input_state(input_state_cb);
    retro_init();
    sceClibPrintf("PCSX AwO: retro_init complete\n");

    game_path[0] = '\0';
    memset(&boot_disc, 0, sizeof(boot_disc));
    for (ret = 1; ret < argc; ret++) {
        if (is_game_path(argv[ret])) {
            snprintf(game_path, sizeof(game_path), "%s", argv[ret]);
        }
    }
    if (!game_path[0]) {
        char launch_params[1024] = {0};
        char launch_path[sizeof(game_path)];
        if (sceAppMgrGetAppParam(launch_params) >= 0) {
            launch_params[sizeof(launch_params) - 1] = '\0';
            if (VitaLiveArea_ParseGamePath(launch_params, launch_path, sizeof(launch_path)) &&
                is_game_path(launch_path)) {
                SceIoStat stat;
                if (sceIoGetstat(launch_path, &stat) >= 0) {
                    snprintf(game_path, sizeof(game_path), "%s", launch_path);
                    sceClibPrintf("PCSX AwO: LiveArea boot %s\n", game_path);
                } else {
                    sceClibPrintf("PCSX AwO: LiveArea ROM not found: %s\n", launch_path);
                }
            }
        }
    }

    for (;;) {
        if (game_path[0] == '\0') {
            ret = VitaRomSelector_Run(game_path, sizeof(game_path), &boot_disc);
            if (ret <= 0) {
                sceClibPrintf("PCSX AwO: ROM selector %s\n", ret < 0 ? "failed" : "closed");
                exit_code = ret < 0 ? 2 : 0;
                break;
            }
        }

        const char *initial_path = boot_disc.path[0] ? boot_disc.path : game_path;
        if (!g_disk_control.set_initial_image ||
            !g_disk_control.set_initial_image(boot_disc.index, initial_path)) {
            sceClibPrintf("PCSX AwO: failed to select boot disc %u\n", boot_disc.index + 1);
            exit_code = 3;
            break;
        }
        sceClibPrintf("PCSX AwO: booting %s\n", game_path);
        memset(&game, 0, sizeof(game));
        game.path = game_path;
        if (!retro_load_game(&game)) {
            sceClibPrintf("PCSX AwO: retro_load_game failed\n");
            exit_code = 3;
            break;
        }
        if (boot_disc.index > 0 &&
            (!g_disk_control.get_image_index ||
             g_disk_control.get_image_index() != boot_disc.index)) {
            sceClibPrintf("PCSX AwO: requested boot disc %u was not selected\n",
                boot_disc.index + 1);
            retro_unload_game();
            exit_code = 3;
            break;
        }

        game_loaded = true;
        input_sample_controllers();
        input_configure_psx_controllers(true);
        g_pause_requested = false;
        g_pause_combo_down = false;
        for (unsigned i = 0; i < 4; ++i)
            input_neutral_pad(&g_pads[i]);
        audio_clear();
        audio_prime();
        audio_set_paused(false);
        SceUInt64 pacing_period_us = pacing_frame_interval_us();
        SceUInt64 pacing_next_us = 0;
        while (!g_exit_requested && game_loaded) {
            pacing_wait_tick(&pacing_next_us, pacing_period_us);
            retro_run();
            if (!g_pause_requested)
                continue;

            g_pause_requested = false;
            audio_set_paused(true);
            int reopen_section = VITA_PAUSE_MAIN;
            for (;;) {
                VitaDiscInfo discs;
                read_disc_info(&discs);
                unsigned disc_selection = discs.current;
                ret = VitaPauseMenu_Run(game_path, reopen_section, &discs, &disc_selection);
                reopen_section = VITA_PAUSE_MAIN;
                wait_for_menu_input_release();

                if (ret == VITA_PAUSE_SAVE_STATE) {
                    const int slot = VitaSaveStates_GetSlot();
                    if (VitaSaveStates_Save(game_path, slot) == 0)
                        sceClibPrintf("PCSX AwO: saved state slot %d\n", slot);
                    else
                        sceClibPrintf("PCSX AwO: failed to save state slot %d\n", slot);
                    reopen_section = VITA_PAUSE_STATES;
                    continue;
                }

                if (ret == VITA_PAUSE_LOAD_STATE) {
                    const int slot = VitaSaveStates_GetSlot();
                    if (VitaSaveStates_Load(game_path, slot) == 0) {
                        sceClibPrintf("PCSX AwO: loaded state slot %d\n", slot);
                        input_sample_controllers();
                        input_configure_psx_controllers(true);
                        audio_clear();
                        audio_prime();
                        ret = VITA_PAUSE_RESUME;
                        break;
                    }
                    sceClibPrintf("PCSX AwO: failed to load state slot %d\n", slot);
                    reopen_section = VITA_PAUSE_STATES;
                    continue;
                }

                if (ret == VITA_PAUSE_CHANGE_DISC) {
                    if (change_disc(disc_selection)) {
                        audio_clear();
                        audio_prime();
                        ret = VITA_PAUSE_RESUME;
                        break;
                    }
                    reopen_section = VITA_PAUSE_DISCS;
                    continue;
                }

                break;
            }

            if (ret == VITA_PAUSE_RESUME) {
                input_sample_controllers();
                input_configure_psx_controllers(false);
                pacing_period_us = pacing_frame_interval_us();
                pacing_next_us = 0;
                audio_set_paused(false);
                continue;
            }

            retro_unload_game();
            game_loaded = false;
            audio_clear();

            if (ret == VITA_PAUSE_RESTART) {
                sceClibPrintf("PCSX AwO: restarting %s\n", game_path);
                memset(&game, 0, sizeof(game));
                game.path = game_path;
                if (!retro_load_game(&game)) {
                    sceClibPrintf("PCSX AwO: restart retro_load_game failed\n");
                    exit_code = 3;
                    g_exit_requested = true;
                    break;
                }
                game_loaded = true;
                input_sample_controllers();
                input_configure_psx_controllers(true);
                g_pause_requested = false;
                g_pause_combo_down = false;
                for (unsigned i = 0; i < 4; ++i)
                    input_neutral_pad(&g_pads[i]);
                audio_set_paused(false);
                continue;
            }

            sceClibPrintf("PCSX AwO: closing game\n");
            game_path[0] = '\0';
        }

        if (g_exit_requested) {
            if (game_loaded) {
                retro_unload_game();
                game_loaded = false;
            }
            break;
        }

        if (game_loaded) {
            retro_unload_game();
            game_loaded = false;
        }
    }

    retro_deinit();
    if (g_rear_touch_enabled)
        sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_STOP);
    audio_finish();
    display_finish();
    if (g_vm_block > 0)
        sceKernelFreeMemBlock(g_vm_block);
    return exit_code;
}
