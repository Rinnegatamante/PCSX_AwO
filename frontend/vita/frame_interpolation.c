#include "frame_interpolation.h"
#include "settings.h"

#include <vitaGL.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/display.h>
#include <stdio.h>
#include <string.h>

#define INTERP_FLOW_LEVELS 3
#define INTERP_PYRAMID_LEVELS 2
#define INTERP_FLOW_W 120
#define INTERP_FLOW_H 68

static const int s_flow_widths[INTERP_FLOW_LEVELS] = {30, 60, 120};
static const int s_flow_heights[INTERP_FLOW_LEVELS] = {17, 34, 68};
static const float s_flow_steps[INTERP_FLOW_LEVELS] = {24.0f, 8.0f, 2.0f};
static const float s_flow_patches[INTERP_FLOW_LEVELS] = {12.0f, 6.0f, 3.0f};
static const float s_flow_min_gains[INTERP_FLOW_LEVELS] = {0.004f, 0.0015f, 0.0005f};

typedef struct InterpImage {
    GLuint texture;
    GLuint framebuffer;
} InterpImage;

static InterpImage s_frames[2];
static InterpImage s_pyramid[2][INTERP_PYRAMID_LEVELS];
static InterpImage s_flow[2][INTERP_FLOW_LEVELS];
static GLuint s_vao;
static GLuint s_vbo;
static GLuint s_downsample_program;
static GLuint s_flow_program;
static GLuint s_blend_program;
static GLuint s_motion_program;
static GLuint s_copy_program;
static GLint s_downsample_texel;
static GLint s_flow_texel;
static GLint s_flow_step;
static GLint s_flow_patch;
static GLint s_flow_parent;
static GLint s_flow_min_gain;
static GLint s_motion_texel;
static int s_width;
static int s_height;
static int s_initialized;
static int s_failed;
static int s_mode;
static int s_stable;
static int s_capture;
static int s_pending;
static int s_have_previous;
static int s_current_index;
static SceUInt64 s_last_frame_us;
static unsigned s_average_interval_us;

static const char *s_vertex =
    "#version 140\n"
    "precision highp float;\n"
    "attribute vec2 a_position;\n"
    "varying vec2 v_uv;\n"
    "void main() { v_uv = a_position * 0.5 + 0.5; gl_Position = vec4(a_position, 0.0, 1.0); }\n";

static const char *s_copy_fragment =
    "#version 140\n"
    "precision highp float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D s_next;\n"
    "void main() { gl_FragColor = texture2D(s_next, v_uv); }\n";

static const char *s_blend_fragment =
    "#version 140\n"
    "precision highp float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D s_prev;\n"
    "uniform sampler2D s_next;\n"
    "void main() { gl_FragColor = 0.5 * (texture2D(s_prev, v_uv) + texture2D(s_next, v_uv)); }\n";

static const char *s_downsample_fragment =
    "#version 140\n"
    "precision highp float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D s_next;\n"
    "uniform vec2 u_texel;\n"
    "void main() {\n"
    "    vec2 delta = u_texel * 0.75;\n"
    "    vec4 a = texture2D(s_next, v_uv + vec2(-delta.x, -delta.y));\n"
    "    vec4 b = texture2D(s_next, v_uv + vec2( delta.x, -delta.y));\n"
    "    vec4 c = texture2D(s_next, v_uv + vec2(-delta.x,  delta.y));\n"
    "    vec4 d = texture2D(s_next, v_uv + vec2( delta.x,  delta.y));\n"
    "    gl_FragColor = (a + b + c + d) * 0.25;\n"
    "}\n";

static const char *s_flow_fragment =
    "#version 140\n"
    "precision highp float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D s_prev;\n"
    "uniform sampler2D s_next;\n"
    "uniform sampler2D s_motion;\n"
    "uniform vec2 u_texel;\n"
    "uniform float u_step_px;\n"
    "uniform float u_patch_px;\n"
    "uniform float u_have_parent;\n"
    "uniform float u_min_gain;\n"
    "float difference(vec2 motion_px) {\n"
    "    vec2 displacement = motion_px * u_texel;\n"
    "    vec2 p = u_patch_px * u_texel;\n"
    "    vec2 q = v_uv + displacement;\n"
    "    if (min(min(q.x,q.y),min(1.0-q.x,1.0-q.y)) < 0.0) return 1.0;\n"
    "    float e = 0.0;\n"
    "    e += dot(abs(texture2D(s_prev,v_uv).rgb - texture2D(s_next,q).rgb), vec3(0.299,0.587,0.114));\n"
    "    e += dot(abs(texture2D(s_prev,v_uv+p).rgb - texture2D(s_next,q+p).rgb), vec3(0.299,0.587,0.114));\n"
    "    e += dot(abs(texture2D(s_prev,v_uv-p).rgb - texture2D(s_next,q-p).rgb), vec3(0.299,0.587,0.114));\n"
    "    return e * 0.3333333;\n"
    "}\n"
    "void main() {\n"
    "    vec2 predicted = vec2(0.0);\n"
    "    if (u_have_parent > 0.5) {\n"
    "        vec4 parent = texture2D(s_motion, v_uv);\n"
    "        if (parent.b > 0.2) predicted = (parent.rg - 0.5) * 128.0;\n"
    "    }\n"
    "    float initialError = difference(predicted);\n"
    "    float bestError = initialError;\n"
    "    vec2 best = predicted;\n"
    "    for (int y = -1; y <= 1; y++) {\n"
    "        for (int x = -1; x <= 1; x++) {\n"
    "            vec2 candidate = clamp(predicted + vec2(float(x),float(y)) * u_step_px, vec2(-64.0), vec2(64.0));\n"
    "            float error = difference(candidate);\n"
    "            if (error < bestError) { bestError = error; best = candidate; }\n"
    "        }\n"
    "    }\n"
    "    if (initialError - bestError < u_min_gain) { best = predicted; bestError = initialError; }\n"
    "    float confidence = clamp(1.0 - bestError * 4.0, 0.0, 1.0);\n"
    "    gl_FragColor = vec4(0.5 + best / 128.0, confidence, 1.0);\n"
    "}\n";

static const char *s_motion_fragment =
    "#version 140\n"
    "precision highp float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D s_prev;\n"
    "uniform sampler2D s_next;\n"
    "uniform sampler2D s_motion;\n"
    "uniform sampler2D s_backward;\n"
    "uniform vec2 u_texel;\n"
    "void main() {\n"
    "    vec4 flow = texture2D(s_motion, v_uv);\n"
    "    vec2 motion_px = (flow.rg - 0.5) * 128.0;\n"
    "    vec2 offset = motion_px * u_texel * 0.5;\n"
    "    vec2 a_uv = v_uv - offset;\n"
    "    vec2 next_guess = v_uv + offset;\n"
    "    vec4 backward = texture2D(s_backward, clamp(next_guess, vec2(0.0), vec2(1.0)));\n"
    "    vec2 backward_px = (backward.rg - 0.5) * 128.0;\n"
    "    vec2 b_uv = v_uv - backward_px * u_texel * 0.5;\n"
    "    vec4 a = texture2D(s_prev, clamp(a_uv, vec2(0.0), vec2(1.0)));\n"
    "    vec4 b = texture2D(s_next, clamp(b_uv, vec2(0.0), vec2(1.0)));\n"
    "    vec4 current = texture2D(s_next, v_uv);\n"
    "    float consistency = length(motion_px + backward_px);\n"
    "    vec3 delta = abs(a.rgb - b.rgb);\n"
    "    float disagreement = dot(delta, vec3(0.299,0.587,0.114));\n"
    "    float channel_error = max(max(delta.r, delta.g), delta.b);\n"
    "    vec2 a_border = min(a_uv, 1.0 - a_uv);\n"
    "    vec2 b_border = min(b_uv, 1.0 - b_uv);\n"
    "    float coverage = min(min(a_border.x, a_border.y), min(b_border.x, b_border.y));\n"
    "    float limit = 2.0 + 0.06 * length(motion_px);\n"
    "    bool aligned = coverage >= 0.0 && min(flow.b, backward.b) > 0.65 &&\n"
    "                   consistency < limit && disagreement < 0.09 && channel_error < 0.18;\n"
    "    gl_FragColor = aligned ? 0.5 * (a + b) : current;\n"
    "}\n";

static GLuint compile_stage(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    GLint ok = GL_FALSE;
    if (!shader)
        return 0;
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {0};
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "PCSX AwO: interpolation shader: %s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint compile_program(const char *fragment)
{
    GLuint vertex = compile_stage(GL_VERTEX_SHADER, s_vertex);
    GLuint pixel = compile_stage(GL_FRAGMENT_SHADER, fragment);
    GLuint program = 0;
    GLint ok = GL_FALSE;
    if (vertex && pixel) {
        program = glCreateProgram();
        glAttachShader(program, vertex);
        glAttachShader(program, pixel);
        glBindAttribLocation(program, 0, "a_position");
        glLinkProgram(program);
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[512] = {0};
            glGetProgramInfoLog(program, sizeof(log), NULL, log);
            fprintf(stderr, "PCSX AwO: interpolation program: %s\n", log);
            glDeleteProgram(program);
            program = 0;
        }
    }
    if (vertex)
        glDeleteShader(vertex);
    if (pixel)
        glDeleteShader(pixel);
    if (!ok)
        return 0;

    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "s_prev"), 0);
    glUniform1i(glGetUniformLocation(program, "s_next"), 1);
    glUniform1i(glGetUniformLocation(program, "s_motion"), 2);
    glUniform1i(glGetUniformLocation(program, "s_backward"), 3);
    glUseProgram(0);
    return program;
}

static int make_target(InterpImage *target, int width, int height, GLint filter)
{
    glGenTextures(1, &target->texture);
    glBindTexture(GL_TEXTURE_2D, target->texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenFramebuffers(1, &target->framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, target->framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, target->texture, 0);
    return target->texture && target->framebuffer &&
           glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

void VitaFrameInterp_Shutdown(void)
{
    for (int i = 0; i < 2; i++) {
        if (s_frames[i].framebuffer)
            glDeleteFramebuffers(1, &s_frames[i].framebuffer);
        if (s_frames[i].texture)
            glDeleteTextures(1, &s_frames[i].texture);
        memset(&s_frames[i], 0, sizeof(s_frames[i]));
    }
    for (int direction = 0; direction < 2; direction++) {
        for (int level = 0; level < INTERP_PYRAMID_LEVELS; level++) {
            InterpImage *image = &s_pyramid[direction][level];
            if (image->framebuffer)
                glDeleteFramebuffers(1, &image->framebuffer);
            if (image->texture)
                glDeleteTextures(1, &image->texture);
            memset(image, 0, sizeof(*image));
        }
        for (int level = 0; level < INTERP_FLOW_LEVELS; level++) {
            InterpImage *image = &s_flow[direction][level];
            if (image->framebuffer)
                glDeleteFramebuffers(1, &image->framebuffer);
            if (image->texture)
                glDeleteTextures(1, &image->texture);
            memset(image, 0, sizeof(*image));
        }
    }
    if (s_downsample_program)
        glDeleteProgram(s_downsample_program);
    if (s_flow_program)
        glDeleteProgram(s_flow_program);
    if (s_blend_program)
        glDeleteProgram(s_blend_program);
    if (s_motion_program)
        glDeleteProgram(s_motion_program);
    if (s_copy_program)
        glDeleteProgram(s_copy_program);
    if (s_vbo)
        glDeleteBuffers(1, &s_vbo);
    if (s_vao)
        glDeleteVertexArrays(1, &s_vao);
    s_downsample_program = s_flow_program = s_blend_program = s_motion_program = s_copy_program = 0;
    s_vao = s_vbo = 0;
    s_width = s_height = s_initialized = s_failed = 0;
    s_capture = s_pending = s_have_previous = s_stable = s_mode = 0;
    s_last_frame_us = 0;
    s_average_interval_us = 0;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);
}

static int ensure_pipeline(int width, int height)
{
    if (s_initialized && s_width == width && s_height == height)
        return 1;
    if (s_failed)
        return 0;
    if (s_initialized) {
        const int mode = s_mode;
        const int capture = s_capture;
        VitaFrameInterp_Shutdown();
        s_mode = mode;
        s_capture = capture;
    }

    const GLfloat quad[] = {-1.f, -1.f, 1.f, -1.f, -1.f, 1.f, 1.f, 1.f};
    s_width = width;
    s_height = height;
    s_downsample_program = compile_program(s_downsample_fragment);
    s_flow_program = compile_program(s_flow_fragment);
    s_blend_program = compile_program(s_blend_fragment);
    s_motion_program = compile_program(s_motion_fragment);
    s_copy_program = compile_program(s_copy_fragment);
    if (!s_downsample_program || !s_flow_program || !s_blend_program ||
        !s_motion_program || !s_copy_program)
        goto failed;
    s_downsample_texel = glGetUniformLocation(s_downsample_program, "u_texel");
    s_flow_texel = glGetUniformLocation(s_flow_program, "u_texel");
    s_flow_step = glGetUniformLocation(s_flow_program, "u_step_px");
    s_flow_patch = glGetUniformLocation(s_flow_program, "u_patch_px");
    s_flow_parent = glGetUniformLocation(s_flow_program, "u_have_parent");
    s_flow_min_gain = glGetUniformLocation(s_flow_program, "u_min_gain");
    s_motion_texel = glGetUniformLocation(s_motion_program, "u_texel");

    glGenVertexArrays(1, &s_vao);
    glGenBuffers(1, &s_vbo);
    if (!s_vao || !s_vbo)
        goto failed;
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (const void *)0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    if (!make_target(&s_frames[0], width, height, GL_LINEAR) ||
        !make_target(&s_frames[1], width, height, GL_LINEAR))
        goto failed;
    for (int direction = 0; direction < 2; direction++) {
        if (!make_target(&s_pyramid[direction][0], width / 2, height / 2, GL_LINEAR) ||
            !make_target(&s_pyramid[direction][1], width / 4, height / 4, GL_LINEAR))
            goto failed;
        for (int level = 0; level < INTERP_FLOW_LEVELS; level++) {
            if (!make_target(&s_flow[direction][level],
                             s_flow_widths[level], s_flow_heights[level], GL_LINEAR))
                goto failed;
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    s_initialized = 1;
    s_current_index = 0;
    s_have_previous = 0;
    return 1;

failed:
    fprintf(stderr, "PCSX AwO: interpolation GPU resources unavailable\n");
    VitaFrameInterp_Shutdown();
    s_failed = 1;
    return 0;
}

void VitaFrameInterp_BeginFrame(void)
{
    const char *setting = VitaSettings_GetOption("vita_frame_interpolation");
    const char *postfx = VitaSettings_GetOption("vita_postfx");
    int requested = 0;
    if (setting && strcmp(setting, "blend") == 0)
        requested = 1;
    else if (setting && strcmp(setting, "motion") == 0)
        requested = 2;
    if (postfx && strcmp(postfx, "off") != 0)
        requested = 0;

    s_capture = s_pending = 0;
    if (requested != s_mode) {
        if (!requested && s_initialized)
            VitaFrameInterp_Shutdown();
        s_mode = requested;
        s_have_previous = 0;
        s_stable = 0;
        s_last_frame_us = 0;
        s_average_interval_us = 0;
    }
    if (!requested || s_failed)
        return;

    SceUInt64 now = sceKernelGetProcessTimeWide();
    SceUInt64 elapsed = s_last_frame_us ? now - s_last_frame_us : 0;
    s_last_frame_us = now;
    if (elapsed < 5000 || elapsed > 150000) {
        s_stable = 0;
        s_average_interval_us = 0;
        s_have_previous = 0;
    } else {
        s_average_interval_us = s_average_interval_us ?
            (s_average_interval_us * 7u + (unsigned)elapsed) / 8u :
            (unsigned)elapsed;
        if (s_average_interval_us >= 30000 && s_average_interval_us <= 52000) {
            if (s_stable < 3)
                ++s_stable;
        } else if (s_average_interval_us < 26000 || s_average_interval_us > 62000) {
            s_stable = 0;
        }
    }

    s_capture = s_stable >= 3;
    if (!s_capture)
        s_have_previous = 0;
}

unsigned VitaFrameInterp_TargetFramebuffer(int width, int height)
{
    if (!s_capture || width <= 0 || height <= 0 || width > 960 || height > 544)
        return 0;
    if (!ensure_pipeline(width, height)) {
        s_capture = 0;
        return 0;
    }
    s_pending = 1;
    return s_frames[s_current_index].framebuffer;
}

void VitaFrameInterp_CancelFrame(void)
{
    s_capture = s_pending = 0;
    s_have_previous = 0;
}

static void use_frame_textures(void)
{
    const int previous = s_current_index ^ 1;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_frames[previous].texture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, s_frames[s_current_index].texture);
}

static void draw_fullscreen(GLuint framebuffer, int width, int height, GLuint program)
{
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glViewport(0, 0, width, height);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(program);
    glBindVertexArray(s_vao);
}

static void downsample_current_frame(void)
{
    const int index = s_current_index;
    GLuint source = s_frames[index].texture;
    int input_width = s_width;
    int input_height = s_height;
    for (int level = 0; level < INTERP_PYRAMID_LEVELS; level++) {
        InterpImage *target = &s_pyramid[index][level];
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, source);
        draw_fullscreen(target->framebuffer, input_width / 2, input_height / 2,
                        s_downsample_program);
        glUniform2f(s_downsample_texel, 1.0f / input_width, 1.0f / input_height);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        source = target->texture;
        input_width /= 2;
        input_height /= 2;
    }
}

static void estimate_bidirectional_flow(void)
{
    const int previous = s_current_index ^ 1;
    for (int level = 0; level < INTERP_FLOW_LEVELS; level++) {
        GLuint old_texture = s_frames[previous].texture;
        GLuint new_texture = s_frames[s_current_index].texture;
        if (level < INTERP_PYRAMID_LEVELS) {
            const int pyramid_level = INTERP_PYRAMID_LEVELS - level - 1;
            old_texture = s_pyramid[previous][pyramid_level].texture;
            new_texture = s_pyramid[s_current_index][pyramid_level].texture;
        }
        for (int direction = 0; direction < 2; direction++) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, direction ? new_texture : old_texture);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, direction ? old_texture : new_texture);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, level ?
                          s_flow[direction][level - 1].texture : 0);

            draw_fullscreen(s_flow[direction][level].framebuffer,
                            s_flow_widths[level], s_flow_heights[level],
                            s_flow_program);
            glUniform2f(s_flow_texel, 1.0f / s_width, 1.0f / s_height);
            glUniform1f(s_flow_step, s_flow_steps[level]);
            glUniform1f(s_flow_patch, s_flow_patches[level]);
            glUniform1f(s_flow_parent, level ? 1.0f : 0.0f);
            glUniform1f(s_flow_min_gain, s_flow_min_gains[level]);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
    }
}

int VitaFrameInterp_Present(void)
{
    if (!s_pending || !s_initialized)
        return 0;
    s_pending = s_capture = 0;
    const GLboolean previous_blend = glIsEnabled(GL_BLEND);
    const GLboolean previous_depth = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean previous_stencil = glIsEnabled(GL_STENCIL_TEST);
    const GLboolean previous_scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean previous_color_mask[4];
    GLint previous_viewport[4];
    GLint previous_active_texture;
    glGetBooleanv(GL_COLOR_WRITEMASK, previous_color_mask);
    glGetIntegerv(GL_VIEWPORT, previous_viewport);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previous_active_texture);

    use_frame_textures();
    if (s_mode == 2)
        downsample_current_frame();
    if (s_have_previous) {
        if (s_mode == 2) {
            estimate_bidirectional_flow();
            use_frame_textures();
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, s_flow[0][INTERP_FLOW_LEVELS - 1].texture);
            glActiveTexture(GL_TEXTURE3);
            glBindTexture(GL_TEXTURE_2D, s_flow[1][INTERP_FLOW_LEVELS - 1].texture);
        }
        draw_fullscreen(0, s_width, s_height,
                        s_mode == 2 ? s_motion_program : s_blend_program);
        if (s_mode == 2)
            glUniform2f(s_motion_texel, 1.0f / s_width, 1.0f / s_height);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        vglSwapBuffers(GL_FALSE);
        sceDisplayWaitVblankStart();
    }

    draw_fullscreen(0, s_width, s_height, s_copy_program);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, s_frames[s_current_index].texture);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glUseProgram(0);
    vglSwapBuffers(GL_FALSE);
    glActiveTexture(previous_active_texture);
    glViewport(previous_viewport[0], previous_viewport[1],
               previous_viewport[2], previous_viewport[3]);
    glColorMask(previous_color_mask[0], previous_color_mask[1],
                previous_color_mask[2], previous_color_mask[3]);
    if (previous_blend)
        glEnable(GL_BLEND);
    if (previous_depth)
        glEnable(GL_DEPTH_TEST);
    if (previous_stencil)
        glEnable(GL_STENCIL_TEST);
    if (previous_scissor)
        glEnable(GL_SCISSOR_TEST);
    s_current_index ^= 1;
    s_have_previous = 1;
    return 1;
}
