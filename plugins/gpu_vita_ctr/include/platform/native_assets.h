/* The renderer's optional overlay asset reader; implementation is external. */
#ifndef PCSX_VITA_GPU_NATIVE_ASSETS_H
#define PCSX_VITA_GPU_NATIVE_ASSETS_H

enum NativeAssetReadMode
{
    NATIVE_ASSET_READ_DATA_FILE,
    NATIVE_ASSET_READ_RAW_CD_SECTORS
};

struct NativeAssetsByteBuffer
{
    unsigned char *data;
    int size;
};

int NativeAssets_ReadBytes(const char *path, int readMode,
                           struct NativeAssetsByteBuffer *bytes);
void NativeAssets_FreeBytes(struct NativeAssetsByteBuffer *bytes);

#endif
