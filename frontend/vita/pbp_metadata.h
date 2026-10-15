#ifndef PCSX_VITA_PBP_METADATA_H
#define PCSX_VITA_PBP_METADATA_H

#include "pbp_io.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace vita_pbp {

struct Info {
    std::string title;
    std::string disc_id;
    bool encrypted;

    Info() : encrypted(false) {}
};

static inline uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void read_sfo(io::File &file, uint64_t file_size,
                            uint32_t offset, uint32_t size, Info &info)
{
    if (size < 20 || size > 65536)
        return;

    std::vector<unsigned char> sfo(size);
    if (!file.read_at(offset, sfo.data(), size, file_size) ||
        memcmp(sfo.data(), "\0PSF", 4) != 0)
        return;

    const uint32_t keys = le32(&sfo[8]);
    const uint32_t data = le32(&sfo[12]);
    const uint32_t count = le32(&sfo[16]);
    if (keys >= size || data >= size || count > 256 ||
        count > (size - 20) / 16)
        return;

    for (uint32_t i = 0; i < count; ++i) {
        const unsigned char *entry = &sfo[20 + i * 16];
        const unsigned key_offset = (unsigned)entry[0] | ((unsigned)entry[1] << 8);
        const unsigned format = (unsigned)entry[2] | ((unsigned)entry[3] << 8);
        const uint32_t value_size = le32(entry + 4);
        const uint32_t value_offset = le32(entry + 12);
        if (key_offset >= size - keys || value_offset >= size - data ||
            value_size == 0 || value_size > size - data - value_offset ||
            format != 0x0204)
            continue;

        const char *key = (const char *)&sfo[keys + key_offset];
        if (!memchr(key, '\0', size - keys - key_offset))
            continue;
        if (strcmp(key, "TITLE") != 0 && strcmp(key, "DISC_ID") != 0)
            continue;

        const char *value = (const char *)&sfo[data + value_offset];
        const char *end = (const char *)memchr(value, '\0', value_size);
        const size_t length = end ? (size_t)(end - value) : value_size;
        std::string result(value, length);
        for (size_t j = 0; j < result.size(); ++j)
            if ((unsigned char)result[j] < 32)
                result[j] = ' ';
        if (strcmp(key, "TITLE") == 0)
            info.title = result;
        else
            info.disc_id = result;
    }
}

static inline bool read_info(const char *path, Info &info)
{
    io::File file;
    if (!file.open_read(path))
        return false;

    unsigned char header[40];
    uint64_t file_size = 0;
    bool valid = file.size(file_size) &&
        file.read_at(0, header, sizeof(header), file_size) &&
        memcmp(header, "\0PBP", 4) == 0;
    if (valid) {
        const uint32_t sfo_begin = le32(header + 8);
        const uint32_t sfo_end = le32(header + 12);
        const uint32_t psar_begin = le32(header + 36);
        if (sfo_begin >= sizeof(header) && sfo_end > sfo_begin &&
            sfo_end <= psar_begin)
            read_sfo(file, file_size, sfo_begin, sfo_end - sfo_begin, info);

        unsigned char signature[16];
        if (psar_begin >= sizeof(header) &&
            file.read_at(psar_begin, signature, sizeof(signature), file_size)) {
            uint32_t header_offset = 0;
            if (memcmp(signature, "PSTITLEIMG", 10) == 0)
                header_offset = 0x200;
            else if (memcmp(signature, "PSISOIMG00", 10) == 0)
                header_offset = 0x400;
            if (header_offset &&
                file.read_at((uint64_t)psar_begin + header_offset,
                    signature, 4, file_size) &&
                memcmp(signature, "\0PGD", 4) == 0)
                info.encrypted = true;
        }
    }
    return valid;
}

}

#endif
