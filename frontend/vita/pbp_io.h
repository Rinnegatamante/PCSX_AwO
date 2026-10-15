#ifndef PCSX_VITA_PBP_IO_H
#define PCSX_VITA_PBP_IO_H

#include <cstddef>
#include <cstdint>
#include <string>

#ifdef __vita__
#include <psp2/io/fcntl.h>
#else
#include <cstdio>
#include <sys/types.h>
#endif

namespace vita_pbp {
namespace io {

class File {
public:
    File()
#ifdef __vita__
        : handle_(-1)
#else
        : handle_(NULL)
#endif
    {
    }

    ~File() { close(); }

    bool open_read(const std::string &path)
    {
        close();
#ifdef __vita__
        handle_ = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0);
        return handle_ >= 0;
#else
        handle_ = std::fopen(path.c_str(), "rb");
        return handle_ != NULL;
#endif
    }

    bool size(uint64_t &bytes)
    {
#ifdef __vita__
        if (handle_ < 0)
            return false;
        const SceOff end = sceIoLseek(handle_, 0, SCE_SEEK_END);
        if (end < 0)
            return false;
        bytes = (uint64_t)end;
#else
        if (!handle_)
            return false;
#ifdef _WIN32
        if (_fseeki64(handle_, 0, SEEK_END) != 0)
            return false;
        const __int64 end = _ftelli64(handle_);
#else
        if (fseeko(handle_, 0, SEEK_END) != 0)
            return false;
        const off_t end = ftello(handle_);
#endif
        if (end < 0)
            return false;
        bytes = (uint64_t)end;
#endif
        return true;
    }

    bool read_at(uint64_t offset, void *data, size_t length, uint64_t file_size)
    {
        if (offset > file_size || length > file_size - offset || !seek(offset))
            return false;
        unsigned char *dest = static_cast<unsigned char *>(data);
        size_t done = 0;
        while (done < length) {
#ifdef __vita__
            const size_t remaining = length - done;
            const SceSize amount = (SceSize)(remaining > 0x7fffffffU ?
                0x7fffffffU : remaining);
            const int read = sceIoRead(handle_, dest + done, amount);
            if (read <= 0)
                return false;
            done += (size_t)read;
#else
            const size_t read = std::fread(dest + done, 1, length - done, handle_);
            if (!read)
                return false;
            done += read;
#endif
        }
        return true;
    }

    bool close()
    {
#ifdef __vita__
        if (handle_ < 0)
            return true;
        const SceUID fd = handle_;
        handle_ = -1;
        return sceIoClose(fd) >= 0;
#else
        if (!handle_)
            return true;
        FILE *file = handle_;
        handle_ = NULL;
        return std::fclose(file) == 0;
#endif
    }

private:
    File(const File &);
    File &operator=(const File &);

    bool seek(uint64_t offset)
    {
        if (offset > INT64_MAX)
            return false;
#ifdef __vita__
        return handle_ >= 0 &&
            sceIoLseek(handle_, (SceOff)offset, SCE_SEEK_SET) == (SceOff)offset;
#else
        if (!handle_)
            return false;
#ifdef _WIN32
        return _fseeki64(handle_, (__int64)offset, SEEK_SET) == 0;
#else
        return fseeko(handle_, (off_t)offset, SEEK_SET) == 0;
#endif
#endif
    }

#ifdef __vita__
    SceUID handle_;
#else
    FILE *handle_;
#endif
};

}
}

#endif
