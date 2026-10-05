// license:BSD-3-Clause
// copyright-holders:smf
/***************************************************************************

    idehd.h

    IDE hard disk

***************************************************************************/

#ifndef PCSX_ATASTORAGE_PORT_H
#define PCSX_ATASTORAGE_PORT_H

#pragma once

#include "ata_mame_compat.h"

#include "atahle_port.h"






class ata_mass_storage_device_base : public ata_hle_device_base
{
public:
	uint16_t *identify_device_buffer() { return m_identify_buffer; }

	void set_master_password(const uint8_t *password)
	{
		m_master_password = password;
		m_master_password_enable = (password != nullptr);
	}

	void set_user_password(const uint8_t *password)
	{
		m_user_password = password;
		m_user_password_enable = (password != nullptr);
	}

	void set_dma_transfer_time(const attotime time) { m_dma_transfer_time = time; }

protected:
	ata_mass_storage_device_base();

	virtual void device_start() override ATTR_COLD;

	virtual int read_sector(uint32_t lba, void *buffer) = 0;
	virtual int write_sector(uint32_t lba, const void *buffer) = 0;
	virtual attotime seek_time();

	virtual void ide_build_identify_device();

	static const int IDE_DISK_SECTOR_SIZE = 512;
	virtual int sector_length() override { return IDE_DISK_SECTOR_SIZE; }
	virtual void process_buffer() override;
	virtual void fill_buffer() override;
	virtual bool is_ready() override { return true; }
	virtual void process_command() override;
	virtual void finished_command() override;
	virtual void perform_diagnostic() override;
	virtual void signature() override;

	int m_can_identify_device;
	uint16_t          m_num_cylinders;
	uint8_t           m_num_sectors;
	uint8_t           m_num_heads;

	virtual uint32_t lba_address();

private:
	void set_geometry(uint8_t sectors, uint8_t heads) { m_num_sectors = sectors; m_num_heads = heads; }
	void finished_read();
	void finished_write();
	void next_sector();
	void security_error();
	void read_first_sector();
	void soft_reset() override;

	uint32_t          m_cur_lba;
	uint16_t          m_block_count;
	uint16_t          m_sectors_until_int;

	uint8_t           m_master_password_enable;
	uint8_t           m_user_password_enable;
	const uint8_t *   m_master_password;
	const uint8_t *   m_user_password;
	// DMA data transfer time for 1 sector
	attotime          m_dma_transfer_time;
};

// ======================> cf_device_base

class cf_device_base : public ata_mass_storage_device_base
{
protected:
	// construction/destruction
	cf_device_base();

	// device_t implementation

	virtual void ide_build_identify_device() override;
	virtual attotime seek_time() override;
	virtual uint8_t calculate_status() override { return ata_hle_device_base::calculate_status(); }
};

#endif // PCSX_ATASTORAGE_PORT_H
