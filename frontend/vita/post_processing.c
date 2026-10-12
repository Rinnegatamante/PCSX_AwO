#include "post_processing.h"
#include "settings.h"
#include "postfx_binaries.h"
#include "postfx_parameters.h"

#include <vitaGL.h>
#include <psp2/kernel/processmgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct VitaPostFXShader {
    const char *name;
    const char *sampler;
} VitaPostFXShader;

static const VitaPostFXShader s_effects[] = {
    {"off", NULL},
    {"crt", "s0"},
    {"crt_bleeding", "s0"},
    {"daltonic", "colorMap"},
    {"fxaa", "iChannel0"},
    {"gamma", "uTexture"},
    {"greyscale", "colorMap"},
    {"hue", "uTexture"},
    {"lcd3x", "s0"},
    {"negative", "colorMap"},
    {"sepia", "colorMap"},
    {"sharp_grid", "s0"},
    {"sharp_scanlines", "s0"},
    {"vignette", "uTexture"}
};

typedef struct VitaPostFXVertex {
    float x, y, z;
    float u, v;
} VitaPostFXVertex;

static GLuint s_texture;
static GLuint s_framebuffer;
static GLuint s_vao;
static GLuint s_vbo;
static GLuint s_program;
static GLuint s_vertex_shader;
static GLuint s_fragment_shader;
static GLint s_time_location;
static GLint s_crop_location;
static GLint s_output_size_location;
static GLint s_daltonic_green_location;
static GLint s_daltonic_blue_location;
static GLint s_parameter_locations[9];
static unsigned s_applied_settings_revision;
static int s_frame_width;
static int s_frame_height;
static int s_loaded_effect;
static int s_failed_effect;
static int s_target_failed;
static int s_pending;
static int s_quad_rect[6] = {-1, -1, -1, -1, -1, -1};

static int VitaPostFX_Selected(void)
{
    const char *choice = VitaSettings_GetOption("vita_postfx");
    if (!choice)
        return 0;
    for (int i = 1; i < (int)(sizeof(s_effects) / sizeof(s_effects[0])); ++i)
        if (strcmp(choice, s_effects[i].name) == 0)
            return i;
    return 0;
}

static void VitaPostFX_UnloadShader(void)
{
    glUseProgram(0);
    if (s_program)
        glDeleteProgram(s_program);
    if (s_vertex_shader)
        glDeleteShader(s_vertex_shader);
    if (s_fragment_shader)
        glDeleteShader(s_fragment_shader);
    s_program = 0;
    s_vertex_shader = 0;
    s_fragment_shader = 0;
    s_loaded_effect = 0;
    s_time_location = -1;
    s_crop_location = -1;
    s_output_size_location = -1;
    s_daltonic_green_location = -1;
    s_daltonic_blue_location = -1;
    s_applied_settings_revision = 0;
}

static GLuint VitaPostFX_LoadShader(const void *binary, size_t length, GLenum type)
{
    if (!binary || length < 16)
        return 0;
    GLuint shader = glCreateShader(type);
    if (shader)
        glShaderBinary(1, &shader, 0, binary, (GLsizei)length);
    return shader;
}

static const VitaPostFXBinary *VitaPostFX_FindBinary(const char *effect)
{
    for (unsigned i = 0; i < vita_postfx_binary_count; ++i)
        if (strcmp(effect, vita_postfx_binaries[i].name) == 0)
            return &vita_postfx_binaries[i];
    return NULL;
}

static int VitaPostFX_LoadProgram(int effect)
{
    if (effect == s_loaded_effect && s_program)
        return 1;
    if (effect == s_failed_effect)
        return 0;

    VitaPostFX_UnloadShader();
    const VitaPostFXShader *desc = &s_effects[effect];
    const VitaPostFXBinary *binary = VitaPostFX_FindBinary(desc->name);
    if (binary) {
        s_vertex_shader = VitaPostFX_LoadShader(binary->vertex, binary->vertex_size,
                                               GL_CG_VERTEX_SHADER_EXT);
        s_fragment_shader = VitaPostFX_LoadShader(binary->fragment, binary->fragment_size,
                                                 GL_CG_FRAGMENT_SHADER_EXT);
    }
    if (s_vertex_shader && s_fragment_shader) {
        s_program = glCreateProgram();
        if (s_program) {
            glAttachShader(s_program, s_vertex_shader);
            glAttachShader(s_program, s_fragment_shader);
            vglBindAttribLocation(s_program, 0, "position", 3, GL_FLOAT);
            vglBindAttribLocation(s_program, 1, "texcoord", 2, GL_FLOAT);
            glLinkProgram(s_program);
            GLint linked = GL_FALSE;
            glGetProgramiv(s_program, GL_LINK_STATUS, &linked);
            if (linked) {
                glUseProgram(s_program);
                GLint sampler = glGetUniformLocation(s_program, desc->sampler);
                if (sampler >= 0)
                    glUniform1i(sampler, 0);
                for (size_t i = 0; i < vita_postfx_parameter_count; ++i) {
                    s_parameter_locations[i] = strcmp(vita_postfx_parameters[i].effect,
                                                      desc->name) == 0 ?
                        glGetUniformLocation(s_program, vita_postfx_parameters[i].uniform) : -1;
                }
                s_time_location = glGetUniformLocation(s_program, "iTime");
                s_crop_location = glGetUniformLocation(s_program, "uCrop");
                s_output_size_location = glGetUniformLocation(s_program, "uOutputSize");
                if (strcmp(desc->name, "daltonic") == 0) {
                    s_daltonic_green_location = glGetUniformLocation(s_program, "uGreenRow");
                    s_daltonic_blue_location = glGetUniformLocation(s_program, "uBlueRow");
                }
                glUseProgram(0);
                s_loaded_effect = effect;
                fprintf(stdout, "PCSX AwO: post-processing shader %s linked\n", desc->name);
                return 1;
            }
        }
    }

    if (s_program) {
        char log[512];
        GLsizei count = 0;
        glGetProgramInfoLog(s_program, sizeof(log), &count, log);
        fprintf(stderr, "PCSX AwO: post-processing shader %s link failed: %s\n",
                desc->name, count > 0 ? log : "(no log)");
    } else {
        fprintf(stderr, "PCSX AwO: failed to load post-processing shader %s\n", desc->name);
    }
    VitaPostFX_UnloadShader();
    s_failed_effect = effect;
    return 0;
}

static int VitaPostFX_CreateTarget(int width, int height)
{
    if (s_framebuffer && s_frame_width == width && s_frame_height == height)
        return 1;
    if (s_framebuffer)
        glDeleteFramebuffers(1, &s_framebuffer);
    if (s_texture)
        glDeleteTextures(1, &s_texture);
    s_framebuffer = 0;
    s_texture = 0;
    s_frame_width = s_frame_height = 0;

    glGenTextures(1, &s_texture);
    if (!s_texture)
        return 0;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &s_framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, s_framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           s_texture, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "PCSX AwO: post-processing framebuffer unavailable: 0x%x\n", (unsigned)status);
        glDeleteFramebuffers(1, &s_framebuffer);
        glDeleteTextures(1, &s_texture);
        s_framebuffer = s_texture = 0;
        return 0;
    }
    s_frame_width = width;
    s_frame_height = height;
    return 1;
}

static int VitaPostFX_CreateQuad(void)
{
    if (s_vao && s_vbo)
        return 1;
    glGenVertexArrays(1, &s_vao);
    glGenBuffers(1, &s_vbo);
    if (!s_vao || !s_vbo)
        return 0;
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(VitaPostFXVertex) * 4, NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(VitaPostFXVertex), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(VitaPostFXVertex),
                          (void *)(3 * sizeof(float)));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    s_quad_rect[0] = -1;
    return 1;
}

unsigned VitaPostFX_TargetFramebuffer(int width, int height)
{
    int effect = VitaPostFX_Selected();
    if (!effect) {
        if (s_framebuffer || s_program)
            VitaPostFX_Shutdown();
        return 0;
    }
    if (width <= 0 || height <= 0 || width > 2048 || height > 2048 ||
        s_target_failed)
        return 0;
    if (!VitaPostFX_LoadProgram(effect))
        return 0;
    if (!VitaPostFX_CreateTarget(width, height) || !VitaPostFX_CreateQuad()) {
        s_target_failed = 1;
        return 0;
    }
    s_pending = 1;
    return s_framebuffer;
}

int VitaPostFX_Finish(int x, int y, int width, int height, int display_width,
                      int display_height)
{
    if (!s_pending)
        return 0;
    s_pending = 0;
    if (!s_program || !s_texture || width <= 0 || height <= 0)
        return 0;
    const float u0 = (float)x / (float)display_width;
    const float v0 = (float)y / (float)display_height;
    const float u1 = (float)(x + width) / (float)display_width;
    const float v1 = (float)(y + height) / (float)display_height;
    const VitaPostFXVertex quad[4] = {
        {-1, -1, 0, u0, v0},
        { 1, -1, 0, u1, v0},
        {-1,  1, 0, u0, v1},
        { 1,  1, 0, u1, v1}
    };

    const GLboolean previous_stencil = glIsEnabled(GL_STENCIL_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, display_width, display_height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    glViewport(x, y, width, height);
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, width, height);
    glUseProgram(s_program);
    const unsigned revision = VitaSettings_GetRevision();
    if (s_applied_settings_revision != revision) {
        for (size_t i = 0; i < vita_postfx_parameter_count; ++i) {
            const GLint location = s_parameter_locations[i];
            if (location < 0)
                continue;
            const VitaPostFXParameter *parameter = &vita_postfx_parameters[i];
            const char *stored = VitaSettings_GetOption(parameter->setting);
            float value = stored ? strtof(stored, NULL) : parameter->default_value;
            if (value < parameter->minimum)
                value = parameter->minimum;
            if (value > parameter->maximum)
                value = parameter->maximum;
            glUniform1f(location, value);
        }
        if (strcmp(s_effects[s_loaded_effect].name, "daltonic") == 0) {
            static const float rows[4][6] = {
                {0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f},
                {0.50894941f, 0.49105389f, 0.00000069f, 0.61732665f, -0.61732265f, 1.00000083f},
                {0.20232475f, 0.79767449f, -0.00000015f, 0.51741093f, -0.51741286f, 0.99999962f},
                {-0.13853643f, 1.13853827f, 0.00000021f, 3.36558457f, -3.36562933f, 0.99999501f}
            };
            int mode = 0;
            const char *protanopia = VitaSettings_GetOption("vita_fx_protanopia");
            const char *deuteranopia = VitaSettings_GetOption("vita_fx_deuteranopia");
            const char *tritanopia = VitaSettings_GetOption("vita_fx_tritanopia");
            if (protanopia && strtof(protanopia, NULL) > 1.0f)
                mode = 1;
            if (deuteranopia && strtof(deuteranopia, NULL) > 1.0f)
                mode = 2;
            if (tritanopia && strtof(tritanopia, NULL) > 1.0f)
                mode = 3;
            if (s_daltonic_green_location >= 0)
                glUniform3f(s_daltonic_green_location, rows[mode][0], rows[mode][1], rows[mode][2]);
            if (s_daltonic_blue_location >= 0)
                glUniform3f(s_daltonic_blue_location, rows[mode][3], rows[mode][4], rows[mode][5]);
        }
        s_applied_settings_revision = revision;
    }
    if (s_time_location >= 0)
        glUniform1f(s_time_location, (float)sceKernelGetProcessTimeLow() / 1000000.0f);
    if (s_crop_location >= 0)
        glUniform4f(s_crop_location, u0, v0, u1 - u0, v1 - v0);
    if (s_output_size_location >= 0)
        glUniform2f(s_output_size_location, (float)width, (float)height);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_texture);
    glBindVertexArray(s_vao);
    if (s_quad_rect[0] != x || s_quad_rect[1] != y ||
        s_quad_rect[2] != width || s_quad_rect[3] != height ||
        s_quad_rect[4] != display_width || s_quad_rect[5] != display_height) {
        glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quad), quad);
        s_quad_rect[0] = x;
        s_quad_rect[1] = y;
        s_quad_rect[2] = width;
        s_quad_rect[3] = height;
        s_quad_rect[4] = display_width;
        s_quad_rect[5] = display_height;
    }
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glUseProgram(0);
    glDisable(GL_SCISSOR_TEST);
    if (previous_stencil)
        glEnable(GL_STENCIL_TEST);
    glViewport(0, 0, display_width, display_height);
    return 1;
}

void VitaPostFX_Discard(void)
{
    s_pending = 0;
}

void VitaPostFX_Shutdown(void)
{
    s_pending = 0;
    VitaPostFX_UnloadShader();
    if (s_vao)
        glDeleteVertexArrays(1, &s_vao);
    if (s_vbo)
        glDeleteBuffers(1, &s_vbo);
    if (s_framebuffer)
        glDeleteFramebuffers(1, &s_framebuffer);
    if (s_texture)
        glDeleteTextures(1, &s_texture);
    s_vao = s_vbo = s_framebuffer = s_texture = 0;
    s_frame_width = s_frame_height = 0;
    s_failed_effect = 0;
    s_target_failed = 0;
    s_quad_rect[0] = -1;
}
