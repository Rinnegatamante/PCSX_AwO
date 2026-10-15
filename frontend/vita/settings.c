#include "settings.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include "paths.h"

#ifndef GAME_SETTINGS_DIR
#define GAME_SETTINGS_DIR PCSX_AWO_GAME_CONFIGS_DIR
#endif
#ifndef GAME_MEMCARD_DIR
#define GAME_MEMCARD_DIR PCSX_AWO_SAVES_DIR
#endif
#define VALUE_SIZE 32
#define GAME_CONFIG_PATH_SIZE 512
#define PSX_MEMCARD_SIZE (128 * 1024)

typedef struct VitaOptionEntry {
    const char *key;
    const char *default_value;
    char value[VALUE_SIZE];
} VitaOptionEntry;

static VitaOptionEntry g_options[] = {
    { "vita_memcard1", "game", {0} },
    { "vita_memcard2", "none", {0} },
    { "pcsx_rearmed_bios", "auto", {0} },
    { "pcsx_rearmed_show_bios_bootlogo", "disabled", {0} },
    { "pcsx_rearmed_rgb32_output", "disabled", {0} },
    { "pcsx_rearmed_frameskip_type", "disabled", {0} },
    { "pcsx_rearmed_region", "auto", {0} },
    { "pcsx_rearmed_display_info", "disabled", {0} },
    { "pcsx_rearmed_vibration", "disabled", {0} },
    { "pcsx_rearmed_dithering", "enabled", {0} },
    { "pcsx_rearmed_neon_interlace_enable_v2", "auto", {0} },
    { "pcsx_rearmed_neon_enhancement_enable", "disabled", {0} },
    { "pcsx_rearmed_neon_enhancement_tex_adj_v2", "enabled", {0} },
    { "pcsx_rearmed_scale_hires", "disabled", {0} },
    { "pcsx_rearmed_spu_reverb", "enabled", {0} },
    { "pcsx_rearmed_spu_interpolation", "gaussian", {0} },
    { "pcsx_rearmed_nocdaudio", "enabled", {0} },
    { "pcsx_rearmed_noxadecoding", "enabled", {0} },
    { "pcsx_rearmed_spu_thread", "disabled", {0} },
    { "pcsx_rearmed_cd_turbo", "disabled", {0} },
    { "pcsx_rearmed_cd_readahead", "64", {0} },
    { "pcsx_rearmed_drc", "enabled", {0} },
    { "pcsx_rearmed_psxclock", "auto", {0} },
    { "pcsx_rearmed_fractional_framerate", "auto", {0} },
    { "pcsx_rearmed_alt_flip", "auto", {0} },
    { "pcsx_rearmed_screen_centering", "borderless", {0} },
    { "vita_aspect_ratio", "4:3", {0} },
    { "vita_renderer", "high_octane", {0} },
    { "vita_frame_interpolation", "off", {0} },
    { "vita_controller_type", "dualshock", {0} },
    { "vita_controller_p2", "dualshock", {0} },
    { "vita_controller_p3", "dualshock", {0} },
    { "vita_controller_p4", "dualshock", {0} },
    { "vita_touch_l2", "rear_top_left", {0} },
    { "vita_touch_r2", "rear_top_right", {0} },
    { "vita_touch_l3", "front_bottom_left", {0} },
    { "vita_touch_r3", "front_bottom_right", {0} },
    { "pcsx_rearmed_multitap", "disabled", {0} },
    { "vita_swap_cross_circle", "disabled", {0} },
    { "vita_postfx", "off", {0} },
    { "vita_fx_protanopia", "2.00", {0} },
    { "vita_fx_deuteranopia", "1.00", {0} },
    { "vita_fx_tritanopia", "1.00", {0} },
    { "vita_fx_gamma", "1.00", {0} },
    { "vita_fx_hue", "1.00", {0} },
    { "vita_fx_saturation", "1.00", {0} },
    { "vita_fx_radius", "1.00", {0} },
    { "vita_fx_softness", "1.00", {0} },
    { "vita_fx_opacity", "1.00", {0} },
};

#define OPTION_COUNT (sizeof(g_options) / sizeof(g_options[0]))

static char g_game_config_path[GAME_CONFIG_PATH_SIZE];
static bool g_game_config_exists;
static bool g_dirty;
static unsigned g_revision = 1;

static VitaOptionEntry *find_option(const char *key)
{
    size_t i;
    if (!key)
        return NULL;
    for (i = 0; i < OPTION_COUNT; ++i)
        if (strcmp(g_options[i].key, key) == 0)
            return &g_options[i];
    return NULL;
}

static void reset_defaults(void)
{
    size_t i;
    for (i = 0; i < OPTION_COUNT; ++i) {
        snprintf(g_options[i].value, sizeof(g_options[i].value), "%s",
            g_options[i].default_value);
    }
}

static int load_options(const char *path)
{
    FILE *file;
    char line[160];

    file = fopen(path, "rb");
    if (!file)
        return -1;

    while (fgets(line, sizeof(line), file)) {
        char *eq;
        char *end;
        VitaOptionEntry *option;

        eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq++ = '\0';

        end = eq + strlen(eq);
        while (end > eq && (end[-1] == '\r' || end[-1] == '\n' ||
                            end[-1] == ' ' || end[-1] == '\t'))
            *--end = '\0';

        option = find_option(line);
        if (!option || !eq[0])
            continue;
        snprintf(option->value, sizeof(option->value), "%s", eq);
    }

    fclose(file);
    return 0;
}

static void notify_options_changed(void)
{
    g_dirty = true;
    ++g_revision;
    if (g_revision == 0)
        g_revision = 1;
}

int VitaSettings_GameConfigPath(const char *game_path, char *out, size_t out_size)
{
    const char *basename;
    const char *p;
    uint64_t hash = UINT64_C(14695981039346656037);
    char label[49];
    size_t i = 0;
    int written;

    if (!out || out_size == 0)
        return 0;
    out[0] = '\0';
    if (!game_path || !game_path[0])
        return 0;

    basename = game_path;
    for (p = game_path; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c == '\\')
            c = '/';
        if (c >= 'A' && c <= 'Z')
            c = (unsigned char)(c + ('a' - 'A'));
        hash ^= c;
        hash *= UINT64_C(1099511628211);
        if (*p == '/' || *p == '\\')
            basename = p + 1;
    }
    if (!basename[0])
        return 0;

    for (p = basename; *p && i < sizeof(label) - 1; ++p) {
        const unsigned char c = (unsigned char)*p;
        if (c >= 'A' && c <= 'Z')
            label[i++] = (char)(c + ('a' - 'A'));
        else if ((c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') || c == '-' || c == '_')
            label[i++] = (char)c;
        else
            label[i++] = '_';
    }
    label[i] = '\0';
    written = snprintf(out, out_size, GAME_SETTINGS_DIR "/%s_%08x%08x.cfg",
        label, (unsigned)(hash >> 32), (unsigned)hash);
    return written > 0 && (size_t)written < out_size;
}

int VitaSettings_GameMemoryCardPath(const char *game_path, unsigned slot,
    char *out, size_t out_size)
{
    char config_path[GAME_CONFIG_PATH_SIZE];
    const char *name;
    const char *extension;
    int written;

    if (!out || out_size == 0)
        return 0;
    out[0] = '\0';
    if ((slot != 1 && slot != 2) ||
        !VitaSettings_GameConfigPath(game_path, config_path, sizeof(config_path)))
        return 0;

    name = strrchr(config_path, '/');
    name = name ? name + 1 : config_path;
    extension = strrchr(name, '.');
    if (!extension || strcmp(extension, ".cfg") != 0)
        return 0;

    written = snprintf(out, out_size, GAME_MEMCARD_DIR "/%.*s_%u.mcd",
        (int)(extension - name), name, slot);
    return written > 0 && (size_t)written < out_size;
}

int VitaSettings_PrepareGameMemoryCard(const char *game_path, unsigned slot,
    char *out, size_t out_size)
{
    char shared_path[GAME_CONFIG_PATH_SIZE];
    char temp_path[GAME_CONFIG_PATH_SIZE];
    unsigned char buffer[4096];
    FILE *source, *destination;
    long size;
    size_t length;
    int written;
    int success = 1;

    if (!VitaSettings_GameMemoryCardPath(game_path, slot, out, out_size))
        return 0;

    destination = fopen(out, "rb");
    if (destination) {
        fclose(destination);
        return 1;
    }
    if (errno != ENOENT)
        return 0;

    written = snprintf(shared_path, sizeof(shared_path),
        GAME_MEMCARD_DIR "/pcsx-card%u.mcd", slot);
    if (written <= 0 || (size_t)written >= sizeof(shared_path))
        return 0;
    source = fopen(shared_path, "rb");
    if (!source)
        return errno == ENOENT;

    if (fseek(source, 0, SEEK_END) != 0) {
        fclose(source);
        return 0;
    }
    size = ftell(source);
    if (size != PSX_MEMCARD_SIZE && size != PSX_MEMCARD_SIZE + 64 &&
        size != PSX_MEMCARD_SIZE + 3904) {
        fclose(source);
        return 0;
    }
    rewind(source);

    written = snprintf(temp_path, sizeof(temp_path), "%s.tmp", out);
    if (written <= 0 || (size_t)written >= sizeof(temp_path)) {
        fclose(source);
        return 0;
    }
    destination = fopen(temp_path, "wb");
    if (!destination) {
        fclose(source);
        return 0;
    }
    while ((length = fread(buffer, 1, sizeof(buffer), source)) != 0)
        if (fwrite(buffer, 1, length, destination) != length) {
            success = 0;
            break;
        }
    if (ferror(source))
        success = 0;
    if (fclose(destination) != 0)
        success = 0;
    fclose(source);

    if (success && rename(temp_path, out) != 0)
        success = 0;
    if (!success)
        remove(temp_path);
    return success;
}

void VitaSettings_Init(void)
{
    reset_defaults();
    g_game_config_path[0] = '\0';
    g_game_config_exists = false;
    g_dirty = false;
}

int VitaSettings_SelectGame(const char *game_path)
{
    char filename[GAME_CONFIG_PATH_SIZE];
    int error;
    if (!VitaSettings_GameConfigPath(game_path, filename, sizeof(filename)))
        return -1;
    if (strcmp(g_game_config_path, filename) == 0)
        return 0;

    reset_defaults();
    snprintf(g_game_config_path, sizeof(g_game_config_path), "%s", filename);
    if (load_options(filename) == 0) {
        g_game_config_exists = true;
        notify_options_changed();
        return 0;
    }

    error = errno;
    g_game_config_exists = false;
    notify_options_changed();
    if (error != ENOENT) {
        g_game_config_path[0] = '\0';
        return -1;
    }
    return 0;
}

int VitaSettings_BeginGame(const char *game_path)
{
    if (VitaSettings_SelectGame(game_path) < 0)
        return -1;
    return g_game_config_exists ? 0 : VitaSettings_Save();
}

void VitaSettings_EndGame(void)
{
    if (!g_game_config_path[0])
        return;
    g_game_config_path[0] = '\0';
    g_game_config_exists = false;
    reset_defaults();
    notify_options_changed();
}

bool VitaSettings_IsGameActive(void)
{
    return g_game_config_path[0] != '\0';
}

const char *VitaSettings_GetOption(const char *key)
{
    VitaOptionEntry *option = find_option(key);
    return option ? option->value : NULL;
}

int VitaSettings_SetOption(const char *key, const char *value)
{
    VitaOptionEntry *option = find_option(key);
    if (!g_game_config_path[0] || !option || !value || !value[0])
        return 0;
    if (strcmp(option->value, value) == 0)
        return 0;

    snprintf(option->value, sizeof(option->value), "%s", value);
    notify_options_changed();
    return 1;
}

unsigned VitaSettings_GetRevision(void)
{
    return g_revision;
}

int VitaSettings_Save(void)
{
    FILE *file;
    size_t i;

    if (!g_game_config_path[0])
        return -1;
    file = fopen(g_game_config_path, "wb");
    if (!file)
        return -1;

    for (i = 0; i < OPTION_COUNT; ++i)
        if (fprintf(file, "%s=%s\n", g_options[i].key, g_options[i].value) < 0) {
            fclose(file);
            return -1;
        }

    if (fclose(file) != 0)
        return -1;
    g_game_config_exists = true;
    return 0;
}

bool VitaSettings_ConsumeDirty(void)
{
    const bool dirty = g_dirty;
    g_dirty = false;
    return dirty;
}
