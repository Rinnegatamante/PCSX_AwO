/* Native CHD host adapter checks, GPL-2.0-or-later. */
#include "../libpcsxcore/arcade/chd_hd.h"
extern "C" {
#include <libchdr/bitstream.h>
}
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/stat.h>

using Sector = std::array<uint8_t, 512>;
static Sector expected(unsigned lba) {
  Sector data{};
  for (unsigned i = 0; i < data.size(); ++i) data[i] = (lba * 17 + i * 13 + (i >> 4)) & 255;
  return data;
}
static std::vector<uint8_t> file_bytes(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  assert(f); return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
static void write_bytes(const std::string &path, const std::vector<uint8_t> &data) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  assert(f); f.write(reinterpret_cast<const char *>(data.data()), data.size()); assert(f);
}
int main(int argc, char **argv) {
  const uint8_t bits[] = {0x92, 0xff, 0x80, 0x03, 0xee};
  auto *stream = create_bitstream(bits, sizeof(bits)); assert(stream);
  assert(bitstream_read(stream, 32) == 0x92ff8003);
  assert(bitstream_read(stream, 8) == 0xee);
  bitstream_seek_bits(stream, 0);
  assert(bitstream_read(stream, 4) == 9);
  assert(bitstream_read(stream, 12) == 0x2ff);
  std::free(stream);
  assert(argc == 2);
  std::string dir = argv[1], image = dir + "/disk.chd", overlay = dir + "/disk.overlay";
  char error[256];
  auto *disk = arcade_chd_hd_open(image.c_str(), nullptr, error, sizeof(error));
  assert(disk && !*error);
  arcade_chd_hd_info_t info{}; arcade_chd_hd_get_info(disk, &info);
  assert(info.cylinders == 2 && info.heads == 2 && info.sectors_per_track == 5 &&
         info.sector_bytes == 512 && info.sector_count == 20 && info.hunk_bytes == 1536);
  assert(!info.inquiry_bytes);
  Sector data{};
  for (unsigned i = 0; i < 20; ++i) {
    assert(!arcade_chd_hd_read(disk, i, data.data(), data.size())); assert(data == expected(i));
  }
  // Cache replacement after the final partial hunk must remain correct.
  for (unsigned i : {0u, 19u, 3u, 2u, 18u, 0u}) {
    assert(!arcade_chd_hd_read(disk, i, data.data(), data.size())); assert(data == expected(i));
  }
  data.fill(0xa5); Sector untouched = data;
  assert(arcade_chd_hd_read(disk, 20, data.data(), data.size()) == -1 && data == untouched);
  assert(arcade_chd_hd_read(disk, 0, data.data(), 511) == -1 && data == untouched);
  assert(arcade_chd_hd_write(disk, 0, data.data(), data.size()) == -1);
  assert(!arcade_chd_hd_flush(disk)); arcade_chd_hd_close(disk);

  disk = arcade_chd_hd_open(image.c_str(), overlay.c_str(), error, sizeof(error)); assert(disk);
  Sector replacement{}; replacement.fill(0x76);
  assert(arcade_chd_hd_write(disk, 20, replacement.data(), 512) == -1);
  assert(!arcade_chd_hd_write(disk, 19, replacement.data(), 512));
  replacement[0] = 0x33;
  assert(!arcade_chd_hd_write(disk, 1, replacement.data(), 512));
  replacement[0] = 0x44;
  assert(!arcade_chd_hd_write(disk, 1, replacement.data(), 512));
  assert(arcade_chd_hd_modified_sectors(disk) == 2);
  assert(!arcade_chd_hd_read(disk, 1, data.data(), 512) && data == replacement);
  assert(!arcade_chd_hd_flush(disk)); auto saved = file_bytes(overlay);
  assert(saved.size() == 80 + 2 * 520);
  assert(!arcade_chd_hd_flush(disk) && file_bytes(overlay) == saved);
  arcade_chd_hd_close(disk);
  disk = arcade_chd_hd_open(image.c_str(), overlay.c_str(), error, sizeof(error)); assert(disk);
  assert(!arcade_chd_hd_read(disk, 1, data.data(), 512) && data == replacement);
  assert(!arcade_chd_hd_read(disk, 0, data.data(), 512) && data == expected(0));
  arcade_chd_hd_close(disk);

  // Close deliberately discards uncommitted writes, preserving the old image.
  disk = arcade_chd_hd_open(image.c_str(), overlay.c_str(), error, sizeof(error)); assert(disk);
  replacement.fill(0xaa); assert(!arcade_chd_hd_write(disk, 0, replacement.data(), 512));
  arcade_chd_hd_close(disk); assert(file_bytes(overlay) == saved);
  std::string other = dir + "/other.chd";
  assert(!arcade_chd_hd_open(other.c_str(), overlay.c_str(), error, sizeof(error)) && *error);
  for (const char *name : {"duplicate.chd", "truncated.chd", "missing.chd"}) {
    std::string path = dir + "/" + name;
    assert(!arcade_chd_hd_open(path.c_str(), nullptr, error, sizeof(error)) && *error);
  }
  std::string bad = dir + "/bad.overlay";
  auto mutated = saved; mutated.pop_back(); write_bytes(bad, mutated);
  assert(!arcade_chd_hd_open(image.c_str(), bad.c_str(), error, sizeof(error)));
  mutated = saved; mutated[90] ^= 1; write_bytes(bad, mutated);
  assert(!arcade_chd_hd_open(image.c_str(), bad.c_str(), error, sizeof(error)));
  mutated = saved; mutated[80] ^= 1; write_bytes(bad, mutated);
  assert(!arcade_chd_hd_open(image.c_str(), bad.c_str(), error, sizeof(error)));
  mutated = saved; mutated.push_back(0); write_bytes(bad, mutated);
  assert(!arcade_chd_hd_open(image.c_str(), bad.c_str(), error, sizeof(error)));

  // Destination changed after open: never overwrite the source CHD, including
  // aliases, and never truncate an unrelated existing directory on failure.
  std::string blocked = dir + "/blocked.overlay";
  disk = arcade_chd_hd_open(image.c_str(), blocked.c_str(), error, sizeof(error)); assert(disk);
  assert(!arcade_chd_hd_write(disk, 0, replacement.data(), 512));
  assert(!symlink(image.c_str(), blocked.c_str()));
  assert(arcade_chd_hd_flush(disk) == -1);
  assert(!unlink(blocked.c_str())); assert(!link(image.c_str(), blocked.c_str()));
  assert(arcade_chd_hd_flush(disk) == -1);
  assert(!unlink(blocked.c_str())); assert(!mkdir(blocked.c_str(), 0700));
  assert(arcade_chd_hd_flush(disk) == -1);
  assert(!rmdir(blocked.c_str())); assert(!arcade_chd_hd_flush(disk));
  arcade_chd_hd_close(disk);
  std::puts("CHD sector cache, read-only source and atomic validated overlay PASS");
}
