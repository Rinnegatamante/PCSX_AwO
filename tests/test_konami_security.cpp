// SPDX-License-Identifier: BSD-3-Clause
#include "../libpcsxcore/arcade/konami_security.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
static KonamiSecureFlashStatus status(KonamiSecureFlash *f) {
    KonamiSecureFlashStatus s{};
    konami_secure_flash_get_status(f, &s);
    return s;
}
struct Master {
    KonamiSecureFlash *flash;
    explicit Master(KonamiSecureFlash *f) : flash(f) {}
    void scl(int v) { konami_secure_flash_scl(flash, v); }
    void sda(int v) { konami_secure_flash_sda(flash, v); }
    void start() {
        scl(0);
        sda(1);
        scl(1);
        sda(0);
        scl(0);
    }
    void stop() {
        scl(0);
        sda(0);
        scl(1);
        sda(1);
        scl(0);
    }
    bool send(unsigned byte) {
        for (int i = 7; i >= 0; i--) {
            scl(0);
            sda((byte >> i) & 1);
            scl(1);
            scl(1);
            scl(0);
        } // repeatedclock1 is not an edge
        sda(1);
        scl(1);
        bool ack = konami_secure_flash_read_sda(flash) == 0;
        scl(0);
        return ack;
    }
    unsigned read(bool ack = true) {
        unsigned value = 0;
        scl(0);
        sda(1);
        for (unsigned i = 0; i < 8; i++) {
            scl(1);
            value = (value << 1) | konami_secure_flash_read_sda(flash);
            scl(0);
        }
        sda(!ack);
        scl(1);
        scl(0);
        return value;
    }
    bool authenticate(unsigned type, unsigned command, unsigned address, const uint8_t *password,
                      bool repeated_start = true) {
        start();
        assert(send(command));
        if (type == 41 && !send(address))
            return false;
        for (unsigned i = 0; i < 8; i++)
            assert(send(password[i]));
        if (repeated_start)
            start();
        return send(type == 41 ? 0xc0 : 0x55);
    }
};
static std::vector<uint8_t> image(unsigned type) {
    std::vector<uint8_t> bytes(type == 41 ? 548 : 132);
    bytes[0] = 0x19;
    bytes[1] = 0x55;
    bytes[2] = 0xaa;
    bytes[3] = 0x71;
    for (unsigned i = 0; i < 8; i++) {
        bytes[4 + i] = uint8_t(0xa0 + i);
        bytes[12 + i] = uint8_t(0xb0 + i);
        if (type == 41)
            bytes[20 + i] = uint8_t(0xc0 + i);
    }
    if (type == 41) {
        bytes[28] = bytes[29] = 0xcc;
        bytes[30] = 0x0c;
        bytes[31] = 0xff;
    } // bothread/write passwords enabled
    unsigned base = type == 41 ? 36 : 20;
    for (unsigned i = base; i < bytes.size(); i++)
        bytes[i] = uint8_t((i - base) * 7 + 3);
    return bytes;
}
static std::vector<uint8_t> exported(KonamiSecureFlash *f, unsigned type) {
    std::vector<uint8_t> b(type == 41 ? 548 : 132);
    assert(konami_secure_flash_get_image(f, b.data(), b.size()) == 0);
    return b;
}
static void common_flash_tests(unsigned type) {
    auto bytes = image(type);
    auto f = konami_secure_flash_create(type, bytes.data(), bytes.size());
    assert(f);
    Master m(f);
    assert(!konami_secure_flash_create(type, nullptr, bytes.size()));
    assert(!konami_secure_flash_create(type, bytes.data(), bytes.size() - 1));
    konami_secure_flash_cs(f, 1);
    assert(konami_secure_flash_read_sda(f) == 1);
    m.start();
    m.send(0xff);
    konami_secure_flash_cs(f, 0);
    konami_secure_flash_rst(f, 1);
    // Reset response shifts on falling SCL, LSB first; repeats after four bytes.
    for (unsigned byte = 0; byte < 5; byte++) {
        unsigned response = 0;
        for (unsigned bit = 0; bit < 8; bit++) {
            m.scl(1);
            m.scl(0);
            response |= konami_secure_flash_read_sda(f) << bit;
        }
        assert(response == bytes[byte % 4]);
    }
    konami_secure_flash_rst(f, 0);
    m.stop();
    uint8_t bad[8] = {};
    unsigned read_command = type == 41 ? 0x20 : 0x81;
    assert(!m.authenticate(type, read_command, 0, bad));
    assert(!status(f).password_matched);
    assert(status(f).password_retry_counter == 1);
    for (unsigned i = 0; i < 3; i++)
        assert(!m.send(type == 41 ? 0xc0 : 0x55));
    assert(status(f).password_retry_counter == 1);
    m.stop();
    assert(m.authenticate(type, read_command, 0, bytes.data() + 12));
    assert(status(f).password_matched);
    assert(status(f).password_retry_counter == 0);
    unsigned base = type == 41 ? 36 : 20;
    for (unsigned i = 0; i < 8; i++)
        assert(m.read() == bytes[base + i]);
    m.stop();
    // Host NACK leaves the address unchanged in the inherited state machine.
    assert(m.authenticate(type, read_command, 0, bytes.data() + 12));
    unsigned first = m.read(false);
    assert(m.read() == first);
    m.stop();
    // Auth without repeated START is bounded and begins at data byte0.
    assert(m.authenticate(type, read_command, 0, bytes.data() + 12, false));
    assert(m.read() == bytes[base]);
    m.stop();
    unsigned write_command = type == 41 ? 0x00 : 0x80;
    assert(m.authenticate(type, write_command, 0, bytes.data() + 4));
    for (unsigned i = 0; i < 7; i++)
        assert(m.send(0xf0 + i));
    assert(exported(f, type)[base] == bytes[base]);
    assert(m.send(0xf7));
    m.stop();
    auto updated = exported(f, type);
    for (unsigned i = 0; i < 8; i++)
        assert(updated[base + i] == 0xf0 + i);
    konami_secure_flash_reset(f);
    assert(exported(f, type) == updated); // pinresetpreservesnewdata
    konami_secure_flash_destroy(f);
    std::printf(
        "X76F%03u resetresponse/authfail-pass/retry/NACK/SCLedges/pagewrite/persistence passed\n",
        type);
}
static void x100_tests() {
    auto bytes = image(100);
    auto f = konami_secure_flash_create(100, bytes.data(), bytes.size());
    Master m(f);
    // Sector13 is last validsector; 14/15 do not read/write outside112bytes.
    assert(m.authenticate(100, 0x81 | (13 << 1), 0, bytes.data() + 12));
    for (unsigned i = 0; i < 8; i++)
        assert(m.read() == bytes[20 + 104 + i]);
    m.stop();
    assert(m.authenticate(100, 0x81 | (14 << 1), 0, bytes.data() + 12));
    assert(m.read() == 0);
    m.stop();
    assert(m.authenticate(100, 0x80 | (15 << 1), 0, bytes.data() + 4));
    for (unsigned i = 0; i < 8; i++)
        assert(m.send(0xff));
    m.stop();
    assert(exported(f, 100) == bytes);
    // Change read password requires writepassword and commits exactlyeightbytes.
    const uint8_t replacement[8] = {8, 7, 6, 5, 4, 3, 2, 1};
    assert(m.authenticate(100, 0xfe, 0, bytes.data() + 4));
    for (auto v : replacement)
        assert(m.send(v));
    m.stop();
    assert(std::memcmp(exported(f, 100).data() + 12, replacement, 8) == 0);
    assert(!m.authenticate(100, 0x81, 0, bytes.data() + 12));
    m.stop();
    assert(m.authenticate(100, 0x81, 0, replacement));
    m.stop();
    konami_secure_flash_destroy(f);
    // Eightfailedpasswordattempts trigger actual upstream destructiveerasepolicy.
    f = konami_secure_flash_create(100, bytes.data(), bytes.size());
    Master retry(f);
    uint8_t bad[8] = {};
    for (unsigned i = 0; i < 8; i++) {
        assert(!retry.authenticate(100, 0x81, 0, bad));
        retry.stop();
    }
    auto erased = exported(f, 100);
    for (unsigned i = 4; i < erased.size(); i++)
        assert(erased[i] == 0);
    assert(status(f).password_retry_counter == 0);
    konami_secure_flash_destroy(f);
}
static void x041_tests() {
    auto bytes = image(41);
    auto f = konami_secure_flash_create(41, bytes.data(), bytes.size());
    Master m(f);
    // Commandbit0 selects second256bytes; each128bytearray wraps locally.
    assert(m.authenticate(41, 0x21, 0x7e, bytes.data() + 12));
    assert(m.read() == bytes[36 + 382]);
    assert(m.read() == bytes[36 + 383]);
    assert(m.read() == bytes[36 + 256]);
    m.stop();
    // Read actualconfigurationbytes underconfigurationpassword.
    assert(m.authenticate(41, 0x80, 0x60, bytes.data() + 20));
    for (unsigned i = 0; i < 8; i++)
        assert(m.read() == bytes[28 + i]);
    m.stop();
    // Configurationpasswordwrite supports arbitrarylength, not onlyfullpages.
    assert(m.authenticate(41, 0x40, 0x10, bytes.data() + 20));
    assert(m.send(0x91));
    assert(m.send(0x92));
    m.stop();
    auto updated = exported(f, 41);
    assert(updated[36 + 16] == 0x91 && updated[36 + 17] == 0x92);
    // Passwordprogram requires two identicalcopies and actualexistingreadpassword.
    const uint8_t replacement[8] = {1, 3, 5, 7, 9, 11, 13, 15};
    assert(m.authenticate(41, 0x80, 0x10, bytes.data() + 12));
    for (unsigned copy = 0; copy < 2; copy++)
        for (auto v : replacement)
            assert(m.send(v));
    m.stop();
    assert(std::memcmp(exported(f, 41).data() + 12, replacement, 8) == 0);
    assert(!m.authenticate(41, 0x20, 0, bytes.data() + 12));
    m.stop();
    assert(m.authenticate(41, 0x20, 0, replacement));
    m.stop();
    konami_secure_flash_destroy(f);
    // Program-only mode mustrejectbitclears evenwhennumericnewbyteisgreater.
    bytes = image(41);
    bytes[28] = 0x0d;
    for (unsigned i = 0; i < 8; i++)
        bytes[36 + i] = 7;
    f = konami_secure_flash_create(41, bytes.data(), bytes.size());
    Master program(f);
    assert(program.authenticate(41, 0, 0, bytes.data() + 4));
    for (unsigned i = 0; i < 7; i++)
        assert(program.send(8));
    assert(!program.send(8));
    program.stop();
    assert(exported(f, 41) == bytes);
    assert(program.authenticate(41, 0, 0, bytes.data() + 4));
    for (unsigned i = 0; i < 8; i++)
        assert(program.send(15));
    program.stop();
    for (unsigned i = 0; i < 8; i++)
        assert(exported(f, 41)[36 + i] == 15);
    konami_secure_flash_destroy(f);
    // Configurationregisterwrites and reset/masserasecommands mutate the actualimage.
    bytes = image(41);
    f = konami_secure_flash_create(41, bytes.data(), bytes.size());
    Master config(f);
    const uint8_t registers[8] = {0xcc, 0xcc, 8, 0x7f, 0, 0x11, 0x22, 0x33};
    assert(config.authenticate(41, 0x80, 0x50, bytes.data() + 20));
    for (auto v : registers)
        assert(config.send(v));
    config.stop();
    assert(std::memcmp(exported(f, 41).data() + 28, registers, 8) == 0);
    config.start();
    assert(config.send(0x80));
    assert(config.send(0x30));
    assert(config.send(0));
    config.stop();
    for (unsigned i = 4; i < 12; i++)
        assert(exported(f, 41)[i] == 0);
    config.start();
    assert(config.send(0x80));
    assert(config.send(0x80));
    assert(config.send(0));
    config.stop();
    auto mass_erased = exported(f, 41);
    for (unsigned i = 4; i < mass_erased.size(); i++)
        assert(mass_erased[i] == 0xff);
    assert(std::memcmp(mass_erased.data(), bytes.data(), 4) == 0);
    konami_secure_flash_destroy(f);
    // Configuredretrylock refuses normalcommands, preservesconfigurationaccess.
    bytes = image(41);
    bytes[30] = 4;
    bytes[31] = 1;
    f = konami_secure_flash_create(41, bytes.data(), bytes.size());
    Master lock(f);
    uint8_t bad[8] = {};
    assert(!lock.authenticate(41, 0x20, 0, bad));
    lock.stop();
    lock.start();
    assert(lock.send(0x20));
    assert(!lock.send(0));
    lock.stop();
    assert(lock.authenticate(41, 0x80, 0x60, bytes.data() + 20));
    assert(lock.read() == bytes[28]);
    lock.stop();
    konami_secure_flash_destroy(f);
}
struct WireMaster {
    KonamiDS2401 *chip;
    uint64_t now = 0;
    explicit WireMaster(KonamiDS2401 *c) : chip(c) {}
    void pin(int v) { assert(konami_ds2401_write_pin(chip, v, now) == 0); }
    void advance(unsigned us) {
        now += us;
        assert(konami_ds2401_advance_to_us(chip, now) == 0);
    }
    int read() { return konami_ds2401_read_pin(chip, now); }
    void reset_bus() {
        pin(0);
        advance(480);
        pin(1);
        assert(read() == 1);
        advance(29);
        assert(read() == 1);
        advance(1);
        assert(read() == 0);
        advance(119);
        assert(read() == 0);
        advance(1);
        assert(read() == 1);
    }
    void write_bit(unsigned bit) {
        pin(0);
        advance(bit ? 6 : 60);
        pin(1);
        advance(bit ? 64 : 10);
    }
    void write_byte(unsigned b) {
        for (unsigned i = 0; i < 8; i++)
            write_bit((b >> i) & 1);
    }
    unsigned read_bit() {
        pin(0);
        assert(read() == 0);
        advance(1);
        pin(1);
        advance(14);
        unsigned bit = read();
        advance(15);
        assert(read() == 1);
        advance(40);
        return bit;
    }
    unsigned read_byte() {
        unsigned b = 0;
        for (unsigned i = 0; i < 8; i++)
            b |= read_bit() << i;
        return b;
    }
};
static void ds2401_tests() {
    const uint8_t rom[8] = {0x75, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x01};
    assert(konami_ds2401_validate_image(rom, 8) == 1);
    uint8_t bad_crc[8];
    std::memcpy(bad_crc, rom, 8);
    bad_crc[0] ^= 1;
    assert(konami_ds2401_validate_image(bad_crc, 8) == 0);
    assert(konami_ds2401_validate_image(rom, 7) == -1);
    auto c = konami_ds2401_create(rom, 8);
    assert(c);
    assert(!konami_ds2401_create(nullptr, 8));
    assert(!konami_ds2401_create(rom, 7));
    WireMaster m{c};
    m.pin(0);
    m.advance(479);
    m.pin(1);
    m.advance(151);
    assert(m.read() == 1); // shortresetdoesnotcausepresence
    m.reset_bus();
    m.write_byte(0x33);
    for (unsigned i = 0; i < 8; i++)
        assert(m.read_byte() == rom[7 - i]);
    assert(m.read_bit() == 1);
    uint8_t exported_rom[8];
    assert(konami_ds2401_get_image(c, exported_rom, 8) == 0 &&
           std::memcmp(rom, exported_rom, 8) == 0);
    assert(konami_ds2401_read_pin(c, m.now - 1) == -1);
    assert(konami_ds2401_write_pin(c, 0, m.now - 1) == -1);
    assert(m.read() == 1);
    m.reset_bus();
    m.write_byte(0x0f);
    for (unsigned i = 0; i < 8; i++)
        assert(m.read_byte() == rom[7 - i]);
    m.reset_bus();
    m.write_byte(0x55);
    assert(m.read_bit() == 1); // unsupportedcommanddoesnotinventresponse
    konami_ds2401_reset(c);
    m.now = 0;
    m.reset_bus();
    m.write_byte(0x33);
    assert(m.read_byte() == rom[7]);
    konami_ds2401_destroy(c);
    std::puts("DS2401 resetthreshold/presencedelays/ROM33-0F/bitwindow/clockrewind/ROMpersistence "
              "passed");
}
int main() {
    common_flash_tests(41);
    common_flash_tests(100);
    x041_tests();
    x100_tests();
    ds2401_tests();
}
