#ifndef PCSX_GPU_MAC_VITA_COMPAT_H
#define PCSX_GPU_MAC_VITA_COMPAT_H

#include <stddef.h>
#include <SDL3/SDL.h>
#include <OpenGL/gl3.h>

#ifndef GL_PALETTE4_RGBA8_OES
#define GL_PALETTE4_RGBA8_OES 0x8B90
#endif

void *vglMemalign(size_t alignment, size_t size);
void *vglAllocFromScratch(size_t size);
void vglFree(void *ptr);
void vglBufferData(GLenum target, const void *data);
int vglReadPixelsChecked(GLint x, GLint y, GLsizei width, GLsizei height,
                         GLenum format, GLenum type, void *pixels);
void vglReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                   GLenum format, GLenum type, void *pixels);
void vglSwapBuffers(GLboolean wait_vblank);
int vglShutdown(void);

int PCSXMacVitaGL_IsCurrent(void);
void PCSXMacVitaGL_UploadP4Texture(GLsizei width, GLsizei height,
                                   const void *vita_palette4_rgba8_payload);

#endif
