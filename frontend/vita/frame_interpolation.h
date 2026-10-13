#ifndef PCSX_VITA_FRAME_INTERPOLATION_H
#define PCSX_VITA_FRAME_INTERPOLATION_H

void VitaFrameInterp_BeginFrame(void);
unsigned VitaFrameInterp_TargetFramebuffer(int width, int height);
int VitaFrameInterp_Present(void);
void VitaFrameInterp_CancelFrame(void);
void VitaFrameInterp_Shutdown(void);

#endif
