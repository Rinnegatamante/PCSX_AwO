#include "chd_hd.h"
#include "ncr53cf96.h"
#include "nscsi_bus.h"
#include "nscsi_hd.h"
#include "nscsi_target.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {
constexpr size_t kSectorBytes = 512;

struct Media {
  arcade_chd_hd_t *chd = nullptr;
  std::vector<uint32_t> write_lbas;
  std::vector<std::vector<uint8_t>> write_samples;
};

static int media_read(void *opaque, uint32_t lba, uint8_t *bytes, size_t size) {
  auto *media = static_cast<Media *>(opaque);
  return arcade_chd_hd_read(media->chd, lba, bytes, size) == 0;
}

static int media_write(void *opaque, uint32_t lba, const uint8_t *bytes,
                       size_t size) {
  auto *media = static_cast<Media *>(opaque);
  media->write_lbas.push_back(lba);
  media->write_samples.emplace_back(bytes, bytes + size);
  return arcade_chd_hd_write(media->chd, lba, bytes, size) == 0;
}

struct Wire {
  NscsiBus *bus = nullptr;
  Ncr53cf96 *chip = nullptr;
  uint32_t own_ctrl = 0, own_data = 0, wait = 0;
  unsigned id = 7;
};

static void drive_ctrl(void *opaque, uint32_t value, uint32_t mask) {
  auto *wire = static_cast<Wire *>(opaque);
  wire->own_ctrl = (wire->own_ctrl & ~mask) | (value & mask);
  nscsi_bus_drive_ctrl(wire->bus, wire->id, value, mask);
}
static void drive_data(void *opaque, uint32_t value) {
  auto *wire = static_cast<Wire *>(opaque);
  wire->own_data = value;
  nscsi_bus_drive_data(wire->bus, wire->id, value);
}
static void wait_ctrl(void *opaque, uint32_t value, uint32_t mask) {
  auto *wire = static_cast<Wire *>(opaque);
  wire->wait = (wire->wait & ~mask) | (value & mask);
  nscsi_bus_wait_ctrl(wire->bus, wire->id, value, mask);
}
static void target_changed(void *opaque, uint32_t ctrl, uint32_t data) {
  auto *wire = static_cast<Wire *>(opaque);
  ncr53cf96_bus_changed(wire->chip, ctrl, data);
}
static void no_line(void *, int) {}

struct Fixture {
  NscsiBus *bus = nullptr;
  nscsi_hd_t *hd = nullptr;
  Ncr53cf96 *chip = nullptr;
  Wire wire{};
  Media media{};
  uint64_t cycle = 0;

  explicit Fixture(arcade_chd_hd_t *chd) {
    media.chd = chd;
    bus = nscsi_bus_create();
    assert(bus);
    wire.bus = bus;
    nscsi_bus_ops_t bus_ops{&wire, target_changed};
    assert(nscsi_bus_attach(bus, wire.id, &bus_ops));

    arcade_chd_hd_info_t info{};
    arcade_chd_hd_get_info(chd, &info);
    nscsi_hd_config_t config{};
    config.media_present = 1;
    config.cylinders = info.cylinders;
    config.heads = info.heads;
    config.sectors_per_track = info.sectors_per_track;
    config.sector_bytes = info.sector_bytes;
    config.media_opaque = &media;
    config.read_sector = media_read;
    config.write_sector = media_write;
    config.inquiry_data = info.inquiry;
    config.inquiry_data_bytes = info.inquiry_bytes;
    hd = nscsi_hd_create(bus, 0, &config);
    assert(hd);
    create_chip();
  }

  ~Fixture() {
    ncr53cf96_destroy(chip);
    nscsi_hd_destroy(hd);
    nscsi_bus_destroy(bus);
  }

  void create_chip() {
    ncr53cf96_bus_ops_t ops{};
    ops.opaque = &wire;
    ops.drive_ctrl = drive_ctrl;
    ops.drive_data = drive_data;
    ops.wait_ctrl = wait_ctrl;
    ops.irq = no_line;
    ops.drq = no_line;
    chip = ncr53cf96_create(16000000, &ops);
    assert(chip);
    wire.chip = chip;
  }

  void tick() {
    ++cycle;
    nscsi_hd_advance_to(hd, (cycle * 125) / 2);
    assert(nscsi_bus_flush(bus));
    ncr53cf96_advance_to(chip, cycle);
    assert(nscsi_bus_flush(bus));
  }
  void ticks(unsigned count = 128) {
    for (unsigned i = 0; i < count; ++i)
      tick();
  }
  ncr53cf96_status_t chip_status() const {
    ncr53cf96_status_t status{};
    ncr53cf96_get_status(chip, &status);
    return status;
  }
  void wait_fifo(bool nonempty) {
    for (unsigned i = 0; i < 5000; ++i) {
      if ((chip_status().fifo_count != 0) == nonempty)
        return;
      tick();
    }
    assert(false && "NCR FIFO did not progress");
  }
  void start(const std::vector<uint8_t> &cdb) {
    nscsi_hd_status_t before{};
    nscsi_hd_get_status(hd, &before);
    for (uint8_t byte : cdb)
      ncr53cf96_write(chip, 2, byte);
    ncr53cf96_write(chip, 8, 7);
    ncr53cf96_write(chip, 4, 0);
    ncr53cf96_write(chip, 3, 0x41);
    for (unsigned i = 0; i < 5000; ++i) {
      tick();
      nscsi_hd_status_t now{};
      nscsi_hd_get_status(hd, &now);
      if (now.commands != before.commands)
        break;
      if (i == 4999) {
        const auto state = chip_status();
        std::fprintf(
            stderr,
            "command %02x stalled: hd=%llu bus=%x/%x ncr=%x int=%x seq=%u\n",
            cdb[0], static_cast<unsigned long long>(now.commands),
            nscsi_bus_ctrl(bus), nscsi_bus_data(bus), state.status,
            state.interrupt_status, state.sequence);
        assert(false && "target did not accept command");
      }
    }
    (void)ncr53cf96_read(chip, 5);
    ticks();
  }
  void begin_dma(size_t bytes) {
    assert(bytes <= 0xffff);
    ncr53cf96_write(chip, 11, 0x08);
    ncr53cf96_write(chip, 0, uint8_t(bytes));
    ncr53cf96_write(chip, 1, uint8_t(bytes >> 8));
    ncr53cf96_write(chip, 14, 0);
    ncr53cf96_write(chip, 3, 0x90);
    assert(nscsi_bus_flush(bus));
  }
  std::vector<uint8_t> data_in(size_t bytes) {
    begin_dma(bytes);
    std::vector<uint8_t> data;
    data.reserve(bytes);
    for (size_t i = 0; i < bytes; ++i) {
      wait_fifo(true);
      data.push_back(ncr53cf96_dma_read(chip));
    }
    ticks(4);
    return data;
  }
  void data_out(const std::vector<uint8_t> &data) {
    begin_dma(data.size());
    for (uint8_t byte : data) {
      for (unsigned i = 0; i < 5000 && !chip_status().drq; ++i) {
        tick();
        assert(i != 4999);
      }
      ncr53cf96_dma_write(chip, byte);
      tick();
    }
    for (unsigned i = 0; i < 20000; ++i) {
      if ((nscsi_bus_ctrl(bus) & NSCSI_PHASE_MASK) == NSCSI_PHASE_STATUS)
        return;
      tick();
    }
    assert(false && "target did not leave DATA OUT");
  }
  uint8_t finish() {
    (void)ncr53cf96_read(chip, 5);
    // CI_COMPLETE consumes STATUS and COMMAND COMPLETE as one initiator
    // operation; CI_MSG_ACCEPT releases the final message handshake.
    ncr53cf96_write(chip, 3, 0x11);
    assert(nscsi_bus_flush(bus));
    ticks();
    assert(chip_status().fifo_count == 2);
    const uint8_t status = ncr53cf96_read(chip, 2);
    assert(ncr53cf96_read(chip, 2) == NSCSI_MESSAGE_COMMAND_COMPLETE);
    (void)ncr53cf96_read(chip, 5);
    ncr53cf96_write(chip, 3, 0x12); // CI_MSG_ACCEPT
    assert(nscsi_bus_flush(bus));
    ticks();
    assert((nscsi_bus_ctrl(bus) & NSCSI_BSY) == 0);
    const auto completed = chip_status();
    if (completed.interrupt_status != 0x20 || completed.faults)
      std::fprintf(stderr, "disconnect: interrupt=%02x faults=%08x cycle=%llu\n",
                   completed.interrupt_status, completed.faults,
                   static_cast<unsigned long long>(cycle));
    assert(completed.interrupt_status == 0x20 && !completed.faults);
    nscsi_target_status_t target{};
    nscsi_target_get_status(nscsi_hd_target(hd), &target);
    assert(!target.faults);
    return status;
  }
  std::vector<uint8_t> execute_in(const std::vector<uint8_t> &cdb,
                                  size_t length) {
    start(cdb);
    auto data = data_in(length);
    assert(finish() == NSCSI_STATUS_GOOD);
    return data;
  }
  uint8_t execute_out(const std::vector<uint8_t> &cdb,
                      const std::vector<uint8_t> &data) {
    start(cdb);
    data_out(data);
    return finish();
  }
};

static std::array<uint8_t, kSectorBytes> generated_sector(unsigned lba) {
  std::array<uint8_t, kSectorBytes> result{};
  for (unsigned i = 0; i < result.size(); ++i)
    result[i] = uint8_t((lba * 17 + i * 13 + (i >> 4)) & 0xff);
  return result;
}
} // namespace

int main(int argc, char **argv) {
  assert(argc == 3);
  const std::string image = argv[1];
  const std::string overlay = argv[2];
  char error[256]{};
  auto *chd =
      arcade_chd_hd_open(image.c_str(), overlay.c_str(), error, sizeof(error));
  assert(chd && !*error);
  arcade_chd_hd_info_t info{};
  arcade_chd_hd_get_info(chd, &info);
  assert(info.cylinders == 2 && info.heads == 2 &&
         info.sectors_per_track == 5 && info.sector_bytes == 512 &&
         info.sector_count == 20 && info.hunk_bytes == 1536);

  {
    Fixture f(chd);
    const auto capacity = f.execute_in({0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 8);
    assert((capacity == std::vector<uint8_t>{0, 0, 0, 19, 0, 0, 2, 0}));
    assert((nscsi_bus_ctrl(f.bus) & NSCSI_BSY) == 0);
  }
  {
    Fixture f(chd);
    const auto inquiry = f.execute_in({0x12, 0, 0, 0, 56, 0}, 56);
    assert(inquiry[0] == 0 && inquiry[2] == 5 && inquiry[3] == 1);
    assert(inquiry[8] == ' '); // mounted image uses source default identity
  }
  {
    Fixture f(chd);
    constexpr unsigned lba = 7;
    const auto expected = generated_sector(lba);
    const std::vector<uint8_t> read_cdb{0x28,         0, 0, 0, 0,
                                        uint8_t(lba), 0, 0, 1, 0};
    assert(f.execute_in(read_cdb, kSectorBytes) ==
           std::vector<uint8_t>(expected.begin(), expected.end()));
  }

  constexpr unsigned write_lba = 8;
  std::vector<uint8_t> payload(kSectorBytes);
  for (size_t i = 0; i < payload.size(); ++i)
    payload[i] = uint8_t(0xf1 ^ ((i * 19) & 0xff));
  {
    Fixture f(chd);
    const std::vector<uint8_t> write_cdb{0x2a, 0, 0, 0, 0, uint8_t(write_lba),
                                         0,    0, 1, 0};
    assert(f.execute_out(write_cdb, payload) == NSCSI_STATUS_GOOD);
    assert(f.media.write_lbas.size() == 2);
    assert(f.media.write_lbas[0] == write_lba &&
           f.media.write_lbas[1] == write_lba);
    assert(std::all_of(f.media.write_samples[0].begin(),
                       f.media.write_samples[0].end(),
                       [](uint8_t byte) { return byte == 0; }));
    assert(f.media.write_samples[1] == payload);
    assert(arcade_chd_hd_modified_sectors(chd) == 1);
  }

  assert(arcade_chd_hd_flush(chd) == 0);
  arcade_chd_hd_close(chd);
  chd =
      arcade_chd_hd_open(image.c_str(), overlay.c_str(), error, sizeof(error));
  assert(chd && !*error);
  {
    Fixture f(chd);
    const std::vector<uint8_t> read_cdb{0x28, 0, 0, 0, 0, uint8_t(write_lba),
                                        0,    0, 1, 0};
    assert(f.execute_in(read_cdb, kSectorBytes) == payload);
  }
  arcade_chd_hd_close(chd);
  std::puts("CHD HDD over NCR→NSCSI: READ/WRITE DMA, status/message, overlay "
            "reopen PASS");
}
