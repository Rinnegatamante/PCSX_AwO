#ifndef PCSX_VITA_ROM_SELECTOR_H
#define PCSX_VITA_ROM_SELECTOR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int VitaRomSelector_Run(char *out_path, size_t out_size);
int VitaPauseMenu_Run(void);

#ifdef __cplusplus
}
#endif

#endif
