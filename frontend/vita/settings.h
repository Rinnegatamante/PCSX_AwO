#ifndef PCSX_VITA_SETTINGS_H
#define PCSX_VITA_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void VitaSettings_Init(void);
int VitaSettings_SelectGame(const char *game_path);
int VitaSettings_BeginGame(const char *game_path);
void VitaSettings_EndGame(void);
bool VitaSettings_IsGameActive(void);
int VitaSettings_GameConfigPath(const char *game_path, char *out, size_t out_size);
int VitaSettings_GameMemoryCardPath(const char *game_path, unsigned slot,
    char *out, size_t out_size);
int VitaSettings_PrepareGameMemoryCard(const char *game_path, unsigned slot,
    char *out, size_t out_size);
const char *VitaSettings_GetOption(const char *key);
int VitaSettings_SetOption(const char *key, const char *value);
int VitaSettings_Save(void);
bool VitaSettings_ConsumeDirty(void);
unsigned VitaSettings_GetRevision(void);

#ifdef __cplusplus
}
#endif

#endif
