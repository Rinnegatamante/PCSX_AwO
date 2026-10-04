// SPDX-License-Identifier: BSD-3-Clause
// copyright-holders:smf
// Standalone adapters for MAME X76F041, X76F100 and DS2401.
#include "konami_security.h"
#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <new>
using std::memcmp;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "konami_security_x76.inc"
#pragma GCC diagnostic pop
struct KonamiSecureFlash {
    unsigned type;
    X76F041 a;
    X76F100 b;
};
struct KonamiDS2401 {
    enum { IDLE, RESET, RESET1, RESET2, COMMAND, READROM };
    int state = IDLE, bit = 0, shift = 0;
    unsigned byte = 0;
    bool rx = true, tx = true;
    uint8_t data[8] = {};
    uint64_t now = 0, main_deadline = UINT64_MAX, reset_deadline = UINT64_MAX;
    void main_tick() {
        switch (state) {
        case RESET1:
            tx = false;
            state = RESET2;
            main_deadline = now + 120;
            break;
        case RESET2:
            tx = true;
            bit = 0;
            shift = 0;
            state = COMMAND;
            break;
        case COMMAND:
            shift = (shift >> 1) | (rx ? 0x80 : 0);
            bit++;
            if (bit == 8) {
                if (shift == 0x33 || shift == 0x0f) {
                    bit = 0;
                    byte = 0;
                    state = READROM;
                } else
                    state = IDLE;
            }
            break;
        case READROM:
            tx = true;
            if (byte == 8)
                state = IDLE;
            break;
        default:
            break;
        }
    }
};

static size_t image_size(unsigned type) { return type == 41 ? 548 : type == 100 ? 132 : 0; }
template <typename Chip> static void copy_image(Chip &c, uint8_t *out) {
    std::memcpy(out, c.m_response_to_reset, 4);
    std::memcpy(out + 4, c.m_write_password, 8);
    std::memcpy(out + 12, c.m_read_password, 8);
}
extern "C" {
KonamiSecureFlash *konami_secure_flash_create(unsigned type, const uint8_t *image, size_t size) {
    if (!image || !image_size(type) || size != image_size(type))
        return nullptr;
    auto f = new (std::nothrow) KonamiSecureFlash{};
    if (!f)
        return nullptr;
    f->type = type;
    if (type == 41) {
        auto &c = f->a;
        std::memcpy(c.m_response_to_reset, image, 4);
        std::memcpy(c.m_write_password, image + 4, 8);
        std::memcpy(c.m_read_password, image + 12, 8);
        std::memcpy(c.m_configuration_password, image + 20, 8);
        std::memcpy(c.m_configuration_registers, image + 28, 8);
        std::memcpy(c.m_data, image + 36, 512);
    } else {
        auto &c = f->b;
        std::memcpy(c.m_response_to_reset, image, 4);
        std::memcpy(c.m_write_password, image + 4, 8);
        std::memcpy(c.m_read_password, image + 12, 8);
        std::memcpy(c.m_data, image + 20, 112);
    }
    konami_secure_flash_reset(f);
    return f;
}
void konami_secure_flash_destroy(KonamiSecureFlash *f) { delete f; }
void konami_secure_flash_reset(KonamiSecureFlash *f) {
    if (f) {
        if (f->type == 41)
            f->a.device_reset();
        else
            f->b.device_reset();
    }
}
void konami_secure_flash_cs(KonamiSecureFlash *f, int v) {
    if (f) {
        if (f->type == 41)
            f->a.write_cs(v != 0);
        else
            f->b.write_cs(v != 0);
    }
}
void konami_secure_flash_rst(KonamiSecureFlash *f, int v) {
    if (f) {
        if (f->type == 41)
            f->a.write_rst(v != 0);
        else
            f->b.write_rst(v != 0);
    }
}
void konami_secure_flash_scl(KonamiSecureFlash *f, int v) {
    if (f) {
        if (f->type == 41)
            f->a.write_scl(v != 0);
        else
            f->b.write_scl(v != 0);
    }
}
void konami_secure_flash_sda(KonamiSecureFlash *f, int v) {
    if (f) {
        if (f->type == 41)
            f->a.write_sda(v != 0);
        else
            f->b.write_sda(v != 0);
    }
}
int konami_secure_flash_read_sda(KonamiSecureFlash *f) {
    if (!f)
        return 1;
    return f->type == 41 ? f->a.read_sda() : f->b.read_sda();
}
int konami_secure_flash_get_image(const KonamiSecureFlash *f, uint8_t *out, size_t size) {
    if (!f || !out || size != image_size(f->type))
        return -1;
    if (f->type == 41) {
        auto &c = f->a;
        copy_image(c, out);
        std::memcpy(out + 20, c.m_configuration_password, 8);
        std::memcpy(out + 28, c.m_configuration_registers, 8);
        std::memcpy(out + 36, c.m_data, 512);
    } else {
        copy_image(f->b, out);
        std::memcpy(out + 20, f->b.m_data, 112);
    }
    return 0;
}
void konami_secure_flash_get_status(const KonamiSecureFlash *f, KonamiSecureFlashStatus *s) {
    if (!f || !s)
        return;
    std::memset(s, 0, sizeof(*s));
    s->type = f->type;
#define STATUS(c)                                                                                  \
    s->cs = c.m_cs;                                                                                \
    s->rst = c.m_rst;                                                                              \
    s->scl = c.m_scl;                                                                              \
    s->sda_in = c.m_sdaw;                                                                          \
    s->sda_out = c.m_sdar;                                                                         \
    s->state = c.m_state;                                                                          \
    s->bit = c.m_bit;                                                                              \
    s->byte = c.m_byte;                                                                            \
    s->command = c.m_command;                                                                      \
    s->password_matched = c.m_is_password_accepted
    if (f->type == 41) {
        auto &c = f->a;
        STATUS(c);
        s->address = c.m_address;
        s->password_retry_counter = c.m_configuration_registers[4];
    } else {
        auto &c = f->b;
        STATUS(c);
        s->password_retry_counter = c.m_password_retry_counter;
    }
#undef STATUS
}
int konami_ds2401_validate_image(const uint8_t *image, size_t size) {
    if (!image || size != 8)
        return -1;
    uint8_t crc = 0;
    for (int index = 7; index >= 1; index--) {
        uint8_t byte = image[index];
        for (unsigned bit = 0; bit < 8; bit++) {
            unsigned mix = (crc ^ byte) & 1;
            crc >>= 1;
            if (mix)
                crc ^= 0x8c;
            byte >>= 1;
        }
    }
    return crc == image[0];
}
KonamiDS2401 *konami_ds2401_create(const uint8_t *image, size_t size) {
    if (!image || size != 8)
        return nullptr;
    auto c = new (std::nothrow) KonamiDS2401;
    if (c)
        std::memcpy(c->data, image, 8);
    return c;
}
void konami_ds2401_destroy(KonamiDS2401 *c) { delete c; }
void konami_ds2401_reset(KonamiDS2401 *c) {
    if (!c)
        return;
    uint8_t data[8];
    std::memcpy(data, c->data, 8);
    *c = KonamiDS2401{};
    std::memcpy(c->data, data, 8);
}
int konami_ds2401_advance_to_us(KonamiDS2401 *c, uint64_t next) {
    // Leave enough headroom for the longest hardware timer.
    if (!c || next < c->now || next > UINT64_MAX - 480)
        return -1;
    for (;;) {
        uint64_t event = std::min(c->main_deadline, c->reset_deadline);
        if (event > next)
            break;
        c->now = event;
        if (c->reset_deadline == event) {
            c->reset_deadline = UINT64_MAX;
            c->state = KonamiDS2401::RESET;
        }
        if (c->main_deadline == event) {
            c->main_deadline = UINT64_MAX;
            c->main_tick();
        }
    }
    c->now = next;
    return 0;
}
int konami_ds2401_write_pin(KonamiDS2401 *c, int level, uint64_t now) {
    if (konami_ds2401_advance_to_us(c, now))
        return -1;
    bool v = level != 0;
    if (!v && c->rx) {
        if (c->state == KonamiDS2401::COMMAND)
            c->main_deadline = now + 30;
        else if (c->state == KonamiDS2401::READROM) {
            if (c->byte >= 8) {
                c->state = KonamiDS2401::IDLE;
                c->tx = true;
                c->reset_deadline = now + 480;
                c->rx = v;
                return 0;
            }
            if (!c->bit)
                c->shift = c->data[7 - c->byte];
            c->tx = (c->shift & 1) != 0;
            c->shift >>= 1;
            if (++c->bit == 8) {
                c->bit = 0;
                c->byte++;
            }
            c->main_deadline = now + 30;
        }
        c->reset_deadline = now + 480;
    } else if (v && !c->rx) {
        if (c->state == KonamiDS2401::RESET) {
            c->state = KonamiDS2401::RESET1;
            c->main_deadline = now + 30;
        }
        c->reset_deadline = UINT64_MAX;
    }
    c->rx = v;
    return 0;
}
int konami_ds2401_read_pin(KonamiDS2401 *c, uint64_t now) {
    if (konami_ds2401_advance_to_us(c, now))
        return -1;
    return c->tx && c->rx;
}
int konami_ds2401_get_image(const KonamiDS2401 *c, uint8_t *out, size_t size) {
    if (!c || !out || size != 8)
        return -1;
    std::memcpy(out, c->data, 8);
    return 0;
}
}
