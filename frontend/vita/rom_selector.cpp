#include "rom_selector.h"
#include "postfx_parameters.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include <png.h>
#include <psp2/ctrl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <vitaGL.h>

#include <imgui.h>
#include <imgui_impl_vitagl.h>

#include "rom_selector.h"
#include "disc_grouping.h"
#include "launch_animation.h"
#include "paths.h"
#include "rom_paths.h"
#include "settings.h"
#include "savestates.h"

#define ROMS_DIR PCSX_AWO_ROMS_DIR
#define COVERS_DIR PCSX_AWO_COVERS_DIR

#define MENU_W 960
#define MENU_H 544
#define DISC_SEGMENTS 64
#define DISC_CACHE_SLOTS 7
#define COVER_UPLOAD_MAX 512
#define BACKGROUND_UPLOAD_W 960
#define BACKGROUND_UPLOAD_H 544

extern "C" int NativeGpu_InitBackend(int width, int height);
extern "C" void NativeGpu_RunBackendTaskSync(void (*task)(void *), void *arg);
extern "C" void VitaCtrNativeRenderer_PresentCurrentFrame(void);
extern "C" void VitaCtrNativeRenderer_SetAspectMode(int mode);
extern "C" void NativeRenderer_SwapWindow(void);

struct RomEntry {
    std::string path;
    std::string name;
    std::string title;
    std::string cover_path;
    std::string disc_serial;
    std::vector<std::string> disc_members;
    uint64_t game_hash;

    RomEntry() : game_hash(0) {}
};

struct CoverEntry {
    std::string title;
    std::string path;
    uint64_t title_hash;
};

struct __attribute__((packed)) PsxSerialTitleHash {
    char serial[10];
    uint64_t title_hash;
};

#include "psx_serial_index.inc"

struct DataTrack {
    std::string path;
    uint64_t data_offset;
    int sector_size;
    int user_offset;

    DataTrack() : data_offset(0), sector_size(0), user_offset(0) {}
};

struct CoverCacheEntry {
    std::string path;
    GLuint texture;
    unsigned int stamp;

    CoverCacheEntry() : texture(0), stamp(0) {}
};

struct DiscMesh {
    float *top_vertices;
    float *top_uvs;
    float *body_vertices;
    float *body_colors;
    int top_vertex_count;
    int body_vertex_count;

    DiscMesh()
        : top_vertices(NULL), top_uvs(NULL), body_vertices(NULL), body_colors(NULL),
          top_vertex_count(0), body_vertex_count(0) {}
};

struct DiscGpuVertex {
    float x, y, z;
    float u, v;
    float r, g, b, a;
};

struct SelectorTask {
    char *out_path;
    size_t out_size;
    VitaBootDisc *boot_disc;
    int result;
};

struct PauseMenuTask {
    const char *game_path;
    const VitaDiscInfo *discs;
    int start_section;
    unsigned disc_selection;
    int result;
};

struct OptionChoice {
    const char *value;
    const char *label;
};

struct SelectorOption {
    const char *category;
    const char *label;
    const char *description;
    const char *key;
    const OptionChoice *choices;
    int choice_count;
    bool psx_clock;
};

static const OptionChoice k_region_choices[] = {
    { "auto", "Auto" }, { "NTSC", "NTSC" }, { "PAL", "PAL" }
};
static const OptionChoice k_bios_choices[] = {
    { "auto", "Auto" }, { "HLE", "HLE" }
};
static const OptionChoice k_bios_logo_choices[] = {
    { "disabled", "Off" }, { "enabled", "On" },
    { "enabled_no_pcsx", "On, no PCSX logo" }
};
static const OptionChoice k_fractional_choices[] = {
    { "auto", "Auto" }, { "disabled", "Off" }, { "enabled", "On" }
};
static const OptionChoice k_flip_choices[] = {
    { "auto", "Auto" }, { "early", "Early" }, { "late", "Late" }
};
static const OptionChoice k_centering_choices[] = {
    { "auto", "Auto" }, { "game", "Game" }, { "borderless", "Borderless" }
};
static const OptionChoice k_toggle_choices[] = {
    { "disabled", "Off" }, { "enabled", "On" }
};
static const OptionChoice k_cd_readahead_choices[] = {
    { "0", "Off" }, { "12", "12 sectors" }, { "32", "32 sectors" },
    { "64", "64 sectors" }, { "128", "128 sectors" }, { "256", "256 sectors" }
};
static const OptionChoice k_interp_choices[] = {
    { "off", "Off" }, { "simple", "Simple" },
    { "gaussian", "Gaussian" }, { "cubic", "Cubic" }
};
static const OptionChoice k_aspect_choices[] = {
    { "4:3", "4:3" },
    { "16:9_stretched", "16:9 Stretched" },
    { "16:9_unstretched", "16:9 Unstretched" }
};
static const OptionChoice k_renderer_choices[] = {
    { "neon", "ReARMed (Neon)" },
    { "high_octane", "High Octane (VitaGL)" }
};
static const OptionChoice k_controller_choices[] = {
    { "dualshock", "DualShock (Analog)" },
    { "digital", "Digital" }
};
static const OptionChoice k_multitap_choices[] = {
    { "disabled", "Off (2 players)" },
    { "port 1", "Port 1 (P1-P4)" },
    { "port 2", "Port 2 (P2-P4)" },
    { "ports 1 and 2", "Both (2+2)" }
};
static const OptionChoice k_frame_interpolation_choices[] = {
    { "off", "Off" }, { "blend", "Blending (Low Quality)" },
    { "motion", "Optical Flow (High Quality)" }
};
static const OptionChoice k_postfx_choices[] = {
    { "off", "Off" }, { "crt", "CRT" },
    { "crt_bleeding", "CRT with Bleeding" },
    { "daltonic", "Daltonic" }, { "fxaa", "FXAA" },
    { "gamma", "Gamma Correction" }, { "greyscale", "Greyscale" },
    { "hue", "Hue & Saturation" }, { "lcd3x", "LCD 3x" },
    { "negative", "Negative" }, { "sepia", "Sepia Tone" },
    { "sharp_grid", "Sharp Bilinear (Grid)" },
    { "sharp_scanlines", "Sharp Bilinear (Scanlines)" },
    { "vignette", "Vignette" }
};

static const SelectorOption k_selector_options[] = {
    { "SYSTEM", "Region", "Select the console video region. Auto detects it from the game; forcing NTSC/PAL changes timing and refresh rate.", "pcsx_rearmed_region", k_region_choices, 3, false },
    { "SYSTEM", "BIOS", "Choose automatic BIOS selection or the built-in HLE BIOS. Auto uses a real BIOS when available. Takes effect on the next game start/restart.", "pcsx_rearmed_bios", k_bios_choices, 2, false },
    { "SYSTEM", "BIOS Boot Logo", "Controls whether the PlayStation BIOS boot animation is shown. Takes effect on the next game start/restart.", "pcsx_rearmed_show_bios_bootlogo", k_bios_logo_choices, 3, false },
    { "SYSTEM", "PSX CPU Clock", "Changes the emulated PlayStation CPU clock. Leave on Auto unless a game needs different CPU timing.", "pcsx_rearmed_psxclock", NULL, 0, true },
    { "SYSTEM", "Turbo CD", "Accelerates the emulated CD-ROM timing to reduce loading times. Some games rely on original drive timing and may misbehave, so disable it if a title has loading or streaming problems.", "pcsx_rearmed_cd_turbo", k_toggle_choices, 2, false },
    { "SYSTEM", "CD Read-Ahead", "Caches upcoming sectors on a background thread. This reduces storage stalls without changing PlayStation CD-ROM timing.", "pcsx_rearmed_cd_readahead", k_cd_readahead_choices, 6, false },
    { "VIDEO", "Aspect Ratio", "4:3 is the original output. 16:9 Stretched fills the screen by stretching 4:3. 16:9 Unstretched is exclusive to High Octane and uses an experimental widescreen projection that may show culling or background artifacts.", "vita_aspect_ratio", k_aspect_choices, 3, false },
    { "VIDEO", "Renderer", "ReARMed is the original NEON software rasterizer; High Octane uses the Vita GPU through vitaGL. Applied when starting or restarting a game.", "vita_renderer", k_renderer_choices, 2, false },
    { "VIDEO", "Frame Generation", "High Octane only: generates intermediate display frames for games rendering near 25-30 FPS. Optical Flow estimates motion between frames. Adds latency and GPU load; disabled when post-processing is enabled.", "vita_frame_interpolation", k_frame_interpolation_choices, 3, false },
    { "VIDEO", "Enhanced Resolution", "NEON only: renders supported low-resolution 3D scenes at 2x internal resolution. Increases CPU and memory usage; may introduce graphical artifacts. Applied on game start/restart.", "pcsx_rearmed_neon_enhancement_enable", k_toggle_choices, 2, false },
    { "VIDEO", "Post-processing", "Optional post-processing shader", "vita_postfx", k_postfx_choices, (int)(sizeof(k_postfx_choices) / sizeof(k_postfx_choices[0])), false },
    { "VIDEO", "  Protanopia", "Daltonic color-blindness simulation. Values above 1 activate this mode unless another active mode takes priority.", "vita_fx_protanopia", NULL, 0, false },
    { "VIDEO", "  Deuteranopia", "Daltonic color-blindness simulation. Values above 1 activate this mode (Tritanopia takes priority).", "vita_fx_deuteranopia", NULL, 0, false },
    { "VIDEO", "  Tritanopia", "Daltonic color-blindness simulation. Values above 1 activate this mode, with highest priority.", "vita_fx_tritanopia", NULL, 0, false },
    { "VIDEO", "  Gamma", "Gamma correction, 1.00 is neutral. Adjustable between 0.10 and 2.00.", "vita_fx_gamma", NULL, 0, false },
    { "VIDEO", "  Hue", "HSV hue multiplier. 1.00 preserves the original hue.", "vita_fx_hue", NULL, 0, false },
    { "VIDEO", "  Saturation", "HSV saturation multiplier. 0.00 removes saturation; 1.00 is neutral.", "vita_fx_saturation", NULL, 0, false },
    { "VIDEO", "  Radius", "Vignette radius. Decrease below 1.00 to darken the edges.", "vita_fx_radius", NULL, 0, false },
    { "VIDEO", "  Softness", "Vignette edge transition width. Higher values produce a softer transition.", "vita_fx_softness", NULL, 0, false },
    { "VIDEO", "  Opacity", "Vignette strength. 0.00 disables the darkening; 1.00 uses full strength.", "vita_fx_opacity", NULL, 0, false },
    { "VIDEO", "Screen Borders", "Borderless removes vertical blanking outside the game's active display range. Auto and Game preserve the console timing canvas when a title intentionally uses it.", "pcsx_rearmed_screen_centering", k_centering_choices, 3, false },
    { "VIDEO", "Fractional Frame Rate", "Uses the console's fractional video timing when appropriate, such as 59.94 Hz instead of exactly 60 Hz.", "pcsx_rearmed_fractional_framerate", k_fractional_choices, 3, false },
    { "VIDEO", "Framebuffer Readout", "Changes when the displayed framebuffer is read. Early/Late can fix games with unusual framebuffer timing.", "pcsx_rearmed_alt_flip", k_flip_choices, 3, false },
    { "INPUT", "Player 1 Controller", "DualShock enables both analog sticks. Use Digital for incompatible games. On PS Vita, the upper rear touchpad maps to L2 on the left and R2 on the right.", "vita_controller_type", k_controller_choices, 2, false },
    { "INPUT", "Player 2 Controller", "PS TV wireless controller 2. Select Digital for games incompatible with DualShock.", "vita_controller_p2", k_controller_choices, 2, false },
    { "INPUT", "Player 3 Controller", "PS TV wireless controller 3. Requires a compatible Multitap game.", "vita_controller_p3", k_controller_choices, 2, false },
    { "INPUT", "Player 4 Controller", "PS TV wireless controller 4. Requires a compatible Multitap game.", "vita_controller_p4", k_controller_choices, 2, false },
    { "INPUT", "Multitap", "PS TV: Off uses ports 1 and 2 normally. Port 1 puts players 1-4 on the first Multitap; Port 2 puts player 1 on port 1 and players 2-4 on the second. Both splits players 1-2 and 3-4 between the two Multitaps. Requires game support.", "pcsx_rearmed_multitap", k_multitap_choices, 4, false },
    { "INPUT", "Swap Cross / Circle", "Exchange Cross (X) and Circle (O) in PS1 games. Emulator menu controls are unchanged. Takes effect immediately.", "vita_swap_cross_circle", k_toggle_choices, 2, false },
    { "AUDIO", "Reverb", "Enables PlayStation SPU reverb effects.", "pcsx_rearmed_spu_reverb", k_toggle_choices, 2, false },
    { "AUDIO", "Interpolation", "Selects the SPU sample interpolation method. Higher-quality modes can slightly increase CPU cost.", "pcsx_rearmed_spu_interpolation", k_interp_choices, 4, false },
    { "AUDIO", "CD Audio", "Enables CD-DA audio tracks used by games that stream music directly from the disc.", "pcsx_rearmed_nocdaudio", k_toggle_choices, 2, false },
    { "AUDIO", "XA Decoding", "Enables XA audio decoding for streamed voices, music and sound effects stored on the disc.", "pcsx_rearmed_noxadecoding", k_toggle_choices, 2, false },
};

static const VitaPostFXParameter *selector_postfx_parameter(const char *key)
{
    for (size_t i = 0; i < vita_postfx_parameter_count; ++i)
        if (strcmp(key, vita_postfx_parameters[i].setting) == 0)
            return &vita_postfx_parameters[i];
    return NULL;
}

static bool selector_option_visible(const SelectorOption &option)
{
    if (strcmp(option.key, "pcsx_rearmed_multitap") == 0 ||
        strcmp(option.key, "vita_controller_p2") == 0 ||
        strcmp(option.key, "vita_controller_p3") == 0 ||
        strcmp(option.key, "vita_controller_p4") == 0) {
        static const bool multi_supported = sceCtrlIsMultiControllerSupported() > 0;
        if (!multi_supported)
            return false;
    }
    const VitaPostFXParameter *parameter = selector_postfx_parameter(option.key);
    if (!parameter)
        return true;
    const char *active = VitaSettings_GetOption("vita_postfx");
    return active && strcmp(parameter->effect, active) == 0;
}

static int selector_option_count(void)
{
    int count = 0;
    for (size_t i = 0; i < sizeof(k_selector_options) / sizeof(k_selector_options[0]); ++i)
        if (selector_option_visible(k_selector_options[i]))
            ++count;
    return count;
}

static const SelectorOption &selector_visible_option(int index)
{
    for (size_t i = 0; i < sizeof(k_selector_options) / sizeof(k_selector_options[0]); ++i) {
        if (!selector_option_visible(k_selector_options[i]))
            continue;
        if (index-- == 0)
            return k_selector_options[i];
    }
    return k_selector_options[0];
}

static bool selector_choice_available(const SelectorOption &option, int index)
{
    if (strcmp(option.key, "vita_aspect_ratio") == 0 &&
        strcmp(option.choices[index].value, "16:9_unstretched") == 0) {
        const char *renderer = VitaSettings_GetOption("vita_renderer");
        return !renderer || strcmp(renderer, "neon") != 0;
    }
    return true;
}

static bool selector_option_available(const SelectorOption &option)
{
    if (strcmp(option.key, "pcsx_rearmed_neon_enhancement_enable") == 0) {
        const char *renderer = VitaSettings_GetOption("vita_renderer");
        return renderer && strcmp(renderer, "neon") == 0;
    }
    if (strcmp(option.key, "vita_frame_interpolation") == 0) {
        const char *renderer = VitaSettings_GetOption("vita_renderer");
        const char *postfx = VitaSettings_GetOption("vita_postfx");
        return renderer && strcmp(renderer, "high_octane") == 0 &&
               (!postfx || strcmp(postfx, "off") == 0);
    }
    return true;
}

struct OverlayGLState {
    GLint framebuffer;
    GLint viewport[4];
    GLint program;
    GLint active_texture;
    GLint texture0;
    GLint array_buffer;
    GLint element_array_buffer;
    GLint blend_equation_rgb;
    GLint blend_equation_alpha;
    GLint blend_src_rgb;
    GLint blend_dst_rgb;
    GLint blend_src_alpha;
    GLint blend_dst_alpha;
    GLint scissor_box[4];
    GLboolean blend;
    GLboolean depth;
    GLboolean cull;
    GLboolean stencil;
    GLboolean scissor;
    GLboolean alpha_test;
    GLboolean depth_mask;
    GLboolean color_mask[4];
};

static CoverCacheEntry g_cover_cache[DISC_CACHE_SLOTS];
static unsigned int g_cover_stamp = 1;
static DiscMesh g_disc_mesh;
static GLuint g_imgui_program = 0;
static GLint g_imgui_screen_loc = -1;
static GLint g_imgui_texture_loc = -1;
static GLuint g_imgui_vao = 0;
static GLuint g_imgui_vbo = 0;
static GLuint g_imgui_ibo = 0;
static GLuint g_disc_program = 0;
static GLint g_disc_screen_loc = -1;
static GLint g_disc_center_loc = -1;
static GLint g_disc_rotation_loc = -1;
static GLint g_disc_roll_loc = -1;
static GLint g_disc_scale_loc = -1;
static GLint g_disc_tint_loc = -1;
static GLint g_disc_texture_loc = -1;
static GLint g_disc_gloss_loc = -1;
static GLuint g_disc_body_vao = 0;
static GLuint g_disc_body_vbo = 0;
static GLuint g_disc_top_vao = 0;
static GLuint g_disc_top_vbo = 0;
static GLuint g_disc_white_texture = 0;
static ImFont *g_font_body = NULL;
static ImFont *g_font_title = NULL;
static ImFont *g_font_small = NULL;

static bool upload_disc_mesh();

static void init_menu_fonts()
{
    ImGuiIO &io = ImGui::GetIO();
    const char *font_path = NULL;
    const char *paths[] = { "app0:Roboto.ttf", "host0:Roboto.ttf" };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        FILE *file = fopen(paths[i], "rb");
        if (file) {
            fclose(file);
            font_path = paths[i];
            break;
        }
    }
    g_font_body = font_path ? io.Fonts->AddFontFromFileTTF(font_path, 18.0f) : NULL;
    if (g_font_body) {
        g_font_title = io.Fonts->AddFontFromFileTTF(font_path, 28.0f);
        g_font_small = io.Fonts->AddFontFromFileTTF(font_path, 14.0f);
        if (!g_font_title)
            g_font_title = g_font_body;
        if (!g_font_small)
            g_font_small = g_font_body;
        io.FontDefault = g_font_body;
    } else {
        g_font_body = io.Fonts->AddFontDefault();
        g_font_title = g_font_body;
        g_font_small = g_font_body;
        sceClibPrintf("[ROMSEL] Roboto unavailable; using default font\n");
    }
}

static ImFont *menu_font(ImFont *font)
{
    return font ? font : ImGui::GetFont();
}

static GLuint compile_shader(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    if (!shader)
        return 0;

    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[1024];
        GLsizei length = 0;
        glGetShaderInfoLog(shader, sizeof(log), &length, log);
        sceClibPrintf("[ROMSEL] shader compile failed: %s\n", length > 0 ? log : "(no log)");
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static bool create_imgui_program()
{
    static const char *vs =
        "attribute vec2 in_pos;\n"
        "attribute vec2 in_uv;\n"
        "attribute vec4 in_color;\n"
        "uniform vec4 u_screen;\n"
        "varying vec2 v_uv;\n"
        "varying vec4 v_color;\n"
        "void main() {\n"
        "  vec2 p = vec2(in_pos.x * (2.0 / u_screen.x) - 1.0,\n"
        "                1.0 - in_pos.y * (2.0 / u_screen.y));\n"
        "  gl_Position = vec4(p, 0.0, 1.0);\n"
        "  v_uv = in_uv;\n"
        "  v_color = in_color;\n"
        "}\n";
    static const char *fs =
        "uniform sampler2D u_texture;\n"
        "varying vec2 v_uv;\n"
        "varying vec4 v_color;\n"
        "void main() {\n"
        "  gl_FragColor = texture2D(u_texture, v_uv) * v_color;\n"
        "}\n";

    GLuint vertex = compile_shader(GL_VERTEX_SHADER, vs);
    GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, fs);
    if (!vertex || !fragment) {
        if (vertex) glDeleteShader(vertex);
        if (fragment) glDeleteShader(fragment);
        return false;
    }

    g_imgui_program = glCreateProgram();
    glAttachShader(g_imgui_program, vertex);
    glAttachShader(g_imgui_program, fragment);
    glBindAttribLocation(g_imgui_program, 0, "in_pos");
    glBindAttribLocation(g_imgui_program, 1, "in_uv");
    glBindAttribLocation(g_imgui_program, 2, "in_color");
    glLinkProgram(g_imgui_program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint linked = GL_FALSE;
    glGetProgramiv(g_imgui_program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[1024];
        GLsizei length = 0;
        glGetProgramInfoLog(g_imgui_program, sizeof(log), &length, log);
        sceClibPrintf("[ROMSEL] ImGui program link failed: %s\n", length > 0 ? log : "(no log)");
        glDeleteProgram(g_imgui_program);
        g_imgui_program = 0;
        return false;
    }

    g_imgui_screen_loc = glGetUniformLocation(g_imgui_program, "u_screen");
    g_imgui_texture_loc = glGetUniformLocation(g_imgui_program, "u_texture");
    glUseProgram(g_imgui_program);
    if (g_imgui_texture_loc >= 0)
        glUniform1i(g_imgui_texture_loc, 0);
    glUseProgram(0);

    glGenVertexArrays(1, &g_imgui_vao);
    glGenBuffers(1, &g_imgui_vbo);
    glGenBuffers(1, &g_imgui_ibo);
    glBindVertexArray(g_imgui_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_imgui_vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_imgui_ibo);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert),
        (void *)offsetof(ImDrawVert, pos));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert),
        (void *)offsetof(ImDrawVert, uv));
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(ImDrawVert),
        (void *)offsetof(ImDrawVert, col));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    return true;
}

static bool create_disc_program()
{
    static const char *vs =
        "attribute vec3 in_pos;\n"
        "attribute vec2 in_uv;\n"
        "attribute vec4 in_color;\n"
        "uniform vec4 u_screen;\n"
        "uniform vec4 u_center;\n"
        "uniform vec4 u_rotation;\n"
        "uniform vec2 u_roll;\n"
        "uniform float u_scale;\n"
        "varying vec2 v_uv;\n"
        "varying vec4 v_color;\n"
        "void main() {\n"
        "  vec3 p = in_pos * u_scale;\n"
        "  float x1 = u_rotation.x * p.x + u_rotation.y * p.z;\n"
        "  float z1 = -u_rotation.y * p.x + u_rotation.x * p.z;\n"
        "  float y2 = u_rotation.z * p.y - u_rotation.w * z1;\n"
        "  float z2 = u_rotation.w * p.y + u_rotation.z * z1;\n"
        "  vec2 rolled = vec2(u_roll.x * x1 - u_roll.y * y2,\n"
        "                     u_roll.y * x1 + u_roll.x * y2);\n"
        "  vec3 world = vec3(rolled.x + u_center.x, rolled.y + u_center.y, z2 + u_center.z);\n"
        "  vec2 clip = vec2(world.x * (2.0 / u_screen.x) - 1.0,\n"
        "                   1.0 - world.y * (2.0 / u_screen.y));\n"
        "  gl_Position = vec4(clip, -world.z / 1000.0, 1.0);\n"
        "  v_uv = in_uv;\n"
        "  v_color = in_color;\n"
        "}\n";
    static const char *fs =
        "uniform sampler2D u_texture;\n"
        "uniform vec4 u_tint;\n"
        "uniform vec4 u_rotation;\n"
        "uniform float u_gloss;\n"
        "varying vec2 v_uv;\n"
        "varying vec4 v_color;\n"
        "void main() {\n"
        "  vec4 color = texture2D(u_texture, v_uv) * v_color * u_tint;\n"
        "  vec2 p = v_uv * 2.0 - 1.0;\n"
        "  float radius = length(p);\n"
        "  vec2 light_dir = normalize(vec2(0.42 + u_rotation.y * 0.38, -0.78 + u_rotation.w * 0.22));\n"
        "  vec2 tangent = vec2(-light_dir.y, light_dir.x);\n"
        "  float stripe = max(0.0, 1.0 - abs(dot(p, tangent) - 0.10) * 3.25);\n"
        "  float rim = smoothstep(0.72, 1.0, radius);\n"
        "  float sheen = (stripe * stripe * 0.30 + rim * 0.10) * min(u_gloss, 1.0);\n"
        "  color.rgb += vec3(sheen * 0.82, sheen * 0.93, sheen);\n"
        "  if (u_gloss > 1.5) {\n"
        "    float sweep2 = max(0.0, 1.0 - abs(dot(p, light_dir) + 0.18) * 4.8);\n"
        "    float grooves = 0.5 + 0.5 * sin(radius * 118.0);\n"
        "    vec3 spectrum = vec3(0.5 + 0.5 * sin(radius * 47.0),\n"
        "                         0.5 + 0.5 * sin(radius * 47.0 + 2.10),\n"
        "                         0.5 + 0.5 * sin(radius * 47.0 + 4.20));\n"
        "    float metal = 0.33 + stripe * stripe * 0.34 + sweep2 * sweep2 * 0.18 + grooves * 0.025;\n"
        "    color.rgb = vec3(metal * 0.92, metal * 0.96, metal);\n"
        "    color.rgb += spectrum * (stripe * 0.10 + sweep2 * 0.055);\n"
        "    color.rgb += vec3(rim * 0.14);\n"
        "    color.a = 1.0;\n"
        "  } else if (u_gloss < 0.5 && v_color.a < 0.5) {\n"
        "    float inner_layer = step(0.10, v_color.a);\n"
        "    float sweep2 = max(0.0, 1.0 - abs(dot(p, light_dir) + 0.12) * 6.0);\n"
        "    float fine = 0.5 + 0.5 * sin(radius * 132.0);\n"
        "    float ring_a = max(0.0, 1.0 - abs(radius - 0.30) * 95.0);\n"
        "    float ring_b = max(0.0, 1.0 - abs(radius - 0.145) * 125.0);\n"
        "    vec3 spectrum = vec3(0.5 + 0.5 * sin(radius * 59.0),\n"
        "                         0.5 + 0.5 * sin(radius * 59.0 + 2.10),\n"
        "                         0.5 + 0.5 * sin(radius * 59.0 + 4.20));\n"
        "    vec3 base = mix(vec3(0.010, 0.007, 0.018), vec3(0.028, 0.019, 0.040), inner_layer);\n"
        "    color.rgb = base + vec3(0.10, 0.085, 0.13) * sweep2 * sweep2 * sweep2;\n"
        "    color.rgb += spectrum * sweep2 * (0.018 + fine * 0.018);\n"
        "    color.rgb += vec3((ring_a + ring_b) * 0.085 + rim * 0.035);\n"
        "    color.a = 1.0;\n"
        "  }\n"
        "  gl_FragColor = color;\n"
        "}\n";

    GLuint vertex = compile_shader(GL_VERTEX_SHADER, vs);
    GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, fs);
    if (!vertex || !fragment) {
        if (vertex) glDeleteShader(vertex);
        if (fragment) glDeleteShader(fragment);
        return false;
    }

    g_disc_program = glCreateProgram();
    glAttachShader(g_disc_program, vertex);
    glAttachShader(g_disc_program, fragment);
    glBindAttribLocation(g_disc_program, 0, "in_pos");
    glBindAttribLocation(g_disc_program, 1, "in_uv");
    glBindAttribLocation(g_disc_program, 2, "in_color");
    glLinkProgram(g_disc_program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint linked = GL_FALSE;
    glGetProgramiv(g_disc_program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[1024];
        GLsizei length = 0;
        glGetProgramInfoLog(g_disc_program, sizeof(log), &length, log);
        sceClibPrintf("[ROMSEL] disc program link failed: %s\n", length > 0 ? log : "(no log)");
        glDeleteProgram(g_disc_program);
        g_disc_program = 0;
        return false;
    }

    g_disc_screen_loc = glGetUniformLocation(g_disc_program, "u_screen");
    g_disc_center_loc = glGetUniformLocation(g_disc_program, "u_center");
    g_disc_rotation_loc = glGetUniformLocation(g_disc_program, "u_rotation");
    g_disc_roll_loc = glGetUniformLocation(g_disc_program, "u_roll");
    g_disc_scale_loc = glGetUniformLocation(g_disc_program, "u_scale");
    g_disc_tint_loc = glGetUniformLocation(g_disc_program, "u_tint");
    g_disc_texture_loc = glGetUniformLocation(g_disc_program, "u_texture");
    g_disc_gloss_loc = glGetUniformLocation(g_disc_program, "u_gloss");

    glUseProgram(g_disc_program);
    if (g_disc_texture_loc >= 0)
        glUniform1i(g_disc_texture_loc, 0);
    glUseProgram(0);

    const uint32_t white = 0xffffffffu;
    glGenTextures(1, &g_disc_white_texture);
    glBindTexture(GL_TEXTURE_2D, g_disc_white_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

static void render_imgui_draw_data(ImDrawData *draw_data)
{
    if (!draw_data || draw_data->CmdListsCount <= 0)
        return;

    glUseProgram(g_imgui_program);
    if (g_imgui_screen_loc >= 0)
        glUniform4f(g_imgui_screen_loc, (float)MENU_W, (float)MENU_H, 0.0f, 0.0f);
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_SCISSOR_TEST);
    glViewport(0, 0, MENU_W, MENU_H);
    glBindVertexArray(g_imgui_vao);

    for (int list_index = 0; list_index < draw_data->CmdListsCount; ++list_index) {
        const ImDrawList *list = draw_data->CmdLists[list_index];
        glBindBuffer(GL_ARRAY_BUFFER, g_imgui_vbo);
        glBufferData(GL_ARRAY_BUFFER,
            (GLsizeiptr)(list->VtxBuffer.Size * (int)sizeof(ImDrawVert)),
            list->VtxBuffer.Data, GL_STREAM_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_imgui_ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
            (GLsizeiptr)(list->IdxBuffer.Size * (int)sizeof(ImDrawIdx)),
            list->IdxBuffer.Data, GL_STREAM_DRAW);

        size_t index_offset = 0;
        for (int cmd_index = 0; cmd_index < list->CmdBuffer.Size; ++cmd_index) {
            const ImDrawCmd &cmd = list->CmdBuffer[cmd_index];
            if (cmd.UserCallback) {
                cmd.UserCallback(list, &cmd);
            } else {
                const int x = (int)cmd.ClipRect.x;
                const int y = MENU_H - (int)cmd.ClipRect.w;
                const int w = (int)(cmd.ClipRect.z - cmd.ClipRect.x);
                const int h = (int)(cmd.ClipRect.w - cmd.ClipRect.y);
                if (w > 0 && h > 0) {
                    glScissor(x, y, w, h);
                    glBindTexture(GL_TEXTURE_2D, (GLuint)(uintptr_t)cmd.TextureId);
                    glDrawElements(GL_TRIANGLES, (GLsizei)cmd.ElemCount,
                        sizeof(ImDrawIdx) == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
                        (void *)(index_offset * sizeof(ImDrawIdx)));
                }
            }
            index_offset += cmd.ElemCount;
        }
    }

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(0);
}

static void destroy_imgui_program()
{
    if (g_imgui_vao)
        glDeleteVertexArrays(1, &g_imgui_vao);
    if (g_imgui_vbo)
        glDeleteBuffers(1, &g_imgui_vbo);
    if (g_imgui_ibo)
        glDeleteBuffers(1, &g_imgui_ibo);
    g_imgui_vao = g_imgui_vbo = g_imgui_ibo = 0;
    if (g_imgui_program)
        glDeleteProgram(g_imgui_program);
    g_imgui_program = 0;
    g_imgui_screen_loc = -1;
    g_imgui_texture_loc = -1;
}

static bool ends_with_ci(const std::string &name, const char *ext)
{
    const size_t ext_len = strlen(ext);
    if (name.size() < ext_len)
        return false;

    const size_t base = name.size() - ext_len;
    for (size_t i = 0; i < ext_len; ++i) {
        char a = name[base + i];
        char b = ext[i];
        if (a >= 'A' && a <= 'Z')
            a = (char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z')
            b = (char)(b + ('a' - 'A'));
        if (a != b)
            return false;
    }
    return true;
}

static bool is_rom_file(const std::string &name)
{
    static const char *exts[] = {
        ".cue", ".chd", ".pbp", ".m3u", ".iso", ".img", ".bin", ".exe"
    };
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); ++i)
        if (ends_with_ci(name, exts[i]))
            return true;
    return false;
}

static std::string strip_extension(const std::string &name)
{
    const size_t slash = name.find_last_of("/\\");
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return name.substr(slash == std::string::npos ? 0 : slash + 1);
    const size_t begin = slash == std::string::npos ? 0 : slash + 1;
    return name.substr(begin, dot - begin);
}

static std::string lower_ascii(std::string value)
{
    for (size_t i = 0; i < value.size(); ++i)
        if (value[i] >= 'A' && value[i] <= 'Z')
            value[i] = (char)(value[i] + ('a' - 'A'));
    return value;
}

static std::string title_root(const std::string &title)
{
    const size_t tag = title.find(" (");
    return tag == std::string::npos ? title : title.substr(0, tag);
}

static std::string canonical_title_root(const std::string &title)
{
    std::string root = title_root(title);
    const size_t dash = root.find(" - ");
    if (dash != std::string::npos && dash > 0 && dash <= 5) {
        bool acronym = true;
        for (size_t i = 0; i < dash; ++i) {
            const char c = root[i];
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
                acronym = false;
                break;
            }
        }
        if (acronym && dash + 3 < root.size())
            root = root.substr(dash + 3);
    }
    return lower_ascii(root);
}

static uint64_t title_root_hash(const std::string &title)
{
    const std::string root = canonical_title_root(title);
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < root.size(); ++i) {
        hash ^= (unsigned char)root[i];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

static uint64_t title_hash_from_serial(const std::string &serial)
{
    size_t lo = 0;
    size_t hi = sizeof(k_psx_serial_title_hashes) / sizeof(k_psx_serial_title_hashes[0]);
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        const int cmp = strcmp(serial.c_str(), k_psx_serial_title_hashes[mid].serial);
        if (cmp == 0)
            return k_psx_serial_title_hashes[mid].title_hash;
        if (cmp < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return 0;
}

static std::string region_tag(const std::string &title)
{
    static const char *tags[] = {
        "(USA)", "(Europe)", "(Japan)", "(World)", "(Australia)", "(Korea)"
    };
    for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); ++i)
        if (title.find(tags[i]) != std::string::npos)
            return tags[i];
    return std::string();
}

static std::string strip_track_suffix(const std::string &title)
{
    const std::string lower = lower_ascii(title);
    const size_t marker = lower.rfind(" (track ");
    if (marker == std::string::npos || title.empty() || title[title.size() - 1] != ')')
        return title;

    const size_t number_begin = marker + 8;
    if (number_begin >= title.size() - 1)
        return title;
    for (size_t i = number_begin; i + 1 < title.size(); ++i)
        if (title[i] < '0' || title[i] > '9')
            return title;
    return title.substr(0, marker);
}

static bool file_exists(const char *path)
{
    SceIoStat stat;
    memset(&stat, 0, sizeof(stat));
    return sceIoGetstat(path, &stat) >= 0;
}

static void scan_cover_index(std::vector<CoverEntry> &covers)
{
    SceUID dir = sceIoDopen(COVERS_DIR);
    if (dir < 0)
        return;

    SceIoDirent ent;
    memset(&ent, 0, sizeof(ent));
    while (sceIoDread(dir, &ent) > 0) {
        const std::string name(ent.d_name);
        if (ends_with_ci(name, ".png")) {
            CoverEntry cover;
            cover.title = strip_extension(name);
            cover.path = std::string(COVERS_DIR) + "/" + name;
            cover.title_hash = title_root_hash(cover.title);
            covers.push_back(cover);
        }
        memset(&ent, 0, sizeof(ent));
    }
    sceIoDclose(dir);
}

static std::string resolve_cover(const std::string &rom_title, const std::vector<CoverEntry> &covers)
{
    const std::string exact = std::string(COVERS_DIR) + "/" + rom_title + ".png";
    if (file_exists(exact.c_str()))
        return exact;

    const std::string wanted = lower_ascii(rom_title);
    for (size_t i = 0; i < covers.size(); ++i)
        if (lower_ascii(covers[i].title) == wanted)
            return covers[i].path;

    const std::string root = canonical_title_root(rom_title);
    const std::string region = region_tag(rom_title);
    for (size_t i = 0; i < covers.size(); ++i) {
        if (canonical_title_root(covers[i].title) != root)
            continue;
        if (!region.empty() && region_tag(covers[i].title) != region)
            continue;
        return covers[i].path;
    }

    for (size_t i = 0; i < covers.size(); ++i)
        if (canonical_title_root(covers[i].title) == root)
            return covers[i].path;

    return std::string();
}

static std::string resolve_cover_hash(uint64_t title_hash, const std::string &rom_title,
                                      const std::vector<CoverEntry> &covers)
{
    if (!title_hash)
        return std::string();

    const std::string region = region_tag(rom_title);
    if (!region.empty()) {
        for (size_t i = 0; i < covers.size(); ++i)
            if (covers[i].title_hash == title_hash && region_tag(covers[i].title) == region)
                return covers[i].path;
    }

    for (size_t i = 0; i < covers.size(); ++i)
        if (covers[i].title_hash == title_hash)
            return covers[i].path;

    return std::string();
}

static bool starts_with_ci(const std::string &value, const char *prefix)
{
    const size_t n = strlen(prefix);
    if (value.size() < n)
        return false;
    for (size_t i = 0; i < n; ++i) {
        char a = value[i];
        char b = prefix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
        if (a != b)
            return false;
    }
    return true;
}

static std::string trim_ascii(const std::string &value)
{
    size_t begin = 0;
    size_t end = value.size();
    while (begin < end && (value[begin] == ' ' || value[begin] == '\t' ||
                           value[begin] == '\r' || value[begin] == '\n'))
        ++begin;
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t' ||
                           value[end - 1] == '\r' || value[end - 1] == '\n'))
        --end;
    return value.substr(begin, end - begin);
}

static std::string path_dirname(const std::string &path)
{
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

static std::string resolve_relative_path(const std::string &base_file, std::string value)
{
    for (size_t i = 0; i < value.size(); ++i)
        if (value[i] == '\\')
            value[i] = '/';

    if (value.find(':') != std::string::npos || (!value.empty() && value[0] == '/'))
        return value;

    const std::string dir = path_dirname(base_file);
    return dir.empty() ? value : dir + "/" + value;
}

static std::vector<std::string> read_m3u_members(const std::string &path)
{
    std::vector<std::string> members;
    FILE *file = fopen(path.c_str(), "rb");
    if (!file)
        return members;
    char line[1024];
    while (members.size() < VITA_MAX_DISCS && fgets(line, sizeof(line), file)) {
        char *end = strpbrk(line, "\r\n");
        if (end)
            *end = '\0';
        if (!line[0] || line[0] == '#')
            continue;
        members.push_back(resolve_relative_path(path, line));
    }
    fclose(file);
    return members;
}

static std::string normalized_disc_path(std::string path)
{
    for (size_t i = 0; i < path.size(); ++i)
        if (path[i] == '\\')
            path[i] = '/';
    path = lower_ascii(path);
    size_t pos;
    while ((pos = path.find("/./")) != std::string::npos)
        path.erase(pos, 2);
    return path;
}

static std::string disc_path_base(const std::string &path)
{
    const size_t dot = path.find_last_of('.');
    return normalized_disc_path(dot == std::string::npos ? path : path.substr(0, dot));
}

static bool disc_image_or_cue(const std::string &path)
{
    return ends_with_ci(path, ".cue") || ends_with_ci(path, ".img") ||
        ends_with_ci(path, ".bin");
}

static bool has_clonecd_metadata(const std::string &path)
{
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos)
        return false;
    const std::string base = path.substr(0, dot);
    return file_exists((base + ".ccd").c_str()) || file_exists((base + ".CCD").c_str());
}

static bool cue_file_from_line(const std::string &line, std::string &out)
{
    size_t pos = 4;
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t'))
        ++pos;
    if (pos >= line.size())
        return false;

    if (line[pos] == '"') {
        const size_t end = line.find('"', pos + 1);
        if (end == std::string::npos)
            return false;
        out = line.substr(pos + 1, end - pos - 1);
    } else {
        size_t end = pos;
        while (end < line.size() && line[end] != ' ' && line[end] != '\t' &&
               line[end] != '\r' && line[end] != '\n')
            ++end;
        out = line.substr(pos, end - pos);
    }
    return !out.empty();
}

static bool parse_cue_data_track(const std::string &cue_path, DataTrack &track)
{
    FILE *file = fopen(cue_path.c_str(), "rb");
    if (!file)
        return false;

    char line_buffer[1024];
    std::string current_file;
    bool data_track = false;
    while (fgets(line_buffer, sizeof(line_buffer), file)) {
        const std::string line = trim_ascii(line_buffer);
        if (starts_with_ci(line, "FILE")) {
            std::string parsed;
            if (cue_file_from_line(line, parsed))
                current_file = parsed;
            continue;
        }

        if (starts_with_ci(line, "TRACK")) {
            int number = 0;
            char mode[32] = {0};
            data_track = false;
            if (sscanf(line.c_str() + 5, "%d %31s", &number, mode) == 2) {
                const std::string lower_mode = lower_ascii(mode);
                if (lower_mode.find("mode1/") == 0 || lower_mode.find("mode2/") == 0) {
                    if (lower_mode.find("/2352") != std::string::npos) {
                        track.sector_size = 2352;
                        track.user_offset = lower_mode.find("mode2/") == 0 ? 24 : 16;
                    } else if (lower_mode.find("/2336") != std::string::npos) {
                        track.sector_size = 2336;
                        track.user_offset = 8;
                    } else if (lower_mode.find("/2048") != std::string::npos) {
                        track.sector_size = 2048;
                        track.user_offset = 0;
                    }
                    if (track.sector_size > 0 && !current_file.empty()) {
                        track.path = resolve_relative_path(cue_path, current_file);
                        track.data_offset = 0;
                        data_track = true;
                    }
                }
            }
            continue;
        }

        if (data_track && starts_with_ci(line, "INDEX 01")) {
            int mm = 0, ss = 0, ff = 0;
            if (sscanf(line.c_str() + 8, "%d:%d:%d", &mm, &ss, &ff) == 3) {
                const uint64_t frames = (uint64_t)(mm * 60 + ss) * 75u + (uint64_t)ff;
                track.data_offset = frames * (uint64_t)track.sector_size;
            }
            fclose(file);
            return !track.path.empty();
        }
    }

    fclose(file);
    return data_track && !track.path.empty();
}

static bool prepare_data_track(const RomEntry &rom, DataTrack &track)
{
    if (ends_with_ci(rom.path, ".m3u")) {
        const std::vector<std::string> members = read_m3u_members(rom.path);
        for (size_t i = 0; i < members.size(); ++i) {
            if (ends_with_ci(members[i], ".m3u"))
                continue;
            RomEntry disc;
            disc.path = members[i];
            if (prepare_data_track(disc, track))
                return true;
        }
        return false;
    }
    if (ends_with_ci(rom.path, ".cue"))
        return parse_cue_data_track(rom.path, track);

    if (!ends_with_ci(rom.path, ".bin") && !ends_with_ci(rom.path, ".img") &&
        !ends_with_ci(rom.path, ".iso"))
        return false;

    track.path = rom.path;
    if (ends_with_ci(rom.path, ".iso")) {
        track.sector_size = 2048;
        track.user_offset = 0;
        return true;
    }

    FILE *file = fopen(track.path.c_str(), "rb");
    if (!file)
        return false;
    unsigned char header[24] = {0};
    const size_t got = fread(header, 1, sizeof(header), file);
    fclose(file);

    static const unsigned char sync[12] = {
        0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00
    };
    if (got >= sizeof(header) && memcmp(header, sync, sizeof(sync)) == 0) {
        track.sector_size = 2352;
        track.user_offset = header[15] == 2 ? 24 : 16;
    } else {
        track.sector_size = 2048;
        track.user_offset = 0;
    }
    return true;
}

static bool read_user_sector(FILE *file, const DataTrack &track, uint32_t lba,
                             unsigned char *out)
{
    const uint64_t offset = track.data_offset + (uint64_t)lba * (uint64_t)track.sector_size +
                            (uint64_t)track.user_offset;
    if (fseek(file, (long)offset, SEEK_SET) != 0)
        return false;
    return fread(out, 1, 2048, file) == 2048;
}

static uint32_t read_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static std::string iso_text_field(const unsigned char *p, size_t size)
{
    size_t begin = 0;
    size_t end = size;
    while (begin < end && p[begin] == ' ')
        ++begin;
    while (end > begin && (p[end - 1] == ' ' || p[end - 1] == 0))
        --end;
    return std::string((const char *)p + begin, end - begin);
}

static std::string normalize_psx_serial(const unsigned char *p, size_t size)
{
    std::string serial;
    for (size_t i = 0; i < size && serial.size() < 9; ++i) {
        char c = (char)p[i];
        if (c >= 'a' && c <= 'z')
            c = (char)(c - ('a' - 'A'));
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            serial.push_back(c);
    }
    return serial;
}

static std::string serial_from_system_cnf(const unsigned char *data, size_t size)
{
    for (size_t i = 0; i + 6 < size; ++i) {
        if (!((data[i] == 'c' || data[i] == 'C') &&
              (data[i + 1] == 'd' || data[i + 1] == 'D') &&
              (data[i + 2] == 'r' || data[i + 2] == 'R') &&
              (data[i + 3] == 'o' || data[i + 3] == 'O') &&
              (data[i + 4] == 'm' || data[i + 4] == 'M')))
            continue;

        size_t pos = i + 5;
        while (pos < size && pos < i + 12 && data[pos] != ':')
            ++pos;
        if (pos >= size || data[pos] != ':')
            continue;
        ++pos;
        while (pos < size && (data[pos] == '\\' || data[pos] == '/' ||
                              data[pos] == ' ' || data[pos] == '\t'))
            ++pos;

        const size_t begin = pos;
        while (pos < size && pos < begin + 32 && data[pos] != ';' &&
               data[pos] != '\r' && data[pos] != '\n')
            ++pos;
        const std::string serial = normalize_psx_serial(data + begin, pos - begin);
        if (serial.size() >= 8)
            return serial;
    }
    return std::string();
}

static uint64_t disc_title_hash(const RomEntry &rom, std::string *serial_out,
                                std::string *volume_out)
{
    DataTrack track;
    if (!prepare_data_track(rom, track))
        return 0;

    FILE *file = fopen(track.path.c_str(), "rb");
    if (!file)
        return 0;

    unsigned char sector[2048];
    if (!read_user_sector(file, track, 16, sector) || sector[0] != 1 ||
        memcmp(sector + 1, "CD001", 5) != 0) {
        fclose(file);
        return 0;
    }

    if (volume_out)
        *volume_out = iso_text_field(sector + 40, 32);

    const unsigned char *root_record = sector + 156;
    if (root_record[0] < 34) {
        fclose(file);
        return 0;
    }

    const uint32_t root_lba = read_le32(root_record + 2);
    const uint32_t root_size = read_le32(root_record + 10);
    const uint32_t root_sectors = std::min<uint32_t>(64, (root_size + 2047u) / 2048u);
    uint32_t cnf_lba = 0;

    for (uint32_t s = 0; s < root_sectors && !cnf_lba; ++s) {
        if (!read_user_sector(file, track, root_lba + s, sector))
            break;
        size_t pos = 0;
        while (pos + 34 <= sizeof(sector)) {
            const unsigned record_len = sector[pos];
            if (!record_len)
                break;
            if (pos + record_len > sizeof(sector) || record_len < 34)
                break;
            const unsigned name_len = sector[pos + 32];
            if (33u + name_len <= record_len) {
                const std::string name((const char *)sector + pos + 33, name_len);
                const std::string lower = lower_ascii(name);
                if (lower == "system.cnf;1" || lower == "system.cnf") {
                    cnf_lba = read_le32(sector + pos + 2);
                    break;
                }
            }
            pos += record_len;
        }
    }

    std::string serial;
    if (cnf_lba && read_user_sector(file, track, cnf_lba, sector))
        serial = serial_from_system_cnf(sector, sizeof(sector));
    fclose(file);

    if (serial_out)
        *serial_out = serial;
    return serial.empty() ? 0 : title_hash_from_serial(serial);
}

static std::string resolve_rom_cover(RomEntry &rom, const std::vector<CoverEntry> &covers)
{
    std::string serial;
    std::string volume;
    const uint64_t metadata_hash = disc_title_hash(rom, &serial, &volume);
    rom.game_hash = metadata_hash;
    rom.disc_serial = serial;
    const std::string metadata_cover = resolve_cover_hash(metadata_hash, rom.title, covers);
    if (!metadata_cover.empty()) {
        sceClibPrintf("[ROMSEL] cover metadata: %s serial=%s volume=%s -> %s\n",
            rom.title.c_str(), serial.c_str(), volume.c_str(), metadata_cover.c_str());
        return metadata_cover;
    }

    const std::string fallback = resolve_cover(rom.title, covers);
    if (fallback.empty())
        sceClibPrintf("[ROMSEL] no cover: %s serial=%s volume=%s\n",
            rom.title.c_str(), serial.c_str(), volume.c_str());
    return fallback;
}

static void scan_rom_dir(const char *root, std::vector<RomEntry> &roms)
{
    SceUID dir = sceIoDopen(root);
    if (dir < 0)
        return;

    SceIoDirent ent;
    memset(&ent, 0, sizeof(ent));
    while (sceIoDread(dir, &ent) > 0) {
        const std::string name(ent.d_name);
        if (is_rom_file(name)) {
            RomEntry rom;
            rom.path = vita_rom_paths::join(root, name);
            rom.name = name;
            rom.title = strip_extension(name);
            roms.push_back(rom);
        }
        memset(&ent, 0, sizeof(ent));
    }
    sceIoDclose(dir);
}

static void scan_roms(std::vector<RomEntry> &roms,
                      const std::vector<std::string> &extra_folders)
{
    scan_rom_dir(ROMS_DIR, roms);
    for (size_t i = 0; i < extra_folders.size(); ++i)
        scan_rom_dir(extra_folders[i].c_str(), roms);

    std::sort(roms.begin(), roms.end(), [](const RomEntry &a, const RomEntry &b) {
        if (lower_ascii(a.title) != lower_ascii(b.title))
            return lower_ascii(a.title) < lower_ascii(b.title);
        return lower_ascii(a.path) < lower_ascii(b.path);
    });

    std::vector<std::string> playlist_members;
    for (size_t i = 0; i < roms.size(); ++i) {
        if (!ends_with_ci(roms[i].path, ".m3u"))
            continue;
        roms[i].disc_members = read_m3u_members(roms[i].path);
        bool complete = !roms[i].disc_members.empty();
        for (size_t j = 0; j < roms[i].disc_members.size(); ++j)
            if (!file_exists(roms[i].disc_members[j].c_str()))
                complete = false;
        if (complete)
            playlist_members.insert(playlist_members.end(),
                roms[i].disc_members.begin(), roms[i].disc_members.end());
    }

    std::vector<RomEntry> filtered;
    filtered.reserve(roms.size());
    std::set<std::string> seen_paths;
    for (size_t i = 0; i < roms.size(); ++i) {
        if (!seen_paths.insert(normalized_disc_path(roms[i].path)).second)
            continue;

        bool in_playlist = false;
        for (size_t j = 0; j < playlist_members.size(); ++j) {
            if (normalized_disc_path(roms[i].path) == normalized_disc_path(playlist_members[j]) ||
                (disc_image_or_cue(roms[i].path) && disc_image_or_cue(playlist_members[j]) &&
                 disc_path_base(roms[i].path) == disc_path_base(playlist_members[j]))) {
                in_playlist = true;
                break;
            }
        }
        if (in_playlist)
            continue;

        if (ends_with_ci(roms[i].name, ".cue") && has_clonecd_metadata(roms[i].path)) {
            const std::string base = roms[i].path.substr(0, roms[i].path.size() - 4);
            if (file_exists((base + ".img").c_str()) || file_exists((base + ".IMG").c_str()))
                continue;
        }

        if (ends_with_ci(roms[i].name, ".bin") ||
            (ends_with_ci(roms[i].name, ".img") && !has_clonecd_metadata(roms[i].path))) {
            bool has_cue = false;
            const std::string bin_title = lower_ascii(strip_track_suffix(roms[i].title));
            for (size_t j = 0; j < roms.size(); ++j) {
                if (j == i || !ends_with_ci(roms[j].name, ".cue"))
                    continue;
                if (lower_ascii(roms[j].title) == bin_title &&
                    lower_ascii(path_dirname(roms[j].path)) == lower_ascii(path_dirname(roms[i].path))) {
                    has_cue = true;
                    break;
                }
            }
            if (has_cue)
                continue;
        }
        filtered.push_back(roms[i]);
    }

    roms.swap(filtered);
}

static bool rom_folder_accessible(const std::string &folder)
{
    const SceUID dir = sceIoDopen(folder.c_str());
    if (dir < 0)
        return false;
    sceIoDclose(dir);
    return true;
}

static std::vector<std::string> rom_folder_children(const std::string &folder)
{
    std::vector<std::string> result;
    if (folder.empty()) {
        static const char *mounts[] = {
            "ux0:", "uma0:", "imc0:", "xmc0:", "grw0:", "ur0:", "host0:"
        };
        for (size_t i = 0; i < sizeof(mounts) / sizeof(mounts[0]); ++i)
            if (rom_folder_accessible(mounts[i]))
                result.push_back(mounts[i]);
        return result;
    }

    const SceUID dir = sceIoDopen(folder.c_str());
    if (dir < 0)
        return result;
    SceIoDirent ent;
    memset(&ent, 0, sizeof(ent));
    while (sceIoDread(dir, &ent) > 0) {
        const std::string name(ent.d_name);
        if (name != "." && name != ".." && !name.empty()) {
            const std::string child = vita_rom_paths::join(folder, name);
            if (child.size() <= vita_rom_paths::kMaxFolderLength &&
                rom_folder_accessible(child))
                result.push_back(name);
        }
        memset(&ent, 0, sizeof(ent));
    }
    sceIoDclose(dir);
    std::sort(result.begin(), result.end(), [](const std::string &a, const std::string &b) {
        return lower_ascii(a) < lower_ascii(b);
    });
    return result;
}

static bool create_disc_playlist(const std::vector<RomEntry> &roms,
                                 const std::vector<int> &marked,
                                 std::string *created_path, std::string *reason)
{
    std::vector<vita_disc_grouping::Candidate> candidates;
    std::string first_region;
    for (size_t i = 0; i < marked.size(); ++i) {
        const RomEntry &rom = roms[marked[i]];
        const std::string region = region_tag(rom.title);
        if (i == 0)
            first_region = region;
        else if ((!region.empty() || !first_region.empty()) && region != first_region) {
            *reason = "Selected discs have different regions.";
            return false;
        }
        if (!ends_with_ci(rom.path, ".cue") && !ends_with_ci(rom.path, ".img") &&
            !ends_with_ci(rom.path, ".bin") && !ends_with_ci(rom.path, ".chd") &&
            !ends_with_ci(rom.path, ".iso")) {
            *reason = "Only individual CD images can be grouped.";
            return false;
        }
        if (!file_exists(rom.path.c_str()) || rom.path.size() >= 900) {
            *reason = "A selected disc is missing or has an invalid path.";
            return false;
        }
        vita_disc_grouping::Candidate candidate;
        candidate.filename = rom.name;
        candidate.title = rom.title;
        candidate.game_hash = rom.game_hash;
        candidate.serial = rom.disc_serial;
        candidates.push_back(candidate);
    }

    std::vector<unsigned> order;
    std::string name;
    if (!vita_disc_grouping::validate(candidates, &order, &name, reason))
        return false;

    const std::string playlist_path = std::string(ROMS_DIR) + "/" + name;
    const SceUID fd = sceIoOpen(playlist_path.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_EXCL, 0666);
    if (fd < 0) {
        *reason = "Cannot create playlist (it may already exist).";
        return false;
    }

    bool ok = true;
    for (size_t i = 0; i < order.size(); ++i) {
        const std::string &disc_path = roms[marked[order[i]]].path;
        const std::string line =
            vita_rom_paths::key(path_dirname(disc_path)) == vita_rom_paths::key(ROMS_DIR) ?
            candidates[order[i]].filename + "\n" : disc_path + "\n";
        if (sceIoWrite(fd, line.data(), (SceSize)line.size()) != (int)line.size()) {
            ok = false;
            break;
        }
    }
    if (sceIoClose(fd) < 0)
        ok = false;
    if (!ok) {
        sceIoRemove(playlist_path.c_str());
        *reason = "Failed to save the playlist.";
        return false;
    }
    *created_path = playlist_path;
    return true;
}

static bool build_disc_mesh()
{
    if (g_disc_mesh.top_vertices)
        return true;

    const int top_count = DISC_SEGMENTS * 6;
    const int body_count = DISC_SEGMENTS * 30;
    const size_t top_v3 = (size_t)top_count * 3u;
    const size_t top_v2 = (size_t)top_count * 2u;
    const size_t body_v3 = (size_t)body_count * 3u;
    const size_t body_v4 = (size_t)body_count * 4u;

    g_disc_mesh.top_vertices = (float *)vglAlloc((uint32_t)(top_v3 * sizeof(float)), VGL_MEM_RAM);
    g_disc_mesh.top_uvs = (float *)vglAlloc((uint32_t)(top_v2 * sizeof(float)), VGL_MEM_RAM);
    g_disc_mesh.body_vertices = (float *)vglAlloc((uint32_t)(body_v3 * sizeof(float)), VGL_MEM_RAM);
    g_disc_mesh.body_colors = (float *)vglAlloc((uint32_t)(body_v4 * sizeof(float)), VGL_MEM_RAM);
    if (!g_disc_mesh.top_vertices || !g_disc_mesh.top_uvs ||
        !g_disc_mesh.body_vertices || !g_disc_mesh.body_colors)
        return false;

    g_disc_mesh.top_vertex_count = top_count;
    g_disc_mesh.body_vertex_count = body_count;

    const float inner = 0.126f;
    const float back_split = 0.30f;
    const float z_top = 0.009f;
    const float z_bottom = -0.009f;
    const float z_inner_back = -0.0055f;
    int top_v = 0;
    int body_v = 0;

    auto push_top = [&](float x, float y) {
        g_disc_mesh.top_vertices[top_v * 3 + 0] = x;
        g_disc_mesh.top_vertices[top_v * 3 + 1] = y;
        g_disc_mesh.top_vertices[top_v * 3 + 2] = z_top;
        g_disc_mesh.top_uvs[top_v * 2 + 0] = x * 0.5f + 0.5f;
        g_disc_mesh.top_uvs[top_v * 2 + 1] = y * 0.5f + 0.5f;
        ++top_v;
    };

    auto push_body = [&](float x, float y, float z, float shade, float material) {
        g_disc_mesh.body_vertices[body_v * 3 + 0] = x;
        g_disc_mesh.body_vertices[body_v * 3 + 1] = y;
        g_disc_mesh.body_vertices[body_v * 3 + 2] = z;
        g_disc_mesh.body_colors[body_v * 4 + 0] = 0.58f * shade;
        g_disc_mesh.body_colors[body_v * 4 + 1] = 0.61f * shade;
        g_disc_mesh.body_colors[body_v * 4 + 2] = 0.66f * shade;
        g_disc_mesh.body_colors[body_v * 4 + 3] = material;
        ++body_v;
    };

    for (int i = 0; i < DISC_SEGMENTS; ++i) {
        const float a0 = (float)i * 6.28318530718f / (float)DISC_SEGMENTS;
        const float a1 = (float)(i + 1) * 6.28318530718f / (float)DISC_SEGMENTS;
        const float c0 = cosf(a0), s0 = sinf(a0);
        const float c1 = cosf(a1), s1 = sinf(a1);

        push_top(c0, s0);
        push_top(c1, s1);
        push_top(c1 * inner, s1 * inner);
        push_top(c0, s0);
        push_top(c1 * inner, s1 * inner);
        push_top(c0 * inner, s0 * inner);

        const float underside = 0.78f + 0.08f * sinf(a0 * 3.0f);
        push_body(c0, s0, z_bottom, underside, 0.0f);
        push_body(c1 * back_split, s1 * back_split, z_bottom, underside, 0.0f);
        push_body(c1, s1, z_bottom, underside, 0.0f);
        push_body(c0, s0, z_bottom, underside, 0.0f);
        push_body(c0 * back_split, s0 * back_split, z_bottom, underside, 0.0f);
        push_body(c1 * back_split, s1 * back_split, z_bottom, underside, 0.0f);

        push_body(c0 * back_split, s0 * back_split, z_bottom, 0.42f, 0.25f);
        push_body(c1 * back_split, s1 * back_split, z_inner_back, 0.42f, 0.25f);
        push_body(c1 * back_split, s1 * back_split, z_bottom, 0.42f, 0.25f);
        push_body(c0 * back_split, s0 * back_split, z_bottom, 0.42f, 0.25f);
        push_body(c0 * back_split, s0 * back_split, z_inner_back, 0.42f, 0.25f);
        push_body(c1 * back_split, s1 * back_split, z_inner_back, 0.42f, 0.25f);

        push_body(c0 * back_split, s0 * back_split, z_inner_back, 0.70f, 0.25f);
        push_body(c1 * inner, s1 * inner, z_inner_back, 0.70f, 0.25f);
        push_body(c1 * back_split, s1 * back_split, z_inner_back, 0.70f, 0.25f);
        push_body(c0 * back_split, s0 * back_split, z_inner_back, 0.70f, 0.25f);
        push_body(c0 * inner, s0 * inner, z_inner_back, 0.70f, 0.25f);
        push_body(c1 * inner, s1 * inner, z_inner_back, 0.70f, 0.25f);

        const float edge = 0.48f + 0.08f * c0;
        push_body(c0, s0, z_bottom, edge, 1.0f);
        push_body(c1, s1, z_bottom, edge, 1.0f);
        push_body(c1, s1, z_top, edge, 1.0f);
        push_body(c0, s0, z_bottom, edge, 1.0f);
        push_body(c1, s1, z_top, edge, 1.0f);
        push_body(c0, s0, z_top, edge, 1.0f);

        push_body(c0 * inner, s0 * inner, z_inner_back, 0.32f, 1.0f);
        push_body(c1 * inner, s1 * inner, z_top, 0.32f, 1.0f);
        push_body(c1 * inner, s1 * inner, z_inner_back, 0.32f, 1.0f);
        push_body(c0 * inner, s0 * inner, z_inner_back, 0.32f, 1.0f);
        push_body(c0 * inner, s0 * inner, z_top, 0.32f, 1.0f);
        push_body(c1 * inner, s1 * inner, z_top, 0.32f, 1.0f);
    }

    return upload_disc_mesh();
}

static void create_disc_vao(GLuint *vao, GLuint *vbo, const DiscGpuVertex *vertices, int count)
{
    glGenVertexArrays(1, vao);
    glGenBuffers(1, vbo);
    glBindVertexArray(*vao);
    glBindBuffer(GL_ARRAY_BUFFER, *vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(count * (int)sizeof(DiscGpuVertex)),
        vertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(DiscGpuVertex),
        (void *)offsetof(DiscGpuVertex, x));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(DiscGpuVertex),
        (void *)offsetof(DiscGpuVertex, u));
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(DiscGpuVertex),
        (void *)offsetof(DiscGpuVertex, r));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

static bool upload_disc_mesh()
{
    std::vector<DiscGpuVertex> body((size_t)g_disc_mesh.body_vertex_count);
    std::vector<DiscGpuVertex> top((size_t)g_disc_mesh.top_vertex_count);

    for (int i = 0; i < g_disc_mesh.body_vertex_count; ++i) {
        DiscGpuVertex &v = body[(size_t)i];
        v.x = g_disc_mesh.body_vertices[i * 3 + 0];
        v.y = g_disc_mesh.body_vertices[i * 3 + 1];
        v.z = g_disc_mesh.body_vertices[i * 3 + 2];
        v.u = v.x * 0.5f + 0.5f;
        v.v = v.y * 0.5f + 0.5f;
        v.r = g_disc_mesh.body_colors[i * 4 + 0];
        v.g = g_disc_mesh.body_colors[i * 4 + 1];
        v.b = g_disc_mesh.body_colors[i * 4 + 2];
        v.a = g_disc_mesh.body_colors[i * 4 + 3];
    }

    for (int i = 0; i < g_disc_mesh.top_vertex_count; ++i) {
        DiscGpuVertex &v = top[(size_t)i];
        v.x = g_disc_mesh.top_vertices[i * 3 + 0];
        v.y = g_disc_mesh.top_vertices[i * 3 + 1];
        v.z = g_disc_mesh.top_vertices[i * 3 + 2];
        v.u = g_disc_mesh.top_uvs[i * 2 + 0];
        v.v = g_disc_mesh.top_uvs[i * 2 + 1];
        v.r = v.g = v.b = v.a = 1.0f;
    }

    create_disc_vao(&g_disc_body_vao, &g_disc_body_vbo, body.data(), g_disc_mesh.body_vertex_count);
    create_disc_vao(&g_disc_top_vao, &g_disc_top_vbo, top.data(), g_disc_mesh.top_vertex_count);
    return g_disc_body_vao != 0 && g_disc_top_vao != 0;
}

static void destroy_disc_mesh()
{
    if (g_disc_body_vao)
        glDeleteVertexArrays(1, &g_disc_body_vao);
    if (g_disc_body_vbo)
        glDeleteBuffers(1, &g_disc_body_vbo);
    if (g_disc_top_vao)
        glDeleteVertexArrays(1, &g_disc_top_vao);
    if (g_disc_top_vbo)
        glDeleteBuffers(1, &g_disc_top_vbo);
    g_disc_body_vao = g_disc_body_vbo = 0;
    g_disc_top_vao = g_disc_top_vbo = 0;

    if (g_disc_mesh.top_vertices)
        vglFree(g_disc_mesh.top_vertices);
    if (g_disc_mesh.top_uvs)
        vglFree(g_disc_mesh.top_uvs);
    if (g_disc_mesh.body_vertices)
        vglFree(g_disc_mesh.body_vertices);
    if (g_disc_mesh.body_colors)
        vglFree(g_disc_mesh.body_colors);
    g_disc_mesh = DiscMesh();
}

static GLuint load_png_texture(const std::string &path, int max_w, int max_h,
                               int *out_width = NULL, int *out_height = NULL)
{
    png_image image;
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&image, path.c_str())) {
        sceClibPrintf("[ROMSEL] PNG open failed: %s (%s)\n", path.c_str(), image.message);
        return 0;
    }

    if (!image.width || !image.height || image.width > 2048 || image.height > 2048 ||
        (uint64_t)image.width * image.height > 3000000u) {
        sceClibPrintf("[ROMSEL] PNG too large: %s (%ux%u)\n",
            path.c_str(), (unsigned)image.width, (unsigned)image.height);
        png_image_free(&image);
        return 0;
    }
    image.format = PNG_FORMAT_RGBA;
    std::vector<unsigned char> pixels(PNG_IMAGE_SIZE(image));
    if (!png_image_finish_read(&image, NULL, pixels.data(), 0, NULL)) {
        sceClibPrintf("[ROMSEL] PNG decode failed: %s (%s)\n", path.c_str(), image.message);
        png_image_free(&image);
        return 0;
    }

    const int src_w = (int)image.width;
    const int src_h = (int)image.height;
    int dst_w = src_w;
    int dst_h = src_h;
    if (dst_w > max_w || dst_h > max_h) {
        const float scale = std::min((float)max_w / (float)dst_w,
                                     (float)max_h / (float)dst_h);
        dst_w = std::max(1, (int)(dst_w * scale));
        dst_h = std::max(1, (int)(dst_h * scale));
    }

    const unsigned char *upload = pixels.data();
    std::vector<unsigned char> scaled;
    if (dst_w != src_w || dst_h != src_h) {
        scaled.resize((size_t)dst_w * dst_h * 4u);
        for (int y = 0; y < dst_h; ++y) {
            const int sy = y * src_h / dst_h;
            for (int x = 0; x < dst_w; ++x) {
                const int sx = x * src_w / dst_w;
                const unsigned char *src = &pixels[((size_t)sy * src_w + sx) * 4u];
                unsigned char *dst = &scaled[((size_t)y * dst_w + x) * 4u];
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                dst[3] = src[3];
            }
        }
        upload = scaled.data();
    }

    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, dst_w, dst_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, upload);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    png_image_free(&image);
    if (out_width)
        *out_width = dst_w;
    if (out_height)
        *out_height = dst_h;
    return texture;
}

static void draw_selector_background(GLuint texture, int width, int height)
{
    const GLuint source = texture ? texture : g_disc_white_texture;
    if (!source)
        return;

    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
    if (texture && width > 0 && height > 0) {
        const float image_ratio = (float)width / (float)height;
        const float screen_ratio = (float)MENU_W / (float)MENU_H;
        if (image_ratio > screen_ratio) {
            u0 = (1.0f - screen_ratio / image_ratio) * 0.5f;
            u1 = 1.0f - u0;
        } else {
            v0 = (1.0f - image_ratio / screen_ratio) * 0.5f;
            v1 = 1.0f - v0;
        }
    }

    const ImVec2 positions[4] = {
        ImVec2(0, 0), ImVec2((float)MENU_W, 0),
        ImVec2((float)MENU_W, (float)MENU_H), ImVec2(0, (float)MENU_H)
    };
    const ImVec2 uvs[4] = {
        ImVec2(u0, v0), ImVec2(u1, v0), ImVec2(u1, v1), ImVec2(u0, v1)
    };
    const ImU32 top = texture ? IM_COL32(165, 172, 198, 255) : IM_COL32(14, 17, 31, 255);
    const ImU32 bottom = texture ? IM_COL32(122, 135, 184, 255) : IM_COL32(25, 32, 73, 255);
    const int corners[] = { 0, 1, 2, 0, 2, 3 };
    ImDrawVert quad[6];
    for (int i = 0; i < 6; ++i) {
        const int corner = corners[i];
        quad[i].pos = positions[corner];
        quad[i].uv = uvs[corner];
        quad[i].col = corner < 2 ? top : bottom;
    }

    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(g_imgui_program);
    if (g_imgui_screen_loc >= 0)
        glUniform4f(g_imgui_screen_loc, (float)MENU_W, (float)MENU_H, 0, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, source);
    glBindVertexArray(g_imgui_vao);
    glBindBuffer(GL_ARRAY_BUFFER, g_imgui_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glUseProgram(0);
    glDepthMask(GL_TRUE);
}

static GLuint get_cover_texture(const std::string &path)
{
    if (path.empty())
        return 0;

    for (int i = 0; i < DISC_CACHE_SLOTS; ++i) {
        if (g_cover_cache[i].texture && g_cover_cache[i].path == path) {
            g_cover_cache[i].stamp = ++g_cover_stamp;
            return g_cover_cache[i].texture;
        }
    }

    int slot = -1;
    unsigned int oldest = 0xffffffffu;
    for (int i = 0; i < DISC_CACHE_SLOTS; ++i) {
        if (!g_cover_cache[i].texture) {
            slot = i;
            break;
        }
        if (g_cover_cache[i].stamp < oldest) {
            oldest = g_cover_cache[i].stamp;
            slot = i;
        }
    }

    GLuint texture = load_png_texture(path, COVER_UPLOAD_MAX, COVER_UPLOAD_MAX);
    if (!texture)
        return 0;

    if (g_cover_cache[slot].texture)
        glDeleteTextures(1, &g_cover_cache[slot].texture);
    g_cover_cache[slot].path = path;
    g_cover_cache[slot].texture = texture;
    g_cover_cache[slot].stamp = ++g_cover_stamp;
    return texture;
}

static void destroy_cover_cache()
{
    for (int i = 0; i < DISC_CACHE_SLOTS; ++i) {
        if (g_cover_cache[i].texture)
            glDeleteTextures(1, &g_cover_cache[i].texture);
        g_cover_cache[i] = CoverCacheEntry();
    }
}

static void draw_disc(float cx, float cy, float z, float radius, float yaw,
                      float pitch, GLuint cover, float roll_degrees)
{
    const float deg_to_rad = 0.01745329251994329577f;
    const float yr = yaw * deg_to_rad;
    const float pr = pitch * deg_to_rad;
    const float rr = roll_degrees * deg_to_rad;

    glUseProgram(g_disc_program);
    if (g_disc_screen_loc >= 0)
        glUniform4f(g_disc_screen_loc, (float)MENU_W, (float)MENU_H, 0.0f, 0.0f);
    if (g_disc_center_loc >= 0)
        glUniform4f(g_disc_center_loc, cx, cy, z, 0.0f);
    if (g_disc_rotation_loc >= 0)
        glUniform4f(g_disc_rotation_loc, cosf(yr), sinf(yr), cosf(pr), sinf(pr));
    if (g_disc_roll_loc >= 0)
        glUniform2f(g_disc_roll_loc, cosf(rr), sinf(rr));
    if (g_disc_scale_loc >= 0)
        glUniform1f(g_disc_scale_loc, radius);

    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_BLEND);
    glBindTexture(GL_TEXTURE_2D, g_disc_white_texture);
    if (g_disc_gloss_loc >= 0)
        glUniform1f(g_disc_gloss_loc, 0.0f);
    if (g_disc_tint_loc >= 0)
        glUniform4f(g_disc_tint_loc, 1.0f, 1.0f, 1.0f, 1.0f);
    glBindVertexArray(g_disc_body_vao);
    glDrawArrays(GL_TRIANGLES, 0, g_disc_mesh.body_vertex_count);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindTexture(GL_TEXTURE_2D, cover ? cover : g_disc_white_texture);
    if (g_disc_gloss_loc >= 0)
        glUniform1f(g_disc_gloss_loc, cover ? 1.0f : 2.0f);
    if (g_disc_tint_loc >= 0)
        glUniform4f(g_disc_tint_loc, 1.0f, 1.0f, 1.0f, 1.0f);
    glBindVertexArray(g_disc_top_vao);
    glDrawArrays(GL_TRIANGLES, 0, g_disc_mesh.top_vertex_count);

    glBindVertexArray(0);
    glUseProgram(0);
}

static float disc_idle_offset(float idle_time, int index)
{
    return sinf(idle_time * 1.6f + (float)index * 0.58f) * 7.0f;
}

enum PadHint {
    PAD_HINT_CROSS,
    PAD_HINT_CIRCLE,
    PAD_HINT_SQUARE,
    PAD_HINT_TRIANGLE,
    PAD_HINT_DPAD,
    PAD_HINT_START,
    PAD_HINT_SELECT,
    PAD_HINT_RIGHT_STICK,
    PAD_HINT_DPAD_HORIZONTAL,
    PAD_HINT_DPAD_VERTICAL
};

static void draw_pad_hint(ImDrawList *draw, ImVec2 pos, PadHint button, const char *label)
{
    const ImVec2 center(pos.x + 10.0f, pos.y + 10.0f);
    const ImU32 outline = IM_COL32(46, 57, 88, 230);
    ImU32 glyph = IM_COL32(107, 172, 255, 255);
    if (button == PAD_HINT_CIRCLE)
        glyph = IM_COL32(249, 112, 125, 255);
    else if (button == PAD_HINT_TRIANGLE)
        glyph = IM_COL32(93, 218, 165, 255);
    else if (button == PAD_HINT_SQUARE)
        glyph = IM_COL32(223, 145, 218, 255);

    draw->AddCircleFilled(center, 11.0f, outline, 24);
    if (button == PAD_HINT_CROSS) {
        draw->AddLine(ImVec2(center.x - 4, center.y - 4),
            ImVec2(center.x + 4, center.y + 4), glyph, 2.0f);
        draw->AddLine(ImVec2(center.x + 4, center.y - 4),
            ImVec2(center.x - 4, center.y + 4), glyph, 2.0f);
    } else if (button == PAD_HINT_CIRCLE) {
        draw->AddCircle(center, 5.0f, glyph, 20, 2.0f);
    } else if (button == PAD_HINT_SQUARE) {
        draw->AddRect(ImVec2(center.x - 5, center.y - 5),
            ImVec2(center.x + 5, center.y + 5), glyph, 0, 0, 1.8f);
    } else if (button == PAD_HINT_TRIANGLE) {
        draw->AddTriangle(ImVec2(center.x, center.y - 5.5f),
            ImVec2(center.x + 5.8f, center.y + 4.8f),
            ImVec2(center.x - 5.8f, center.y + 4.8f), glyph, 1.8f);
    } else if (button == PAD_HINT_DPAD ||
               button == PAD_HINT_DPAD_HORIZONTAL ||
               button == PAD_HINT_DPAD_VERTICAL) {
        if (button != PAD_HINT_DPAD_VERTICAL) {
            draw->AddLine(ImVec2(center.x - 6, center.y),
                ImVec2(center.x - 2, center.y - 4), glyph, 1.6f);
            draw->AddLine(ImVec2(center.x - 6, center.y),
                ImVec2(center.x - 2, center.y + 4), glyph, 1.6f);
            draw->AddLine(ImVec2(center.x + 6, center.y),
                ImVec2(center.x + 2, center.y - 4), glyph, 1.6f);
            draw->AddLine(ImVec2(center.x + 6, center.y),
                ImVec2(center.x + 2, center.y + 4), glyph, 1.6f);
        }
        if (button != PAD_HINT_DPAD_HORIZONTAL) {
            draw->AddLine(ImVec2(center.x, center.y - 6),
                ImVec2(center.x - 4, center.y - 2), glyph, 1.6f);
            draw->AddLine(ImVec2(center.x, center.y - 6),
                ImVec2(center.x + 4, center.y - 2), glyph, 1.6f);
            draw->AddLine(ImVec2(center.x, center.y + 6),
                ImVec2(center.x - 4, center.y + 2), glyph, 1.6f);
            draw->AddLine(ImVec2(center.x, center.y + 6),
                ImVec2(center.x + 4, center.y + 2), glyph, 1.6f);
        }
    } else if (button == PAD_HINT_SELECT) {
        draw->AddCircleFilled(ImVec2(center.x - 4, center.y), 1.5f, glyph, 10);
        draw->AddCircleFilled(ImVec2(center.x, center.y), 1.5f, glyph, 10);
        draw->AddCircleFilled(ImVec2(center.x + 4, center.y), 1.5f, glyph, 10);
    } else if (button == PAD_HINT_RIGHT_STICK) {
        draw->AddCircle(center, 7.0f, glyph, 24, 1.5f);
        draw->AddCircleFilled(ImVec2(center.x + 1.8f, center.y - 1.8f),
            3.5f, glyph, 16);
    } else {
        draw->AddLine(ImVec2(center.x - 4, center.y - 2),
            ImVec2(center.x + 4, center.y - 2), glyph, 1.8f);
        draw->AddLine(ImVec2(center.x - 4, center.y + 2),
            ImVec2(center.x + 4, center.y + 2), glyph, 1.8f);
    }
    ImFont *font = menu_font(g_font_small);
    const float text_height = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, label).y;
    draw->AddText(font, font->FontSize,
        ImVec2(pos.x + 27.0f, center.y - text_height * 0.5f),
        IM_COL32(225, 231, 247, 255), label);
}

struct FooterHint {
    PadHint button;
    const char *label;
};

static void draw_footer_hints(ImDrawList *draw, float y,
                              const FooterHint *hints, int count)
{
    if (count <= 0)
        return;

    const float margin = 16.0f;
    const float slot_width = (MENU_W - margin * 2.0f) / (float)count;
    ImFont *font = menu_font(g_font_small);
    for (int i = 0; i < count; ++i) {
        const float text_width = font->CalcTextSizeA(
            font->FontSize, FLT_MAX, 0.0f, hints[i].label).x;
        const float group_width = 28.0f + text_width;
        const float slot_center = margin + slot_width * (i + 0.5f);
        draw_pad_hint(draw,
            ImVec2(slot_center - group_width * 0.5f + 1.0f, y),
            hints[i].button, hints[i].label);
    }
}

static void draw_menu_footer(ImDrawList *draw, bool selector, bool marked,
                             bool pause_main = false, bool multi_disc = false)
{
    draw->AddRectFilled(ImVec2(0, 496), ImVec2(MENU_W, MENU_H),
        IM_COL32(9, 12, 26, 195));
    draw->AddLine(ImVec2(0, 496), ImVec2(MENU_W, 496),
        IM_COL32(150, 167, 218, 35), 1.0f);
    const float y = 510.0f;
    FooterHint hints[9];
    int count = 0;
    if (selector) {
        hints[count++] = { PAD_HINT_CROSS, "Play" };
        hints[count++] = { PAD_HINT_DPAD_HORIZONTAL, "Games" };
        if (multi_disc)
            hints[count++] = { PAD_HINT_DPAD_VERTICAL, "Discs" };
        hints[count++] = { PAD_HINT_SQUARE, "Mark" };
        hints[count++] = { PAD_HINT_TRIANGLE, marked ? "Create set" : "Group" };
        hints[count++] = { PAD_HINT_START, "Options" };
        hints[count++] = { PAD_HINT_RIGHT_STICK, "Rotate" };
        hints[count++] = { PAD_HINT_SELECT, "Folders" };
        hints[count++] = { PAD_HINT_CIRCLE, "Exit" };
    } else {
        hints[count++] = { PAD_HINT_DPAD, "Select" };
        hints[count++] = { PAD_HINT_CROSS, "Confirm" };
        if (pause_main)
            hints[count++] = { PAD_HINT_START, "Start+Select: Resume" };
        else
            hints[count++] = { PAD_HINT_SQUARE, "Back" };
    }
    draw_footer_hints(draw, y, hints, count);
}

static void draw_disc_swap_hint(ImDrawList *draw, float x, float y, ImFont *font)
{
    const ImU32 accent = IM_COL32(248, 191, 69, 255);
    const ImU32 secondary = IM_COL32(158, 174, 214, 255);
    const float left = x + 8.0f, right = x + 30.0f;
    draw->AddCircle(ImVec2(left, y), 7.0f, secondary, 20, 1.5f);
    draw->AddCircle(ImVec2(right, y), 7.0f, secondary, 20, 1.5f);
    draw->AddCircleFilled(ImVec2(left, y), 1.8f, secondary, 12);
    draw->AddCircleFilled(ImVec2(right, y), 1.8f, secondary, 12);
    draw->AddLine(ImVec2(x + 14, y - 10), ImVec2(x + 26, y - 10), accent, 1.5f);
    draw->AddLine(ImVec2(x + 26, y - 10), ImVec2(x + 23, y - 13), accent, 1.5f);
    draw->AddLine(ImVec2(x + 26, y - 10), ImVec2(x + 23, y - 7), accent, 1.5f);
    draw->AddLine(ImVec2(x + 24, y + 10), ImVec2(x + 12, y + 10), accent, 1.5f);
    draw->AddLine(ImVec2(x + 12, y + 10), ImVec2(x + 15, y + 7), accent, 1.5f);
    draw->AddLine(ImVec2(x + 12, y + 10), ImVec2(x + 15, y + 13), accent, 1.5f);
    draw->AddText(font, font->FontSize, ImVec2(x + 49, y - 7),
        secondary, "Swap discs");
}

static std::string fit_ui_title(std::string title, ImFont *font, float width)
{
    if (font->CalcTextSizeA(font->FontSize, 10000.0f, 0, title.c_str()).x <= width)
        return title;
    while (!title.empty()) {
        unsigned char removed;
        do {
            removed = (unsigned char)title.back();
            title.pop_back();
        } while (!title.empty() && (removed & 0xc0u) == 0x80u);
        const std::string clipped = title + "...";
        if (font->CalcTextSizeA(font->FontSize, 10000.0f, 0, clipped.c_str()).x <= width)
            return clipped;
    }
    return "...";
}

static void restore_ui_state()
{
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_ALPHA_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glActiveTexture(GL_TEXTURE0);
    glViewport(0, 0, MENU_W, MENU_H);
}

static void save_overlay_gl_state(OverlayGLState &state)
{
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &state.framebuffer);
    glGetIntegerv(GL_VIEWPORT, state.viewport);
    glGetIntegerv(GL_CURRENT_PROGRAM, &state.program);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &state.active_texture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &state.texture0);
    glActiveTexture((GLenum)state.active_texture);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &state.array_buffer);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &state.element_array_buffer);
    glGetIntegerv(GL_BLEND_EQUATION, &state.blend_equation_rgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &state.blend_equation_alpha);
    glGetIntegerv(GL_BLEND_SRC_RGB, &state.blend_src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &state.blend_dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &state.blend_src_alpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &state.blend_dst_alpha);
    glGetIntegerv(GL_SCISSOR_BOX, state.scissor_box);
    state.blend = glIsEnabled(GL_BLEND);
    state.depth = glIsEnabled(GL_DEPTH_TEST);
    state.cull = glIsEnabled(GL_CULL_FACE);
    state.stencil = glIsEnabled(GL_STENCIL_TEST);
    state.scissor = glIsEnabled(GL_SCISSOR_TEST);
    state.alpha_test = glIsEnabled(GL_ALPHA_TEST);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &state.depth_mask);
    glGetBooleanv(GL_COLOR_WRITEMASK, state.color_mask);
}

static void set_gl_enable(GLenum cap, GLboolean enabled)
{
    if (enabled)
        glEnable(cap);
    else
        glDisable(cap);
}

static void restore_overlay_gl_state(const OverlayGLState &state)
{
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)state.framebuffer);
    glViewport(state.viewport[0], state.viewport[1], state.viewport[2], state.viewport[3]);
    glUseProgram((GLuint)state.program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)state.texture0);
    glActiveTexture((GLenum)state.active_texture);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)state.array_buffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)state.element_array_buffer);
    glBlendEquationSeparate((GLenum)state.blend_equation_rgb, (GLenum)state.blend_equation_alpha);
    glBlendFuncSeparate((GLenum)state.blend_src_rgb, (GLenum)state.blend_dst_rgb,
        (GLenum)state.blend_src_alpha, (GLenum)state.blend_dst_alpha);
    glScissor(state.scissor_box[0], state.scissor_box[1],
        state.scissor_box[2], state.scissor_box[3]);
    glDepthMask(state.depth_mask);
    glColorMask(state.color_mask[0], state.color_mask[1], state.color_mask[2], state.color_mask[3]);
    set_gl_enable(GL_BLEND, state.blend);
    set_gl_enable(GL_DEPTH_TEST, state.depth);
    set_gl_enable(GL_CULL_FACE, state.cull);
    set_gl_enable(GL_STENCIL_TEST, state.stencil);
    set_gl_enable(GL_SCISSOR_TEST, state.scissor);
    set_gl_enable(GL_ALPHA_TEST, state.alpha_test);
}

static std::string selector_option_value(const SelectorOption &option)
{
    const char *current = VitaSettings_GetOption(option.key);
    if (!current)
        return "?";

    if (option.psx_clock) {
        if (strcmp(current, "auto") == 0)
            return "Auto";
        return std::string(current) + "%";
    }

    for (int i = 0; i < option.choice_count; ++i)
        if (strcmp(current, option.choices[i].value) == 0)
            return option.choices[i].label;

    return current;
}

static void apply_aspect_setting(void)
{
    const char *value = VitaSettings_GetOption("vita_aspect_ratio");
    int mode = 0;
    if (value && strcmp(value, "16:9_stretched") == 0)
        mode = 1;
    else if (value && strcmp(value, "16:9_unstretched") == 0)
        mode = 2;
    const char *renderer = VitaSettings_GetOption("vita_renderer");
    if (renderer && strcmp(renderer, "neon") == 0 && mode == 2)
        mode = 0;
    VitaCtrNativeRenderer_SetAspectMode(mode);
}

static void adjust_selector_option(int option_index, int direction)
{
    if (option_index < 0 || option_index >= selector_option_count())
        return;

    const SelectorOption &option = selector_visible_option(option_index);
    if (!selector_option_available(option))
        return;
    const char *current = VitaSettings_GetOption(option.key);
    if (!current)
        return;

    const VitaPostFXParameter *parameter = selector_postfx_parameter(option.key);
    if (parameter) {
        const float current_value = strtof(current, NULL);
        const float next_value = std::max(parameter->minimum, std::min(parameter->maximum,
            floorf((current_value - parameter->minimum) / parameter->step + 0.5f +
                   (direction > 0 ? 1.0f : -1.0f)) * parameter->step +
                   parameter->minimum));
        char text[16];
        snprintf(text, sizeof(text), "%.2f", next_value);
        if (VitaSettings_SetOption(option.key, text))
            VitaSettings_Save();
        return;
    }

    if (option.psx_clock) {
        char value[16];
        if (strcmp(current, "auto") == 0) {
            snprintf(value, sizeof(value), "%d", direction > 0 ? 30 : 100);
        } else {
            int clock = atoi(current) + (direction > 0 ? 1 : -1);
            if (clock > 100 || clock < 30)
                snprintf(value, sizeof(value), "auto");
            else
                snprintf(value, sizeof(value), "%d", clock);
        }
        if (VitaSettings_SetOption(option.key, value))
            VitaSettings_Save();
        return;
    }

    int current_index = 0;
    for (int i = 0; i < option.choice_count; ++i) {
        if (strcmp(current, option.choices[i].value) == 0) {
            current_index = i;
            break;
        }
    }

    for (int tries = 0; tries < option.choice_count; ++tries) {
        current_index += direction > 0 ? 1 : -1;
        if (current_index < 0)
            current_index = option.choice_count - 1;
        if (current_index >= option.choice_count)
            current_index = 0;
        if (selector_choice_available(option, current_index))
            break;
    }

    if (VitaSettings_SetOption(option.key, option.choices[current_index].value)) {
        VitaSettings_Save();
        if (strcmp(option.key, "vita_aspect_ratio") == 0 ||
            strcmp(option.key, "vita_renderer") == 0)
            apply_aspect_setting();
    }
}

static void draw_selector_options(int selected)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.68f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.97f, 0.98f, 1.0f));
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiSetCond_Always);
    ImGui::SetNextWindowSize(ImVec2((float)MENU_W, (float)MENU_H), ImGuiSetCond_Always);
    ImGui::Begin("Selector Options", NULL,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    const float content_x = 120.0f;
    const float content_w = (float)MENU_W - content_x * 2.0f;
    const float row_h = 26.0f;
    const float options_top = 100.0f;
    const float options_bottom = 438.0f;
    const float scrollbar_x = content_x + content_w - 6.0f;
    const float option_right = scrollbar_x - 14.0f;
    const int option_count = selector_option_count();
    ImDrawList *draw = ImGui::GetWindowDrawList();

    ImGui::SetWindowFontScale(1.35f);
    ImGui::SetCursorPos(ImVec2(content_x, 48.0f));
    ImGui::Text("OPTIONS");
    ImGui::SetWindowFontScale(1.0f);
    draw->AddLine(ImVec2(content_x, 88.0f),
        ImVec2((float)MENU_W - content_x, 88.0f),
        IM_COL32(220, 220, 220, 150), 1.0f);
    float layout_y = options_top;
    float selected_y = options_top;
    const char *last_category = NULL;
    for (int i = 0; i < option_count; ++i) {
        const SelectorOption &option = selector_visible_option(i);
        if (!last_category || strcmp(last_category, option.category) != 0) {
            layout_y += 20.0f;
            last_category = option.category;
        }
        if (i == selected)
            selected_y = layout_y;
        layout_y += row_h;
    }
    const float max_scroll = std::max(0.0f, layout_y - options_bottom);
    const float target_scroll = selected_y - (options_top + (options_bottom - options_top - row_h) * 0.5f);
    const float scroll_y = std::max(0.0f, std::min(max_scroll, target_scroll));
    float y = options_top - scroll_y;
    last_category = NULL;

    if (max_scroll > 0.0f) {
        const float viewport_h = options_bottom - options_top;
        const float content_h = layout_y - options_top;
        const float thumb_h = std::max(40.0f, viewport_h * viewport_h / content_h);
        const float thumb_y = options_top + scroll_y * (viewport_h - thumb_h) / max_scroll;
        draw->AddRectFilled(ImVec2(scrollbar_x, options_top),
            ImVec2(scrollbar_x + 6.0f, options_bottom), IM_COL32(47, 56, 63, 185), 3.0f);
        draw->AddRectFilled(ImVec2(scrollbar_x, thumb_y),
            ImVec2(scrollbar_x + 6.0f, thumb_y + thumb_h), IM_COL32(0, 190, 220, 240), 3.0f);
    }

    ImGui::PushClipRect(ImVec2(content_x, options_top),
        ImVec2(option_right, options_bottom), true);
    for (int i = 0; i < option_count; ++i) {
        const SelectorOption &option = selector_visible_option(i);
        if (!last_category || strcmp(last_category, option.category) != 0) {
            if (y + 20.0f > options_top && y < options_bottom) {
                ImGui::SetCursorPos(ImVec2(content_x, y));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.78f, 0.92f, 1.0f));
                ImGui::Text("%s", option.category);
                ImGui::PopStyleColor();
            }
            y += 20.0f;
            last_category = option.category;
        }

        if (y + row_h > options_top && y < options_bottom) {
            if (i == selected) {
                draw->AddRectFilled(ImVec2(content_x, y),
                    ImVec2(option_right, y + row_h), IM_COL32(38, 42, 47, 220));
                draw->AddRect(ImVec2(content_x, y),
                    ImVec2(option_right, y + row_h), IM_COL32(0, 190, 220, 255));
            }

            const bool unavailable = !selector_option_available(option);
            if (unavailable)
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.47f, 0.49f, 1.0f));
            ImGui::SetCursorPos(ImVec2(content_x + 18.0f, y + 5.0f));
            ImGui::Text("%s", option.label);
            if (unavailable)
                ImGui::PopStyleColor();

            const std::string value = selector_option_value(option);
            const ImVec2 value_size = ImGui::CalcTextSize(value.c_str());
            ImGui::SetCursorPos(ImVec2(option_right - value_size.x - 18.0f, y + 5.0f));
            const bool inactive = unavailable ||
                (strcmp(option.key, "vita_aspect_ratio") == 0 &&
                 !selector_choice_available(option,
                     strcmp(VitaSettings_GetOption("vita_aspect_ratio"), "16:9_unstretched") == 0 ? 2 : 0));
            if (inactive)
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.47f, 0.49f, 1.0f));
            ImGui::Text("%s", value.c_str());
            if (inactive)
                ImGui::PopStyleColor();
        }
        y += row_h;
    }
    ImGui::PopClipRect();

    if (selected >= 0 && selected < option_count) {
        const float description_y = 454.0f;
        draw->AddLine(ImVec2(content_x, description_y - 8.0f),
            ImVec2((float)MENU_W - content_x, description_y - 8.0f),
            IM_COL32(180, 180, 180, 110), 1.0f);
        ImGui::SetCursorPos(ImVec2(content_x, description_y));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.77f, 0.80f, 1.0f));
        ImGui::PushTextWrapPos(content_x + content_w);
        ImGui::PushClipRect(ImVec2(content_x, description_y),
            ImVec2(content_x + content_w, MENU_H - 43.0f), true);
        const SelectorOption &option = selector_visible_option(selected);
        const bool neon_only = !selector_option_available(option);
        const bool inactive = strcmp(option.key, "vita_aspect_ratio") == 0 &&
            strcmp(VitaSettings_GetOption("vita_aspect_ratio"), "16:9_unstretched") == 0 &&
            !selector_choice_available(option, 2);
        const bool interpolation = strcmp(option.key, "vita_frame_interpolation") == 0;
        const char *renderer = VitaSettings_GetOption("vita_renderer");
        ImGui::TextWrapped("%s", neon_only && interpolation ?
            (renderer && strcmp(renderer, "neon") == 0 ?
                "Requires High Octane. The selected interpolation mode is saved but has no effect with NEON." :
                "Frame interpolation is unavailable while post-processing is enabled.") :
            (neon_only ?
            "Only available with the ReARMed NEON renderer. The saved setting has no effect on High Octane." :
            (inactive ? "16:9 Unstretched requires High Octane. The saved selection is preserved and 4:3 is used with Neon." :
            option.description)));
        ImGui::PopClipRect();
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }

    draw_menu_footer(draw, false, false);

    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

static void draw_rom_folders_menu(const std::vector<std::string> &extra_folders,
                                  int selected, bool browsing,
                                  const std::string &current_folder,
                                  const std::vector<std::string> &subfolders,
                                  const std::string &message)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.80f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.97f, 0.98f, 1.0f));
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiSetCond_Always);
    ImGui::SetNextWindowSize(ImVec2((float)MENU_W, (float)MENU_H), ImGuiSetCond_Always);
    ImGui::Begin("ROM Folders", NULL,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    const float left = 115.0f, right = MENU_W - left;
    ImDrawList *draw = ImGui::GetWindowDrawList();
    ImGui::SetWindowFontScale(1.35f);
    ImGui::SetCursorPos(ImVec2(left, 43.0f));
    ImGui::Text("%s", browsing ? "CHOOSE ROM DIRECTORY" : "ROM FOLDERS");
    ImGui::SetWindowFontScale(1.0f);
    draw->AddLine(ImVec2(left, 90.0f), ImVec2(right, 90.0f),
        IM_COL32(220, 220, 220, 140), 1.0f);
    ImGui::SetCursorPos(ImVec2(left, 105.0f));
    if (browsing) {
        const std::string path = current_folder.empty() ?
            "Select a storage device" :
            fit_ui_title(current_folder, menu_font(g_font_body), right - left);
        ImGui::Text("%s", path.c_str());
    } else {
        ImGui::Text("Default folder is always scanned. Up to %d extra folders.",
            vita_rom_paths::kMaxExtraFolders);
    }

    const int count = browsing ? (int)subfolders.size() : (int)extra_folders.size() + 2;
    const int visible_rows = 9;
    const int first = std::max(0, std::min(selected - visible_rows / 2,
        count - visible_rows));
    for (int row = 0; row < visible_rows && first + row < count; ++row) {
        const int index = first + row;
        const float y = 149.0f + (float)row * 31.0f;
        if (index == selected) {
            draw->AddRectFilled(ImVec2(left, y), ImVec2(right, y + 29.0f),
                IM_COL32(38, 42, 47, 220));
            draw->AddRect(ImVec2(left, y), ImVec2(right, y + 29.0f),
                IM_COL32(0, 190, 220, 255));
        }
        std::string label;
        if (browsing)
            label = subfolders[index];
        else if (index == 0)
            label = std::string(ROMS_DIR) + "  [Default]";
        else if (index <= (int)extra_folders.size())
            label = extra_folders[index - 1];
        else
            label = "+ Add another folder...";
        label = fit_ui_title(label, menu_font(g_font_body), right - left - 32.0f);
        ImGui::SetCursorPos(ImVec2(left + 14.0f, y + 5.0f));
        ImGui::Text("%s", label.c_str());
    }
    if (count == 0) {
        ImGui::SetCursorPos(ImVec2(left, 175.0f));
        ImGui::Text("No subdirectories found.");
    }
    if (!message.empty()) {
        ImGui::SetCursorPos(ImVec2(left, 446.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.80f, 0.52f, 1.0f));
        ImGui::Text("%s", fit_ui_title(message, menu_font(g_font_body),
            right - left).c_str());
        ImGui::PopStyleColor();
    }
    draw->AddRectFilled(ImVec2(0, 496), ImVec2(MENU_W, MENU_H),
        IM_COL32(9, 12, 26, 195));
    draw->AddLine(ImVec2(0, 496), ImVec2(MENU_W, 496),
        IM_COL32(150, 167, 218, 35), 1.0f);
    const float footer_y = 510.0f;
    FooterHint hints[5];
    int hint_count = 0;
    hints[hint_count++] = { PAD_HINT_DPAD, "Scroll (hold)" };
    hints[hint_count++] = { PAD_HINT_CROSS, browsing ? "Open" : "Add" };
    hints[hint_count++] = { PAD_HINT_SQUARE, browsing ? "Add here" : "Remove" };
    hints[hint_count++] = { PAD_HINT_CIRCLE, browsing ? "Up" : "Done" };
    if (browsing)
        hints[hint_count++] = { PAD_HINT_TRIANGLE, "Cancel" };
    draw_footer_hints(draw, footer_y, hints, hint_count);
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

static void draw_text_overlay(const std::vector<RomEntry> &roms, int selected,
                              const std::vector<int> &marked, float carousel_position,
                              float idle_time, const std::string &feedback,
                              unsigned selected_disc)
{
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiSetCond_Always);
    ImGui::SetNextWindowSize(ImVec2((float)MENU_W, (float)MENU_H), ImGuiSetCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("ROM Selector", NULL,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImDrawList *draw = ImGui::GetWindowDrawList();
    ImFont *body = menu_font(g_font_body);
    ImFont *small = menu_font(g_font_small);
    ImFont *title_font = menu_font(g_font_title);
    char counter[48];
    snprintf(counter, sizeof(counter), "%u / %u", roms.empty() ? 0u : (unsigned)selected + 1,
             (unsigned)roms.size());
    const float counter_width = small->CalcTextSizeA(small->FontSize, 1000.0f, 0.0f,
        counter).x;
    draw->AddRectFilled(ImVec2(32, 15), ImVec2(54 + counter_width, 39),
        IM_COL32(31, 42, 72, 210), 12.0f);
    draw->AddText(small, small->FontSize, ImVec2(43, 19),
        IM_COL32(205, 214, 240, 255), counter);
    const char *brand = "PCSX AwO v.0.5.0";
    const float brand_width = small->CalcTextSizeA(small->FontSize, 1000.0f, 0.0f,
        brand).x;
    const float badge_width = std::max(122.0f, brand_width + 20.0f);
    draw->AddRectFilled(ImVec2(938.0f - badge_width, 15), ImVec2(938, 39),
        IM_COL32(79, 37, 59, 195), 12.0f);
    draw->AddText(small, small->FontSize,
        ImVec2(938.0f - (badge_width + brand_width) * 0.5f, 19),
        IM_COL32(255, 171, 193, 255), brand);

    for (size_t i = 0; i < marked.size(); ++i) {
        const float distance = (float)marked[i] - carousel_position;
        const float abs_distance = fabsf(distance);
        if (abs_distance > 3.2f)
            continue;
        const float radius = 128.0f * std::max(0.50f, 1.0f - abs_distance * 0.19f);
        const float x = 282.0f + distance * 176.0f + radius * 0.65f;
        const float y = 267.0f + abs_distance * 12.0f +
                        disc_idle_offset(idle_time, marked[i]) - radius * 0.67f;
        if (x > 586.0f)
            continue;
        draw->AddCircleFilled(ImVec2(x, y), 14.0f, IM_COL32(248, 188, 61, 250));
        char label[8];
        snprintf(label, sizeof(label), "%u", (unsigned)i + 1);
        const ImVec2 size = small->CalcTextSizeA(small->FontSize, 80, 0, label);
        draw->AddText(small, small->FontSize,
            ImVec2(x - size.x * 0.5f, y - size.y * 0.5f),
            IM_COL32(25, 27, 42, 255), label);
    }

    if (!roms.empty()) {
        const RomEntry &rom = roms[selected];
        const float px = 588.0f, py = 56.0f, pw = 356.0f, ph = 430.0f;
        draw->AddRectFilled(ImVec2(px, py), ImVec2(px + pw, py + ph),
            IM_COL32(21, 27, 52, 239), 12.0f);
        draw->AddRect(ImVec2(px, py), ImVec2(px + pw, py + ph),
            IM_COL32(92, 107, 161, 54), 12.0f);
        draw->PushClipRect(ImVec2(px + 16, 70), ImVec2(px + pw - 16, 120), true);
        const std::string display_title = fit_ui_title(title_root(rom.title),
            title_font, pw - 32.0f);
        draw->AddText(title_font, title_font->FontSize, ImVec2(px + 16, 75),
            IM_COL32(243, 245, 255, 255), display_title.c_str());
        draw->PopClipRect();
        draw->AddText(small, small->FontSize, ImVec2(px + 16, 123),
            IM_COL32(164, 174, 204, 255), "Sony PlayStation  /  PS1");
        draw->AddLine(ImVec2(px + 16, 157), ImVec2(px + pw - 16, 157),
            IM_COL32(102, 116, 162, 65), 1.0f);

        const bool playlist = ends_with_ci(rom.name, ".m3u");
        if (playlist) {
            const std::vector<std::string> &discs = rom.disc_members;
            char subtitle[64];
            snprintf(subtitle, sizeof(subtitle), "DISC SET  /  %u DISCS", (unsigned)discs.size());
            draw->AddText(small, small->FontSize, ImVec2(px + 16, 172),
                IM_COL32(250, 191, 69, 255), subtitle);
            const float row_height = discs.size() > 5 ? 30.0f : 47.0f;
            for (size_t i = 0; i < discs.size(); ++i) {
                const float y = 200.0f + (float)i * row_height;
                if (y + row_height >= py + ph - 15.0f)
                    break;
                if (i == selected_disc) {
                    draw->AddRectFilled(ImVec2(px + 10, y),
                        ImVec2(px + pw - 10, y + row_height - 3),
                        IM_COL32(107, 85, 55, 135), 5.0f);
                    draw->AddRectFilled(ImVec2(px + 10, y),
                        ImVec2(px + 13, y + row_height - 3),
                        IM_COL32(255, 193, 61, 255));
                }
                char disc_label[24];
                snprintf(disc_label, sizeof(disc_label), "DISC %u", (unsigned)i + 1);
                draw->AddText(small, small->FontSize, ImVec2(px + 22, y + 5),
                    i == selected_disc ? IM_COL32(255, 214, 124, 255) :
                        IM_COL32(225, 231, 249, 255),
                    disc_label);
                if (i == selected_disc) {
                    draw->AddText(small, small->FontSize, ImVec2(px + pw - 71, y + 5),
                        IM_COL32(255, 214, 124, 255), "BOOT");
                }
                if (row_height > 40.0f) {
                    const std::string &path = discs[i];
                    const size_t slash = path.find_last_of("/\\");
                    const std::string filename = path.substr(slash == std::string::npos ? 0 : slash + 1);
                    draw->PushClipRect(ImVec2(px + 22, y + 22),
                        ImVec2(px + pw - 18, y + row_height - 3), true);
                    draw->AddText(small, small->FontSize, ImVec2(px + 22, y + 24),
                        IM_COL32(160, 172, 203, 255), filename.c_str());
                    draw->PopClipRect();
                }
            }
            draw_disc_swap_hint(draw, px + 17, py + ph - 24, small);
        } else {
            draw->AddText(small, small->FontSize, ImVec2(px + 16, 172),
                IM_COL32(250, 191, 69, 255), "GAME DETAILS");
            const std::string region = region_tag(rom.title);
            const std::string disc_id = rom.disc_serial.empty() ? "Not detected" : rom.disc_serial;
            const size_t extension = rom.name.find_last_of('.');
            const std::string format = extension == std::string::npos ? "Unknown" : rom.name.substr(extension + 1);
            const char *renderer = VitaSettings_GetOption("vita_renderer");
            const char *backend = renderer && strcmp(renderer, "neon") == 0 ?
                "ReARMed NEON" : "High Octane";
            const char *labels[] = { "REGION", "DISC ID", "FORMAT", "RENDERER" };
            const std::string values[] = { region.empty() ? "Unknown" : region, disc_id, format, backend };
            for (int i = 0; i < 4; ++i) {
                const float y = 206.0f + i * 57.0f;
                draw->AddText(small, small->FontSize, ImVec2(px + 17, y),
                    IM_COL32(143, 157, 194, 255), labels[i]);
                draw->AddText(body, body->FontSize, ImVec2(px + 17, y + 18),
                    IM_COL32(233, 238, 253, 255), values[i].c_str());
            }
        }
        if (!marked.empty()) {
            char marked_label[64];
            snprintf(marked_label, sizeof(marked_label), "%u discs selected",
                (unsigned)marked.size());
            draw->AddText(body, body->FontSize, ImVec2(34, 457),
                IM_COL32(255, 214, 124, 255), marked_label);
        }
    } else {
        draw->AddText(title_font, title_font->FontSize, ImVec2(205, 228),
            IM_COL32(240, 242, 251, 255), "No PlayStation games found");
        draw->AddText(small, small->FontSize, ImVec2(308, 272),
            IM_COL32(166, 177, 209, 255), "ux0:data/pcsx-awo/roms");
    }

    if (!feedback.empty()) {
        draw->AddRectFilled(ImVec2(28, 411), ImVec2(576, 447),
            IM_COL32(28, 36, 66, 230), 6.0f);
        draw->PushClipRect(ImVec2(38, 418), ImVec2(563, 440), true);
        draw->AddText(small, small->FontSize, ImVec2(41, 421),
            IM_COL32(248, 211, 137, 255), feedback.c_str());
        draw->PopClipRect();
    }
    draw_menu_footer(draw, true, !marked.empty(), false,
        !roms.empty() && roms[selected].disc_members.size() > 1);
    ImGui::End();
    ImGui::PopStyleVar();
}

static void draw_launch_overlay(const vita_launch_animation::Pose &pose,
                                float center_x, float center_y)
{
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiSetCond_Always);
    ImGui::SetNextWindowSize(ImVec2((float)MENU_W, (float)MENU_H), ImGuiSetCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Launch Transition", NULL,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImDrawList *draw = ImGui::GetWindowDrawList();
    if (pose.iris_active) {
        const float radius = pose.iris_radius;
        if (radius <= 1.0f) {
            draw->AddRectFilled(ImVec2(0, 0),
                ImVec2((float)MENU_W, (float)MENU_H), IM_COL32(0, 0, 0, 255));
        } else {
            const float tau = 6.2831853071795864769f;
            const float outer_radius = 1500.0f;
            const int segments = 72;
            const ImU32 black = IM_COL32(0, 0, 0, 255);
            const ImDrawListFlags flags = draw->Flags;
            draw->Flags &= ~ImDrawListFlags_AntiAliasedFill;
            for (int i = 0; i < segments; ++i) {
                const float a = tau * (float)i / (float)segments;
                const float b = tau * (float)(i + 1) / (float)segments;
                const float ax = cosf(a), ay = sinf(a);
                const float bx = cosf(b), by = sinf(b);
                draw->AddQuadFilled(
                    ImVec2(center_x + ax * radius, center_y + ay * radius),
                    ImVec2(center_x + ax * outer_radius, center_y + ay * outer_radius),
                    ImVec2(center_x + bx * outer_radius, center_y + by * outer_radius),
                    ImVec2(center_x + bx * radius, center_y + by * radius), black);
            }
            draw->Flags = flags;
            draw->AddCircle(ImVec2(center_x, center_y), radius, black, segments, 1.5f);
        }
    }

    ImGui::End();
    ImGui::PopStyleVar();
}

static void draw_pause_menu(int selected, const VitaDiscInfo *discs)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.42f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.97f, 0.98f, 1.0f));
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiSetCond_Always);
    ImGui::SetNextWindowSize(ImVec2((float)MENU_W, (float)MENU_H), ImGuiSetCond_Always);
    ImGui::Begin("Pause Menu", NULL,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    const float content_x = 110.0f;
    const float content_w = (float)MENU_W - content_x * 2.0f;

    ImGui::SetWindowFontScale(1.35f);
    ImGui::SetCursorPos(ImVec2(content_x, 82.0f));
    ImGui::Text("PAUSED");
    ImGui::SetWindowFontScale(1.0f);

    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddLine(ImVec2(content_x, 126.0f),
        ImVec2((float)MENU_W - content_x, 126.0f),
        IM_COL32(220, 220, 220, 150), 1.0f);

    static const char *items[] = { "Resume Game", "Restart Game", "Change Disc", "Save States", "Options", "Close Game" };
    const bool has_discs = discs && discs->count > 1;
    const int item_count = has_discs ? 6 : 5;
    const float first_row_y = 154.0f;
    const float row_h = 46.0f;
    for (int i = 0; i < item_count; ++i) {
        const float row_y = first_row_y + i * 56.0f;
        if (selected == i) {
            draw->AddRectFilled(ImVec2(content_x, row_y),
                ImVec2(content_x + content_w, row_y + row_h),
                IM_COL32(42, 46, 50, 210));
            draw->AddRect(ImVec2(content_x, row_y),
                ImVec2(content_x + content_w, row_y + row_h),
                IM_COL32(0, 190, 220, 255));
        }
        ImGui::SetCursorPos(ImVec2(content_x + 24.0f, row_y + 14.0f));
        ImGui::Text("%s", items[i + (!has_discs && i > 1 ? 1 : 0)]);
        if (has_discs && i == 2) {
            char status[40];
            if (discs->current < discs->count)
                snprintf(status, sizeof(status), "Disc %u/%u", discs->current + 1, discs->count);
            else
                snprintf(status, sizeof(status), "No disc inserted");
            const ImVec2 status_size = ImGui::CalcTextSize(status);
            ImGui::SetCursorPos(ImVec2(content_x + content_w - status_size.x - 24.0f,
                row_y + 14.0f));
            ImGui::Text("%s", status);
        }
    }

    draw_menu_footer(draw, false, false, true);
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

static void draw_save_states_menu(int selected, const char *game_path)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.42f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.97f, 0.98f, 1.0f));
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiSetCond_Always);
    ImGui::SetNextWindowSize(ImVec2((float)MENU_W, (float)MENU_H), ImGuiSetCond_Always);
    ImGui::Begin("Save States", NULL,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    const float content_x = 110.0f;
    const float content_w = (float)MENU_W - content_x * 2.0f;
    ImGui::SetWindowFontScale(1.35f);
    ImGui::SetCursorPos(ImVec2(content_x, 82.0f));
    ImGui::Text("SAVE STATES");
    ImGui::SetWindowFontScale(1.0f);

    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddLine(ImVec2(content_x, 126.0f),
        ImVec2((float)MENU_W - content_x, 126.0f),
        IM_COL32(220, 220, 220, 150), 1.0f);

    static const char *items[] = { "Save State", "Load State", "State Slot" };
    const int state_slot = VitaSaveStates_GetSlot();
    const bool state_exists = VitaSaveStates_Exists(game_path, state_slot);
    const float first_row_y = 154.0f;
    const float row_h = 46.0f;

    for (int i = 0; i < 3; ++i) {
        const float row_y = first_row_y + i * 56.0f;
        if (selected == i) {
            draw->AddRectFilled(ImVec2(content_x, row_y),
                ImVec2(content_x + content_w, row_y + row_h),
                IM_COL32(42, 46, 50, 210));
            draw->AddRect(ImVec2(content_x, row_y),
                ImVec2(content_x + content_w, row_y + row_h),
                IM_COL32(0, 190, 220, 255));
        }

        ImGui::SetCursorPos(ImVec2(content_x + 24.0f, row_y + 14.0f));
        if (i == 1 && !state_exists)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.48f, 0.50f, 0.53f, 1.0f));
        ImGui::Text("%s", items[i]);
        if (i == 1 && !state_exists)
            ImGui::PopStyleColor();

        if (i == 2) {
            char slot_text[32];
            snprintf(slot_text, sizeof(slot_text), "%d%s", state_slot,
                state_exists ? "  Saved" : "  Empty");
            const ImVec2 slot_size = ImGui::CalcTextSize(slot_text);
            ImGui::SetCursorPos(ImVec2(content_x + content_w - slot_size.x - 24.0f,
                row_y + 14.0f));
            ImGui::Text("%s", slot_text);
        }
    }

    draw_menu_footer(draw, false, false);
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

static void draw_disc_menu(unsigned selected, const VitaDiscInfo *discs)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.42f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.96f, 0.97f, 0.98f, 1.0f));
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiSetCond_Always);
    ImGui::SetNextWindowSize(ImVec2((float)MENU_W, (float)MENU_H), ImGuiSetCond_Always);
    ImGui::Begin("Change Disc", NULL,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    const float content_x = 110.0f;
    const float content_w = (float)MENU_W - content_x * 2.0f;
    ImGui::SetWindowFontScale(1.35f);
    ImGui::SetCursorPos(ImVec2(content_x, 70.0f));
    ImGui::Text("CHANGE DISC");
    ImGui::SetWindowFontScale(1.0f);
    ImDrawList *draw = ImGui::GetWindowDrawList();
    draw->AddLine(ImVec2(content_x, 114.0f),
        ImVec2((float)MENU_W - content_x, 114.0f),
        IM_COL32(220, 220, 220, 150), 1.0f);

    for (unsigned i = 0; i < discs->count; ++i) {
        const float y = 130.0f + i * 39.0f;
        if (selected == i) {
            draw->AddRectFilled(ImVec2(content_x, y),
                ImVec2(content_x + content_w, y + 35.0f),
                IM_COL32(42, 46, 50, 210));
            draw->AddRect(ImVec2(content_x, y),
                ImVec2(content_x + content_w, y + 35.0f),
                IM_COL32(0, 190, 220, 255));
        }
        ImGui::PushClipRect(ImVec2(content_x + 16.0f, y),
            ImVec2(content_x + content_w - 118.0f, y + 35.0f), true);
        ImGui::SetCursorPos(ImVec2(content_x + 18.0f, y + 9.0f));
        ImGui::Text("%u. %s", i + 1, discs->labels[i]);
        ImGui::PopClipRect();
        if (i == discs->current) {
            ImGui::SetCursorPos(ImVec2(content_x + content_w - 100.0f, y + 9.0f));
            ImGui::Text("Inserted");
        }
    }

    draw_menu_footer(draw, false, false);
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
}

static void pause_menu_task(void *arg)
{
    PauseMenuTask *task = (PauseMenuTask *)arg;
    task->result = 0;

    ImGui::CreateContext();
    init_menu_fonts();
    if (!ImGui_ImplVitaGL_Init()) {
        sceClibPrintf("[PAUSE] ImGui init failed\n");
        ImGui::DestroyContext();
        return;
    }
    ImGui_ImplVitaGL_TouchUsage(false);
    ImGui_ImplVitaGL_MouseStickUsage(false);
    ImGui_ImplVitaGL_GamepadUsage(false);
    ImGui::GetIO().MouseDrawCursor = false;
    ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowBorderSize = 0.0f;
    ImGui::GetStyle().WindowRounding = 5.0f;

    ImGui_ImplVitaGL_InvalidateDeviceObjects();
    if (!ImGui_ImplVitaGL_CreateDeviceObjects()) {
        sceClibPrintf("[PAUSE] ImGui font texture creation failed\n");
        ImGui_ImplVitaGL_Shutdown();
        ImGui::DestroyContext();
        return;
    }
    if (!create_imgui_program()) {
        sceClibPrintf("[PAUSE] ImGui custom shader failed\n");
        ImGui_ImplVitaGL_Shutdown();
        ImGui::DestroyContext();
        return;
    }

    int selected = 0;
    int options_selected = 0;
    int save_states_selected = 0;
    const bool has_discs = task->discs && task->discs->count > 1;
    const int item_count = has_discs ? 6 : 5;
    unsigned discs_selected = has_discs && task->discs->current < task->discs->count ?
        task->discs->current : 0;
    bool options_open = false;
    bool save_states_open = task->start_section == VITA_PAUSE_STATES;
    bool discs_open = has_discs && task->start_section == VITA_PAUSE_DISCS;
    uint32_t previous_buttons = 0;
    bool combo_released = false;
    const uint32_t combo = SCE_CTRL_START | SCE_CTRL_SELECT;

    for (;;) {
        SceCtrlData pad;
        memset(&pad, 0, sizeof(pad));
        if (sceCtrlPeekBufferPositiveExt2(0, &pad, 1) < 0)
            sceCtrlPeekBufferPositive(0, &pad, 1);

        const uint32_t pressed = pad.buttons & ~previous_buttons;
        const bool combo_down = (pad.buttons & combo) == combo;
        const bool previous_combo_down = (previous_buttons & combo) == combo;

        if (!combo_released) {
            if (!combo_down)
                combo_released = true;
        } else if (combo_down && !previous_combo_down) {
            task->result = 0;
            break;
        }

        if (options_open) {
            const int option_count = selector_option_count();
            if (pressed & SCE_CTRL_UP) {
                options_selected--;
                if (options_selected < 0)
                    options_selected = option_count - 1;
            }
            if (pressed & SCE_CTRL_DOWN) {
                options_selected++;
                if (options_selected >= option_count)
                    options_selected = 0;
            }
            if (pressed & SCE_CTRL_LEFT)
                adjust_selector_option(options_selected, -1);
            if ((pressed & SCE_CTRL_RIGHT) || (pressed & SCE_CTRL_CROSS))
                adjust_selector_option(options_selected, 1);
            if ((pressed & SCE_CTRL_SQUARE) || (pressed & SCE_CTRL_TRIANGLE))
                options_open = false;
        } else if (save_states_open) {
            if (pressed & SCE_CTRL_UP)
                save_states_selected = save_states_selected > 0 ? save_states_selected - 1 : 2;
            if (pressed & SCE_CTRL_DOWN)
                save_states_selected = save_states_selected < 2 ? save_states_selected + 1 : 0;
            if (save_states_selected == 2) {
                if (pressed & SCE_CTRL_LEFT)
                    VitaSaveStates_SetSlot(VitaSaveStates_GetSlot() - 1);
                if (pressed & SCE_CTRL_RIGHT)
                    VitaSaveStates_SetSlot(VitaSaveStates_GetSlot() + 1);
            }
            if (pressed & SCE_CTRL_CROSS) {
                if (save_states_selected == 0) {
                    task->result = VITA_PAUSE_SAVE_STATE;
                    break;
                }
                if (save_states_selected == 1) {
                    if (VitaSaveStates_Exists(task->game_path, VitaSaveStates_GetSlot())) {
                        task->result = VITA_PAUSE_LOAD_STATE;
                        break;
                    }
                } else if (save_states_selected == 2) {
                    VitaSaveStates_SetSlot(VitaSaveStates_GetSlot() + 1);
                }
            }
            if ((pressed & SCE_CTRL_SQUARE) || (pressed & SCE_CTRL_TRIANGLE))
                save_states_open = false;
        } else if (discs_open) {
            if (pressed & SCE_CTRL_UP)
                discs_selected = discs_selected > 0 ? discs_selected - 1 : task->discs->count - 1;
            if (pressed & SCE_CTRL_DOWN)
                discs_selected = discs_selected + 1 < task->discs->count ? discs_selected + 1 : 0;
            if (pressed & SCE_CTRL_CROSS) {
                task->disc_selection = discs_selected;
                task->result = VITA_PAUSE_CHANGE_DISC;
                break;
            }
            if ((pressed & SCE_CTRL_SQUARE) || (pressed & SCE_CTRL_TRIANGLE))
                discs_open = false;
        } else {
            if (pressed & SCE_CTRL_UP)
                selected = selected > 0 ? selected - 1 : item_count - 1;
            if (pressed & SCE_CTRL_DOWN)
                selected = selected + 1 < item_count ? selected + 1 : 0;
            if (pressed & SCE_CTRL_CROSS) {
                if (selected == 0) {
                    task->result = VITA_PAUSE_RESUME;
                    break;
                } else if (selected == 1) {
                    task->result = VITA_PAUSE_RESTART;
                    break;
                } else if (has_discs && selected == 2) {
                    discs_open = true;
                } else if (selected == (has_discs ? 3 : 2)) {
                    save_states_open = true;
                } else if (selected == (has_discs ? 4 : 3)) {
                    options_open = true;
                } else {
                    task->result = VITA_PAUSE_CLOSE_GAME;
                    break;
                }
            }
        }
        previous_buttons = pad.buttons;

        VitaCtrNativeRenderer_PresentCurrentFrame();
        OverlayGLState overlay_state;
        save_overlay_gl_state(overlay_state);
        restore_ui_state();
        ImGui_ImplVitaGL_NewFrame();
        if (options_open)
            draw_selector_options(options_selected);
        else if (save_states_open)
            draw_save_states_menu(save_states_selected, task->game_path);
        else if (discs_open)
            draw_disc_menu(discs_selected, task->discs);
        else
            draw_pause_menu(selected, task->discs);
        ImGui::Render();
        render_imgui_draw_data(ImGui::GetDrawData());
        restore_overlay_gl_state(overlay_state);
        NativeRenderer_SwapWindow();
        sceKernelDelayThread(16000);
    }

    glFinish();
    destroy_imgui_program();
    ImGui_ImplVitaGL_Shutdown();
    ImGui::DestroyContext();
}

static void selector_task(void *arg)
{
    SelectorTask *task = (SelectorTask *)arg;
    task->result = -1;

    std::vector<RomEntry> roms;
    std::vector<CoverEntry> covers;
    std::vector<std::string> extra_folders = vita_rom_paths::load();
    scan_roms(roms, extra_folders);
    scan_cover_index(covers);
    for (size_t i = 0; i < roms.size(); ++i)
        roms[i].cover_path = resolve_rom_cover(roms[i], covers);

    ImGui::CreateContext();
    init_menu_fonts();
    if (!ImGui_ImplVitaGL_Init()) {
        sceClibPrintf("[ROMSEL] ImGui init failed\n");
        ImGui::DestroyContext();
        return;
    }
    ImGui_ImplVitaGL_TouchUsage(false);
    ImGui_ImplVitaGL_MouseStickUsage(false);
    ImGui_ImplVitaGL_GamepadUsage(false);
    ImGui::GetIO().MouseDrawCursor = false;
    ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowBorderSize = 0.0f;
    ImGui::GetStyle().WindowRounding = 0.0f;

    ImGui_ImplVitaGL_InvalidateDeviceObjects();
    if (!ImGui_ImplVitaGL_CreateDeviceObjects()) {
        sceClibPrintf("[ROMSEL] ImGui font texture creation failed\n");
        ImGui_ImplVitaGL_Shutdown();
        ImGui::DestroyContext();
        return;
    }

    if (!create_imgui_program()) {
        sceClibPrintf("[ROMSEL] ImGui custom shader failed\n");
        ImGui_ImplVitaGL_Shutdown();
        ImGui::DestroyContext();
        return;
    }

    if (!create_disc_program()) {
        sceClibPrintf("[ROMSEL] disc shader failed\n");
        ImGui_ImplVitaGL_Shutdown();
        ImGui::DestroyContext();
        return;
    }

    if (!build_disc_mesh()) {
        sceClibPrintf("[ROMSEL] disc mesh failed\n");
        ImGui_ImplVitaGL_Shutdown();
        ImGui::DestroyContext();
        return;
    }

    GLuint background_texture = 0;
    int background_width = 0, background_height = 0;
    const std::string background_path = std::string(PCSX_AWO_APP_ROOT) + "/bg.png";
    if (file_exists(background_path.c_str()))
        background_texture = load_png_texture(background_path,
            BACKGROUND_UPLOAD_W, BACKGROUND_UPLOAD_H,
            &background_width, &background_height);

    task->result = 0;

    int selected = 0;
    std::vector<unsigned> selected_discs(roms.size(), 0);
    float carousel_position = 0.0f;
    float manual_yaw = 0.0f;
    float manual_pitch = 0.0f;
    float repeat_hold = 0.0f;
    float repeat_time = 0.0f;
    int repeat_dir = 0;
    bool options_open = false;
    int options_selected = 0;
    bool folders_open = false;
    bool folder_browser_open = false;
    bool folders_changed = false;
    int folders_selected = 0;
    int browser_selected = 0;
    int folder_repeat_dir = 0;
    bool folder_repeat_browser = false;
    float folder_repeat_hold = 0.0f;
    float folder_repeat_time = 0.0f;
    std::string browser_path;
    std::vector<std::string> browser_children;
    std::string folders_feedback;
    std::vector<int> marked;
    std::string feedback;
    float feedback_time = 0.0f;
    float idle_time = 0.0f;
    bool launch_pending = false;
    bool launching = false;
    SceUInt64 launch_started = 0;
    float launch_center_x = 282.0f;
    float launch_center_y = 267.0f;
    float launch_radius = 128.0f;
    float launch_yaw = -7.0f;
    float launch_pitch = 25.0f;
    uint32_t previous_buttons = 0;
    SceUInt64 previous_time = sceKernelGetProcessTimeWide();

    auto rescan_roms = [&](const std::string &wanted) {
        roms.clear();
        scan_roms(roms, extra_folders);
        selected_discs.assign(roms.size(), 0);
        marked.clear();
        selected = 0;
        for (size_t i = 0; i < roms.size(); ++i) {
            roms[i].cover_path = resolve_rom_cover(roms[i], covers);
            if (roms[i].path == wanted)
                selected = (int)i;
        }
        carousel_position = (float)selected;
        manual_yaw = manual_pitch = 0.0f;
    };

    for (;;) {
        const SceUInt64 now = sceKernelGetProcessTimeWide();
        float dt = (float)(now - previous_time) / 1000000.0f;
        previous_time = now;
        if (dt <= 0.0f || dt > 0.1f)
            dt = 1.0f / 60.0f;
        if (!launching)
            idle_time += dt;
        if (!launching && feedback_time > 0.0f) {
            feedback_time -= dt;
            if (feedback_time <= 0.0f)
                feedback.clear();
        }

        const float launch_time = launching ?
            (float)(now - launch_started) / 1000000.0f : 0.0f;
        if (launching && launch_time >= vita_launch_animation::kFinish)
            break;

        SceCtrlData pad;
        memset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositiveExt2(0, &pad, 1);
        const uint32_t pressed = pad.buttons & ~previous_buttons;
        previous_buttons = pad.buttons;
        const bool options_was_open = options_open;
        const bool folders_were_open = folders_open;
        if (!folders_open) {
            folder_repeat_dir = 0;
            folder_repeat_hold = folder_repeat_time = 0.0f;
        }

        if (!launch_pending && !launching && folders_open) {
            const uint32_t arrows = pad.buttons & (SCE_CTRL_UP | SCE_CTRL_DOWN);
            const int held_dir = arrows == SCE_CTRL_UP ? -1 :
                (arrows == SCE_CTRL_DOWN ? 1 : 0);
            int move_dir = 0;
            if (held_dir != folder_repeat_dir || folder_repeat_browser != folder_browser_open) {
                folder_repeat_dir = held_dir;
                folder_repeat_browser = folder_browser_open;
                folder_repeat_hold = folder_repeat_time = 0.0f;
                if ((held_dir < 0 && (pressed & SCE_CTRL_UP)) ||
                    (held_dir > 0 && (pressed & SCE_CTRL_DOWN)))
                    move_dir = held_dir;
            } else if (held_dir != 0) {
                folder_repeat_hold += dt;
                if (folder_repeat_hold >= 0.32f) {
                    folder_repeat_time += dt;
                    const float interval = folder_repeat_hold >= 1.25f ? 0.045f : 0.075f;
                    if (folder_repeat_time >= interval) {
                        folder_repeat_time -= interval;
                        move_dir = held_dir;
                    }
                }
            }
            const int folder_count = folder_browser_open ? (int)browser_children.size() :
                (int)extra_folders.size() + 2;
            if (move_dir != 0 && folder_count > 0) {
                int &index = folder_browser_open ? browser_selected : folders_selected;
                index = (index + folder_count + move_dir) % folder_count;
            }
            if (folder_browser_open) {
                const int count = (int)browser_children.size();
                if (count > 0 && (pressed & SCE_CTRL_CROSS)) {
                    browser_path = browser_path.empty() ? browser_children[browser_selected] :
                        vita_rom_paths::join(browser_path, browser_children[browser_selected]);
                    browser_children = rom_folder_children(browser_path);
                    browser_selected = 0;
                    folder_repeat_dir = 0;
                    folders_feedback.clear();
                } else if (pressed & SCE_CTRL_SQUARE) {
                    std::vector<std::string> updated = extra_folders;
                    if (browser_path.empty())
                        folders_feedback = "Select a storage device first.";
                    else if (!vita_rom_paths::add(updated, browser_path))
                        folders_feedback = "Folder already selected or the limit was reached.";
                    else if (!vita_rom_paths::save(updated))
                        folders_feedback = "Failed to save the ROM folder list.";
                    else {
                        extra_folders.swap(updated);
                        folders_changed = true;
                        folders_selected = (int)extra_folders.size();
                        folder_browser_open = false;
                        folders_feedback = "Folder added. Press Circle to scan for games.";
                    }
                } else if (pressed & SCE_CTRL_TRIANGLE) {
                    folder_browser_open = false;
                    folders_feedback.clear();
                } else if (pressed & SCE_CTRL_CIRCLE) {
                    if (browser_path.empty()) {
                        folder_browser_open = false;
                    } else {
                        browser_path = vita_rom_paths::parent(browser_path);
                        browser_children = rom_folder_children(browser_path);
                        browser_selected = 0;
                        folder_repeat_dir = 0;
                    }
                    folders_feedback.clear();
                }
            } else {
                const int count = (int)extra_folders.size() + 2;
                if ((pressed & SCE_CTRL_CROSS) && folders_selected == count - 1) {
                    browser_path.clear();
                    browser_children = rom_folder_children(browser_path);
                    browser_selected = 0;
                    folder_browser_open = true;
                    folders_feedback.clear();
                } else if ((pressed & SCE_CTRL_SQUARE) && folders_selected > 0 &&
                           folders_selected < count - 1) {
                    std::vector<std::string> updated = extra_folders;
                    vita_rom_paths::remove(updated, folders_selected - 1);
                    if (!vita_rom_paths::save(updated)) {
                        folders_feedback = "Failed to save the ROM folder list.";
                    } else {
                        extra_folders.swap(updated);
                        folders_selected = std::min(folders_selected,
                            (int)extra_folders.size() + 1);
                        folders_changed = true;
                        folders_feedback = "Folder removed. Press Circle to rescan.";
                    }
                } else if (pressed & SCE_CTRL_CIRCLE) {
                    folders_open = false;
                    if (folders_changed) {
                        const std::string wanted = roms.empty() ? std::string() : roms[selected].path;
                        rescan_roms(wanted);
                        folders_changed = false;
                    }
                    folders_feedback.clear();
                }
            }
        } else if (!launch_pending && !launching && options_open) {
            const int option_count = selector_option_count();
            if (pressed & SCE_CTRL_UP) {
                options_selected--;
                if (options_selected < 0)
                    options_selected = option_count - 1;
            }
            if (pressed & SCE_CTRL_DOWN) {
                options_selected++;
                if (options_selected >= option_count)
                    options_selected = 0;
            }
            if (pressed & SCE_CTRL_LEFT)
                adjust_selector_option(options_selected, -1);
            if ((pressed & SCE_CTRL_RIGHT) || (pressed & SCE_CTRL_CROSS))
                adjust_selector_option(options_selected, 1);
            if ((pressed & SCE_CTRL_SQUARE) || (pressed & SCE_CTRL_TRIANGLE) ||
                (pressed & SCE_CTRL_CIRCLE) || (pressed & SCE_CTRL_START))
                options_open = false;
        } else if (!launch_pending && !launching && !roms.empty()) {
            int held_dir = 0;
            const uint32_t prev_mask = SCE_CTRL_LEFT;
            const uint32_t next_mask = SCE_CTRL_RIGHT;
            const bool previous_held = (pad.buttons & prev_mask) != 0;
            const bool next_held = (pad.buttons & next_mask) != 0;
            if (previous_held && !next_held)
                held_dir = -1;
            else if (next_held && !previous_held)
                held_dir = 1;

            auto move_selection = [&](int direction) {
                selected += direction;
                if (selected < 0)
                    selected = (int)roms.size() - 1;
                if (selected >= (int)roms.size())
                    selected = 0;
                manual_yaw = 0.0f;
                manual_pitch = 0.0f;
            };

            const bool previous_pressed = (pressed & prev_mask) != 0;
            const bool next_pressed = (pressed & next_mask) != 0;
            if (held_dir && previous_pressed != next_pressed) {
                repeat_dir = previous_pressed ? -1 : 1;
                repeat_hold = 0.0f;
                repeat_time = 0.0f;
                move_selection(repeat_dir);
            } else if (held_dir) {
                if (held_dir != repeat_dir) {
                    repeat_dir = held_dir;
                    repeat_hold = 0.0f;
                    repeat_time = 0.0f;
                } else {
                    repeat_hold += dt;
                    if (repeat_hold > 0.35f) {
                        repeat_time += dt;
                        if (repeat_time >= 0.09f) {
                            repeat_time = 0.0f;
                            move_selection(repeat_dir);
                        }
                    }
                }
            } else {
                repeat_dir = 0;
                repeat_hold = 0.0f;
                repeat_time = 0.0f;
            }

            const unsigned disc_count = (unsigned)roms[selected].disc_members.size();
            if (disc_count > 1) {
                if (pressed & SCE_CTRL_UP)
                    selected_discs[selected] =
                        (selected_discs[selected] + disc_count - 1) % disc_count;
                else if (pressed & SCE_CTRL_DOWN)
                    selected_discs[selected] = (selected_discs[selected] + 1) % disc_count;
            }

            const int rx = (int)pad.rx - 128;
            const int ry = (int)pad.ry - 128;
            const int deadzone = 18;
            const float rotation_speed = 135.0f;
            if (rx > deadzone || rx < -deadzone)
                manual_yaw += ((float)rx / 127.0f) * rotation_speed * dt;
            if (ry > deadzone || ry < -deadzone)
                manual_pitch += ((float)ry / 127.0f) * rotation_speed * dt;
            if (manual_yaw > 360.0f) manual_yaw -= 360.0f;
            if (manual_yaw < -360.0f) manual_yaw += 360.0f;
            if (manual_pitch > 360.0f) manual_pitch -= 360.0f;
            if (manual_pitch < -360.0f) manual_pitch += 360.0f;

            if (pressed & SCE_CTRL_CROSS) {
                const RomEntry &rom = roms[selected];
                if (rom.path.size() >= task->out_size) {
                    feedback = "ROM path exceeds the supported length.";
                    feedback_time = 4.0f;
                    continue;
                }
                const unsigned disc = selected_discs[selected];
                const std::string image_path = disc < rom.disc_members.size() ?
                    rom.disc_members[disc] : std::string();
                if (disc > 0 && (image_path.empty() ||
                    image_path.size() >= sizeof(task->boot_disc->path))) {
                    feedback = "Selected disc path is invalid or too long.";
                    feedback_time = 4.0f;
                    continue;
                }
                snprintf(task->out_path, task->out_size, "%s", rom.path.c_str());
                task->boot_disc->index = disc;
                snprintf(task->boot_disc->path, sizeof(task->boot_disc->path),
                    "%s", image_path.c_str());
                task->result = 1;
                launch_pending = true;
            }
            if (!launch_pending && (pressed & SCE_CTRL_SQUARE)) {
                std::vector<int>::iterator it = std::find(marked.begin(), marked.end(), selected);
                if (it != marked.end()) {
                    marked.erase(it);
                } else if (marked.size() >= VITA_MAX_DISCS) {
                    feedback = "A playlist can contain at most 8 discs.";
                    feedback_time = 4.0f;
                } else if (ends_with_ci(roms[selected].name, ".m3u") ||
                           ends_with_ci(roms[selected].name, ".pbp") ||
                           ends_with_ci(roms[selected].name, ".exe")) {
                    feedback = "Only individual CD images can be grouped.";
                    feedback_time = 4.0f;
                } else {
                    marked.push_back(selected);
                }
            }
            if (!launch_pending && (pressed & SCE_CTRL_TRIANGLE)) {
                std::string created_path, reason;
                if (create_disc_playlist(roms, marked, &created_path, &reason)) {
                    const unsigned grouped = (unsigned)marked.size();
                    rescan_roms(created_path);
                    char success[80];
                    snprintf(success, sizeof(success), "Grouped %u discs into a new playlist.", grouped);
                    feedback = success;
                } else {
                    feedback = reason;
                }
                feedback_time = 4.0f;
            }
        }

        if (!launching && !launch_pending && !options_was_open && !folders_were_open) {
            if (pressed & SCE_CTRL_SELECT) {
                folders_open = true;
                folders_selected = 0;
                folders_feedback.clear();
            } else if (pressed & SCE_CTRL_START)
                options_open = true;
            else if (pressed & SCE_CTRL_CIRCLE)
                break;
        }

        if (!launching) {
            const float target = (float)selected;
            const float blend = std::min(1.0f, dt * 10.0f);
            carousel_position += (target - carousel_position) * blend;
        }
        if (launch_pending && fabsf((float)selected - carousel_position) < 0.06f) {
            const float distance = (float)selected - carousel_position;
            const float abs_distance = fabsf(distance);
            launch_center_x = 282.0f + distance * 176.0f;
            launch_center_y = 267.0f + abs_distance * 12.0f +
                disc_idle_offset(idle_time, selected);
            launch_radius = 128.0f * std::max(0.50f, 1.0f - abs_distance * 0.19f);
            launch_yaw = -distance * 18.0f - 7.0f + manual_yaw;
            launch_pitch = 25.0f + manual_pitch;
            launch_started = now;
            launch_pending = false;
            launching = true;
        }
        const vita_launch_animation::Pose launch_pose =
            vita_launch_animation::sample(launch_time);

        ImGui_ImplVitaGL_NewFrame();
        glViewport(0, 0, MENU_W, MENU_H);
        glClearColor(0.025f, 0.030f, 0.040f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        draw_selector_background(background_texture, background_width, background_height);

        if (!roms.empty()) {
            glEnable(GL_DEPTH_TEST);
            glDepthMask(GL_TRUE);
            glDepthFunc(GL_LEQUAL);
            glDisable(GL_CULL_FACE);
            glDisable(GL_BLEND);

            const int begin = std::max(0, selected - 3);
            const int end = std::min((int)roms.size() - 1, selected + 3);
            const float center_x = 282.0f;
            const float center_y = 267.0f;

            for (int pass = 0; pass < 2; ++pass) {
                for (int index = begin; index <= end; ++index) {
                    const bool is_selected = index == selected;
                    if ((pass == 0 && is_selected) || (pass == 1 && !is_selected))
                        continue;

                    const float distance = (float)index - carousel_position;
                    const float abs_distance = fabsf(distance);
                    if (abs_distance > 3.2f)
                        continue;

                    const float scale = std::max(0.50f, 1.0f - abs_distance * 0.19f);
                    float x = center_x + distance * 176.0f;
                    float y = center_y + abs_distance * 12.0f +
                              disc_idle_offset(idle_time, index);
                    const float z = 80.0f - abs_distance * 28.0f;
                    float yaw = -distance * 18.0f - 7.0f + (is_selected ? manual_yaw : 0.0f);
                    float pitch = 25.0f + (is_selected ? manual_pitch : 0.0f);
                    const float radius = 128.0f * scale;
                    float roll = 0.0f;
                    if (launching && is_selected) {
                        x = launch_center_x;
                        y = launch_center_y + launch_pose.drop_y;
                        yaw = launch_yaw * (1.0f - launch_pose.face_blend);
                        pitch = launch_pitch * (1.0f - launch_pose.face_blend);
                        roll = launch_pose.roll_degrees;
                    }
                    if (x - radius > 588.0f || x + radius < -5.0f)
                        continue;
                    GLuint cover = 0;
                    if (is_selected || abs_distance <= 2.1f)
                        cover = get_cover_texture(roms[index].cover_path);

                    if (is_selected)
                        glClear(GL_DEPTH_BUFFER_BIT);
                    draw_disc(x, y, z, launching && is_selected ? launch_radius : radius,
                        yaw, pitch, cover, roll);
                }
            }
        }

        restore_ui_state();
        if (launching)
            draw_launch_overlay(launch_pose, launch_center_x, (float)MENU_H + 12.0f);
        else if (folders_open)
            draw_rom_folders_menu(extra_folders,
                folder_browser_open ? browser_selected : folders_selected,
                folder_browser_open, browser_path, browser_children, folders_feedback);
        else if (options_open)
            draw_selector_options(options_selected);
        else
            draw_text_overlay(roms, selected, marked, carousel_position, idle_time, feedback,
                roms.empty() ? 0 : selected_discs[selected]);
        ImGui::Render();
        render_imgui_draw_data(ImGui::GetDrawData());
        vglSwapBuffers(GL_FALSE);
    }

    glFinish();
    if (background_texture)
        glDeleteTextures(1, &background_texture);
    destroy_cover_cache();
    destroy_disc_mesh();
    if (g_disc_white_texture)
        glDeleteTextures(1, &g_disc_white_texture);
    g_disc_white_texture = 0;
    if (g_disc_program)
        glDeleteProgram(g_disc_program);
    g_disc_program = 0;
    g_disc_screen_loc = g_disc_center_loc = g_disc_rotation_loc = g_disc_roll_loc = -1;
    g_disc_scale_loc = g_disc_tint_loc = g_disc_texture_loc = g_disc_gloss_loc = -1;
    destroy_imgui_program();
    ImGui_ImplVitaGL_Shutdown();
    ImGui::DestroyContext();
}

extern "C" int VitaRomSelector_Run(char *out_path, size_t out_size,
                                 VitaBootDisc *boot_disc)
{
    if (!out_path || out_size == 0 || !boot_disc)
        return -1;

    out_path[0] = '\0';
    memset(boot_disc, 0, sizeof(*boot_disc));
    if (!NativeGpu_InitBackend(MENU_W, MENU_H))
        return -1;

    SelectorTask task;
    task.out_path = out_path;
    task.out_size = out_size;
    task.boot_disc = boot_disc;
    task.result = 0;
    NativeGpu_RunBackendTaskSync(selector_task, &task);
    return task.result;
}

extern "C" int VitaPauseMenu_Run(const char *game_path, int start_section,
    const VitaDiscInfo *discs, unsigned *disc_selection)
{
    PauseMenuTask task;
    task.game_path = game_path;
    task.discs = discs;
    task.start_section = start_section;
    task.disc_selection = discs ? discs->current : 0;
    task.result = VITA_PAUSE_RESUME;
    NativeGpu_RunBackendTaskSync(pause_menu_task, &task);
    if (disc_selection)
        *disc_selection = task.disc_selection;
    return task.result;
}
