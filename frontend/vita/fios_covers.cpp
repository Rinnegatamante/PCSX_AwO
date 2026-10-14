#include "fios_covers.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <malloc.h>

#include <psp2/sysmodule.h>

#include "paths.h"

namespace {

enum {
    kPathMax = 192,
    kCacheBlockSize = 64 * 1024,
    kCacheBlockCount = 64,
    kDearchiverWorkSize = 192 * 1024
};

#define FIOS_STORAGE_SIZE(num, size) \
    ((num) * (size) + ((((num) + 7) / 8 + 7) & ~7))

struct FiosBuffer {
    void *pointer;
    size_t length;
};

struct FiosParams {
    uint32_t initialized : 1;
    uint32_t params_size : 15;
    uint32_t path_max : 16;
    uint32_t profiling;
    uint32_t io_thread_count;
    uint32_t threads_per_scheduler;
    uint32_t extra_flag : 1;
    uint32_t extra_flags : 31;
    uint32_t max_chunk;
    uint8_t max_decompressor_thread_count;
    uint8_t reserved1;
    uint8_t reserved2;
    uint8_t reserved3;
    intptr_t reserved4;
    intptr_t reserved5;
    FiosBuffer op_storage;
    FiosBuffer fh_storage;
    FiosBuffer dh_storage;
    FiosBuffer chunk_storage;
    void *vprintf_callback;
    void *memcpy_callback;
    void *profile_callback;
    int thread_priority[3];
    int thread_affinity[3];
    int thread_stack_size[3];
};

struct FiosDearchiverContext {
    size_t size;
    size_t work_buffer_size;
    void *work_buffer;
    intptr_t flags;
    intptr_t reserved[3];
};

struct FiosCacheContext {
    size_t size;
    size_t work_buffer_size;
    size_t block_size;
    void *work_buffer;
    const char *path;
    intptr_t flags;
    intptr_t reserved[3];
};

static_assert(sizeof(FiosParams) == 116, "FIOS2 params ABI mismatch");
static_assert(sizeof(FiosBuffer) == 8, "FIOS2 buffer ABI mismatch");
static_assert(sizeof(FiosDearchiverContext) == 28, "FIOS2 dearchiver ABI mismatch");
static_assert(sizeof(FiosCacheContext) == 36, "FIOS2 cache ABI mismatch");

alignas(8) static unsigned char s_op_storage[FIOS_STORAGE_SIZE(32, 168 + kPathMax)];
alignas(8) static unsigned char s_chunk_storage[FIOS_STORAGE_SIZE(128, 64)];
alignas(8) static unsigned char s_fh_storage[FIOS_STORAGE_SIZE(32, 80 + kPathMax)];
alignas(8) static unsigned char s_dh_storage[FIOS_STORAGE_SIZE(8, 80 + kPathMax)];
static FiosDearchiverContext s_dearchiver = {};
static FiosCacheContext s_cache = {};

const char kMountPath[] = PCSX_AWO_APP_ROOT "/.covers_psarc";
const char kIndexName[] = "__covers_index.txt";

extern "C" {
int sceFiosIsInitialized(void);
int sceFiosInitialize(const FiosParams *params);
void sceFiosTerminate(void);
int sceFiosIOFilterAdd(int index, void *callback, void *context);
void sceFiosIOFilterCache(void);
void sceFiosIOFilterPsarcDearchiver(void);
int64_t sceFiosArchiveGetMountBufferSizeSync(const void *attr, const char *path,
                                            void *params);
int sceFiosArchiveMountSync(const void *attr, int32_t *handle, const char *archive,
                            const char *mount_path, FiosBuffer buffer, void *params);
int sceFiosFHOpenSync(const void *attr, int32_t *handle, const char *path,
                      const void *params);
int64_t sceFiosFHGetSize(int32_t handle);
int64_t sceFiosFHReadSync(const void *attr, int32_t handle, void *dst, int64_t size);
int sceFiosFHCloseSync(const void *attr, int32_t handle);
}

}

VitaFiosCovers::VitaFiosCovers()
    : owns_fios_(false), mounted_(false), archive_handle_(-1),
      mount_buffer_(NULL), dearchiver_buffer_(NULL), cache_buffer_(NULL)
{
}

VitaFiosCovers::~VitaFiosCovers()
{
    close();
}

bool VitaFiosCovers::is_open() const
{
    return mounted_;
}

bool VitaFiosCovers::cache_enabled() const
{
    return cache_buffer_ != NULL;
}

const std::vector<std::string> &VitaFiosCovers::names() const
{
    return names_;
}

bool VitaFiosCovers::contains(const std::string &name) const
{
    return mounted_ && index_.find(name) != index_.end();
}

void VitaFiosCovers::close()
{
    names_.clear();
    index_.clear();
    if (owns_fios_)
        sceFiosTerminate();
    owns_fios_ = false;
    mounted_ = false;
    archive_handle_ = -1;
    free(mount_buffer_);
    mount_buffer_ = NULL;
    free(cache_buffer_);
    cache_buffer_ = NULL;
    free(dearchiver_buffer_);
    dearchiver_buffer_ = NULL;
    memset(&s_dearchiver, 0, sizeof(s_dearchiver));
    memset(&s_cache, 0, sizeof(s_cache));
}

bool VitaFiosCovers::read_mounted_file(const std::string &name,
    std::vector<unsigned char> &data, size_t max_size)
{
    data.clear();
    if (!mounted_ || name.empty())
        return false;

    const std::string path = std::string(kMountPath) + "/" + name;
    int32_t handle = -1;
    if (sceFiosFHOpenSync(NULL, &handle, path.c_str(), NULL) < 0)
        return false;

    const int64_t size = sceFiosFHGetSize(handle);
    bool success = size >= 0 && (uint64_t)size <= max_size;
    if (success) {
        data.resize((size_t)size);
        size_t offset = 0;
        while (offset < data.size()) {
            const int64_t received = sceFiosFHReadSync(NULL, handle,
                data.data() + offset, (int64_t)(data.size() - offset));
            if (received <= 0 || (uint64_t)received > data.size() - offset) {
                success = false;
                break;
            }
            offset += (size_t)received;
        }
    }
    if (sceFiosFHCloseSync(NULL, handle) < 0)
        success = false;
    if (!success)
        data.clear();
    return success;
}

bool VitaFiosCovers::read(const std::string &name,
    std::vector<unsigned char> &data, size_t max_size)
{
    if (!contains(name)) {
        data.clear();
        return false;
    }
    return read_mounted_file(name, data, max_size);
}

bool VitaFiosCovers::open(const char *archive_path)
{
    close();
    if (!archive_path || !archive_path[0] ||
        sceSysmoduleLoadModule(SCE_SYSMODULE_FIOS2) < 0)
        return false;
    if (sceFiosIsInitialized() > 0)
        return false;

    FiosParams params = {};
    params.params_size = sizeof(params);
    params.path_max = kPathMax;
    params.io_thread_count = 2;
    params.threads_per_scheduler = 1;
    params.max_chunk = 256 * 1024;
    params.max_decompressor_thread_count = 2;
    params.op_storage = { s_op_storage, sizeof(s_op_storage) };
    params.fh_storage = { s_fh_storage, sizeof(s_fh_storage) };
    params.dh_storage = { s_dh_storage, sizeof(s_dh_storage) };
    params.chunk_storage = { s_chunk_storage, sizeof(s_chunk_storage) };
    params.thread_priority[0] = 64;
    params.thread_priority[1] = 191;
    params.thread_priority[2] = 191;
    params.thread_affinity[0] = 0x10000;
    params.thread_stack_size[0] = 8 * 1024;
    params.thread_stack_size[1] = 16 * 1024;
    params.thread_stack_size[2] = 8 * 1024;

    if (sceFiosInitialize(&params) < 0)
        return false;
    owns_fios_ = true;

    dearchiver_buffer_ = memalign(64, kDearchiverWorkSize);
    if (!dearchiver_buffer_) {
        close();
        return false;
    }
    s_dearchiver.size = sizeof(s_dearchiver);
    s_dearchiver.work_buffer_size = kDearchiverWorkSize;
    s_dearchiver.work_buffer = dearchiver_buffer_;
    if (sceFiosIOFilterAdd(0, reinterpret_cast<void *>(sceFiosIOFilterPsarcDearchiver),
                           &s_dearchiver) < 0) {
        close();
        return false;
    }

    cache_buffer_ = memalign(8, kCacheBlockSize * kCacheBlockCount);
    if (cache_buffer_) {
        s_cache.size = sizeof(s_cache);
        s_cache.work_buffer_size = kCacheBlockSize * kCacheBlockCount;
        s_cache.block_size = kCacheBlockSize;
        s_cache.work_buffer = cache_buffer_;
        s_cache.path = archive_path;
        if (sceFiosIOFilterAdd(1, reinterpret_cast<void *>(sceFiosIOFilterCache),
                               &s_cache) < 0) {
            free(cache_buffer_);
            cache_buffer_ = NULL;
            memset(&s_cache, 0, sizeof(s_cache));
        }
    }

    const int64_t mount_size = sceFiosArchiveGetMountBufferSizeSync(NULL, archive_path, NULL);
    if (mount_size <= 0 || mount_size > 16 * 1024 * 1024) {
        close();
        return false;
    }
    mount_buffer_ = memalign(64, (size_t)mount_size);
    if (!mount_buffer_) {
        close();
        return false;
    }
    FiosBuffer buffer = { mount_buffer_, (size_t)mount_size };
    if (sceFiosArchiveMountSync(NULL, &archive_handle_, archive_path,
                                kMountPath, buffer, NULL) < 0) {
        close();
        return false;
    }
    mounted_ = true;

    std::vector<unsigned char> index_bytes;
    if (!read_mounted_file(kIndexName, index_bytes, 1024 * 1024)) {
        close();
        return false;
    }
    size_t start = 0;
    while (start < index_bytes.size()) {
        size_t end = start;
        while (end < index_bytes.size() && index_bytes[end] != '\n')
            ++end;
        size_t trimmed_end = end;
        if (trimmed_end > start && index_bytes[trimmed_end - 1] == '\r')
            --trimmed_end;
        if (trimmed_end > start) {
            const std::string name((const char *)index_bytes.data() + start,
                                   trimmed_end - start);
            if (name.find('\0') != std::string::npos ||
                name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
                !index_.insert(name).second) {
                close();
                return false;
            }
            names_.push_back(name);
        }
        start = end + 1;
    }
    if (names_.empty() || names_.size() > 10000) {
        close();
        return false;
    }
    return true;
}
