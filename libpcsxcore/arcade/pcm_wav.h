/* GPL-2.0-or-later. PCM16 stereo capture for arcade board sample engines. */
#ifndef PCSX_ARCADE_PCM_WAV_H
#define PCSX_ARCADE_PCM_WAV_H
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
typedef struct PcmWav {
    FILE *file;
    uint64_t bytes;
    unsigned rate;
    int error;
} PcmWav;
int pcm_wav_open(PcmWav *wav, const char *path, unsigned rate);
int pcm_wav_append4(PcmWav *wav, const int16_t *samples, size_t frames);
int pcm_wav_close(PcmWav *wav);
#endif
