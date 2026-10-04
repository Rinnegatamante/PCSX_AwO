/* GPL-2.0-or-later. */
#include "pcm_wav.h"
#include <string.h>
static void le16(unsigned char *p, unsigned n) { p[0] = n; p[1] = n >> 8; }
static void le32(unsigned char *p, uint32_t n)
{ p[0] = n; p[1] = n >> 8; p[2] = n >> 16; p[3] = n >> 24; }
static int header(PcmWav *wav)
{
    unsigned char data[44] = {0};
    if (wav->bytes > UINT32_MAX - 36) return -1;
    memcpy(data, "RIFF", 4); le32(data + 4, (uint32_t)wav->bytes + 36);
    memcpy(data + 8, "WAVEfmt ", 8); le32(data + 16, 16);
    le16(data + 20, 1); le16(data + 22, 2); le32(data + 24, wav->rate);
    le32(data + 28, wav->rate * 4); le16(data + 32, 4); le16(data + 34, 16);
    memcpy(data + 36, "data", 4); le32(data + 40, (uint32_t)wav->bytes);
    return fseek(wav->file, 0, SEEK_SET) || fwrite(data, 1, sizeof(data), wav->file) != sizeof(data) ? -1 : 0;
}
int pcm_wav_open(PcmWav *wav, const char *path, unsigned rate)
{
    if (!wav || !path || !rate || rate > UINT32_MAX / 4 || wav->file) return -1;
    wav->error = 0; wav->bytes = 0; wav->rate = rate;
    wav->file = fopen(path, "wb");
    if (!wav->file) { wav->error = 1; return -1; }
    if (header(wav)) { fclose(wav->file); wav->file = NULL; wav->error = 1; return -1; }
    return 0;
}
int pcm_wav_append4(PcmWav *wav, const int16_t *samples, size_t frames)
{
    unsigned char buffer[1024];
    size_t i, count;
    if (!wav || !samples) return -1;
    if (!wav->file) return wav->error ? -1 : 0;
    if (wav->error || frames > (UINT32_MAX - 36 - wav->bytes) / 4) { wav->error = 1; return -1; }
    while (frames) {
        count = frames > 256 ? 256 : frames;
        for (i = 0; i < count; i++) {
            le16(buffer + i * 4, (uint16_t)samples[i * 4]);
            le16(buffer + i * 4 + 2, (uint16_t)samples[i * 4 + 1]);
        }
        if (fwrite(buffer, 4, count, wav->file) != count) { wav->error = 1; return -1; }
        wav->bytes += count * 4; samples += count * 4; frames -= count;
    }
    return 0;
}
int pcm_wav_close(PcmWav *wav)
{
    int failed;
    if (!wav) return -1;
    if (!wav->file) return wav->error ? -1 : 0;
    failed = wav->error || header(wav);
    if (fclose(wav->file)) failed = 1;
    wav->file = NULL; wav->error = failed;
    return failed ? -1 : 0;
}
