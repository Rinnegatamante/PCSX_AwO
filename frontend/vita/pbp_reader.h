#ifndef PCSX_VITA_PBP_READER_H
#define PCSX_VITA_PBP_READER_H

#ifdef __cplusplus
extern "C" {
#endif

int VitaPbp_Open(const char *path, unsigned disc_index);
unsigned VitaPbp_ProbeDiscCount(const char *path);
void VitaPbp_Close(void);
int VitaPbp_IsOpen(void);
unsigned VitaPbp_DiscCount(void);
unsigned VitaPbp_SectorCount(void);
int VitaPbp_ReadSector(unsigned sector, void *dest);
void *VitaPbp_GetBuffer(void);

#ifdef __cplusplus
}
#endif

#endif
