#ifndef PCSX_VITA_FIOS_COVERS_H
#define PCSX_VITA_FIOS_COVERS_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

class VitaFiosCovers {
public:
    VitaFiosCovers();
    ~VitaFiosCovers();

    bool open(const char *archive_path);
    void close();
    bool is_open() const;
    bool cache_enabled() const;
    bool contains(const std::string &name) const;
    bool read(const std::string &name, std::vector<unsigned char> &bytes,
              size_t max_size = 8 * 1024 * 1024);
    const std::vector<std::string> &names() const;

private:
    VitaFiosCovers(const VitaFiosCovers &) = delete;
    VitaFiosCovers &operator=(const VitaFiosCovers &) = delete;

    bool read_mounted_file(const std::string &name,
                           std::vector<unsigned char> &bytes, size_t max_size);

    bool owns_fios_;
    bool mounted_;
    int32_t archive_handle_;
    void *mount_buffer_;
    void *dearchiver_buffer_;
    void *cache_buffer_;
    std::vector<std::string> names_;
    std::unordered_set<std::string> index_;
};

#endif
