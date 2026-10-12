#ifndef PCSX_VITA_POST_PROCESSING_H
#define PCSX_VITA_POST_PROCESSING_H

#ifdef __cplusplus
extern "C" {
#endif

unsigned VitaPostFX_TargetFramebuffer(int width, int height);
int VitaPostFX_Finish(int x, int y, int width, int height,
                      int display_width, int display_height);
void VitaPostFX_Discard(void);
void VitaPostFX_Shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
