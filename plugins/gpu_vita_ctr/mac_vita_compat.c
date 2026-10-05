#include "platform/mac_vita_compat.h"

#include <stdlib.h>

extern SDL_Window *g_window;

typedef struct ScratchAllocation {
    void *pointer;
    size_t size;
} ScratchAllocation;

static ScratchAllocation s_scratch[4];

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
