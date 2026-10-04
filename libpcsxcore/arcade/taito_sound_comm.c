/* license:BSD-3-Clause
 * copyright-holders:Philip Bennett
 */
#include "taito_sound_comm.h"

#include <stdlib.h>
#include <string.h>

#define PORT01_FULL        0x01u
#define PORT23_FULL        0x02u
#define PORT01_FULL_MASTER 0x04u
#define PORT23_FULL_MASTER 0x08u

struct TaitoSoundComm {
    TaitoSoundCommConfig config;
    uint8_t mainmode;
    uint8_t submode;
    uint8_t status;
    uint8_t nmi_enabled;
    uint8_t slavedata[4];
    uint8_t masterdata[4];
};

static void update_nmi(TaitoSoundComm *device)
{
    uint8_t pending = device->status & (PORT23_FULL | PORT01_FULL);
    if (device->config.sound_nmi_line != NULL)
        device->config.sound_nmi_line(device->config.opaque,
                                      pending != 0 && device->nmi_enabled != 0);
}

static void service_slave(TaitoSoundComm *device)
{
    if (device->config.request_synchronization != NULL)
        device->config.request_synchronization(device->config.opaque);
}

int taito_sound_comm_create(const TaitoSoundCommConfig *config,
                            TaitoSoundComm **out_device)
{
    TaitoSoundComm *device;
    if (out_device == NULL)
        return TAITO_SOUND_COMM_ARGUMENT;
    *out_device = NULL;
    device = (TaitoSoundComm *)calloc(1, sizeof(*device));
    if (device == NULL)
        return TAITO_SOUND_COMM_NOMEM;
    if (config != NULL)
        device->config = *config;
    *out_device = device;
    return TAITO_SOUND_COMM_OK;
}

void taito_sound_comm_destroy(TaitoSoundComm *device)
{
    free(device);
}

void taito_sound_comm_reset(TaitoSoundComm *device)
{
    if (device == NULL)
        return;
    device->mainmode = 0;
    device->submode = 0;
    device->status = 0;
    device->nmi_enabled = 0;
    memset(device->slavedata, 0, sizeof(device->slavedata));
    memset(device->masterdata, 0, sizeof(device->masterdata));
}

void taito_sound_comm_master_port_write(TaitoSoundComm *device, uint8_t value)
{
    if (device != NULL)
        device->mainmode = value & 0x0fu;
}

void taito_sound_comm_master_comm_write(TaitoSoundComm *device, uint8_t value)
{
    if (device == NULL)
        return;
    service_slave(device);
    value &= 0x0fu;
    switch (device->mainmode) {
    case 0:
    case 2:
        device->slavedata[device->mainmode++] = value;
        break;
    case 1:
        device->slavedata[device->mainmode++] = value;
        device->status |= PORT01_FULL;
        update_nmi(device);
        break;
    case 3:
        device->slavedata[device->mainmode++] = value;
        device->status |= PORT23_FULL;
        update_nmi(device);
        break;
    case 4:
        if (device->config.sound_reset_line != NULL)
            device->config.sound_reset_line(device->config.opaque, value != 0);
        break;
    default:
        break;
    }
}

int taito_sound_comm_master_comm_read(TaitoSoundComm *device,
                                      int side_effects_enabled,
                                      uint8_t *value)
{
    if (device == NULL || value == NULL)
        return TAITO_SOUND_COMM_ARGUMENT;
    if (side_effects_enabled)
        service_slave(device);
    *value = 0;
    switch (device->mainmode) {
    case 0:
    case 2:
        *value = device->masterdata[device->mainmode];
        if (side_effects_enabled)
            ++device->mainmode;
        break;
    case 1:
        *value = device->masterdata[1];
        if (side_effects_enabled) {
            device->status &= (uint8_t)~PORT01_FULL_MASTER;
            ++device->mainmode;
        }
        break;
    case 3:
        *value = device->masterdata[3];
        if (side_effects_enabled) {
            device->status &= (uint8_t)~PORT23_FULL_MASTER;
            ++device->mainmode;
        }
        break;
    case 4:
        *value = device->status;
        break;
    default:
        break;
    }
    return TAITO_SOUND_COMM_OK;
}

void taito_sound_comm_slave_port_write(TaitoSoundComm *device, uint8_t value)
{
    if (device != NULL)
        device->submode = value & 0x0fu;
}

void taito_sound_comm_slave_comm_write(TaitoSoundComm *device, uint8_t value)
{
    if (device == NULL)
        return;
    value &= 0x0fu;
    switch (device->submode) {
    case 0:
    case 2:
        device->masterdata[device->submode++] = value;
        break;
    case 1:
        device->masterdata[device->submode++] = value;
        device->status |= PORT01_FULL_MASTER;
        break;
    case 3:
        device->masterdata[device->submode++] = value;
        device->status |= PORT23_FULL_MASTER;
        break;
    case 4:
        break;
    case 5:
        device->nmi_enabled = 0;
        update_nmi(device);
        break;
    case 6:
        device->nmi_enabled = 1;
        update_nmi(device);
        break;
    default:
        break;
    }
}

int taito_sound_comm_slave_comm_read(TaitoSoundComm *device,
                                     int side_effects_enabled,
                                     uint8_t *value)
{
    if (device == NULL || value == NULL)
        return TAITO_SOUND_COMM_ARGUMENT;
    *value = 0;
    switch (device->submode) {
    case 0:
    case 2:
        *value = device->slavedata[device->submode];
        if (side_effects_enabled)
            ++device->submode;
        break;
    case 1:
        *value = device->slavedata[1];
        if (side_effects_enabled) {
            device->status &= (uint8_t)~PORT01_FULL;
            ++device->submode;
            update_nmi(device);
        }
        break;
    case 3:
        *value = device->slavedata[3];
        if (side_effects_enabled) {
            device->status &= (uint8_t)~PORT23_FULL;
            ++device->submode;
            update_nmi(device);
        }
        break;
    case 4:
        *value = device->status;
        break;
    default:
        break;
    }
    return TAITO_SOUND_COMM_OK;
}

uint8_t taito_sound_comm_status(const TaitoSoundComm *device)
{
    return device == NULL ? 0 : device->status;
}

int taito_sound_comm_nmi_enabled(const TaitoSoundComm *device)
{
    return device != NULL && device->nmi_enabled != 0;
}
