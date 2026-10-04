/* license:BSD-3-Clause
 * copyright-holders:Philip Bennett
 */
#ifndef PCSX_ARCADE_TAITO_SOUND_COMM_H
#define PCSX_ARCADE_TAITO_SOUND_COMM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TaitoSoundComm TaitoSoundComm;

typedef struct TaitoSoundCommConfig {
    /* TC0140SYT/PC060HA callbacks. Omitted callbacks are disconnected. */
    void (*sound_nmi_line)(void *opaque, int asserted);
    void (*sound_reset_line)(void *opaque, int asserted);
    /* Request the MAME scheduler barrier before a master comm access. The
     * host services the slave AFTER the current bus operation returns; this
     * callback must not run the slave inline or reenter the device. */
    void (*request_synchronization)(void *opaque);
    void *opaque;
} TaitoSoundCommConfig;

enum {
    TAITO_SOUND_COMM_OK = 0,
    TAITO_SOUND_COMM_ARGUMENT = -1,
    TAITO_SOUND_COMM_NOMEM = -2
};

int taito_sound_comm_create(const TaitoSoundCommConfig *config,
                            TaitoSoundComm **out_device);
void taito_sound_comm_destroy(TaitoSoundComm *device);

/* Device reset clears internal state without driving either callback, as in
 * MAME's device_reset implementation. */
void taito_sound_comm_reset(TaitoSoundComm *device);

/* The master is normally the game CPU; the slave is the sound CPU. All data
 * writes are four-bit values. The independent mode selectors are masked to
 * their low nibble. */
void taito_sound_comm_master_port_write(TaitoSoundComm *device, uint8_t value);
void taito_sound_comm_master_comm_write(TaitoSoundComm *device, uint8_t value);
int taito_sound_comm_master_comm_read(TaitoSoundComm *device,
                                      int side_effects_enabled,
                                      uint8_t *value);
void taito_sound_comm_slave_port_write(TaitoSoundComm *device, uint8_t value);
void taito_sound_comm_slave_comm_write(TaitoSoundComm *device, uint8_t value);
int taito_sound_comm_slave_comm_read(TaitoSoundComm *device,
                                     int side_effects_enabled,
                                     uint8_t *value);

/* Status bits: 0/1 mean a master-to-slave packet is full, 2/3 mean a
 * slave-to-master packet is full. These peek helpers do not alter state. */
uint8_t taito_sound_comm_status(const TaitoSoundComm *device);
int taito_sound_comm_nmi_enabled(const TaitoSoundComm *device);

#ifdef __cplusplus
}
#endif
#endif
