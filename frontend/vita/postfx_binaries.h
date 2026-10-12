#ifndef PCSX_VITA_POSTFX_BINARIES_H
#define PCSX_VITA_POSTFX_BINARIES_H

#include <stddef.h>

typedef struct VitaPostFXBinary {
    const char *name;
    const void *vertex;
    size_t vertex_size;
    const void *fragment;
    size_t fragment_size;
} VitaPostFXBinary;

extern const VitaPostFXBinary vita_postfx_binaries[];
extern const unsigned vita_postfx_binary_count;

#endif
