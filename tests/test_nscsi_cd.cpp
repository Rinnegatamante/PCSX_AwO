#include "ncr53cf96.h"
#include "nscsi_bus.h"
#include "nscsi_cd.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

struct Disc {
  bool inserted = true;
  bool read_ok = true;
  uint64_t sequence = 1;
  unsigned reads = 0;
  unsigned starts = 0;
  unsigned pauses = 0;
  uint32_t audio_start = 0, audio_length = 0;
  nscsi_cd_audio_status_t audio{0, 0, 0, 0};
  std::array<uint8_t, NSCSI_CD_SECTOR_BYTES> sector{};
  std::array<float, 4> gains{1, 1, 1, 1};
};
static int disc_present(void *p) { return static_cast<Disc *>(p)->inserted; }
static uint64_t disc_sequence(void *p) {
  return static_cast<Disc *>(p)->sequence;
}
static int read_sector(void *p, uint32_t lba, uint8_t *out) {
  auto *d = static_cast<Disc *>(p);
  ++d->reads;
  for (size_t i = 0; i < d->sector.size(); ++i)
    out[i] = uint8_t((lba * 7 + i * 13 + 3) & 0xff);
  std::copy(out, out + NSCSI_CD_SECTOR_BYTES, d->sector.begin());
  return d->inserted && d->read_ok;
}
static uint32_t track_start(void *, unsigned track) {
  if (track == 0)
    return 0;
  if (track == 1)
    return 100;
  if (track == 2)
    return 200;
  if (track == 0xaa)
    return 300;
  return 0;
}
static unsigned last_track(void *) { return 2; }
static unsigned track_for_lba(void *, uint32_t lba) {
  return lba < 100 ? 0 : lba < 200 ? 1 : 2;
}
static uint8_t adr_control(void *, unsigned track) {
  return track == 1 ? 0x10 : 0x14;
}
static int is_audio(void *, unsigned track) { return track == 1; }
static void cdda_start(void *p, uint32_t lba, uint32_t count) {
  auto *d = static_cast<Disc *>(p);
  ++d->starts;
  d->audio_start = lba;
  d->audio_length = count;
  d->audio = {1, 0, 0, lba};
}
static void cdda_pause(void *p, int pause) {
  auto *d = static_cast<Disc *>(p);
  ++d->pauses;
  d->audio.paused = pause;
}
static void cdda_status(void *p, nscsi_cd_audio_status_t *s) {
  *s = static_cast<Disc *>(p)->audio;
}
static void cdda_gain(void *p, unsigned channel, float gain) {
  auto *d = static_cast<Disc *>(p);
  assert(channel < d->gains.size());
  d->gains[channel] = gain;
}
static nscsi_cd_media_ops_t media_ops(Disc *d) {
  return {d,          disc_present,  disc_sequence, read_sector, track_start,
          last_track, track_for_lba, adr_control,   is_audio,    cdda_start,
          cdda_pause, cdda_status,   cdda_gain};
}

struct DirectHost {
  NscsiBus *bus = nullptr;
  uint32_t ctrl = 0, data = 0;
  unsigned events = 0;
};
static void host_changed(void *p, uint32_t c, uint32_t d) {
  auto *h = static_cast<DirectHost *>(p);
  h->ctrl = c;
  h->data = d;
  ++h->events;
}
struct Direct {
  NscsiBus *bus = nscsi_bus_create();
  DirectHost host{bus};
  Disc disc;
  NscsiCdrom *cd = nullptr;
  uint64_t now = 0;
  Direct() {
    assert(bus);
    nscsi_bus_ops_t ops{&host, host_changed};
    assert(nscsi_bus_attach(bus, 7, &ops));
    nscsi_cdrom_config_t config{};
    config.bus = bus;
    config.scsi_id = 4;
    config.media = media_ops(&disc);
    cd = nscsi_cdrom_create(&config);
    assert(cd);
  }
  ~Direct() {
    nscsi_cdrom_destroy(cd);
    nscsi_bus_destroy(bus);
  }
  void ctrl(uint32_t lines, uint32_t mask = NSCSI_ALL) {
    nscsi_bus_drive_ctrl(bus, 7, lines, mask);
    assert(nscsi_bus_flush(bus));
  }
  void data(uint32_t value) {
    nscsi_bus_drive_data(bus, 7, value);
    assert(nscsi_bus_flush(bus));
  }
  void pulse_ack() {
    ctrl(NSCSI_ACK, NSCSI_ACK);
    ctrl(0, NSCSI_ACK);
  }
  std::pair<std::vector<uint8_t>, uint8_t>
  transact(const std::vector<uint8_t> &cdb, size_t data_in,
           const std::vector<uint8_t> &data_out = {}) {
    assert(cdb.size() >= 6);
    data((1u << 7) | (1u << 4));
    ctrl(NSCSI_SEL);
    now += 400;
    nscsi_cdrom_advance_to(cd, now);
    assert(nscsi_bus_ctrl(bus) & NSCSI_BSY);
    ctrl(0, NSCSI_SEL);
    data(0);
    for (uint8_t value : cdb) {
      data(value);
      pulse_ack();
      data(0);
    }
    if (!data_out.empty()) {
      assert((nscsi_bus_ctrl(bus) & NSCSI_PHASE_MASK) == NSCSI_PHASE_DATA_OUT);
      for (uint8_t value : data_out) {
        data(value);
        pulse_ack();
        data(0);
      }
    }
    std::vector<uint8_t> out;
    if (data_in) {
      assert((nscsi_bus_ctrl(bus) & NSCSI_PHASE_MASK) == NSCSI_PHASE_DATA_IN);
      out.reserve(data_in);
      for (size_t i = 0; i < data_in; ++i) {
        assert(nscsi_bus_ctrl(bus) & NSCSI_REQ);
        out.push_back(uint8_t(nscsi_bus_data(bus)));
        pulse_ack();
      }
    }
    assert((nscsi_bus_ctrl(bus) & NSCSI_PHASE_MASK) == NSCSI_PHASE_STATUS);
    uint8_t status = uint8_t(nscsi_bus_data(bus));
    pulse_ack();
    assert((nscsi_bus_ctrl(bus) & NSCSI_PHASE_MASK) == NSCSI_PHASE_MSG_IN);
    assert(uint8_t(nscsi_bus_data(bus)) == NSCSI_MESSAGE_COMMAND_COMPLETE);
    pulse_ack();
    assert((nscsi_bus_ctrl(bus) & NSCSI_BSY) == 0);
    return {out, status};
  }
};

struct NcrWire {
  NscsiBus *bus = nullptr;
  Ncr53cf96 *ncr = nullptr;
  uint32_t wait = 0;
};
static void ncr_drive_ctrl(void *p, uint32_t lines, uint32_t mask) {
  auto *w = static_cast<NcrWire *>(p);
  nscsi_bus_drive_ctrl(w->bus, 7, lines, mask);
}
static void ncr_drive_data(void *p, uint32_t value) {
  auto *w = static_cast<NcrWire *>(p);
  nscsi_bus_drive_data(w->bus, 7, value);
}
static void ncr_wait_ctrl(void *p, uint32_t lines, uint32_t mask) {
  auto *w = static_cast<NcrWire *>(p);
  w->wait = (w->wait & ~mask) | (lines & mask);
  nscsi_bus_wait_ctrl(w->bus, 7, lines, mask);
}
static void ncr_bus_changed(void *p, uint32_t ctrl, uint32_t data) {
  auto *w = static_cast<NcrWire *>(p);
  ncr53cf96_bus_changed(w->ncr, ctrl, data);
}
static void no_line(void *, int) {}

static void cd_source_identity_and_local_protocol_commands() {
  Direct d;
  nscsi_cdrom_status_t s{};
  nscsi_cdrom_get_status(d.cd, &s);
  assert(std::string(reinterpret_cast<char *>(s.manufacturer), 8) ==
         "TOSHIBA ");
  assert(std::string(reinterpret_cast<char *>(s.product), 16) ==
         "CD-ROM XM-5401TA");
  assert(std::string(reinterpret_cast<char *>(s.revision), 4) == "3605");
  assert(s.inquiry_data == 0x98 && s.compliance == 0x02 &&
         s.block_size == 2048);

  auto [inq, inq_status] = d.transact({0x12, 0, 0, 0, 36, 0}, 36);
  assert(inq_status == NSCSI_STATUS_GOOD && inq.size() == 36);
  assert(inq[0] == 0x05 && inq[1] == 0x80 && inq[2] == 0x02 && inq[4] == 0x20);
  assert(std::string(reinterpret_cast<char *>(inq.data() + 8), 8) ==
         "TOSHIBA ");
  assert(std::string(reinterpret_cast<char *>(inq.data() + 16), 16) ==
         "CD-ROM XM-5401TA");
  assert(std::string(reinterpret_cast<char *>(inq.data() + 32), 4) == "3605");
  d.disc.sequence++;
  auto [inquiry_after_change, inquiry_after_change_status] =
      d.transact({0x12, 0, 0, 0, 36, 0}, 36);
  assert(inquiry_after_change_status == NSCSI_STATUS_GOOD &&
         inquiry_after_change.size() == 36);
  auto [attention_tur, attention_status] = d.transact({0, 0, 0, 0, 0, 0}, 0);
  assert(attention_tur.empty() &&
         attention_status == NSCSI_STATUS_CHECK_CONDITION);
  auto [ready_tur, ready_status] = d.transact({0, 0, 0, 0, 0, 0}, 0);
  assert(ready_tur.empty() && ready_status == NSCSI_STATUS_GOOD);

  auto [capacity, cap_status] =
      d.transact({0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 8);
  assert(cap_status == NSCSI_STATUS_GOOD);
  assert(capacity == std::vector<uint8_t>(
                         {0, 0, 1, 43, 0, 0, 8, 0})); // leadout 300 -> last 299
  auto [lun_inquiry, lun_status] = d.transact({0x12, 0x20, 0, 0, 36, 0}, 36);
  assert(lun_status == NSCSI_STATUS_GOOD && lun_inquiry[0] == 0x7f);

  auto [toc, toc_status] = d.transact({0x43, 0, 0, 0, 0, 0, 0, 0, 20, 0}, 20);
  assert(toc_status == NSCSI_STATUS_GOOD && toc.size() == 20);
  assert(toc[0] == 0 && toc[1] == 26 && toc[2] == 1 && toc[3] == 2);
  assert(toc[6] == 1 && toc[7] == 0 && toc[11] == 0);
  assert(toc[13] == 0x10 && toc[14] == 2);

  auto [mode, mode_status] = d.transact({0x1a, 0x08, 0x0e, 0, 18, 0}, 18);
  assert(mode_status == NSCSI_STATUS_GOOD && mode.size() == 18);
  assert(mode[0] == 19 && mode[1] == 0 && mode[2] == 0x80 && mode[3] == 0);
  assert(mode[4] == 0x8e && mode[5] == 0x0e && mode[6] == 0x04);

  std::vector<uint8_t> mode_data(12, 0);
  mode_data[3] = 8;
  mode_data[10] = 2; // 512-byte blocks
  auto [empty, select_status] =
      d.transact({0x15, 0, 0, 0, 12, 0}, 0, mode_data);
  assert(empty.empty() && select_status == NSCSI_STATUS_GOOD);
  nscsi_cdrom_get_status(d.cd, &s);
  assert(s.block_size == 512);
  auto [capacity512, cap512_status] =
      d.transact({0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 8);
  assert(cap512_status == NSCSI_STATUS_GOOD);
  assert(capacity512 ==
         std::vector<uint8_t>({0, 0, 4, 175, 0, 0, 2, 0})); // 1200 blocks - 1
  auto [small_read, small_status] = d.transact({0x08, 0, 0, 1, 1, 0}, 512);
  assert(small_status == NSCSI_STATUS_GOOD && small_read.size() == 512);
  for (size_t i = 0; i < small_read.size(); ++i)
    assert(small_read[i] == uint8_t(((512 + i) * 13 + 3) & 0xff));
  d.disc.read_ok = false;
  auto [failed_read, failed_status] = d.transact({0x08, 0, 0, 4, 1, 0}, 512);
  assert(failed_status == NSCSI_STATUS_GOOD &&
         failed_read == std::vector<uint8_t>(512, 0));
  nscsi_cdrom_get_status(d.cd, &s);
  assert(s.faults & NSCSI_CD_FAULT_BAD_MEDIA_READ);
  d.disc.read_ok = true;
  nscsi_cdrom_reset(d.cd);
  nscsi_cdrom_get_status(d.cd, &s);
  assert(s.block_size == 512);

  std::vector<uint8_t> audio_page(20, 0);
  audio_page[4] = 0x0e;
  audio_page[5] = 0x0e;
  audio_page[6] = 0x02;
  audio_page[13] = 128;
  audio_page[15] = 64;
  auto [page_data, page_status] =
      d.transact({0x15, 0, 0, 0, 20, 0}, 0, audio_page);
  assert(page_data.empty() && page_status == NSCSI_STATUS_GOOD);
  nscsi_cdrom_get_status(d.cd, &s);
  assert(s.cdda_sotc);
  assert(d.disc.gains[0] > 0.50f && d.disc.gains[0] < 0.51f);
  assert(d.disc.gains[1] > 0.25f && d.disc.gains[1] < 0.26f);
}

static void media_attention_and_cdda_callbacks_are_source_backed() {
  Direct d;
  d.disc.sequence++;
  auto [tur, status] = d.transact({0, 0, 0, 0, 0, 0}, 0);
  assert(tur.empty() && status == NSCSI_STATUS_CHECK_CONDITION);
  nscsi_cdrom_status_t s{};
  nscsi_cdrom_get_status(d.cd, &s);
  assert(s.sense_key == 0x06 && s.asc == 0);
  auto [tur2, status2] = d.transact({0, 0, 0, 0, 0, 0}, 0);
  assert(tur2.empty() && status2 == NSCSI_STATUS_GOOD);

  d.disc.audio = {1, 0, 0, 125};
  auto [subq, sub_status] =
      d.transact({0x42, 0x02, 0x40, 1, 0, 0, 0, 0, 16, 0}, 16);
  assert(sub_status == NSCSI_STATUS_GOOD && subq.size() == 16);
  assert(subq[1] == 0x11 && subq[4] == 1 && subq[6] == 2);
  assert(subq[8] == 0 && subq[9] == 0 && subq[10] == 1 && subq[11] == 50);
  assert(subq[12] == 0 && subq[13] == 0 && subq[14] == 0 && subq[15] == 25);

  auto [play_data, play_status] =
      d.transact({0x48, 0, 0, 0, 2, 1, 0, 2, 1, 0}, 0);
  assert(play_data.empty() && play_status == NSCSI_STATUS_GOOD);
  assert(d.disc.starts == 1 && d.disc.audio_start == 100 &&
         d.disc.audio_length == 100);
  auto [pause_data, pause_status] =
      d.transact({0x4b, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 0);
  assert(pause_data.empty() && pause_status == NSCSI_STATUS_GOOD);
  assert(d.disc.pauses == 1 && d.disc.audio.paused == 1);

  d.disc.inserted = false;
  d.disc.sequence++;
  auto [changed, changed_status] = d.transact({0, 0, 0, 0, 0, 0}, 0);
  assert(changed.empty() && changed_status == NSCSI_STATUS_CHECK_CONDITION);
  auto [missing, missing_status] = d.transact({0, 0, 0, 0, 0, 0}, 0);
  assert(missing.empty() && missing_status == NSCSI_STATUS_CHECK_CONDITION);
  nscsi_cdrom_get_status(d.cd, &s);
  assert(s.sense_key == 0x02 && s.asc == 0x3a);
}

static void large_reads_vendor_lengths_and_mode_pages() {
  Direct d;
  // READ(6)'s zero transfer field is 256 logical blocks, not an empty read.
  auto [read6, status6] = d.transact({0x08, 0, 0, 0, 0, 0}, 256 * 2048);
  assert(status6 == NSCSI_STATUS_GOOD && read6.size() == 256 * 2048);
  for (size_t n = 0; n < read6.size(); ++n)
    assert(read6[n] == uint8_t(((n / 2048) * 7 + (n % 2048) * 13 + 3) & 255));
  auto [read10, status10] = d.transact({0x28, 0, 0, 0, 0, 0, 0, 0, 33, 0}, 33 * 2048);
  assert(status10 == NSCSI_STATUS_GOOD && read10.size() == 33 * 2048);
  auto [empty_read, empty_status] = d.transact({0x28, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 0);
  assert(empty_read.empty() && empty_status == NSCSI_STATUS_GOOD);
  auto [pages, page_status] = d.transact({0x1a, 0, 0x3f, 0, 255, 0}, 129);
  assert(page_status == NSCSI_STATUS_GOOD && pages.size() == 129 && pages[0] == 128);
  assert(pages[12] == 0x31 && pages[13] == 42);
  assert(std::string(reinterpret_cast<char *>(pages.data() + 14), 41) ==
         "BlueSCSI is the BEST STOLEN FROM BLUESCSI");
  assert(pages[55] == 0 && pages[56] == 0x30 && pages[80] == 0x8e);
  auto [toc, toc_status] = d.transact({0x43, 0, 0, 0, 0, 0, 0xab, 0, 4, 0}, 4);
  // The source returns a valid four-byte header even with zero descriptors.
  assert(toc_status == NSCSI_STATUS_GOOD && toc == std::vector<uint8_t>({0, 2, 1, 2}));
  auto [toolbox, toolbox_status] = d.transact({0xd0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 0);
  assert(toolbox.empty() && toolbox_status == NSCSI_STATUS_CHECK_CONDITION);
  nscsi_cdrom_status_t s{}; nscsi_cdrom_get_status(d.cd, &s);
  assert(s.faults == NSCSI_CD_FAULT_UNSUPPORTED_TOOLBOX && s.target.cdb_length == 10);
  auto [ready, ready_status] = d.transact({0, 0, 0, 0, 0, 0}, 0);
  assert(ready.empty() && ready_status == NSCSI_STATUS_GOOD);
}

static void real_ncr_reads_source_supplied_sector_and_completes_scsi() {
  NscsiBus *bus = nscsi_bus_create();
  assert(bus);
  NcrWire wire{};
  wire.bus = bus;
  nscsi_bus_ops_t bus_ops{&wire, ncr_bus_changed};
  assert(nscsi_bus_attach(bus, 7, &bus_ops));
  ncr53cf96_bus_ops_t ncr_ops{};
  ncr_ops.opaque = &wire;
  ncr_ops.drive_ctrl = ncr_drive_ctrl;
  ncr_ops.drive_data = ncr_drive_data;
  ncr_ops.wait_ctrl = ncr_wait_ctrl;
  ncr_ops.irq = no_line;
  ncr_ops.drq = no_line;
  wire.ncr = ncr53cf96_create(16000000, &ncr_ops);
  assert(wire.ncr);
  Disc disc;
  nscsi_cdrom_config_t config{};
  config.bus = bus;
  config.scsi_id = 4;
  config.media = media_ops(&disc);
  NscsiCdrom *cd = nscsi_cdrom_create(&config);
  assert(cd);
  std::array<uint8_t, NSCSI_CD_SECTOR_BYTES> expected{};
  for (size_t i = 0; i < expected.size(); ++i)
    expected[i] = uint8_t((i * 13 + 3) & 0xff);
  const uint8_t cdb[10] = {0x28, 0, 0, 0, 0, 0, 0, 0, 1, 0};
  for (uint8_t v : cdb)
    ncr53cf96_write(wire.ncr, 2, v);
  ncr53cf96_write(wire.ncr, 8, 7);
  ncr53cf96_write(wire.ncr, 4, 4);
  ncr53cf96_write(wire.ncr, 3, 0x41);
  for (uint64_t cycle = 1; cycle <= 2600; ++cycle) {
    nscsi_cdrom_advance_to(cd, (cycle * 125) / 2);
    nscsi_bus_flush(bus);
    ncr53cf96_advance_to(wire.ncr, cycle);
    nscsi_bus_flush(bus);
    nscsi_cdrom_status_t cs{};
    nscsi_cdrom_get_status(cd, &cs);
    if (cs.target.cdb_length == sizeof(cdb) && cycle > 2200)
      break;
  }
  ncr53cf96_status_t ns{};
  ncr53cf96_get_status(wire.ncr, &ns);
  assert(ns.interrupt_status & 0x08);
  (void)ncr53cf96_read(wire.ncr, 5);
  ncr53cf96_write(wire.ncr, 11, 0x08);
  ncr53cf96_write(wire.ncr, 0, 0x00);
  ncr53cf96_write(wire.ncr, 1, 0x08);
  ncr53cf96_write(wire.ncr, 14, 0);
  ncr53cf96_write(wire.ncr, 3, 0x90);
  std::vector<uint8_t> actual;
  actual.reserve(NSCSI_CD_SECTOR_BYTES);
  for (uint64_t cycle = ns.cycles + 1;
       actual.size() < expected.size() && cycle < ns.cycles + 20000; ++cycle) {
    nscsi_cdrom_advance_to(cd, (cycle * 125) / 2);
    nscsi_bus_flush(bus);
    ncr53cf96_advance_to(wire.ncr, cycle);
    nscsi_bus_flush(bus);
    ncr53cf96_status_t current{};
    ncr53cf96_get_status(wire.ncr, &current);
    while (current.fifo_count && actual.size() < expected.size()) {
      actual.push_back(ncr53cf96_dma_read(wire.ncr));
      ncr53cf96_get_status(wire.ncr, &current);
    }
  }
  assert(actual == std::vector<uint8_t>(expected.begin(), expected.end()));
  assert(disc.reads == 1);
  ncr53cf96_get_status(wire.ncr, &ns);
  (void)ncr53cf96_read(wire.ncr, 5);
  ncr53cf96_write(wire.ncr, 0, 1);
  ncr53cf96_write(wire.ncr, 1, 0);
  ncr53cf96_write(wire.ncr, 14, 0);
  ncr53cf96_write(wire.ncr, 3, 0x90);
  for (uint64_t cycle = ns.cycles + 1; cycle < ns.cycles + 128; ++cycle) {
    nscsi_cdrom_advance_to(cd, (cycle * 125) / 2);
    nscsi_bus_flush(bus);
    ncr53cf96_advance_to(wire.ncr, cycle);
    nscsi_bus_flush(bus);
  }
  assert(ncr53cf96_dma_read(wire.ncr) == NSCSI_STATUS_GOOD);
  (void)ncr53cf96_read(wire.ncr, 5);
  ncr53cf96_write(wire.ncr, 3, 0x11); // CI_COMPLETE
  for (uint64_t cycle = ns.cycles + 128; cycle < ns.cycles + 256; ++cycle) {
    nscsi_cdrom_advance_to(cd, (cycle * 125) / 2);
    nscsi_bus_flush(bus);
    ncr53cf96_advance_to(wire.ncr, cycle);
    nscsi_bus_flush(bus);
  }
  assert(ncr53cf96_read(wire.ncr, 2) == NSCSI_MESSAGE_COMMAND_COMPLETE);
  ncr53cf96_write(wire.ncr, 3, 0x12); // CI_MSG_ACCEPT
  for (uint64_t cycle = ns.cycles + 256; cycle < ns.cycles + 384; ++cycle) {
    nscsi_cdrom_advance_to(cd, (cycle * 125) / 2);
    nscsi_bus_flush(bus);
    ncr53cf96_advance_to(wire.ncr, cycle);
    nscsi_bus_flush(bus);
  }
  ncr53cf96_get_status(wire.ncr, &ns);
  assert(ns.interrupt_status & 0x20);
  assert(nscsi_bus_ctrl(bus) == 0);
  nscsi_cdrom_destroy(cd);
  ncr53cf96_destroy(wire.ncr);
  nscsi_bus_destroy(bus);
}

static void creation_requires_media_callbacks_and_valid_block_geometry() {
  NscsiBus *bus = nscsi_bus_create();
  assert(bus);
  nscsi_cdrom_config_t config{};
  config.bus = bus;
  config.scsi_id = 4;
  assert(nscsi_cdrom_create(&config) == nullptr);
  Disc disc;
  config.media = media_ops(&disc);
  config.block_size = 300;
  assert(nscsi_cdrom_create(&config) == nullptr);
  nscsi_bus_destroy(bus);
}

int main() {
  cd_source_identity_and_local_protocol_commands();
  media_attention_and_cdda_callbacks_are_source_backed();
  large_reads_vendor_lengths_and_mode_pages();
  real_ncr_reads_source_supplied_sector_and_completes_scsi();
  creation_requires_media_callbacks_and_valid_block_geometry();
  std::puts("NSCSI CD-ROM target tests passed");
}
