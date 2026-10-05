/* G-Net CHD + ATA HLE + PC Card composition.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "gnet_storage.h"

#include "ata_storage.h"
#include "chd_hd.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <vector>

struct gnet_storage {
  arcade_chd_hd_t *disk = nullptr;
  ata_storage_t *ata = nullptr;
  gnet_pccard_t *card = nullptr;
  rf5c296_card_ops_t rf_ops{};
  arcade_chd_hd_info_t disk_info{};
  gnet_storage_info_t info{};
  std::vector<uint8_t> cis, key, identify;
  unsigned callback_depth = 0;
  unsigned operation_depth = 0;

  ~gnet_storage() {
    if (ata && card)
      (void)ata_storage_set_pccard(ata, nullptr);
    if (card) {
      (void)gnet_pccard_try_destroy(card);
      card = nullptr;
    }
    if (ata) {
      (void)ata_storage_try_destroy(ata);
      ata = nullptr;
    }
    if (disk) {
      arcade_chd_hd_close(disk);
      disk = nullptr;
    }
  }
};

namespace {
constexpr size_t kMaxMetadataBytes = 16 * 1024 * 1024;

struct DepthGuard {
  explicit DepthGuard(unsigned &depth_) : depth(depth_) { ++depth; }
  ~DepthGuard() { --depth; }
  unsigned &depth;
};

void set_error(char *out, size_t capacity, const char *message) {
  if (out && capacity)
    std::snprintf(out, capacity, "%s", message);
}

int read_metadata(arcade_chd_hd_t *disk, arcade_chd_hd_metadata_kind_t kind,
                  std::vector<uint8_t> *out, bool *present,
                  size_t *exact_size) {
  size_t required = 0;
  auto status = arcade_chd_hd_read_metadata(disk, kind, nullptr, 0, &required);
  if (status == ARCADE_CHD_HD_METADATA_MISSING) {
    *present = false;
    out->clear();
    *exact_size = 0;
    return 0;
  }
  if (status == ARCADE_CHD_HD_METADATA_ERROR ||
      status == ARCADE_CHD_HD_METADATA_ARGUMENT ||
      required > kMaxMetadataBytes)
    return -1;
  if (status == ARCADE_CHD_HD_METADATA_OK && required != 0) return -1;
  if (status != ARCADE_CHD_HD_METADATA_OK &&
      status != ARCADE_CHD_HD_METADATA_BUFFER_TOO_SMALL)
    return -1;

  try {
    out->resize(required);
  } catch (...) {
    return -2;
  }
  size_t actual = std::numeric_limits<size_t>::max();
  status = arcade_chd_hd_read_metadata(disk, kind,
                                       required ? out->data() : nullptr,
                                       required, &actual);
  if (status != ARCADE_CHD_HD_METADATA_OK || actual != required) {
    out->clear();
    return -1;
  }
  *present = true;
  *exact_size = required;
  return 0;
}

int media_read(void *opaque, uint32_t lba, void *buffer) {
  auto *storage = static_cast<gnet_storage *>(opaque);
  DepthGuard guard(storage->callback_depth);
  return arcade_chd_hd_read(storage->disk, lba,
                            static_cast<uint8_t *>(buffer), 512) == 0;
}

int media_write(void *opaque, uint32_t lba, const void *buffer) {
  auto *storage = static_cast<gnet_storage *>(opaque);
  DepthGuard guard(storage->callback_depth);
  return arcade_chd_hd_write(storage->disk, lba,
                             static_cast<const uint8_t *>(buffer), 512) == 0;
}

uint16_t card_read_memory(void *opaque, uint32_t offset, uint16_t mem_mask) {
  auto *storage = static_cast<gnet_storage *>(opaque);
  DepthGuard guard(storage->callback_depth);
  const auto *ops = gnet_pccard_card_ops(storage->card);
  return ops->read_memory(ops->opaque, offset, mem_mask);
}

void card_write_memory(void *opaque, uint32_t offset, uint16_t data,
                       uint16_t mem_mask) {
  auto *storage = static_cast<gnet_storage *>(opaque);
  DepthGuard guard(storage->callback_depth);
  const auto *ops = gnet_pccard_card_ops(storage->card);
  ops->write_memory(ops->opaque, offset, data, mem_mask);
}

uint16_t card_read_reg(void *opaque, uint32_t offset, uint16_t mem_mask) {
  auto *storage = static_cast<gnet_storage *>(opaque);
  DepthGuard guard(storage->callback_depth);
  const auto *ops = gnet_pccard_card_ops(storage->card);
  return ops->read_reg(ops->opaque, offset, mem_mask);
}

void card_write_reg(void *opaque, uint32_t offset, uint16_t data,
                    uint16_t mem_mask) {
  auto *storage = static_cast<gnet_storage *>(opaque);
  DepthGuard guard(storage->callback_depth);
  const auto *ops = gnet_pccard_card_ops(storage->card);
  ops->write_reg(ops->opaque, offset, data, mem_mask);
}

void card_reset(void *opaque) {
  auto *storage = static_cast<gnet_storage *>(opaque);
  DepthGuard guard(storage->callback_depth);
  (void)gnet_pccard_reset(storage->card);
}

}  // namespace

extern "C" int gnet_storage_open(const gnet_storage_config_t *config,
                                  gnet_storage_t **out, char *error,
                                  size_t error_capacity) {
  if (out) *out = nullptr;
  if (error && error_capacity) error[0] = '\0';
  if (!config || !out || !config->chd_path || !*config->chd_path ||
      config->card_variant < GNET_PCCARD_ATA_FLASH ||
      config->card_variant > GNET_PCCARD_TAITO_COMPACT_FLASH ||
      (config->expected_sha1_size &&
       (!config->expected_sha1 || config->expected_sha1_size != 20)) ||
      (!config->expected_sha1 && config->expected_sha1_size)) {
    set_error(error, error_capacity, "invalid G-Net storage configuration");
    return GNET_STORAGE_ARGUMENT;
  }

  auto *storage = new (std::nothrow) gnet_storage;
  if (!storage) {
    set_error(error, error_capacity, "out of memory");
    return GNET_STORAGE_NOMEM;
  }
  auto fail = [&](int result, const char *message) {
    set_error(error, error_capacity, message);
    delete storage;
    return result;
  };

  char chd_error[256]{};
  storage->disk = arcade_chd_hd_open(config->chd_path, config->overlay_path,
                                     chd_error, sizeof(chd_error));
  if (!storage->disk)
    return fail(GNET_STORAGE_CHD, *chd_error ? chd_error : "cannot open CHD");
  arcade_chd_hd_get_info(storage->disk, &storage->disk_info);
  if (config->expected_sha1_size &&
      std::memcmp(config->expected_sha1, storage->disk_info.sha1, 20) != 0)
    return fail(GNET_STORAGE_HASH_MISMATCH, "CHD SHA1 does not match expected media");

  const auto &geometry = storage->disk_info;
  const uint64_t chs = uint64_t(geometry.cylinders) * geometry.heads *
                       geometry.sectors_per_track;
  if (geometry.sector_bytes != 512 || !geometry.cylinders ||
      geometry.cylinders > UINT16_MAX || !geometry.heads ||
      geometry.heads > UINT8_MAX || !geometry.sectors_per_track ||
      geometry.sectors_per_track > UINT8_MAX || !geometry.sector_count ||
      chs > INT32_MAX || chs != geometry.sector_count)
    return fail(GNET_STORAGE_GEOMETRY,
                "CHD geometry is not supported by G-Net ATA (512-byte sectors, bounded CHS)");

  bool cis_present = false, key_present = false, identify_present = false;
  int meta = read_metadata(storage->disk, ARCADE_CHD_HD_METADATA_GNET_CIS,
                           &storage->cis, &cis_present,
                           &storage->info.metadata_cis_bytes);
  if (meta == -2) return fail(GNET_STORAGE_NOMEM, "out of memory reading CIS metadata");
  if (meta) return fail(GNET_STORAGE_METADATA, "cannot read CIS metadata");
  meta = read_metadata(storage->disk, ARCADE_CHD_HD_METADATA_GNET_KEY,
                       &storage->key, &key_present,
                       &storage->info.metadata_key_bytes);
  if (meta == -2) return fail(GNET_STORAGE_NOMEM, "out of memory reading KEY metadata");
  if (meta) return fail(GNET_STORAGE_METADATA, "cannot read KEY metadata");
  meta = read_metadata(storage->disk, ARCADE_CHD_HD_METADATA_ATA_IDENTIFY,
                       &storage->identify, &identify_present,
                       &storage->info.metadata_identify_bytes);
  if (meta == -2) return fail(GNET_STORAGE_NOMEM, "out of memory reading IDNT metadata");
  if (meta) return fail(GNET_STORAGE_METADATA, "cannot read IDNT metadata");
  storage->info.cylinders = geometry.cylinders;
  storage->info.heads = geometry.heads;
  storage->info.sectors_per_track = geometry.sectors_per_track;
  storage->info.sector_count = geometry.sector_count;
  std::copy(std::begin(geometry.sha1), std::end(geometry.sha1),
            std::begin(storage->info.sha1));
  storage->info.metadata_cis_present = cis_present;
  storage->info.metadata_key_present = key_present;
  storage->info.metadata_identify_present = identify_present;

  ata_storage_config_t ata_config{};
  ata_config.sector_count = geometry.sector_count;
  ata_config.cylinders = static_cast<uint16_t>(geometry.cylinders);
  ata_config.heads = static_cast<uint8_t>(geometry.heads);
  ata_config.sectors_per_track = static_cast<uint8_t>(geometry.sectors_per_track);
  ata_config.dma_transfer_time_ns = config->dma_transfer_time_ns;
  ata_config.model = "Taito G-Net PC Card";
  if (identify_present && storage->identify.size() == 512) {
    ata_config.identify_data = storage->identify.data();
    ata_config.identify_size = storage->identify.size();
  }
  ata_config.media_opaque = storage;
  ata_config.read_sector = media_read;
  ata_config.write_sector = media_write;
  if (ata_storage_create(&ata_config, &storage->ata) != ATA_STORAGE_OK)
    return fail(GNET_STORAGE_ATA, "cannot construct source ATA HLE engine");

  static const uint8_t empty_cis = 0;
  gnet_pccard_config_t card_config{};
  card_config.variant = config->card_variant;
  if (cis_present) {
    card_config.cis = storage->cis.empty() ? &empty_cis : storage->cis.data();
    card_config.cis_size = storage->cis.size();
  }
  card_config.key = storage->key.empty() ? nullptr : storage->key.data();
  card_config.key_size = key_present ? storage->key.size() : 0;
  card_config.ata = ata_storage_gnet_pccard_ops(storage->ata);
  if (gnet_pccard_create(&card_config, &storage->card) != GNET_PCCARD_OK)
    return fail(GNET_STORAGE_NOMEM, "cannot construct G-Net PC Card");
  if (ata_storage_set_pccard(storage->ata, storage->card) != ATA_STORAGE_OK)
    return fail(GNET_STORAGE_ATA, "cannot attach PC Card to ATA engine");

  storage->rf_ops = {storage, card_read_memory, card_write_memory,
                     card_read_reg, card_write_reg, card_reset};
  *out = storage;
  return GNET_STORAGE_OK;
}

extern "C" const rf5c296_card_ops_t *
gnet_storage_card_ops(gnet_storage_t *storage) {
  return storage ? &storage->rf_ops : nullptr;
}

extern "C" int gnet_storage_run_until(gnet_storage_t *storage,
                                       uint64_t monotonic_ns) {
  if (!storage) return GNET_STORAGE_ARGUMENT;
  if (storage->callback_depth || storage->operation_depth)
    return GNET_STORAGE_BUSY;
  DepthGuard guard(storage->operation_depth);
  const int result = ata_storage_run_until(storage->ata, monotonic_ns);
  return result == ATA_STORAGE_OK ? GNET_STORAGE_OK :
         result == ATA_STORAGE_BUSY ? GNET_STORAGE_BUSY : GNET_STORAGE_ARGUMENT;
}

extern "C" uint64_t gnet_storage_now(const gnet_storage_t *storage) {
  return storage ? ata_storage_now(storage->ata) : 0;
}

extern "C" int gnet_storage_reset(gnet_storage_t *storage) {
  if (!storage) return GNET_STORAGE_ARGUMENT;
  if (storage->callback_depth || storage->operation_depth)
    return GNET_STORAGE_BUSY;
  DepthGuard guard(storage->operation_depth);
  const int result = gnet_pccard_reset(storage->card);
  return result == GNET_PCCARD_OK ? GNET_STORAGE_OK : GNET_STORAGE_ATA;
}

extern "C" int gnet_storage_flush(gnet_storage_t *storage) {
  if (!storage) return GNET_STORAGE_ARGUMENT;
  if (storage->callback_depth || storage->operation_depth)
    return GNET_STORAGE_BUSY;
  DepthGuard guard(storage->operation_depth);
  return arcade_chd_hd_flush(storage->disk) == 0 ? GNET_STORAGE_OK :
                                                   GNET_STORAGE_CHD;
}

extern "C" void gnet_storage_get_info(const gnet_storage_t *storage,
                                        gnet_storage_info_t *info) {
  if (info) *info = storage ? storage->info : gnet_storage_info_t{};
}

extern "C" int gnet_storage_is_locked(const gnet_storage_t *storage) {
  return storage ? gnet_pccard_is_locked(storage->card) : 0;
}

extern "C" uint8_t gnet_storage_ata_status(const gnet_storage_t *storage) {
  return storage ? ata_storage_status(storage->ata) : 0;
}

extern "C" int gnet_storage_try_destroy(gnet_storage_t *storage) {
  if (!storage) return GNET_STORAGE_ARGUMENT;
  if (storage->callback_depth || storage->operation_depth)
    return GNET_STORAGE_BUSY;
  if (storage->ata && storage->card)
    (void)ata_storage_set_pccard(storage->ata, nullptr);
  if (storage->card) {
    const int result = gnet_pccard_try_destroy(storage->card);
    if (result != GNET_PCCARD_OK) return GNET_STORAGE_BUSY;
    storage->card = nullptr;
  }
  if (storage->ata) {
    const int result = ata_storage_try_destroy(storage->ata);
    if (result == ATA_STORAGE_BUSY) return GNET_STORAGE_BUSY;
    if (result != ATA_STORAGE_OK) return GNET_STORAGE_ATA;
    storage->ata = nullptr;
  }
  delete storage;
  return GNET_STORAGE_OK;
}

extern "C" void gnet_storage_destroy(gnet_storage_t *storage) {
  if (storage) (void)gnet_storage_try_destroy(storage);
}
