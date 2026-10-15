#include "pbp_reader.h"
#include "pbp_io.h"

#include <cstdint>
#include <cstring>
#include <vector>

extern "C" {
#include "psxtract/crypto.h"
#include "psxtract/lz.h"
}

namespace vita_pbp {
namespace {
static const unsigned kBlockBytes = 16 * 2352;
static const unsigned kHeaderBytes = 0xB6600;
static const unsigned kMapBytes = 0x2A0;
static const unsigned kMaxDiscs = 5;

static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t le16(const unsigned char *p)
{
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static void put32(unsigned char *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (unsigned char)(value >> (i * 8));
}

static void edc_ecc_tables(unsigned char forward[256], unsigned char backward[256],
                           uint32_t edc[256])
{
    for (unsigned i = 0; i < 256; ++i) {
        unsigned j = (i << 1) ^ ((i & 0x80) ? 0x11d : 0);
        forward[i] = (unsigned char)j;
        backward[i ^ j] = (unsigned char)i;
        uint32_t crc = i;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xd8018001u : 0u);
        edc[i] = crc;
    }
}

struct SectorTables {
    unsigned char forward[256];
    unsigned char backward[256];
    uint32_t edc[256];

    SectorTables() { edc_ecc_tables(forward, backward, edc); }

    uint32_t checksum(const unsigned char *src, unsigned count) const
    {
        uint32_t crc = 0;
        for (unsigned i = 0; i < count; ++i)
            crc = (crc >> 8) ^ edc[(crc ^ src[i]) & 0xff];
        return crc;
    }

    void parity(const unsigned char *source, unsigned major_count,
                unsigned minor_count, unsigned major_mult,
                unsigned minor_inc, unsigned char *dest) const
    {
        const unsigned size = major_count * minor_count;
        for (unsigned major = 0; major < major_count; ++major) {
            unsigned index = (major >> 1) * major_mult + (major & 1);
            unsigned char a = 0, b = 0;
            for (unsigned minor = 0; minor < minor_count; ++minor) {
                const unsigned char value = source[index];
                index += minor_inc;
                if (index >= size)
                    index -= size;
                a ^= value;
                b ^= value;
                a = forward[a];
            }
            a = backward[forward[a] ^ b];
            dest[major] = a;
            dest[major + major_count] = a ^ b;
        }
    }
};

static unsigned char bcd(unsigned value)
{
    return (unsigned char)(((value / 10) << 4) | (value % 10));
}

static bool repair_sector(unsigned char *sector, uint32_t sector_index,
                          const SectorTables &tables)
{
    const unsigned mode = sector[15];
    if (mode == 0) {
        for (unsigned i = 0; i < 2352; ++i)
            if (sector[i] != 0)
                return false;
        return true;
    }
    if (mode != 1 && mode != 2)
        return false;

    const uint32_t absolute = sector_index + 150;
    if (absolute / (75 * 60) >= 100)
        return false;
    sector[0] = 0;
    memset(sector + 1, 0xff, 10);
    sector[11] = 0;
    sector[12] = bcd(absolute / (75 * 60));
    sector[13] = bcd((absolute / 75) % 60);
    sector[14] = bcd(absolute % 75);

    if (mode == 1) {
        put32(sector + 2064, tables.checksum(sector, 2064));
        memset(sector + 2068, 0, 8);
    } else {
        if (memcmp(sector + 16, sector + 20, 4) != 0)
            return false;
        if (sector[18] & 0x20) {
            if (le32(sector + 2348))
                put32(sector + 2348, tables.checksum(sector + 16, 2332));
            return true;
        }
        put32(sector + 2072, tables.checksum(sector + 16, 2056));
        memset(sector + 12, 0, 4);
    }

    tables.parity(sector + 12, 86, 24, 2, 86, sector + 2076);
    tables.parity(sector + 12, 52, 43, 86, 88, sector + 2248);
    if (mode == 2) {
        sector[12] = bcd(absolute / (75 * 60));
        sector[13] = bcd((absolute / 75) % 60);
        sector[14] = bcd(absolute % 75);
        sector[15] = 2;
    }
    return true;
}

struct Block {
    uint32_t offset;
    uint16_t size;
};

}
}

namespace vita_pbp {

class PbpReader {
    struct CacheLine {
        unsigned block;
        uint64_t age;
        unsigned char data[kBlockBytes];

        CacheLine() : block(UINT32_MAX), age(0) {}
    };

    io::File file_;
    uint64_t file_size_;
    uint64_t disc_base_;
    unsigned disc_count_;
    unsigned sector_count_;
    uint64_t clock_;
    std::vector<Block> blocks_;
    std::vector<CacheLine> cache_;
    std::vector<unsigned char> compressed_;
    unsigned char last_sector_[2352];
    SectorTables tables_;

public:
    PbpReader() : file_size_(0), disc_base_(0), disc_count_(0),
        sector_count_(0), clock_(0), cache_(8),
        compressed_(kBlockBytes + 0x100, 0)
    {
        memset(last_sector_, 0, sizeof(last_sector_));
    }

    int open(const char *path, unsigned requested_disc)
    {
        if (!path || !file_.open_read(path) || !file_.size(file_size_))
            return -1;

        unsigned char header[40], signature[16], key[16] = {0};
        if (!file_.read_at(0, header, sizeof(header), file_size_) ||
            memcmp(header, "\0PBP", 4))
            return -1;
        const uint64_t psar = le32(header + 36);
        if (!file_.read_at(psar, signature, sizeof(signature), file_size_))
            return -1;

        const bool multi = memcmp(signature, "PSTITLEIMG", 10) == 0;
        const bool single = memcmp(signature, "PSISOIMG00", 10) == 0;
        if (!multi && !single)
            return 0;

        uint32_t disc_offsets[kMaxDiscs] = {0};
        if (multi) {
            unsigned char map[kMapBytes];
            if (!file_.read_at(psar + 0x200, map, sizeof(map), file_size_))
                return -1;
            if (memcmp(map, "\0PGD", 4) != 0)
                return 0;
            kirk_init();
            if (decrypt_pgd(map, sizeof(map), 2, key) < 0x14)
                return -1;
            for (unsigned i = 0; i < kMaxDiscs; ++i) {
                const uint32_t offset = le32(map + 0x90 + 4 * i);
                if (!offset)
                    continue;
                if (disc_count_ && offset <= disc_offsets[disc_count_ - 1])
                    return -1;
                disc_offsets[disc_count_++] = offset;
            }
        } else {
            if (!file_.read_at(psar + 0x400, signature, 4, file_size_))
                return -1;
            if (memcmp(signature, "\0PGD", 4) != 0)
                return 0;
            kirk_init();
            disc_count_ = 1;
        }

        if (!disc_count_)
            return -1;
        if (requested_disc >= disc_count_)
            requested_disc = 0;

        disc_base_ = psar + disc_offsets[requested_disc];
        std::vector<unsigned char> header_data(kHeaderBytes);
        if (!file_.read_at(disc_base_ + 0x400,
                           header_data.data(), header_data.size(), file_size_) ||
            memcmp(header_data.data(), "\0PGD", 4) != 0)
            return -1;
        memset(key, 0, sizeof(key));
        const int plain = decrypt_pgd(header_data.data(), (int)header_data.size(), 2, key);
        if (plain <= 0x3c00 || plain > (int)kHeaderBytes - 0x90)
            return -1;

        const unsigned char *table = header_data.data() + 0x90 + 0x3c00;
        const unsigned entries = ((unsigned)plain - 0x3c00) / 32;
        for (unsigned i = 0; i < entries; ++i) {
            const unsigned char *entry = table + i * 32;
            Block block;
            block.offset = le32(entry);
            block.size = le16(entry + 4);
            if (!block.size)
                break;
            const uint64_t absolute = disc_base_ + 0x100000 + block.offset;
            if (block.size > kBlockBytes || absolute > file_size_ ||
                block.size > file_size_ - absolute)
                return -1;
            blocks_.push_back(block);
        }
        if (blocks_.empty() || blocks_.size() > UINT32_MAX / 16)
            return -1;
        sector_count_ = (unsigned)blocks_.size() * 16;
        return 1;
    }

    unsigned disc_count() const { return disc_count_; }
    unsigned sector_count() const { return sector_count_; }

    int read_sector(unsigned sector, void *dest)
    {
        if (sector >= sector_count_)
            return -1;
        const unsigned index = sector >> 4;
        CacheLine *line = NULL;
        for (size_t i = 0; i < cache_.size(); ++i) {
            if (cache_[i].block == index) {
                line = &cache_[i];
                break;
            }
        }
        if (!line) {
            line = &cache_[0];
            for (size_t i = 1; i < cache_.size(); ++i)
                if (cache_[i].age < line->age)
                    line = &cache_[i];
            line->block = UINT32_MAX;
            const Block &block = blocks_[index];
            const uint64_t offset = disc_base_ + 0x100000 + block.offset;
            if (!file_.read_at(offset, compressed_.data(), block.size, file_size_))
                return -1;
            memset(compressed_.data() + block.size, 0, 8);
            if (block.size == kBlockBytes)
                memcpy(line->data, compressed_.data(), kBlockBytes);
            else if (decompress(line->data, compressed_.data(), kBlockBytes,
                                block.size) != (int)kBlockBytes)
                return -1;
            for (unsigned i = 0; i < 16; ++i)
                if (!repair_sector(line->data + i * 2352, index * 16 + i, tables_))
                    return -1;
            line->block = index;
        }
        line->age = ++clock_;
        memcpy(last_sector_, line->data + (sector & 15) * 2352, 2352);
        if (dest)
            memcpy(dest, last_sector_, 2352);
        return 2352;
    }

    void *buffer() { return last_sector_ + 12; }
};

static PbpReader *g_reader = NULL;

}

extern "C" {

unsigned VitaPbp_ProbeDiscCount(const char *path)
{
    vita_pbp::io::File file;
    uint64_t file_size = 0;
    unsigned char header[40], signature[16];
    if (!path || !file.open_read(path) || !file.size(file_size) ||
        !file.read_at(0, header, sizeof(header), file_size) ||
        memcmp(header, "\0PBP", 4) != 0)
        return 0;

    const uint64_t psar = vita_pbp::le32(header + 36);
    if (!file.read_at(psar, signature, sizeof(signature), file_size))
        return 0;
    if (memcmp(signature, "PSISOIMG00", 10) == 0)
        return 1;
    if (memcmp(signature, "PSTITLEIMG", 10) != 0)
        return 0;

    unsigned char map[0x2a0];
    if (!file.read_at(psar + 0x200, map, sizeof(map), file_size))
        return 0;

    const unsigned char *offsets = map;
    unsigned capacity = 8;
    if (memcmp(map, "\0PGD", 4) == 0) {
        unsigned char key[16] = {0};
        kirk_init();
        if (decrypt_pgd(map, sizeof(map), 2, key) < 0x14)
            return 0;
        offsets = map + 0x90;
        capacity = 5;
    }

    unsigned count = 0;
    uint32_t previous = 0;
    for (unsigned i = 0; i < capacity; ++i) {
        const uint32_t offset = vita_pbp::le32(offsets + i * 4);
        if (!offset)
            break;
        if (offset <= previous || psar + (uint64_t)offset + 0x400 > file_size)
            return 0;
        previous = offset;
        ++count;
    }
    return count;
}

int VitaPbp_Open(const char *path, unsigned disc_index)
{
    VitaPbp_Close();
    vita_pbp::PbpReader *reader = new vita_pbp::PbpReader();
    const int result = reader->open(path, disc_index);
    if (result == 1)
        vita_pbp::g_reader = reader;
    else
        delete reader;
    return result;
}

void VitaPbp_Close(void)
{
    delete vita_pbp::g_reader;
    vita_pbp::g_reader = NULL;
}

int VitaPbp_IsOpen(void)
{
    return vita_pbp::g_reader != NULL;
}

unsigned VitaPbp_DiscCount(void)
{
    return vita_pbp::g_reader ? vita_pbp::g_reader->disc_count() : 0;
}

unsigned VitaPbp_SectorCount(void)
{
    return vita_pbp::g_reader ? vita_pbp::g_reader->sector_count() : 0;
}

int VitaPbp_ReadSector(unsigned sector, void *dest)
{
    return vita_pbp::g_reader ? vita_pbp::g_reader->read_sector(sector, dest) : -1;
}

void *VitaPbp_GetBuffer(void)
{
    return vita_pbp::g_reader ? vita_pbp::g_reader->buffer() : NULL;
}

}
