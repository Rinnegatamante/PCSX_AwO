#include "ncr53cf96.h"
#include "nscsi_bus.h"
#include "nscsi_hd.h"
#include "nscsi_target.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
constexpr size_t kSectorBytes = 512;

struct Disk {
  nscsi_hd_t *owner = nullptr;
  bool attempt_reentry = false;
  std::vector<std::array<uint8_t, kSectorBytes>> sectors;
  std::vector<uint32_t> writes;
  std::vector<std::vector<uint8_t>> write_samples;
  unsigned reads = 0;
  unsigned fail_read_call = 0;
};

static int disk_read(void *opaque, uint32_t lba, uint8_t *sector, size_t size) {
  auto *disk = static_cast<Disk *>(opaque);
  ++disk->reads;
  if (disk->reads == disk->fail_read_call) return 0;
  if (disk->attempt_reentry)
    nscsi_hd_reset(disk->owner);
  if (size != kSectorBytes || lba >= disk->sectors.size())
    return 0;
  std::copy(disk->sectors[lba].begin(), disk->sectors[lba].end(), sector);
  return 1;
}

static int disk_write(void *opaque, uint32_t lba, const uint8_t *sector,
                      size_t size) {
  auto *disk = static_cast<Disk *>(opaque);
  disk->writes.push_back(lba);
  disk->write_samples.emplace_back(sector, sector + size);
  if (size != kSectorBytes || lba >= disk->sectors.size())
    return 0;
  std::copy(sector, sector + size, disk->sectors[lba].begin());
  return 1;
}

struct NcrWire {
  NscsiBus *bus = nullptr;
  Ncr53cf96 *chip = nullptr;
  unsigned id = 7;
  uint32_t own_ctrl = 0, own_data = 0, wait = 0;
};

struct ManualWire {
  NscsiBus *bus = nullptr;
  uint32_t ctrl = 0, data = 0;
};
static void manual_changed(void *, uint32_t, uint32_t) {}

struct ManualTarget {
  NscsiBus *bus = nullptr;
  nscsi_hd_t *hd = nullptr;
  Disk disk;
  ManualWire wire{};
  uint64_t now = 0;

  explicit ManualTarget(bool writable = true) {
    disk.sectors.resize(8);
    for (size_t lba = 0; lba < disk.sectors.size(); ++lba)
      for (size_t i = 0; i < kSectorBytes; ++i)
        disk.sectors[lba][i] = uint8_t((lba * 31 + i * 7) & 0xff);
    bus = nscsi_bus_create();
    assert(bus);
    wire.bus = bus;
    nscsi_bus_ops_t ops{&wire, manual_changed};
    assert(nscsi_bus_attach(bus, 7, &ops));
    nscsi_hd_config_t config{};
    config.media_present = 1;
    config.cylinders = 2;
    config.heads = 2;
    config.sectors_per_track = 2;
    config.sector_bytes = kSectorBytes;
    config.media_opaque = &disk;
    config.read_sector = disk_read;
    config.write_sector = writable ? disk_write : nullptr;
    hd = nscsi_hd_create(bus, 4, &config);
    assert(hd);
    disk.owner = hd;
  }

  ~ManualTarget() {
    nscsi_hd_destroy(hd);
    nscsi_bus_destroy(bus);
  }

  void drive_data(uint32_t value) {
    nscsi_bus_drive_data(bus, 7, value);
    assert(nscsi_bus_flush(bus));
  }
  void drive_ctrl(uint32_t value, uint32_t mask = NSCSI_ALL) {
    nscsi_bus_drive_ctrl(bus, 7, value, mask);
    assert(nscsi_bus_flush(bus));
  }
  uint8_t receive() {
    const uint8_t value = uint8_t(nscsi_bus_data_external(bus, 7));
    drive_ctrl(NSCSI_ACK);
    drive_ctrl(0);
    return value;
  }
  void send(uint8_t value) {
    drive_data(value);
    drive_ctrl(NSCSI_ACK);
    drive_ctrl(0);
  }
  void select_and_send(const std::vector<uint8_t> &cdb) {
    drive_data((1u << 7) | (1u << 4));
    drive_ctrl(NSCSI_SEL | NSCSI_ATN);
    now += 400;
    nscsi_hd_advance_to(hd, now);
    assert(nscsi_bus_flush(bus));
    drive_ctrl(NSCSI_ATN);
    drive_data(0x80); // IDENTIFY, LUN 0
    drive_ctrl(NSCSI_ATN | NSCSI_ACK);
    drive_ctrl(NSCSI_ATN);
    drive_data(0x08); // source-compatible NO OP message
    drive_ctrl(NSCSI_ATN | NSCSI_ACK);
    drive_ctrl(0); // drop ATN on ACK falling edge
    for (uint8_t byte : cdb)
      send(byte);
  }
  std::vector<uint8_t> execute_in(const std::vector<uint8_t> &cdb,
                                  size_t length, uint8_t expected_status) {
    select_and_send(cdb);
    std::vector<uint8_t> data;
    for (size_t i = 0; i < length; ++i)
      data.push_back(receive());
    assert((nscsi_bus_ctrl(bus) & NSCSI_PHASE_MASK) == NSCSI_PHASE_STATUS);
    assert(receive() == expected_status);
    assert((nscsi_bus_ctrl(bus) & NSCSI_PHASE_MASK) == NSCSI_PHASE_MSG_IN);
    assert(receive() == NSCSI_MESSAGE_COMMAND_COMPLETE);
    assert((nscsi_bus_ctrl(bus) & NSCSI_BSY) == 0);
    return data;
  }
  uint8_t execute_out(const std::vector<uint8_t> &cdb,
                      const std::vector<uint8_t> &payload,
                      uint8_t expected_status) {
    select_and_send(cdb);
    if ((nscsi_bus_ctrl(bus) & NSCSI_PHASE_MASK) == NSCSI_PHASE_DATA_OUT) {
      for (uint8_t byte : payload)
        send(byte);
    }
    assert((nscsi_bus_ctrl(bus) & NSCSI_PHASE_MASK) == NSCSI_PHASE_STATUS);
    const uint8_t status = receive();
    assert(status == expected_status);
    assert((nscsi_bus_ctrl(bus) & NSCSI_PHASE_MASK) == NSCSI_PHASE_MSG_IN);
    assert(receive() == NSCSI_MESSAGE_COMMAND_COMPLETE);
    assert((nscsi_bus_ctrl(bus) & NSCSI_BSY) == 0);
    return status;
  }
  uint8_t execute_no_data(const std::vector<uint8_t> &cdb,
                          uint8_t expected_status) {
    select_and_send(cdb);
    assert((nscsi_bus_ctrl(bus) & NSCSI_PHASE_MASK) == NSCSI_PHASE_STATUS);
    const uint8_t status = receive();
    assert(status == expected_status);
    assert((nscsi_bus_ctrl(bus) & NSCSI_PHASE_MASK) == NSCSI_PHASE_MSG_IN);
    assert(receive() == NSCSI_MESSAGE_COMMAND_COMPLETE);
    assert((nscsi_bus_ctrl(bus) & NSCSI_BSY) == 0);
    return status;
  }
};

static void drive_ctrl(void *opaque, uint32_t value, uint32_t mask) {
  auto *wire = static_cast<NcrWire *>(opaque);
  wire->own_ctrl = (wire->own_ctrl & ~mask) | (value & mask);
  nscsi_bus_drive_ctrl(wire->bus, wire->id, value, mask);
}
static void drive_data(void *opaque, uint32_t value) {
  auto *wire = static_cast<NcrWire *>(opaque);
  wire->own_data = value;
  nscsi_bus_drive_data(wire->bus, wire->id, value);
}
static void wait_ctrl(void *opaque, uint32_t value, uint32_t mask) {
  auto *wire = static_cast<NcrWire *>(opaque);
  wire->wait = (wire->wait & ~mask) | (value & mask);
  nscsi_bus_wait_ctrl(wire->bus, wire->id, value, mask);
}
static void bus_changed(void *opaque, uint32_t ctrl, uint32_t data) {
  auto *wire = static_cast<NcrWire *>(opaque);
  ncr53cf96_bus_changed(wire->chip, ctrl, data);
}
static void no_line(void *, int) {}

struct Fixture {
  NscsiBus *bus = nullptr;
  nscsi_hd_t *hd = nullptr;
  Ncr53cf96 *ncr = nullptr;
  NcrWire wire{};
  Disk disk;
  uint64_t cycle = 0;
  uint64_t command_start = 0;
  uint64_t issued_commands = 0;

  explicit Fixture(bool writable = true, bool media = true) {
    disk.sectors.resize(8);
    for (size_t lba = 0; lba < disk.sectors.size(); ++lba)
      for (size_t i = 0; i < kSectorBytes; ++i)
        disk.sectors[lba][i] = uint8_t((lba * 31 + i * 7) & 0xff);

    bus = nscsi_bus_create();
    assert(bus);
    wire.bus = bus;
    nscsi_bus_ops_t bus_ops{&wire, bus_changed};
    assert(nscsi_bus_attach(bus, wire.id, &bus_ops));
    ncr53cf96_bus_ops_t chip_ops{};
    chip_ops.opaque = &wire;
    chip_ops.drive_ctrl = drive_ctrl;
    chip_ops.drive_data = drive_data;
    chip_ops.wait_ctrl = wait_ctrl;
    chip_ops.irq = no_line;
    chip_ops.drq = no_line;
    ncr = ncr53cf96_create(16000000, &chip_ops);
    assert(ncr);
    wire.chip = ncr;

    nscsi_hd_config_t config{};
    config.media_present = media;
    config.cylinders = 2;
    config.heads = 2;
    config.sectors_per_track = 2;
    config.sector_bytes = kSectorBytes;
    config.media_opaque = &disk;
    config.read_sector = disk_read;
    config.write_sector = writable ? disk_write : nullptr;
    hd = nscsi_hd_create(bus, 0, &config);
    if (media)
      assert(hd);
    else
      assert(!hd);
    disk.owner = hd;
  }

  ~Fixture() {
    nscsi_hd_destroy(hd);
    ncr53cf96_destroy(ncr);
    nscsi_bus_destroy(bus);
  }

  void tick() {
    ++cycle;
    nscsi_hd_advance_to(hd, (cycle * 125) / 2);
    assert(nscsi_bus_flush(bus));
    ncr53cf96_advance_to(ncr, cycle);
    assert(nscsi_bus_flush(bus));
  }

  void ticks(unsigned count = 128) {
    for (unsigned i = 0; i < count; ++i)
      tick();
  }

  void restart_initiator() {
    nscsi_hd_reset(hd);
    assert(nscsi_bus_flush(bus));
    if (ncr)
      ncr53cf96_destroy(ncr);
    nscsi_bus_detach(bus, wire.id);
    assert(nscsi_bus_flush(bus));
    wire.own_ctrl = wire.own_data = wire.wait = 0;
    nscsi_bus_ops_t bus_ops{&wire, bus_changed};
    assert(nscsi_bus_attach(bus, wire.id, &bus_ops));
    ncr53cf96_bus_ops_t chip_ops{};
    chip_ops.opaque = &wire;
    chip_ops.drive_ctrl = drive_ctrl;
    chip_ops.drive_data = drive_data;
    chip_ops.wait_ctrl = wait_ctrl;
    chip_ops.irq = no_line;
    chip_ops.drq = no_line;
    ncr = ncr53cf96_create(16000000, &chip_ops);
    assert(ncr);
    wire.chip = ncr;
  }

  ncr53cf96_status_t chip_status() const {
    ncr53cf96_status_t status{};
    ncr53cf96_get_status(ncr, &status);
    return status;
  }

  void wait_fifo(bool nonempty) {
    for (unsigned i = 0; i < 5000; ++i) {
      const auto status = chip_status();
      if ((status.fifo_count != 0) == nonempty)
        return;
      tick();
    }
    assert(false && "NCR FIFO did not make progress");
  }

  void start(const std::vector<uint8_t> &cdb) {
    if (issued_commands)
      restart_initiator();
    nscsi_hd_status_t status{};
    nscsi_hd_get_status(hd, &status);
    command_start = status.commands;
    for (uint8_t byte : cdb)
      ncr53cf96_write(ncr, 2, byte);
    ncr53cf96_write(ncr, 8, 7);    // initiator ID
    ncr53cf96_write(ncr, 4, 0);    // target ID
    ncr53cf96_write(ncr, 3, 0x41); // select with ATN
    for (unsigned i = 0; i < 5000; ++i) {
      tick();
      nscsi_hd_get_status(hd, &status);
      if (status.commands != command_start)
        break;
    }
    assert(status.commands == command_start + 1);
    ++issued_commands;
    (void)ncr53cf96_read(ncr, 5); // acknowledge selection completion
    ticks();
  }

  void begin_dma(size_t bytes) {
    assert(bytes <= 0xffff);
    ncr53cf96_write(ncr, 11, 0x08);
    ncr53cf96_write(ncr, 0, uint8_t(bytes));
    ncr53cf96_write(ncr, 1, uint8_t(bytes >> 8));
    ncr53cf96_write(ncr, 14, 0);
    ncr53cf96_write(ncr, 3, 0x90);
    assert(nscsi_bus_flush(bus));
  }

  std::vector<uint8_t> data_in(size_t bytes) {
    begin_dma(bytes);
    std::vector<uint8_t> result;
    result.reserve(bytes);
    for (size_t i = 0; i < bytes; ++i) {
      wait_fifo(true);
      result.push_back(ncr53cf96_dma_read(ncr));
    }
    ticks(4);
    return result;
  }

  void data_out(const std::vector<uint8_t> &bytes) {
    begin_dma(bytes.size());
    for (uint8_t value : bytes) {
      for (unsigned i = 0; i < 5000; ++i) {
        const auto status = chip_status();
        if (status.drq)
          break;
        tick();
        assert(i != 4999);
      }
      ncr53cf96_dma_write(ncr, value);
      tick();
    }
    for (unsigned i = 0; i < 20000; ++i) {
      if ((nscsi_bus_ctrl(bus) & NSCSI_PHASE_MASK) == NSCSI_PHASE_STATUS)
        return;
      tick();
    }
    assert(false && "target did not finish DATA OUT phase");
  }

  uint8_t finish() {
    (void)ncr53cf96_read(ncr, 5); // data transfer complete
    ncr53cf96_write(ncr, 11, 0x08);
    ncr53cf96_write(ncr, 0, 1);
    ncr53cf96_write(ncr, 1, 0);
    ncr53cf96_write(ncr, 14, 0);
    ncr53cf96_write(ncr, 3, 0x90);
    assert(nscsi_bus_flush(bus));
    wait_fifo(true);
    const uint8_t status = ncr53cf96_dma_read(ncr);
    (void)ncr53cf96_read(ncr, 5);
    ncr53cf96_write(ncr, 3, 0x11); // fetch COMMAND COMPLETE message
    assert(nscsi_bus_flush(bus));
    ticks();
    (void)ncr53cf96_read(ncr, 2);
    ncr53cf96_write(ncr, 3, 0x12); // accept message
    assert(nscsi_bus_flush(bus));
    ticks();
    return status;
  }

  std::vector<uint8_t> execute_in(const std::vector<uint8_t> &cdb,
                                  size_t length) {
    start(cdb);
    auto result = data_in(length);
    assert(finish() == NSCSI_STATUS_GOOD);
    return result;
  }

  uint8_t execute_no_data(const std::vector<uint8_t> &cdb) {
    start(cdb);
    return finish();
  }

  uint8_t execute_out(const std::vector<uint8_t> &cdb,
                      const std::vector<uint8_t> &data) {
    start(cdb);
    data_out(data);
    return finish();
  }
};

void integration_read_capacity_inquiry_and_request_sense() {
  ManualTarget f;
  const auto capacity =
      f.execute_in({0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 8, NSCSI_STATUS_GOOD);
  assert((capacity == std::vector<uint8_t>{0, 0, 0, 7, 0, 0, 2, 0}));

  const auto inquiry =
      f.execute_in({0x12, 0, 0, 0, 56, 0}, 56, NSCSI_STATUS_GOOD);
  assert(inquiry[0] == 0 && inquiry[2] == 5 && inquiry[3] == 1);
  assert(std::memcmp(inquiry.data() + 8, " SEAGATE          ST225N1.00", 28) ==
         0);
  assert(inquiry[37] == 8 && inquiry[39] == 0x99 && inquiry[53] == 0xff);

  f.execute_no_data({0x00, 0, 0, 0, 0, 0}, NSCSI_STATUS_GOOD);
}

void integration_reads_writes_and_write_quirks() {
  ManualTarget f;
  const uint32_t lba = 3;
  std::vector<uint8_t> expected(f.disk.sectors[lba].begin(),
                                f.disk.sectors[lba].end());
  const std::vector<uint8_t> read10{0x28, 0, 0, 0, 0, 3, 0, 0, 1, 0};
  const auto received = f.execute_in(read10, kSectorBytes, NSCSI_STATUS_GOOD);
  assert(received == expected);

  /* Source WRITE preflight calls image->write(lba, block) before DATA OUT.
   * Since a prior READ populated block, this first callback writes those old
   * bytes to the new LBA; the payload subsequently overwrites the sector. */
  std::vector<uint8_t> payload(kSectorBytes);
  for (size_t i = 0; i < payload.size(); ++i)
    payload[i] = uint8_t(255 - ((i * 11) & 0xff));
  const std::vector<uint8_t> write10{0x2a, 0, 0, 0, 0, 4, 0, 0, 1, 0};
  assert(f.execute_out(write10, payload, NSCSI_STATUS_GOOD) ==
         NSCSI_STATUS_GOOD);
  assert(f.disk.writes.size() >= 2);
  assert(f.disk.writes[f.disk.writes.size() - 2] == 4);
  assert(f.disk.writes.back() == 4);
  assert(f.disk.write_samples[f.disk.write_samples.size() - 2] == expected);
  assert(std::equal(payload.begin(), payload.end(), f.disk.sectors[4].begin()));

  const std::vector<uint8_t> read6{0x08, 0, 0, 0, 1, 0};
  const auto first = f.execute_in(read6, kSectorBytes, NSCSI_STATUS_GOOD);
  assert(std::equal(first.begin(), first.end(), f.disk.sectors[0].begin()));

  std::vector<uint8_t> second_payload(kSectorBytes, 0x6d);
  const std::vector<uint8_t> write6{0x0a, 0, 0, 0, 1, 0};
  assert(f.execute_out(write6, second_payload, NSCSI_STATUS_GOOD) ==
         NSCSI_STATUS_GOOD);
  assert(std::equal(second_payload.begin(), second_payload.end(),
                    f.disk.sectors[0].begin()));
}

void command_sense_mode_and_simple_source_cases() {
  ManualTarget f;
  const std::vector<uint8_t> unsupported{0x20, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  f.execute_no_data(unsupported, NSCSI_STATUS_CHECK_CONDITION);
  nscsi_hd_status_t error{};
  nscsi_hd_get_status(f.hd, &error);
  assert(error.last_sense[0] == 0xf0 && error.last_sense[2] == 5 &&
         error.last_sense[7] == 10 && error.last_sense[12] == 0);

  const auto sense =
      f.execute_in({0x03, 0, 0, 0, 18, 0}, 18, NSCSI_STATUS_GOOD);
  assert(sense[0] == 0xf0 && sense[2] == 5 && sense[7] == 10);
  assert(sense[12] == 0 && sense[13] == 0);

  const auto mode =
      f.execute_in({0x1a, 0, 0x3f, 0, 128, 0}, 128, NSCSI_STATUS_GOOD);
  assert(mode[0] >= 11 && mode[3] == 8);
  assert(mode[4] == 0 && mode[5] == 0 && mode[6] == 0 && mode[7] == 7);

  f.execute_no_data({0x1b, 0, 0, 0, 1, 0}, NSCSI_STATUS_GOOD);
  f.execute_no_data({0x35, 0, 0, 0, 0, 0, 0, 0, 0, 0}, NSCSI_STATUS_GOOD);
  const auto diag = f.execute_in({0x1c, 0, 0, 0, 7, 0}, 7, NSCSI_STATUS_GOOD);
  assert((diag == std::vector<uint8_t>{0, 6, 0, 0, 0, 0, 0}));
  f.execute_no_data({0x1d, 4, 0, 0, 0, 0}, NSCSI_STATUS_GOOD);
  assert(f.execute_out({0x15, 0, 0, 0, 2, 0}, {0x11, 0x22},
                       NSCSI_STATUS_GOOD) == NSCSI_STATUS_GOOD);
  f.execute_no_data({0x2f, 0, 0, 0, 0, 0, 0, 0, 0, 0}, NSCSI_STATUS_GOOD);
  f.execute_no_data({0x2f, 2, 0, 0, 0, 0, 0, 0, 0, 0},
                    NSCSI_STATUS_CHECK_CONDITION);
}

void source_format_and_failure_behavior() {
  ManualTarget f;
  /* FORMAT UNIT's source loop writes zero sectors using cyl*head*sector. */
  f.execute_no_data({0x04, 0, 0, 0, 0, 0}, NSCSI_STATUS_GOOD);
  assert(std::all_of(f.disk.sectors[0].begin(), f.disk.sectors[0].end(),
                     [](uint8_t v) { return v == 0; }));

  ManualTarget readonly(false);
  const std::vector<uint8_t> write6{0x0a, 0, 0, 0, 1, 0};
  assert(readonly.execute_out(write6, std::vector<uint8_t>(kSectorBytes, 0x77),
                              NSCSI_STATUS_CHECK_CONDITION) ==
         NSCSI_STATUS_CHECK_CONDITION);
  nscsi_hd_status_t readonly_status{};
  nscsi_hd_get_status(readonly.hd, &readonly_status);
  assert(readonly_status.last_sense[2] == 5 &&
         readonly_status.last_sense[12] == 0x24);
  const auto readonly_sense =
      readonly.execute_in({0x03, 0, 0, 0, 18, 0}, 18, NSCSI_STATUS_GOOD);
  assert(readonly_sense[2] == 5 && readonly_sense[12] == 0x24);

  Fixture absent(true, false);
  assert(absent.hd == nullptr); // no media means no selectable target ID

  nscsi_hd_config_t invalid{};
  invalid.media_present = 1;
  invalid.cylinders = invalid.heads = invalid.sectors_per_track = 1;
  invalid.sector_bytes = 513;
  invalid.read_sector = disk_read;
  assert(nscsi_hd_create(f.bus, 1, &invalid) == nullptr);
}

void source_preflight_and_lazy_cache_reads() {
  ManualTarget f;
  const std::vector<uint8_t> cdb{0x28, 0, 0, 0, 0, 3, 0, 0, 1, 0};
  f.execute_in(cdb, 512, NSCSI_STATUS_GOOD);
  assert(f.disk.reads == 2); // Preflight does not set source cur_lba.
  f.execute_in(cdb, 512, NSCSI_STATUS_GOOD);
  assert(f.disk.reads == 3); // Same cache LBA: only the mandatory preflight.
  nscsi_hd_reset(f.hd);
  f.disk.fail_read_call = 5; // Successful preflight, failed lazy reread.
  auto bytes = f.execute_in(cdb, 512, NSCSI_STATUS_GOOD);
  assert(f.disk.reads == 5 && bytes == std::vector<uint8_t>(512, 0));
  nscsi_hd_status_t status{}; nscsi_hd_get_status(f.hd, &status);
  assert(status.read_failures == 1 && (status.faults & NSCSI_HD_FAULT_MEDIA_READ));

  ManualTarget retained;
  retained.execute_in(cdb, 512, NSCSI_STATUS_GOOD);
  auto expected = std::vector<uint8_t>(retained.disk.sectors[3].begin(), retained.disk.sectors[3].end());
  retained.disk.fail_read_call = retained.disk.reads + 1;
  retained.execute_no_data({0x28, 0, 0, 0, 0, 4, 0, 0, 1, 0}, NSCSI_STATUS_CHECK_CONDITION);
  retained.execute_out({0x2a, 0, 0, 0, 0, 5, 0, 0, 1, 0}, std::vector<uint8_t>(512, 0x7c), NSCSI_STATUS_GOOD);
  assert(retained.disk.write_samples[0] == expected); // Failed preflight did not erase old block.
}

void ncr_runs_real_hd_sector_reads_and_writes() {
  Fixture read;
  read.disk.attempt_reentry = true;
  const auto expected = std::vector<uint8_t>(read.disk.sectors[3].begin(),
                                             read.disk.sectors[3].end());
  const auto actual =
      read.execute_in({0x28, 0, 0, 0, 0, 3, 0, 0, 1, 0}, kSectorBytes);
  assert(actual == expected);
  read.disk.attempt_reentry = false;
  nscsi_hd_status_t read_status{};
  nscsi_hd_get_status(read.hd, &read_status);
  assert(read_status.faults & NSCSI_HD_FAULT_REENTRANT);

  Fixture write;
  std::vector<uint8_t> payload(kSectorBytes, 0x41);
  assert(write.execute_out({0x2a, 0, 0, 0, 0, 4, 0, 0, 1, 0}, payload) ==
         NSCSI_STATUS_GOOD);
  assert(std::equal(payload.begin(), payload.end(),
                    write.disk.sectors[4].begin()));
  assert(write.disk.write_samples.size() >= 2);
  assert(std::all_of(write.disk.write_samples.front().begin(),
                     write.disk.write_samples.front().end(),
                     [](uint8_t byte) { return byte == 0; }));
  nscsi_hd_status_t status{};
  nscsi_hd_get_status(write.hd, &status);
  assert(status.faults & NSCSI_HD_FAULT_UNINITIALIZED_PREFLIGHT);
}
} // namespace

int main() {
  integration_read_capacity_inquiry_and_request_sense();
  integration_reads_writes_and_write_quirks();
  command_sense_mode_and_simple_source_cases();
  source_format_and_failure_behavior();
  source_preflight_and_lazy_cache_reads();
  ncr_runs_real_hd_sector_reads_and_writes();
  std::puts(
      "nscsi hard-disk commands, media callbacks and NCR integration passed");
  return 0;
}
