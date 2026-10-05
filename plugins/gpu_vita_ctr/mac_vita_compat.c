#include "platform/mac_vita_compat.h"

#include <stdio.h>
#include <stdlib.h>

extern SDL_Window *g_window;

typedef struct ScratchAllocation {
    void *pointer;
    size_t size;
} ScratchAllocation;

static ScratchAllocation s_scratch[4];
static unsigned char *s_capture_rgba;
static int s_capture_width;
static int s_capture_height;

void *vglMemalign(size_t alignment, size_t size)
{
    void *pointer = NULL;
    if (posix_memalign(&pointer, alignment, size) != 0)
        return NULL;
    return pointer;
}

void vglFree(void *pointer)
{
    free(pointer);
}

void *vglAllocFromScratch(size_t size)
{
    void *pointer = malloc(size);
    unsigned i;
    if (pointer == NULL)
        return NULL;
    for (i = 0; i < sizeof(s_scratch) / sizeof(s_scratch[0]); i++) {
        if (s_scratch[i].pointer == NULL) {
            s_scratch[i].pointer = pointer;
            s_scratch[i].size = size;
            return pointer;
        }
    }
    free(pointer);
    return NULL;
}

void vglBufferData(GLenum target, const void *data)
{
	size_t size = 0;
	unsigned i;
	void *allocation = NULL;
	for (i = 0; i < sizeof(s_scratch) / sizeof(s_scratch[0]); i++) {
		if (s_scratch[i].pointer == data && data != NULL) {
			size = s_scratch[i].size;
			allocation = s_scratch[i].pointer;
			s_scratch[i].pointer = NULL;
			s_scratch[i].size = 0;
			break;
		}
	}
	glBufferData(target, (GLsizeiptr)size, data, GL_STREAM_DRAW);
	free(allocation);
}

int vglReadPixelsChecked(GLint x, GLint y, GLsizei width, GLsizei height,
                         GLenum format, GLenum type, void *pixels)
{
    glReadPixels(x, y, width, height, format, type, pixels);
    return glGetError() == GL_NO_ERROR;
}

void vglReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                   GLenum format, GLenum type, void *pixels)
{
    glReadPixels(x, y, width, height, format, type, pixels);
}

void vglSwapBuffers(GLboolean wait_vblank)
{
    (void)wait_vblank;
    if (g_window != NULL)
        SDL_GL_SwapWindow(g_window);
}

int vglShutdown(void)
{
    unsigned i;
    for (i = 0; i < sizeof(s_scratch) / sizeof(s_scratch[0]); i++) {
        free(s_scratch[i].pointer);
        s_scratch[i].pointer = NULL;
        s_scratch[i].size = 0;
    }
    return 0;
}

int PCSXMacVitaGL_IsCurrent(void)
{
    return SDL_GL_GetCurrentContext() != NULL;
}

int PCSXMacVitaGL_CaptureCurrentFrame(void)
{
    GLint previous_read_framebuffer = 0;
    GLint previous_read_buffer = GL_BACK;
    GLint previous_pack_alignment = 4;
    int width = 0;
    int height = 0;
    size_t size;
    unsigned char *capture;

    if (g_window == NULL || SDL_GL_GetCurrentContext() == NULL)
        return 0;
    SDL_GetWindowSizeInPixels(g_window, &width, &height);
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384)
        return 0;
    size = (size_t)width * (size_t)height * 4u;
    capture = (unsigned char *)realloc(s_capture_rgba, size);
    if (capture == NULL)
        return 0;
    s_capture_rgba = capture;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read_framebuffer);
    glGetIntegerv(GL_READ_BUFFER, &previous_read_buffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &previous_pack_alignment);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glFinish();
    while (glGetError() != GL_NO_ERROR) {
    }
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, s_capture_rgba);
    if (glGetError() != GL_NO_ERROR) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)previous_read_framebuffer);
        glReadBuffer((GLenum)previous_read_buffer);
        glPixelStorei(GL_PACK_ALIGNMENT, previous_pack_alignment);
        return 0;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)previous_read_framebuffer);
    glReadBuffer((GLenum)previous_read_buffer);
    glPixelStorei(GL_PACK_ALIGNMENT, previous_pack_alignment);
    s_capture_width = width;
    s_capture_height = height;
    return 1;
}

int PCSXMacVitaGL_WriteCapturePpm(const char *path)
{
    FILE *file;
    unsigned char *row;
    int y;
    if (s_capture_rgba == NULL || s_capture_width <= 0 || s_capture_height <= 0)
        return 0;
    file = fopen(path, "wb");
    if (file == NULL)
        return -1;
    row = (unsigned char *)malloc((size_t)s_capture_width * 3u);
    if (row == NULL) {
        fclose(file);
        return -1;
    }
    if (fprintf(file, "P6\n%d %d\n255\n", s_capture_width, s_capture_height) < 0) {
        free(row);
        fclose(file);
        return -1;
    }
    for (y = s_capture_height - 1; y >= 0; y--) {
        const unsigned char *src = s_capture_rgba + (size_t)y * s_capture_width * 4u;
        int x;
        for (x = 0; x < s_capture_width; x++) {
            row[x * 3 + 0] = src[x * 4 + 0];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 2];
        }
        if (fwrite(row, 1, (size_t)s_capture_width * 3u, file) != (size_t)s_capture_width * 3u) {
            free(row);
            fclose(file);
            return -1;
        }
    }
    free(row);
    if (fclose(file))
        return -1;
    fprintf(stderr, "capture=%dx%d source=OpenGL-back-buffer-before-swap\n", s_capture_width, s_capture_height);
    return 1;
}

void PCSXMacVitaGL_UploadP4Texture(GLsizei width, GLsizei height,
                                   const void *vita_palette4_rgba8_payload)
{
    const size_t pixels = (size_t)width * (size_t)height;
    const unsigned char *payload = (const unsigned char *)vita_palette4_rgba8_payload;
    unsigned char *rgba = (unsigned char *)malloc(pixels * 4u);
    const unsigned char *palette;
    const unsigned char *indices;
    size_t i;
    if (rgba == NULL || payload == NULL) {
        free(rgba);
        return;
    }

    palette = payload;
    indices = payload + 16u * 4u;
    for (i = 0; i < pixels; i++) {
        const unsigned char packed = indices[i >> 1];
        const unsigned char palette_index = (i & 1u) ? (packed & 0x0fu) : (packed >> 4);
        rgba[i * 4u + 0] = palette[palette_index * 4u + 0];
        rgba[i * 4u + 1] = palette[palette_index * 4u + 1];
        rgba[i * 4u + 2] = palette[palette_index * 4u + 2];
        rgba[i * 4u + 3] = palette[palette_index * 4u + 3];
    }

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    free(rgba);
}
