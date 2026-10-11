#ifndef PCSX_VITA_ROM_SELECTOR_H
#define PCSX_VITA_ROM_SELECTOR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VITA_DISC_PATH_SIZE 1024

typedef struct VitaBootDisc {
    unsigned index;
    char path[VITA_DISC_PATH_SIZE];
} VitaBootDisc;

int VitaRomSelector_Run(char *out_path, size_t out_size, VitaBootDisc *boot_disc);

#define VITA_MAX_DISCS 8
#define VITA_DISC_LABEL_SIZE 128

typedef struct VitaDiscInfo {
    unsigned count;
    unsigned current;
    char labels[VITA_MAX_DISCS][VITA_DISC_LABEL_SIZE];
} VitaDiscInfo;

enum VitaPauseMenuSection {
    VITA_PAUSE_MAIN = 0,
    VITA_PAUSE_STATES = 1,
    VITA_PAUSE_DISCS = 2,
};

enum VitaPauseMenuResult {
    VITA_PAUSE_RESUME = 0,
    VITA_PAUSE_RESTART = 1,
    VITA_PAUSE_CLOSE_GAME = 2,
    VITA_PAUSE_SAVE_STATE = 3,
    VITA_PAUSE_LOAD_STATE = 4,
    VITA_PAUSE_CHANGE_DISC = 5,
};

int VitaPauseMenu_Run(const char *game_path, int start_section,
    const VitaDiscInfo *discs, unsigned *disc_selection);

#ifdef __cplusplus
}
#endif

#endif
