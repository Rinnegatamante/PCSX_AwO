/* MAME-derived NSCSI CD-ROM target, restricted to the common device base and
 * Toshiba XM-5401 identity.  BSD-3-Clause; see nscsi_cd_LICENSE.txt.
 */
#include "nscsi_cd.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

namespace {
constexpr unsigned BUF_MAIN = 0;
constexpr unsigned BUF_SENSE = 1;
constexpr unsigned BUF_SECTOR = 2;
constexpr unsigned LEAD_OUT = 0xaa;
constexpr uint8_t STATUS_GOOD = NSCSI_STATUS_GOOD;
constexpr uint8_t STATUS_CHECK = NSCSI_STATUS_CHECK_CONDITION;
constexpr uint8_t SK_NO_SENSE = 0x00;
constexpr uint8_t SK_NOT_READY = 0x02;
constexpr uint8_t SK_ILLEGAL_REQUEST = 0x05;
constexpr uint8_t SK_UNIT_ATTENTION = 0x06;
constexpr uint8_t ASC_INVALID_FIELD_CDB = 0x24;
constexpr uint8_t ASC_MEDIUM_NOT_PRESENT = 0x3a;
constexpr uint8_t ASC_READ_ERROR = 0x11;
constexpr uint8_t ASC_INVALID_TRACK = 0x14;
constexpr uint8_t ASC_AUDIO_NOT_PLAYABLE = 0x64;
constexpr size_t SENSE_BYTES = 18;
constexpr size_t RESPONSE_LIMIT = NSCSI_CD_MAX_BUFFER;

uint32_t be24(const uint8_t *p) {
  return (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
}
uint32_t be32(const uint8_t *p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
         (uint32_t(p[2]) << 8) | p[3];
}
uint16_t be16(const uint8_t *p) {
  return uint16_t((uint16_t(p[0]) << 8) | p[1]);
}
void put_be16(uint8_t *p, uint16_t v) {
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v);
}
void put_be32(uint8_t *p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}
uint32_t to_msf(uint32_t frame) {
  const uint32_t minutes = frame / (75 * 60);
  const uint32_t seconds = (frame / 75) % 60;
  const uint32_t frames = frame % 75;
  return (minutes << 16) | (seconds << 8) | frames;
}
} // namespace

struct NscsiCdrom {
  NscsiTarget *target = nullptr;
  nscsi_cd_media_ops_t media{};
  uint32_t block_size = NSCSI_CD_SECTOR_BYTES;
  uint32_t faults = 0;
  uint64_t sequence = 0;
  uint32_t lba = 0;
  uint32_t cur_sector = UINT32_MAX;
  std::array<uint8_t, NSCSI_CD_SECTOR_BYTES> sector{};
  std::array<uint8_t, 256> mode_data{};
  uint8_t mode_data_size = 12;
  std::array<uint8_t, SENSE_BYTES> sense{};
  std::vector<uint8_t> response;
  bool removal_prevented = false;
  bool cdda_sotc = false;
  bool in_callback = false;
  bool mode_write_active = false;
  uint8_t inquiry_data = 0x98;
  uint8_t compliance = 0x02;
  std::array<uint8_t, 8> manufacturer{'T', 'O', 'S', 'H', 'I', 'B', 'A', ' '};
  std::array<uint8_t, 16> product{'C', 'D', '-', 'R', 'O', 'M', ' ', 'X',
                                  'M', '-', '5', '4', '0', '1', 'T', 'A'};
  std::array<uint8_t, 4> revision{'3', '6', '0', '5'};
};

namespace {
void set_sense(NscsiCdrom *cd, uint8_t key, uint8_t asc = 0, uint8_t ascq = 0) {
  cd->sense.fill(0);
  cd->sense[0] = 0x70;
  cd->sense[2] = key & 0x0f;
  cd->sense[7] = 10;
  cd->sense[12] = asc;
  cd->sense[13] = ascq;
}
void clear_response(NscsiCdrom *cd) { cd->response.clear(); }
void queue_status(NscsiCdrom *cd, uint8_t status) {
  if (!nscsi_target_status_complete(cd->target, status))
    cd->faults |= NSCSI_CD_FAULT_BACKEND;
}
void good(NscsiCdrom *cd) { queue_status(cd, STATUS_GOOD); }
void check(NscsiCdrom *cd, uint8_t key, uint8_t asc = 0, uint8_t ascq = 0) {
  set_sense(cd, key, asc, ascq);
  queue_status(cd, STATUS_CHECK);
}
void no_medium(NscsiCdrom *cd) {
  check(cd, SK_NOT_READY, ASC_MEDIUM_NOT_PRESENT);
}
bool present(NscsiCdrom *cd) {
  return cd->media.present(cd->media.opaque) != 0;
}
unsigned get_lun(NscsiCdrom *cd, uint8_t cdb_lun) {
  nscsi_target_status_t target{};
  nscsi_target_get_status(cd->target, &target);
  if (target.identify & 0x80)
    return target.identify & 7;
  return (cdb_lun >> 5) & 7;
}
void data_in(NscsiCdrom *cd, unsigned id, size_t size) {
  if (id != BUF_SECTOR && size > RESPONSE_LIMIT) {
    check(cd, SK_ILLEGAL_REQUEST, ASC_INVALID_FIELD_CDB);
    return;
  }
  if (!nscsi_target_data_in(cd->target, id, size))
    cd->faults |= NSCSI_CD_FAULT_BACKEND;
  good(cd);
}
void response_in(NscsiCdrom *cd, std::vector<uint8_t> data) {
  cd->response = std::move(data);
  data_in(cd, BUF_MAIN, cd->response.size());
}
void response_in_limit(NscsiCdrom *cd, std::vector<uint8_t> data,
                       size_t limit) {
  data.resize(std::min(data.size(), limit));
  response_in(cd, std::move(data));
}
void get_audio_status(NscsiCdrom *cd, nscsi_cd_audio_status_t *status) {
  *status = {};
  cd->media.cdda_status(cd->media.opaque, status);
}
void finish_mode_select(NscsiCdrom *cd) {
  const size_t size = cd->mode_data_size;
  if (size >= 12 && cd->mode_data[3] == 8) {
    const uint32_t block = be24(&cd->mode_data[9]);
    if (block && NSCSI_CD_SECTOR_BYTES % block == 0)
      cd->block_size = block;
    else cd->faults |= NSCSI_CD_FAULT_MALFORMED_MODE;
  }
  const size_t page_offset = 4 + cd->mode_data[3];
  if (page_offset < size && (cd->mode_data[page_offset] & 0x3f) == 0x0e &&
      page_offset + 15 < size) {
    cd->cdda_sotc = (cd->mode_data[page_offset + 2] & 0x02) != 0;
    cd->media.cdda_set_gain(cd->media.opaque, 0,
                            cd->mode_data[page_offset + 9] / 255.0f);
    cd->media.cdda_set_gain(cd->media.opaque, 1,
                            cd->mode_data[page_offset + 11] / 255.0f);
  }
}

std::vector<uint8_t> inquiry_data(NscsiCdrom *cd, unsigned lun, size_t size) {
  std::vector<uint8_t> out(size, 0);
  if (!size)
    return out;
  out[0] = lun ? 0x7f : 0x05;
  if (size > 1)
    out[1] = 0x80;
  if (size > 2)
    out[2] = cd->compliance;
  if (size > 3)
    out[3] = 0x02;
  if (size > 4)
    out[4] = 0x20;
  if (size > 7)
    out[7] = cd->inquiry_data;
  for (size_t i = 0; i < cd->manufacturer.size() && 8 + i < size; ++i)
    out[8 + i] = cd->manufacturer[i];
  for (size_t i = 0; i < cd->product.size() && 16 + i < size; ++i)
    out[16 + i] = cd->product[i];
  for (size_t i = 0; i < cd->revision.size() && 32 + i < size; ++i)
    out[32 + i] = cd->revision[i];
  for (size_t i = 8; i < std::min<size_t>(size, 36); ++i)
    if (!out[i])
      out[i] = 0x20;
  return out;
}

void do_mode_sense(NscsiCdrom *cd, const uint8_t *cdb) {
  const unsigned lun = get_lun(cd, cdb[1]);
  if (lun)
    return check(cd, SK_ILLEGAL_REQUEST, 0x25);
  const unsigned page = cdb[2] & 0x3f;
  const size_t requested = cdb[4];
  const bool no_descriptor = (cdb[1] & 0x08) != 0;
  std::vector<uint8_t> out(1, 0);
  out.push_back(0x00); // medium type
  out.push_back(0x80); // write protect / cache
  out.push_back(no_descriptor ? 0 : 8);
  if (!no_descriptor) {
    out.push_back(0); // density
    uint32_t last = 0;
    if (present(cd))
      last = cd->media.track_start(cd->media.opaque, LEAD_OUT) *
                 (NSCSI_CD_SECTOR_BYTES / cd->block_size) -
             1;
    out.push_back(uint8_t(last >> 16));
    out.push_back(uint8_t(last >> 8));
    out.push_back(uint8_t(last));
    out.push_back(0);
    out.push_back(0);
    out.push_back(uint8_t(cd->block_size >> 8));
    out.push_back(uint8_t(cd->block_size));
  }
  bool unsupported = false;
  const int pmin = page == 0x3f ? 0 : int(page);
  const int pmax = page == 0x3f ? 0x3e : int(page);
  static const uint8_t apple_magic[0x17] = {
      0x00, 0x41, 0x50, 0x50, 0x4c, 0x45, 0x20, 0x43, 0x4f, 0x4d, 0x50, 0x55,
      0x54, 0x45, 0x52, 0x2c, 0x20, 0x49, 0x4e, 0x43, 0x20, 0x20, 0x20};
  static const char bluescsi_magic[] =
      "BlueSCSI is the BEST STOLEN FROM BLUESCSI\0";
  for (int p = pmax; p >= pmin; --p) {
    auto append = [&](std::initializer_list<uint8_t> bytes) {
      out.insert(out.end(), bytes.begin(), bytes.end());
    };
    switch (p) {
    case 0x00:
      append({0x80, 0x02, 0x00, 0x00});
      break;
    case 0x01:
      append({0x01, 0x0a, 0x01, 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0});
      break;
    case 0x02:
      append({0x82, 0x0e, 0xe6, 0x1a, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
      break;
    case 0x0e:
      append({0x8e, 0x0e, uint8_t(0x04 | (cd->cdda_sotc ? 0x02 : 0)), 0, 0, 0,
              0, 0, 1, 0xff, 2, 0xff, 4, 0xff, 8, 0xff});
      break;
    case 0x30:
      out.push_back(0x30);
      out.insert(out.end(), std::begin(apple_magic), std::end(apple_magic));
      break;
    case 0x31:
      out.push_back(0x31);
      out.push_back(42);
      out.insert(out.end(), bluescsi_magic, bluescsi_magic + 42);
      break;
    default:
      if (page != 0x3f)
        unsupported = true;
      break;
    }
  }
  out[0] = uint8_t(out.size() - 1);
  if (unsupported)
    return check(cd, SK_ILLEGAL_REQUEST, ASC_INVALID_FIELD_CDB);
  out.resize(std::min(out.size(), requested));
  response_in(cd, std::move(out));
}

void do_read_toc(NscsiCdrom *cd, const uint8_t *cdb) {
  if (!present(cd))
    return no_medium(cd);
  const bool msf = (cdb[1] & 0x02) != 0;
  const size_t allocation = be16(&cdb[7]);
  unsigned format = cdb[2] & 15;
  if (!format)
    format = (cdb[9] >> 6) & 3;
  std::vector<uint8_t> out;
  auto append_desc = [&](unsigned track, unsigned internal) {
    out.push_back(0);
    out.push_back(cd->media.track_adr_control(cd->media.opaque, internal));
    out.push_back(uint8_t(track));
    out.push_back(0);
    uint32_t start = cd->media.track_start(cd->media.opaque, internal);
    if (msf)
      start = to_msf(start + 150);
    const size_t p = out.size();
    out.resize(p + 4);
    put_be32(&out[p], start);
  };
  if (format == 0) {
    const unsigned last = cd->media.last_track(cd->media.opaque);
    const unsigned start_track = cdb[6];
    unsigned tracks = 0;
    if (!start_track)
      tracks = last + 1;
    else if (start_track <= last)
      tracks = (last - start_track) + 2;
    else if (start_track <= LEAD_OUT)
      tracks = 1;
    if (tracks > 256)
      return check(cd, SK_ILLEGAL_REQUEST);
    const uint16_t full_length = uint16_t(2 + tracks * 8);
    out.resize(4);
    put_be16(out.data(), full_length);
    out[2] = 1;
    out[3] = uint8_t(last);
    unsigned first = start_track ? start_track : 1;
    for (unsigned i = 0; i < tracks; ++i) {
      const bool leadout = i == tracks - 1;
      append_desc(leadout ? LEAD_OUT : first + i,
                  leadout ? LEAD_OUT : first + i - 1);
    }
  } else if (format == 1) {
    out.resize(4);
    put_be16(out.data(), 10);
    out[2] = 1;
    out[3] = 1;
    append_desc(1, 0);
  } else {
    return check(cd, SK_ILLEGAL_REQUEST);
  }
  out.resize(std::min(out.size(), allocation));
  response_in(cd, std::move(out));
}

void do_read_subchannel(NscsiCdrom *cd, const uint8_t *cdb) {
  if (!present(cd))
    return no_medium(cd);
  const bool msf = (cdb[1] & 0x02) != 0;
  const bool subq = (cdb[2] & 0x40) != 0;
  const unsigned parameter = cdb[3];
  const size_t allocation = be16(&cdb[7]);
  if (allocation > RESPONSE_LIMIT)
    return check(cd, SK_ILLEGAL_REQUEST, ASC_INVALID_FIELD_CDB);
  std::vector<uint8_t> out(std::max<size_t>(allocation, 16), 0);
  nscsi_cd_audio_status_t audio{};
  get_audio_status(cd, &audio);
  if (allocation > 1) {
    if (audio.active)
      out[1] = audio.paused ? 0x12 : 0x11;
    else
      out[1] = audio.ended ? 0x13 : 0x15;
  }
  if (subq && parameter == 1) {
    const uint32_t current = audio.active ? audio.lba : 0;
    const unsigned track_index =
        cd->media.track_for_lba(cd->media.opaque, current);
    out[2] = 0;
    out[3] = 12;
    out[4] = 1;
    out[5] = uint8_t((audio.active ? 0 : 4) | 0x10);
    out[6] = uint8_t(track_index + 1);
    out[7] = 0;
    uint32_t absolute = current;
    if (msf)
      absolute = to_msf(absolute);
    put_be32(&out[8], absolute);
    uint32_t relative =
        current - cd->media.track_start(cd->media.opaque, track_index);
    if (msf)
      relative = to_msf(relative);
    put_be32(&out[12], relative);
  }
  out.resize(allocation);
  response_in(cd, std::move(out));
}

void do_read_disc_info(NscsiCdrom *cd) {
  if (!present(cd))
    return no_medium(cd);
  std::vector<uint8_t> out(34, 0);
  out[1] = 32;
  out[2] = 2;
  out[3] = 1;
  out[4] = 1;
  out[5] = 1;
  out[6] = uint8_t(cd->media.last_track(cd->media.opaque));
  uint32_t start = to_msf(cd->media.track_start(cd->media.opaque, 0) + 150);
  put_be32(&out[16], start);
  uint32_t leadout =
      to_msf(cd->media.track_start(cd->media.opaque, LEAD_OUT) + 150);
  put_be32(&out[20], leadout);
  response_in(cd, std::move(out));
}

void handle_command(NscsiCdrom *cd, const uint8_t *cdb, size_t length) {
  clear_response(cd);
  if (!length)
    return check(cd, SK_ILLEGAL_REQUEST);
  if (cd->media.sequence_counter(cd->media.opaque) != cd->sequence &&
      cdb[0] != 0x12) {
    cd->cur_sector = UINT32_MAX;
    cd->sequence = cd->media.sequence_counter(cd->media.opaque);
    return check(cd, SK_UNIT_ATTENTION);
  }
  const uint8_t op = cdb[0];
  const unsigned lun = get_lun(cd, cdb[1]);
  switch (op) {
  case 0x00: // TEST UNIT READY
    if (present(cd))
      good(cd);
    else
      no_medium(cd);
    return;
  case 0x08: { // READ(6)
    if (!present(cd))
      return no_medium(cd);
    cd->lba = be24(&cdb[1]) & 0x1fffff;
    unsigned blocks = cdb[4] ? cdb[4] : 256;
    data_in(cd, BUF_SECTOR, size_t(blocks) * cd->block_size);
    return;
  }
  case 0x12: // INQUIRY
    if (cdb[2] != 0)
      return good(cd); // source has no response data for unsupported VPD pages
    response_in(cd, inquiry_data(cd, lun, cdb[4]));
    return;
  case 0x03: { // REQUEST SENSE, generic nscsi_full_device behavior
    if ((cdb[1] & 0x1f) || cdb[2] || cdb[3])
      return check(cd, SK_ILLEGAL_REQUEST, ASC_INVALID_FIELD_CDB);
    data_in(cd, BUF_SENSE, std::min<size_t>(SENSE_BYTES, cdb[4]));
    return;
  }
  case 0x15: // MODE SELECT(6)
    cd->mode_data_size = cdb[4];
    cd->mode_write_active = cd->mode_data_size != 0;
    if (cd->mode_data_size)
      if (!nscsi_target_data_out(cd->target, BUF_SECTOR, cd->mode_data_size))
        cd->faults |= NSCSI_CD_FAULT_BACKEND;
    good(cd);
    return;
  case 0x1b: // START STOP UNIT (load/eject are acknowledged but not performed)
    good(cd);
    return;
  case 0x1c: { // RECEIVE DIAGNOSTIC RESULTS
    const size_t requested = be16(&cdb[3]);
    response_in_limit(cd, {0, 6, 0, 0, 0, 0, 0}, requested);
    return;
  }
  case 0x1d: { // SEND DIAGNOSTIC
    if (cdb[1] & 0x04)
      return good(cd);
    const size_t requested = be16(&cdb[3]);
    response_in_limit(cd, {0, 6, 0, 0, 0, 0, 0, 0}, requested);
    return;
  }
  case 0x1e: // PREVENT/ALLOW MEDIUM REMOVAL
    cd->removal_prevented = (cdb[4] & 1) != 0;
    good(cd);
    return;
  case 0x25: { // READ CAPACITY(10)
    if (!present(cd))
      return no_medium(cd);
    const uint32_t last = cd->media.track_start(cd->media.opaque, LEAD_OUT) *
                              (NSCSI_CD_SECTOR_BYTES / cd->block_size) -
                          1;
    std::vector<uint8_t> out(8, 0);
    put_be32(out.data(), last);
    put_be16(&out[6], uint16_t(cd->block_size));
    response_in(cd, std::move(out));
    return;
  }
  case 0x28: { // READ(10)
    if (!present(cd))
      return no_medium(cd);
    cd->lba = be32(&cdb[2]);
    const unsigned blocks = be16(&cdb[7]);
    data_in(cd, BUF_SECTOR, size_t(blocks) * cd->block_size);
    return;
  }
  case 0x1a: // MODE SENSE(6)
    do_mode_sense(cd, cdb);
    return;
  case 0x51: // READ DISC INFORMATION
    do_read_disc_info(cd);
    return;
  case 0x42: // READ SUB-CHANNEL
    do_read_subchannel(cd, cdb);
    return;
  case 0x43: // READ TOC/PMA/ATIP
    do_read_toc(cd, cdb);
    return;
  case 0x48: { // PLAY AUDIO TRACK/INDEX
    const unsigned start_track = cdb[4];
    const unsigned requested_end = cdb[7];
    if (start_track > requested_end || !start_track)
      return check(cd, SK_ILLEGAL_REQUEST, 0, ASC_INVALID_TRACK);
    if (!present(cd))
      return no_medium(cd);
    const unsigned end_track =
        cd->cdda_sotc
            ? start_track
            : std::min(cd->media.last_track(cd->media.opaque), requested_end);
    const uint32_t start =
        cd->media.track_start(cd->media.opaque, start_track - 1);
    const uint32_t end = cd->media.track_start(cd->media.opaque, end_track);
    const unsigned track = cd->media.track_for_lba(cd->media.opaque, start);
    if (cd->media.track_is_audio(cd->media.opaque, track)) {
      cd->media.cdda_start(cd->media.opaque, start, end - start);
      good(cd);
      set_sense(cd, SK_NO_SENSE, 0, ASC_READ_ERROR);
    } else {
      check(cd, SK_ILLEGAL_REQUEST, ASC_AUDIO_NOT_PLAYABLE);
    }
    return;
  }
  case 0x4b: // PAUSE/RESUME
    if (present(cd))
      cd->media.cdda_pause(cd->media.opaque, (cdb[8] & 1) == 0);
    good(cd);
    return;
  default:
    break;
  }

  if (op >= 0xd0 && op <= 0xda) {
    cd->faults |= NSCSI_CD_FAULT_UNSUPPORTED_TOOLBOX;
    return check(cd, SK_ILLEGAL_REQUEST, ASC_INVALID_FIELD_CDB);
  }
  if (op == 0x01)
    return good(cd);             // REZERO UNIT, generic target behavior
  check(cd, SK_ILLEGAL_REQUEST); // generic target's unknown command behavior
  (void)length;
}

void command_callback(void *opaque, NscsiTarget *target, const uint8_t *cdb,
                      size_t length) {
  auto *cd = static_cast<NscsiCdrom *>(opaque);
  cd->target = target;
  cd->in_callback = true;
  try { handle_command(cd, cdb, length); }
  catch (const std::bad_alloc &) {
    cd->faults |= NSCSI_CD_FAULT_BACKEND;
    check(cd, SK_ILLEGAL_REQUEST);
  }
  cd->in_callback = false;
}
uint8_t read_callback(void *opaque, unsigned buffer, size_t offset) {
  auto *cd = static_cast<NscsiCdrom *>(opaque);
  if (buffer == BUF_MAIN)
    return offset < cd->response.size() ? cd->response[offset] : 0;
  if (buffer == BUF_SENSE)
    return offset < cd->sense.size() ? cd->sense[offset] : 0;
  if (buffer != BUF_SECTOR)
    return 0;
  const uint64_t byte_offset = uint64_t(cd->lba) * cd->block_size + offset;
  const uint64_t sector64 = byte_offset / NSCSI_CD_SECTOR_BYTES;
  if (sector64 > UINT32_MAX) {
    cd->faults |= NSCSI_CD_FAULT_BAD_MEDIA_READ;
    return 0;
  }
  const uint32_t sector = uint32_t(sector64);
  const size_t in_sector = size_t(byte_offset % NSCSI_CD_SECTOR_BYTES);
  if (sector != cd->cur_sector) {
    cd->cur_sector = sector;
    cd->in_callback = true;
    const int read_ok = cd->media.read_mode1_sector(cd->media.opaque, sector,
                                                    cd->sector.data());
    cd->in_callback = false;
    if (!read_ok) {
      /* MAME explicitly zero-fills an unreadable MODE1 sector. */
      cd->sector.fill(0);
      cd->faults |= NSCSI_CD_FAULT_BAD_MEDIA_READ;
    }
  }
  return cd->sector[in_sector];
}
void write_callback(void *opaque, unsigned buffer, size_t offset,
                    uint8_t value) {
  auto *cd = static_cast<NscsiCdrom *>(opaque);
  if (buffer == BUF_SECTOR && cd->mode_write_active &&
      offset < cd->mode_data.size()) {
    cd->mode_data[offset] = value;
    if (offset + 1 == cd->mode_data_size) {
      cd->in_callback = true;
      finish_mode_select(cd);
      cd->in_callback = false;
      cd->mode_write_active = false;
    }
  }
}

nscsi_target_backend_t target_backend(NscsiCdrom *cd) {
  nscsi_target_backend_t backend{};
  backend.opaque = cd;
  backend.command = command_callback;
  backend.read_data = read_callback;
  backend.write_data = write_callback;
  backend.command_done = [](void *, uint8_t op, size_t length) -> int {
    return (op & 0xd0) == 0xd0 ? int(length == 10) : -1;
  };
  return backend;
}
bool callbacks_valid(const nscsi_cd_media_ops_t &m) {
  return m.present && m.sequence_counter && m.read_mode1_sector &&
         m.track_start && m.last_track && m.track_for_lba &&
         m.track_adr_control && m.track_is_audio && m.cdda_start &&
         m.cdda_pause && m.cdda_status && m.cdda_set_gain;
}
} // namespace

extern "C" NscsiCdrom *nscsi_cdrom_create(const nscsi_cdrom_config_t *config) {
  if (!config || !config->bus || config->scsi_id >= 16 ||
      !callbacks_valid(config->media))
    return nullptr;
  const uint32_t block =
      config->block_size ? config->block_size : NSCSI_CD_SECTOR_BYTES;
  if (!block || NSCSI_CD_SECTOR_BYTES % block)
    return nullptr;
  auto *cd = new (std::nothrow) NscsiCdrom;
  if (!cd)
    return nullptr;
  cd->media = config->media;
  cd->block_size = block;
  cd->sequence = cd->media.sequence_counter(cd->media.opaque);
  set_sense(cd, SK_NO_SENSE);
  const auto backend = target_backend(cd);
  cd->target = nscsi_target_create(config->bus, config->scsi_id, &backend);
  if (!cd->target) {
    delete cd;
    return nullptr;
  }
  return cd;
}
extern "C" void nscsi_cdrom_destroy(NscsiCdrom *cd) {
  if (!cd)
    return;
  if (cd->in_callback) {
    cd->faults |= NSCSI_CD_FAULT_REENTRANT;
    return;
  }
  nscsi_target_destroy(cd->target);
  delete cd;
}
extern "C" void nscsi_cdrom_reset(NscsiCdrom *cd) {
  if (!cd)
    return;
  if (cd->in_callback) {
    cd->faults |= NSCSI_CD_FAULT_REENTRANT;
    return;
  }
  nscsi_target_reset(cd->target);
  cd->in_callback = true;
  cd->sequence = cd->media.sequence_counter(cd->media.opaque);
  cd->in_callback = false;
  cd->lba = 0;
  cd->cur_sector = UINT32_MAX;
  cd->mode_data_size = 12;
  cd->response.clear();
  cd->mode_write_active = false;
  set_sense(cd, SK_NO_SENSE);
}
extern "C" void nscsi_cdrom_advance_to(NscsiCdrom *cd, uint64_t now_ns) {
  if (!cd)
    return;
  if (cd->in_callback) {
    cd->faults |= NSCSI_CD_FAULT_REENTRANT;
    return;
  }
  nscsi_target_advance_to(cd->target, now_ns);
}
extern "C" void nscsi_cdrom_get_status(const NscsiCdrom *cd,
                                       nscsi_cdrom_status_t *status) {
  if (!cd || !status)
    return;
  *status = {};
  status->block_size = cd->block_size;
  status->faults = cd->faults;
  status->media_sequence = cd->sequence;
  status->sense_key = cd->sense[2] & 0x0f;
  status->asc = cd->sense[12];
  status->ascq = cd->sense[13];
  status->removal_prevented = cd->removal_prevented;
  status->cdda_sotc = cd->cdda_sotc;
  std::copy(cd->manufacturer.begin(), cd->manufacturer.end(),
            status->manufacturer);
  std::copy(cd->product.begin(), cd->product.end(), status->product);
  std::copy(cd->revision.begin(), cd->revision.end(), status->revision);
  status->inquiry_data = cd->inquiry_data;
  status->compliance = cd->compliance;
  nscsi_target_get_status(cd->target, &status->target);
}
