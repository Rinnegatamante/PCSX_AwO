/* license:BSD-3-Clause
 * copyright-holders:Olivier Galibert
 * Standalone C++ adapter based on MAME hd.cpp and nscsi_hle.cpp at
 * deb2f54fe7300e7a192690318beb860159bfd52a.
 */
#include "nscsi_hd.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

namespace {
constexpr unsigned kMainBuffer = 0;
constexpr unsigned kSenseBuffer = 1;
constexpr unsigned kDiskBuffer = 2;
constexpr size_t kMaxSectorBytes = 512;
constexpr size_t kCommandBufferBytes = 4096;
constexpr uint8_t kGood = NSCSI_STATUS_GOOD;
constexpr uint8_t kCheck = NSCSI_STATUS_CHECK_CONDITION;

uint32_t read_u24be(const uint8_t *p) {
  return (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
}
uint32_t read_u32be(const uint8_t *p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
         (uint32_t(p[2]) << 8) | p[3];
}
uint16_t read_u16be(const uint8_t *p) {
  return uint16_t((uint16_t(p[0]) << 8) | p[1]);
}
void write_u16be(uint8_t *p, uint16_t v) {
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v);
}
void write_u24be(uint8_t *p, uint32_t v) {
  p[0] = uint8_t(v >> 16);
  p[1] = uint8_t(v >> 8);
  p[2] = uint8_t(v);
}
void write_u32be(uint8_t *p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}
} // namespace

struct nscsi_hd {
  NscsiTarget *target = nullptr;
  nscsi_hd_config_t config{};
  uint64_t blocks = 0;
  uint64_t commands = 0;
  uint64_t read_failures = 0;
  uint64_t write_failures = 0;
  uint32_t faults = 0;
  uint32_t lba = 0;
  int64_t current_lba = -1;
  std::array<uint8_t, kMaxSectorBytes> block{};
  std::array<uint8_t, kCommandBufferBytes> command_data{};
  std::array<uint8_t, 18> sense{};
  std::array<uint8_t, 48> inquiry{};
  size_t inquiry_bytes = 0;
  bool block_known = false;
  bool busy = false;
};

namespace {
void set_sense(nscsi_hd_t *d, uint8_t key, uint8_t asc = 0, uint8_t ascq = 0) {
  d->sense.fill(0);
  /* This is MAME's fixed-format sense construction, including its 0xf0
   * response byte (valid bit plus 0x70 response code). */
  d->sense[0] = 0xf0;
  d->sense[2] = key & 0x0f;
  d->sense[7] = 10;
  d->sense[12] = asc;
  d->sense[13] = ascq;
}

void complete(nscsi_hd_t *d, NscsiTarget *target, uint8_t status) {
  /* status_complete queues STATUS, COMMAND COMPLETE, and BUS FREE in the
   * existing target engine, matching nscsi_full_device::scsi_status_complete.
   */
  if (!nscsi_target_status_complete(target, status))
    d->faults |= NSCSI_HD_FAULT_BACKEND;
}

void check_condition(nscsi_hd_t *d, NscsiTarget *target, uint8_t key,
                     uint8_t asc = 0, uint8_t ascq = 0) {
  complete(d, target, kCheck);
  set_sense(d, key, asc, ascq);
}

void data_in_then_good(nscsi_hd_t *d, NscsiTarget *target, unsigned buffer,
                       size_t bytes) {
  if (!nscsi_target_data_in(target, buffer, bytes)) {
    d->faults |= NSCSI_HD_FAULT_BACKEND;
    check_condition(d, target, 0x05, 0x24);
    return;
  }
  complete(d, target, kGood);
}

void data_out_then_good(nscsi_hd_t *d, NscsiTarget *target, unsigned buffer,
                        size_t bytes) {
  if (!nscsi_target_data_out(target, buffer, bytes)) {
    d->faults |= NSCSI_HD_FAULT_BACKEND;
    check_condition(d, target, 0x05, 0x24);
    return;
  }
  complete(d, target, kGood);
}

bool valid_lun(nscsi_hd_t *d, NscsiTarget *target, const uint8_t *cdb) {
  nscsi_target_status_t status{};
  nscsi_target_get_status(target, &status);
  const unsigned lun =
      (status.identify & 0x80) ? (status.identify & 7) : (cdb[1] >> 5);
  return lun == 0;
}

bool read_sector(nscsi_hd_t *d, uint32_t lba, bool zero_on_failure = false) {
  if (lba >= d->blocks || !d->config.read_sector ||
      !d->config.read_sector(d->config.media_opaque, lba, d->block.data(),
                             d->config.sector_bytes)) {
    ++d->read_failures;
    d->faults |= NSCSI_HD_FAULT_MEDIA_READ;
    if (zero_on_failure) d->block.fill(0);
    d->block_known = false;
    return false;
  }
  d->block_known = true;
  return true;
}

bool write_sector(nscsi_hd_t *d, uint32_t lba, const uint8_t *sector) {
  if (lba >= d->blocks || !d->config.write_sector ||
      !d->config.write_sector(d->config.media_opaque, lba, sector,
                              d->config.sector_bytes)) {
    ++d->write_failures;
    d->faults |= NSCSI_HD_FAULT_MEDIA_WRITE;
    return false;
  }
  return true;
}

void build_inquiry(nscsi_hd_t *d, uint8_t lun) {
  d->command_data.fill(0);
  std::fill_n(d->command_data.begin() + 8, 28, uint8_t(0x20));
  d->command_data[0] = lun ? 0x7f : 0x00;
  d->command_data[1] = 0x00;
  d->command_data[2] = 0x05;
  d->command_data[3] = 0x01;
  d->command_data[4] = 52;
  if (d->inquiry_bytes) {
    std::copy_n(d->inquiry.begin(), d->inquiry_bytes,
                d->command_data.begin() + 8);
  } else {
    static constexpr char kDefaultIdentity[] = " SEAGATE          ST225N1.00";
    std::copy_n(kDefaultIdentity, 28, d->command_data.begin() + 8);
    d->command_data[36] = 0x00;
    d->command_data[37] = 0x08;
    d->command_data[38] = 0x00;
    d->command_data[39] = 0x99;
    d->command_data[40] = 0xa0;
    d->command_data[41] = 0x27;
    d->command_data[42] = 0x34;
    d->command_data[43] = 0x01;
    d->command_data[44] = 0x04;
    d->command_data[45] = 0xa0;
    d->command_data[46] = 0x01;
    d->command_data[47] = 0x18;
    d->command_data[48] = 0x07;
    d->command_data[49] = 0x00;
    d->command_data[50] = 0xa0;
    d->command_data[51] = 0x00;
    d->command_data[52] = 0x00;
    d->command_data[53] = 0xff;
  }
}

size_t append_mode_page(nscsi_hd_t *d, size_t pos, unsigned page) {
  auto *out = d->command_data.data();
  switch (page) {
  case 0x00:
    out[pos++] = 0x80;
    out[pos++] = 0x02;
    out[pos++] = 0;
    out[pos++] = 0;
    break;
  case 0x01: {
    static constexpr uint8_t page_data[12] = {0x01, 0x0a, 0x26, 0, 0, 0,
                                              0,    0,    0,    0, 0, 0};
    std::copy(std::begin(page_data), std::end(page_data), out + pos);
    pos += sizeof(page_data);
    break;
  }
  case 0x02: {
    static constexpr uint8_t page_data[16] = {0x02, 0x0e, 0, 0, 0, 0, 0, 0,
                                              0,    0,    0, 0, 0, 0, 0, 0};
    std::copy(std::begin(page_data), std::end(page_data), out + pos);
    pos += sizeof(page_data);
    break;
  }
  case 0x03:
    out[pos++] = 0x83;
    out[pos++] = 0x16;
    write_u16be(out + pos, uint16_t(d->config.cylinders * d->config.heads));
    pos += 2;
    pos += 6;
    write_u16be(out + pos, uint16_t(d->config.sectors_per_track));
    pos += 2;
    write_u16be(out + pos, uint16_t(d->config.sector_bytes));
    pos += 2;
    pos += 10;
    break;
  case 0x04:
    out[pos++] = 0x84;
    out[pos++] = 0x16;
    write_u24be(out + pos, d->config.cylinders);
    pos += 3;
    out[pos++] = uint8_t(d->config.heads);
    pos += 14;
    write_u16be(out + pos, 10000);
    pos += 2;
    pos += 2;
    break;
  case 0x08:
    out[pos++] = 0x08;
    out[pos++] = 0x0a;
    pos += 10;
    break;
  case 0x30: {
    static constexpr char page_data[] = "APPLE COMPUTER, INC   ";
    out[pos++] = 0xb0;
    out[pos++] = 0x16;
    std::copy_n(page_data, 22, out + pos);
    pos += 22;
    break;
  }
  default:
    break;
  }
  return pos;
}

size_t build_mode_sense(nscsi_hd_t *d, unsigned page) {
  auto *out = d->command_data.data();
  d->command_data.fill(0);
  size_t pos = 1;
  out[pos++] = 0x00;
  out[pos++] = 0x00;
  out[pos++] = 0x08;
  out[pos++] = 0x00;
  write_u24be(out + pos, uint32_t(d->blocks - 1));
  pos += 3;
  out[pos++] = 0;
  write_u24be(out + pos, d->config.sector_bytes);
  pos += 3;

  const unsigned max_page = page == 0x3f ? 0x3e : page;
  const unsigned min_page = page == 0x3f ? 0 : page;
  for (int current = int(max_page); current >= int(min_page); --current) {
    switch (current) {
    case 0x00:
    case 0x01:
    case 0x02:
    case 0x03:
    case 0x04:
    case 0x08:
    case 0x30:
      pos = append_mode_page(d, pos, unsigned(current));
      break;
    default:
      break;
    }
  }
  out[0] = uint8_t(pos - 1);
  return pos;
}

void command(nscsi_hd_t *d, NscsiTarget *target, const uint8_t *cdb,
             size_t length) {
  if (!d)
    return;
  if (d->busy) {
    d->faults |= NSCSI_HD_FAULT_REENTRANT;
    return;
  }
  d->busy = true;
  ++d->commands;
  const uint8_t opcode = cdb[0];
  const unsigned lun = (length > 1 && !valid_lun(d, target, cdb)) ? 1 : 0;
  switch (opcode) {
  case 0x00: // TEST UNIT READY
    if (lun)
      check_condition(d, target, 0x05, 0x25);
    else
      complete(d, target, kGood);
    break;
  case 0x01: // REZERO UNIT, handled by nscsi_full_device
    complete(d, target, kGood);
    break;
  case 0x03: // REQUEST SENSE
    if ((cdb[1] & 0x1f) || cdb[2] || cdb[3]) {
      check_condition(d, target, 0x05, 0x24);
      break;
    }
    data_in_then_good(d, target, kSenseBuffer, std::min<size_t>(18, cdb[4]));
    break;
  case 0x08: { // READ (6)
    d->lba = read_u24be(cdb + 1) & 0x1fffff;
    const uint32_t count = cdb[4] ? cdb[4] : 256;
    if (!read_sector(d, d->lba)) {
      check_condition(d, target, 0x05, 0x24);
      break;
    }
    data_in_then_good(d, target, kDiskBuffer,
                      size_t(count) * d->config.sector_bytes);
    break;
  }
  case 0x0a: { // WRITE (6)
    d->lba = read_u24be(cdb + 1) & 0x1fffff;
    const uint32_t count = cdb[4] ? cdb[4] : 256;
    if (!d->block_known)
      d->faults |= NSCSI_HD_FAULT_UNINITIALIZED_PREFLIGHT;
    if (!write_sector(d, d->lba, d->block.data())) {
      check_condition(d, target, 0x05, 0x24);
      break;
    }
    data_out_then_good(d, target, kDiskBuffer,
                       size_t(count) * d->config.sector_bytes);
    break;
  }
  case 0x12: { // INQUIRY, page 0 only (source returns GOOD/no data otherwise)
    build_inquiry(d, lun);
    if (cdb[2] == 0)
      data_in_then_good(d, target, kMainBuffer, std::min<size_t>(56, cdb[4]));
    else
      complete(d, target, kGood);
    break;
  }
  case 0x1a: { // MODE SENSE (6)
    if (lun) {
      check_condition(d, target, 0x05, 0x25);
      break;
    }
    const unsigned page = cdb[2] & 0x3f;
    const bool supported = page == 0x3f || page == 0x00 || page == 0x01 ||
                           page == 0x02 || page == 0x03 || page == 0x04 ||
                           page == 0x08 || page == 0x30;
    if (!supported) {
      check_condition(d, target, 0x05, 0x24);
      break;
    }
    const size_t total = build_mode_sense(d, page);
    data_in_then_good(d, target, kMainBuffer, std::min<size_t>(total, cdb[4]));
    break;
  }
  case 0x1b: // START STOP UNIT
  case 0x35: // SYNCHRONIZE CACHE (10)
    complete(d, target, kGood);
    break;
  case 0x1c: { // RECEIVE DIAGNOSTIC RESULTS
    const size_t requested = read_u16be(cdb + 3);
    d->command_data.fill(0);
    d->command_data[1] = 6;
    data_in_then_good(d, target, kMainBuffer, std::min<size_t>(requested, 7));
    break;
  }
  case 0x1d: { // SEND DIAGNOSTIC
    if (cdb[1] & 4) {
      complete(d, target, kGood);
      break;
    }
    const size_t requested = read_u16be(cdb + 3);
    d->command_data.fill(0);
    d->command_data[1] = 6;
    data_in_then_good(d, target, kMainBuffer, std::min<size_t>(requested, 8));
    break;
  }
  case 0x04: // FORMAT UNIT
  {
    std::array<uint8_t, kMaxSectorBytes> zero_sector{};
    for (uint32_t cylinder = 0; cylinder < d->config.cylinders; ++cylinder)
      for (uint32_t head = 0; head < d->config.heads; ++head)
        for (uint32_t sector = 0; sector < d->config.sectors_per_track;
             ++sector) {
          /* Preserve source expression `cyl * head * sector` (not a CHS to
           * LBA conversion) and its zero-filled temporary sector. */
          const uint64_t source_lba = uint64_t(cylinder) * head * sector;
          (void)write_sector(d, uint32_t(source_lba), zero_sector.data());
        }
  }
    complete(d, target, kGood);
    break;
  case 0x25: { // READ CAPACITY (10)
    write_u32be(d->command_data.data(), uint32_t(d->blocks - 1));
    write_u32be(d->command_data.data() + 4, d->config.sector_bytes);
    data_in_then_good(d, target, kMainBuffer, 8);
    break;
  }
  case 0x28: { // READ (10)
    d->lba = read_u32be(cdb + 2);
    const uint32_t count = read_u16be(cdb + 7);
    if (!read_sector(d, d->lba)) {
      check_condition(d, target, 0x05, 0x24);
      break;
    }
    data_in_then_good(d, target, kDiskBuffer,
                      size_t(count) * d->config.sector_bytes);
    break;
  }
  case 0x2a: { // WRITE (10)
    d->lba = read_u32be(cdb + 2);
    const uint32_t count = read_u16be(cdb + 7);
    if (!d->block_known)
      d->faults |= NSCSI_HD_FAULT_UNINITIALIZED_PREFLIGHT;
    if (!write_sector(d, d->lba, d->block.data())) {
      check_condition(d, target, 0x05, 0x24);
      break;
    }
    data_out_then_good(d, target, kDiskBuffer,
                       size_t(count) * d->config.sector_bytes);
    break;
  }
  case 0x2f: // VERIFY (10); BYTCHK set falls through source unknown handler
    if (!(cdb[1] & 2))
      complete(d, target, kGood);
    else
      check_condition(d, target, 0x05);
    break;
  case 0x15: // MODE SELECT (6), source accepts the parameter data without use
    data_out_then_good(d, target, kDiskBuffer, cdb[4]);
    break;
  default:
    check_condition(d, target, 0x05);
    break;
  }
  d->busy = false;
}

uint8_t read_data(void *opaque, unsigned buffer, size_t offset) {
  auto *d = static_cast<nscsi_hd_t *>(opaque);
  if (!d || d->busy) {
    if (d)
      d->faults |= NSCSI_HD_FAULT_REENTRANT;
    return 0;
  }
  d->busy = true;
  uint8_t result = 0;
  if (buffer == kMainBuffer) {
    if (offset < d->command_data.size())
      result = d->command_data[offset];
  } else if (buffer == kSenseBuffer) {
    if (offset < d->sense.size())
      result = d->sense[offset];
  } else if (buffer == kDiskBuffer) {
    const uint64_t lba = uint64_t(d->lba) + offset / d->config.sector_bytes;
    const size_t byte_offset = offset % d->config.sector_bytes;
    if (lba < d->blocks) {
      if (int64_t(lba) != d->current_lba) {
        d->current_lba = int64_t(lba);
        (void)read_sector(d, uint32_t(lba), true);
      }
      result = d->block[byte_offset];
    } else {
      ++d->read_failures;
      d->faults |= NSCSI_HD_FAULT_MEDIA_READ;
    }
  } else {
    d->faults |= NSCSI_HD_FAULT_BACKEND;
  }
  d->busy = false;
  return result;
}

void write_data(void *opaque, unsigned buffer, size_t offset, uint8_t value) {
  auto *d = static_cast<nscsi_hd_t *>(opaque);
  if (!d || d->busy) {
    if (d)
      d->faults |= NSCSI_HD_FAULT_REENTRANT;
    return;
  }
  d->busy = true;
  if (buffer != kDiskBuffer) {
    d->faults |= NSCSI_HD_FAULT_BACKEND;
  } else {
    const size_t byte_offset = offset % d->config.sector_bytes;
    const uint64_t lba = uint64_t(d->lba) + offset / d->config.sector_bytes;
    d->block[byte_offset] = value;
    d->current_lba = int64_t(lba);
    if (byte_offset == d->config.sector_bytes - 1) {
      d->block_known = true;
      if (lba < d->blocks) (void)write_sector(d, uint32_t(lba), d->block.data());
    }
  }
  d->busy = false;
}

nscsi_target_backend_t make_backend(nscsi_hd_t *d) {
  nscsi_target_backend_t backend{};
  backend.opaque = d;
  backend.command = [](void *opaque, NscsiTarget *target, const uint8_t *cdb,
                       size_t length) {
    command(static_cast<nscsi_hd_t *>(opaque), target, cdb, length);
  };
  backend.read_data = read_data;
  backend.write_data = write_data;
  return backend;
}
} // namespace

extern "C" nscsi_hd_t *nscsi_hd_create(NscsiBus *bus, unsigned scsi_id,
                                       const nscsi_hd_config_t *config) {
  if (!bus || !config || !config->media_present)
    return nullptr;
  if (!config->read_sector || !config->cylinders || !config->heads ||
      !config->sectors_per_track || !config->sector_bytes ||
      config->sector_bytes > kMaxSectorBytes ||
      config->inquiry_data_bytes > 48 ||
      (config->inquiry_data_bytes && !config->inquiry_data))
    return nullptr;
  const uint64_t max_blocks =
      uint64_t(std::numeric_limits<uint32_t>::max()) + 1;
  const uint64_t cylinders = config->cylinders;
  const uint64_t heads = config->heads;
  const uint64_t sectors = config->sectors_per_track;
  if (cylinders > max_blocks / heads)
    return nullptr;
  const uint64_t cylinder_heads = cylinders * heads;
  if (cylinder_heads > max_blocks / sectors)
    return nullptr;
  const uint64_t blocks = cylinder_heads * sectors;

  auto *d = new (std::nothrow) nscsi_hd;
  if (!d)
    return nullptr;
  d->config = *config;
  d->blocks = blocks;
  d->inquiry_bytes = config->inquiry_data_bytes;
  if (d->inquiry_bytes)
    std::copy_n(config->inquiry_data, d->inquiry_bytes, d->inquiry.begin());
  set_sense(d, 0);
  const auto backend = make_backend(d);
  d->target = nscsi_target_create(bus, scsi_id, &backend);
  if (!d->target) {
    delete d;
    return nullptr;
  }
  return d;
}

extern "C" void nscsi_hd_destroy(nscsi_hd_t *d) {
  if (!d)
    return;
  if (d->busy) {
    d->faults |= NSCSI_HD_FAULT_REENTRANT;
    return;
  }
  nscsi_target_destroy(d->target);
  delete d;
}

extern "C" NscsiTarget *nscsi_hd_target(nscsi_hd_t *d) {
  return d ? d->target : nullptr;
}

extern "C" void nscsi_hd_advance_to(nscsi_hd_t *d, uint64_t now_ns) {
  if (!d)
    return;
  if (d->busy) {
    d->faults |= NSCSI_HD_FAULT_REENTRANT;
    return;
  }
  nscsi_target_advance_to(d->target, now_ns);
}

extern "C" void nscsi_hd_reset(nscsi_hd_t *d) {
  if (!d)
    return;
  if (d->busy) {
    d->faults |= NSCSI_HD_FAULT_REENTRANT;
    return;
  }
  nscsi_target_reset(d->target);
  d->current_lba = -1;
  set_sense(d, 0);
}

extern "C" void nscsi_hd_get_status(const nscsi_hd_t *d,
                                    nscsi_hd_status_t *status) {
  if (!d || !status)
    return;
  status->faults = d->faults;
  status->commands = d->commands;
  status->read_failures = d->read_failures;
  status->write_failures = d->write_failures;
  status->last_lba = d->lba;
  status->block_known = d->block_known;
  std::copy(d->sense.begin(), d->sense.end(), status->last_sense);
}
