#include "../libpcsxcore/arcade/gnet_storage.h"
#include "../libpcsxcore/arcade/chd_hd.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace {
constexpr uint8_t kDrq = 0x08;

std::vector<uint8_t> file_bytes(const std::string &path) {
  std::ifstream stream(path, std::ios::binary);
  assert(stream);
  return {std::istreambuf_iterator<char>(stream),
          std::istreambuf_iterator<char>()};
}

gnet_storage_t *open_storage(const std::string &chd,
                             gnet_pccard_variant_t variant,
                             const std::string *overlay = nullptr,
                             const uint8_t *expected_sha1 = nullptr) {
  gnet_storage_config_t config{};
  config.chd_path = chd.c_str();
  config.overlay_path = overlay ? overlay->c_str() : nullptr;
  config.card_variant = variant;
  config.expected_sha1 = expected_sha1;
  config.expected_sha1_size = expected_sha1 ? 20 : 0;
  char error[256]{};
  gnet_storage_t *storage = nullptr;
  const int result = gnet_storage_open(&config, &storage, error, sizeof(error));
  if (result != GNET_STORAGE_OK)
    std::fprintf(stderr, "open %s failed (%d): %s\n", chd.c_str(), result, error);
  assert(result == GNET_STORAGE_OK && storage);
  return storage;
}

void settle_reset(gnet_storage_t *storage) {
  assert(gnet_storage_run_until(storage, 2'000'000) == GNET_STORAGE_OK);
  assert(gnet_storage_run_until(storage, 4'000'000) == GNET_STORAGE_OK);
}

void write_taskfile(rf5c296_device_t *rf, uint32_t reg, uint16_t value) {
  if (reg == 0) {
    rf5c296_io_write(rf, 0, value, 0xffff);
    return;
  }
  const uint16_t mask = (reg & 1) ? 0xff00 : 0x00ff;
  const uint16_t data = (reg & 1) ? static_cast<uint16_t>(value << 8) : value;
  rf5c296_io_write(rf, reg / 2, data, mask);
}

uint16_t read_taskfile(rf5c296_device_t *rf, uint32_t reg) {
  if (reg == 0) return rf5c296_io_read(rf, 0, 0xffff);
  const uint16_t mask = (reg & 1) ? 0xff00 : 0x00ff;
  const uint16_t data = rf5c296_io_read(rf, reg / 2, mask);
  return (reg & 1) ? data >> 8 : data;
}

void unlock_type1(rf5c296_device_t *rf, const std::array<uint8_t, 5> &key) {
  assert(rf5c296_mem_read(rf, 0x201, 0xffff) == 1);
  for (unsigned i = 0; i < 9; ++i)
    rf5c296_mem_write(rf, 0x280 + i, i < key.size() ? key[i] : 0, 0xffff);
  assert(rf5c296_mem_read(rf, 0x201, 0xffff) == 0);
}

void unlock_type2(rf5c296_device_t *rf,
                  const std::array<uint8_t, 5> &key,
                  gnet_storage_t *storage) {
  write_taskfile(rf, 7, 0xfe);
  assert(gnet_storage_ata_status(storage) & 0x40);
  write_taskfile(rf, 7, 0xfc);
  assert(gnet_storage_ata_status(storage) & kDrq);
  std::array<uint8_t, 512> buffer{};
  std::copy(key.begin(), key.end(), buffer.begin() + 2);
  for (unsigned i = 0; i < 256; ++i) {
    uint16_t word = uint16_t(buffer[i * 2]) |
                    (uint16_t(buffer[i * 2 + 1]) << 8);
    write_taskfile(rf, 0, word);
  }
  assert(gnet_storage_run_until(storage,
           gnet_storage_now(storage) + 100'000) == GNET_STORAGE_OK);
  assert(!gnet_storage_is_locked(storage));
}

void unlock_cf(rf5c296_device_t *rf,
               const std::array<uint8_t, 5> &key,
               gnet_storage_t *storage) {
  write_taskfile(rf, 1, key[0]);
  write_taskfile(rf, 2, key[1]);
  write_taskfile(rf, 3, key[2]);
  write_taskfile(rf, 4, key[3]);
  write_taskfile(rf, 5, key[4]);
  write_taskfile(rf, 7, 0x0f);
  assert(!gnet_storage_is_locked(storage));
}

void unlock(gnet_storage_t *storage, gnet_pccard_variant_t variant,
            const std::array<uint8_t, 5> &key) {
  if (variant == GNET_PCCARD_ATA_FLASH) return;
  if (variant == GNET_PCCARD_TAITO_TYPE1) {
    auto *rf = rf5c296_create(gnet_storage_card_ops(storage));
    assert(rf);
    unlock_type1(rf, key);
    rf5c296_destroy(rf);
  } else {
    auto *rf = rf5c296_create(gnet_storage_card_ops(storage));
    assert(rf);
    if (variant == GNET_PCCARD_TAITO_TYPE2)
      unlock_type2(rf, key, storage);
    else
      unlock_cf(rf, key, storage);
    rf5c296_destroy(rf);
  }
}

uint16_t read_identify_word0(gnet_storage_t *storage) {
  auto *rf = rf5c296_create(gnet_storage_card_ops(storage));
  assert(rf);
  if (gnet_storage_now(storage) < 4'000'000)
    settle_reset(storage);
  write_taskfile(rf, 7, 0xec);
  assert(gnet_storage_run_until(storage,
           gnet_storage_now(storage) + 10'000) == GNET_STORAGE_OK);
  assert(gnet_storage_ata_status(storage) & kDrq);
  const uint16_t result = read_taskfile(rf, 0);
  rf5c296_destroy(rf);
  return result;
}

void identify_and_sector_roundtrip(gnet_storage_t *storage) {
  auto *rf = rf5c296_create(gnet_storage_card_ops(storage));
  assert(rf);
  if (gnet_storage_now(storage) < 4'000'000)
    assert(gnet_storage_run_until(storage, 4'000'000) == GNET_STORAGE_OK);
  write_taskfile(rf, 7, 0xec);
  assert(gnet_storage_run_until(storage,
           gnet_storage_now(storage) + 10'000) == GNET_STORAGE_OK);
  assert(gnet_storage_ata_status(storage) & kDrq);
  std::array<uint8_t, 512> identify{};
  for (unsigned i = 0; i < 256; ++i) {
    const uint16_t word = read_taskfile(rf, 0);
    identify[i * 2] = static_cast<uint8_t>(word);
    identify[i * 2 + 1] = static_cast<uint8_t>(word >> 8);
  }
  assert(identify[0] == 0x34 && identify[1] == 0x12);
  assert(identify[510] == 0xfe && identify[511] == 0x01);

  std::array<uint8_t, 512> written{};
  for (unsigned i = 0; i < written.size(); ++i)
    written[i] = static_cast<uint8_t>(0x5b ^ i * 19);
  write_taskfile(rf, 2, 1);
  write_taskfile(rf, 3, 3);
  write_taskfile(rf, 4, 0);
  write_taskfile(rf, 5, 0);
  write_taskfile(rf, 6, 0x40);
  write_taskfile(rf, 7, 0x30);
  assert(gnet_storage_ata_status(storage) & kDrq);
  for (unsigned i = 0; i < 256; ++i) {
    const uint16_t word = uint16_t(written[i * 2]) |
                          (uint16_t(written[i * 2 + 1]) << 8);
    write_taskfile(rf, 0, word);
  }
  assert(gnet_storage_run_until(storage,
           gnet_storage_now(storage) + 100'000) == GNET_STORAGE_OK);

  write_taskfile(rf, 2, 1);
  write_taskfile(rf, 3, 3);
  write_taskfile(rf, 4, 0);
  write_taskfile(rf, 5, 0);
  write_taskfile(rf, 6, 0x40);
  write_taskfile(rf, 7, 0x20);
  assert(gnet_storage_run_until(storage,
           gnet_storage_now(storage)) == GNET_STORAGE_OK);
  assert(gnet_storage_ata_status(storage) & kDrq);
  std::array<uint8_t, 512> readback{};
  for (unsigned i = 0; i < 256; ++i) {
    const uint16_t word = read_taskfile(rf, 0);
    readback[i * 2] = static_cast<uint8_t>(word);
    readback[i * 2 + 1] = static_cast<uint8_t>(word >> 8);
  }
  assert(readback == written);
  rf5c296_destroy(rf);
}

std::array<uint8_t, 512> read_sector3(gnet_storage_t *storage) {
  auto *rf = rf5c296_create(gnet_storage_card_ops(storage));
  assert(rf);
  write_taskfile(rf, 2, 1);
  write_taskfile(rf, 3, 3);
  write_taskfile(rf, 4, 0);
  write_taskfile(rf, 5, 0);
  write_taskfile(rf, 6, 0x40);
  write_taskfile(rf, 7, 0x20);
  assert(gnet_storage_run_until(storage,
           gnet_storage_now(storage)) == GNET_STORAGE_OK);
  assert(gnet_storage_ata_status(storage) & kDrq);
  std::array<uint8_t, 512> result{};
  for (unsigned i = 0; i < 256; ++i) {
    const uint16_t word = read_taskfile(rf, 0);
    result[i * 2] = static_cast<uint8_t>(word);
    result[i * 2 + 1] = static_cast<uint8_t>(word >> 8);
  }
  rf5c296_destroy(rf);
  return result;
}

std::array<uint8_t, 20> image_sha1(const std::string &path) {
  char error[256]{};
  auto *disk = arcade_chd_hd_open(path.c_str(), nullptr, error, sizeof(error));
  assert(disk);
  arcade_chd_hd_info_t info{};
  arcade_chd_hd_get_info(disk, &info);
  arcade_chd_hd_close(disk);
  std::array<uint8_t, 20> result{};
  std::copy(std::begin(info.sha1), std::end(info.sha1), result.begin());
  return result;
}

void test_metadata_and_card_variants(const std::string &dir) {
  const std::array<uint8_t, 5> key{{0x10, 0x21, 0x32, 0x43, 0x54}};
  const auto expected_sha1 = image_sha1(dir + "/complete.chd");
  for (auto variant : {GNET_PCCARD_TAITO_TYPE1, GNET_PCCARD_TAITO_TYPE2,
                       GNET_PCCARD_TAITO_COMPACT_FLASH,
                       GNET_PCCARD_ATA_FLASH}) {
    const std::string path = dir + "/complete.chd";
    const std::string overlay = dir + "/variant-" +
        std::to_string(static_cast<int>(variant)) + ".overlay";
    auto *storage = open_storage(path, variant, &overlay, expected_sha1.data());
    gnet_storage_info_t info{};
    gnet_storage_get_info(storage, &info);
    assert(info.cylinders == 2 && info.heads == 2 &&
           info.sectors_per_track == 5 && info.sector_count == 20);
    assert(info.metadata_cis_present && info.metadata_cis_bytes == 4);
    assert(info.metadata_key_present && info.metadata_key_bytes == 5);
    assert(info.metadata_identify_present && info.metadata_identify_bytes == 512);
    assert(gnet_storage_is_locked(storage) ==
           (variant != GNET_PCCARD_ATA_FLASH));
    assert(gnet_storage_reset(storage) == GNET_STORAGE_OK);
    assert(gnet_storage_is_locked(storage) ==
           (variant != GNET_PCCARD_ATA_FLASH));
    settle_reset(storage);
    if (variant != GNET_PCCARD_ATA_FLASH) unlock(storage, variant, key);
    identify_and_sector_roundtrip(storage);
    assert(gnet_storage_flush(storage) == GNET_STORAGE_OK);
    assert(gnet_storage_try_destroy(storage) == GNET_STORAGE_OK);
  }

  auto *missing = open_storage(dir + "/missing.chd", GNET_PCCARD_TAITO_TYPE1);
  gnet_storage_info_t missing_info{};
  gnet_storage_get_info(missing, &missing_info);
  assert(!missing_info.metadata_cis_present && !missing_info.metadata_key_present &&
         !missing_info.metadata_identify_present);
  auto *rf = rf5c296_create(gnet_storage_card_ops(missing));
  assert(rf && rf5c296_mem_read(rf, 0, 0xffff) == 0x01);
  assert(!gnet_storage_is_locked(missing));
  assert(read_identify_word0(missing) == 0x848a);
  rf5c296_io_write(rf, 0x1f0, 0x03, 0x00ff);
  rf5c296_io_write(rf, 0x1f0, 0x00, 0xff00);
  rf5c296_destroy(rf);
  assert(gnet_storage_reset(missing) == GNET_STORAGE_OK);
  assert(gnet_storage_try_destroy(missing) == GNET_STORAGE_OK);

  auto *empty = open_storage(dir + "/empty-cis.chd", GNET_PCCARD_ATA_FLASH);
  gnet_storage_info_t empty_info{};
  gnet_storage_get_info(empty, &empty_info);
  assert(empty_info.metadata_cis_present && empty_info.metadata_cis_bytes == 0);
  rf = rf5c296_create(gnet_storage_card_ops(empty));
  assert(rf && rf5c296_mem_read(rf, 0, 0xffff) == 0xff);
  rf5c296_destroy(rf);
  assert(gnet_storage_try_destroy(empty) == GNET_STORAGE_OK);

  for (auto variant : {GNET_PCCARD_TAITO_TYPE1, GNET_PCCARD_TAITO_TYPE2,
                       GNET_PCCARD_TAITO_COMPACT_FLASH}) {
    auto *wrong_key = open_storage(dir + "/wrong-key.chd", variant);
    gnet_storage_info_t wrong_info{};
    gnet_storage_get_info(wrong_key, &wrong_info);
    assert(wrong_info.metadata_key_present && wrong_info.metadata_key_bytes == 4);
    assert(!gnet_storage_is_locked(wrong_key));
    assert(gnet_storage_try_destroy(wrong_key) == GNET_STORAGE_OK);
  }

  auto *wrong_identify = open_storage(dir + "/wrong-identify.chd",
                                      GNET_PCCARD_ATA_FLASH);
  gnet_storage_info_t wrong_identify_info{};
  gnet_storage_get_info(wrong_identify, &wrong_identify_info);
  assert(wrong_identify_info.metadata_identify_present &&
         wrong_identify_info.metadata_identify_bytes == 511);
  assert(read_identify_word0(wrong_identify) == 0x848a);
  assert(gnet_storage_try_destroy(wrong_identify) == GNET_STORAGE_OK);
}

void test_overlay_reopen_and_sha_guard(const std::string &dir) {
  const std::string chd = dir + "/complete.chd";
  const std::string overlay = dir + "/gnet.overlay";
  const auto original = file_bytes(chd);
  const auto expected_sha1 = image_sha1(chd);
  auto *storage = open_storage(chd, GNET_PCCARD_ATA_FLASH, &overlay,
                               expected_sha1.data());
  identify_and_sector_roundtrip(storage);
  assert(gnet_storage_flush(storage) == GNET_STORAGE_OK);
  assert(gnet_storage_try_destroy(storage) == GNET_STORAGE_OK);
  assert(file_bytes(chd) == original);
  storage = open_storage(chd, GNET_PCCARD_ATA_FLASH, &overlay,
                         expected_sha1.data());
  settle_reset(storage);
  std::array<uint8_t, 512> written{};
  for (unsigned i = 0; i < written.size(); ++i)
    written[i] = static_cast<uint8_t>(0x5b ^ i * 19);
  assert(read_sector3(storage) == written);
  assert(gnet_storage_try_destroy(storage) == GNET_STORAGE_OK);
  assert(file_bytes(chd) == original);

  gnet_storage_config_t config{};
  config.chd_path = chd.c_str();
  config.card_variant = GNET_PCCARD_ATA_FLASH;
  std::array<uint8_t, 20> wrong = expected_sha1;
  wrong[0] ^= 0xff;
  config.expected_sha1 = wrong.data();
  config.expected_sha1_size = wrong.size();
  char error[256]{};
  storage = nullptr;
  assert(gnet_storage_open(&config, &storage, error, sizeof(error)) ==
         GNET_STORAGE_HASH_MISMATCH);
  assert(!storage && std::strstr(error, "SHA1"));
}

void test_open_rejects_bad_metadata_and_geometry(const std::string &dir) {
  for (const auto [file, expected] :
       {std::pair{"broken-cis.chd", GNET_STORAGE_METADATA},
        std::pair{"bad-geometry.chd", GNET_STORAGE_GEOMETRY}}) {
    gnet_storage_config_t config{};
    const std::string path = dir + "/" + file;
    config.chd_path = path.c_str();
    config.card_variant = GNET_PCCARD_ATA_FLASH;
    char error[256]{};
    gnet_storage_t *storage = nullptr;
    const int result = gnet_storage_open(&config, &storage, error, sizeof(error));
    assert(result == expected);
    assert(!storage && *error);
  }
}
}  // namespace

int main(int argc, char **argv) {
  assert(argc == 2);
  const std::string dir = argv[1];
  test_metadata_and_card_variants(dir);
  test_overlay_reopen_and_sha_guard(dir);
  test_open_rejects_bad_metadata_and_geometry(dir);
  std::puts("G-Net CHD + ATA + PC Card composition, PIO and overlay PASS");
}
