// SPDX-License-Identifier: BSD-3-Clause
// copyright-holders:smf
// MAME X76 secure flash pin state machines; generated, do not edit.
struct X76F041 {
void verboselog(int,const char*,...) {}
void device_reset();
void write_cs(int);
void write_rst(int);
void write_scl(int);
void write_sda(int);
int read_sda();
uint8_t *password();
void password_ok();
int data_offset();
void load_address();
	enum configuration_register_t
	{
		// If set to 1, retry counter is incremented when an invalid password is provided
		CR_RETRY_COUNTER_ENABLE_BIT = 0x04,

		// If set to 1, retry counter will be reset when a correct password is provided
		CR_RETRY_COUNTER_RESET_BIT = 0x08,

		// 10 = If retry counter is enabled, deny all commands when retry register equals retry counter
		// 00, 01, 11 = If retry counter is enabled, allow only configuration commands when retry register equals retry counter
		CR_UNAUTHORIZED_ACCESS_BITS = 0xc0,
	};

	enum configuration_registers_t
	{
		CONFIG_BCR1 = 0, // Array Control Register
		CONFIG_BCR2 = 1, // Array Control Register 2
		CONFIG_CR   = 2, // Configuration Register
		CONFIG_RR   = 3, // Retry Register
		CONFIG_RC   = 4  // Reset Counter
	};

	enum bcr_t
	{
		BCR_X = 8,
		BCR_Y = 4,
		BCR_Z = 2,
		BCR_T = 1
	};

	enum command_t
	{
		COMMAND_WRITE = 0x00,
		COMMAND_READ = 0x20,
		COMMAND_WRITE_USE_CONFIGURATION_PASSWORD = 0x40,
		COMMAND_READ_USE_CONFIGURATION_PASSWORD = 0x60,
		COMMAND_CONFIGURATION = 0x80
	};

	enum configuration_t
	{
		CONFIGURATION_PROGRAM_WRITE_PASSWORD = 0x00,
		CONFIGURATION_PROGRAM_READ_PASSWORD = 0x10,
		CONFIGURATION_PROGRAM_CONFIGURATION_PASSWORD = 0x20,
		CONFIGURATION_RESET_WRITE_PASSWORD = 0x30,
		CONFIGURATION_RESET_READ_PASSWORD = 0x40,
		CONFIGURATION_PROGRAM_CONFIGURATION_REGISTERS = 0x50,
		CONFIGURATION_READ_CONFIGURATION_REGISTERS = 0x60,
		CONFIGURATION_MASS_PROGRAM = 0x70,
		CONFIGURATION_MASS_ERASE = 0x80
	};

	enum state_t
	{
		STATE_STOP,
		STATE_RESPONSE_TO_RESET,
		STATE_LOAD_COMMAND,
		STATE_LOAD_ADDRESS,
		STATE_LOAD_PASSWORD,
		STATE_VERIFY_PASSWORD,
		STATE_READ_DATA,
		STATE_WRITE_DATA,
		STATE_CONFIGURATION_WRITE_DATA,
		STATE_READ_CONFIGURATION_REGISTERS,
		STATE_WRITE_CONFIGURATION_REGISTERS,

		STATE_PROGRAM_WRITE_PASSWORD,
		STATE_PROGRAM_READ_PASSWORD,
		STATE_PROGRAM_CONFIGURATION_PASSWORD,

		STATE_RESET_WRITE_PASSWORD,
		STATE_RESET_READ_PASSWORD,
		STATE_MASS_PROGRAM,
		STATE_MASS_ERASE
	};

	// internal state
	int m_cs;
	int m_rst;
	int m_scl;
	int m_sdaw;
	int m_sdar;
	int m_state;
	int m_shift;
	int m_bit;
	int m_byte;
	int m_command;
	int m_address;
	bool m_is_password_accepted;
	uint8_t m_write_buffer[8];
	uint8_t m_response_to_reset[4];
	uint8_t m_write_password[8];
	uint8_t m_read_password[8];
	uint8_t m_configuration_password[8];
	uint8_t m_configuration_registers[8];
	uint8_t m_data[512];
	uint8_t m_password_temp[16];
};
void X76F041::device_reset()
{
	std::fill( std::begin( m_write_buffer ), std::end( m_write_buffer ), 0 );
	std::fill( std::begin( m_password_temp ), std::end( m_password_temp ), 0 );

	m_cs = 0;
	m_rst = 0;
	m_scl = 0;
	m_sdaw = 0;
	m_sdar = 0;
	m_state = STATE_STOP;
	m_shift = 0;
	m_bit = 0;
	m_byte = 0;
	m_command = 0;
	m_address = 0;
	m_is_password_accepted = false;
}

void X76F041::write_cs(int state)
{
	if( m_cs != state )
	{
		verboselog( 2, "cs=%d\n", state );
	}

	if( m_cs != 0 && state == 0 )
	{
		/* enable chip */
		m_state = STATE_STOP;
	}

	if( m_cs == 0 && state != 0 )
	{
		/* disable chip */
		m_state = STATE_STOP;
		/* high impendence? */
		m_sdar = 0;
	}

	m_cs = state;
}

void X76F041::write_rst(int state)
{
	if( m_rst != state )
	{
		verboselog( 2, "rst=%d\n", state );
	}

	if( m_rst == 0 && state != 0 && m_cs == 0 )
	{
		verboselog( 1, "goto response to reset\n" );
		m_state = STATE_RESPONSE_TO_RESET;
		m_bit = 0;
		m_byte = 0;
	}

	m_rst = state;
}

uint8_t *X76F041::password()
{
	switch( m_command & 0xe0 )
	{
	case COMMAND_WRITE:
		return m_write_password;

	case COMMAND_READ:
		return m_read_password;

	case COMMAND_CONFIGURATION:
		if( m_address == CONFIGURATION_PROGRAM_WRITE_PASSWORD )
			return m_write_password;

		if( m_address == CONFIGURATION_PROGRAM_READ_PASSWORD )
			return m_read_password;

		return m_configuration_password;

	default:
		return m_configuration_password;
	}
}

void X76F041::password_ok()
{
	m_byte = 0; // Separate password-byte cursor from authenticated data cursor.
	if( m_configuration_registers[CONFIG_CR] & CR_RETRY_COUNTER_RESET_BIT )
		m_configuration_registers[CONFIG_RC] = 0;

	switch( m_command & 0xe0 )
	{
	case COMMAND_WRITE:
		m_state = STATE_WRITE_DATA;
		break;
	case COMMAND_READ:
		m_state = STATE_READ_DATA;
		break;
	case COMMAND_WRITE_USE_CONFIGURATION_PASSWORD:
		m_state = STATE_CONFIGURATION_WRITE_DATA;
		break;
	case COMMAND_READ_USE_CONFIGURATION_PASSWORD:
		m_state = STATE_READ_DATA;
		break;
	case COMMAND_CONFIGURATION:
		switch( m_address )
		{
		case CONFIGURATION_PROGRAM_WRITE_PASSWORD:
			m_state = STATE_PROGRAM_WRITE_PASSWORD;
			m_byte = 0;
			break;
		case CONFIGURATION_PROGRAM_READ_PASSWORD:
			m_state = STATE_PROGRAM_READ_PASSWORD;
			m_byte = 0;
			break;
		case CONFIGURATION_PROGRAM_CONFIGURATION_PASSWORD:
			m_state = STATE_PROGRAM_CONFIGURATION_PASSWORD;
			m_byte = 0;
			break;
		case CONFIGURATION_RESET_WRITE_PASSWORD:
			m_state = STATE_RESET_WRITE_PASSWORD;
			break;
		case CONFIGURATION_RESET_READ_PASSWORD:
			m_state = STATE_RESET_READ_PASSWORD;
			break;
		case CONFIGURATION_PROGRAM_CONFIGURATION_REGISTERS:
			m_state = STATE_WRITE_CONFIGURATION_REGISTERS;
			m_byte = 0;
			break;
		case CONFIGURATION_READ_CONFIGURATION_REGISTERS:
			m_state = STATE_READ_CONFIGURATION_REGISTERS;
			m_byte = 0;
			break;
		case CONFIGURATION_MASS_PROGRAM:
			m_state = STATE_MASS_PROGRAM;
			break;
		case CONFIGURATION_MASS_ERASE:
			m_state = STATE_MASS_ERASE;
			break;
		default:
			break;
		}
	}
}

void X76F041::load_address()
{
	m_address = m_shift;

	verboselog( 1, "-> address: %02x\n", m_address );

	if( ( m_configuration_registers[CONFIG_CR] & CR_RETRY_COUNTER_ENABLE_BIT ) != 0 &&
		m_configuration_registers[CONFIG_RR] == m_configuration_registers[CONFIG_RC] &&
		( m_configuration_registers[CONFIG_CR] & CR_UNAUTHORIZED_ACCESS_BITS ) == 0x80 )
	{
		// No commands are allowed
		verboselog( 1, "unauthorized access rejected\n" );
		m_state = STATE_STOP;
		m_sdar = 1;
		m_byte = 0;
		return;
	}

	if( ( m_command & 0xe0 ) == COMMAND_CONFIGURATION )
	{
		// Configuration commands can be used regardless of array control register bits
		if( m_address == CONFIGURATION_RESET_WRITE_PASSWORD ||
			m_address == CONFIGURATION_RESET_READ_PASSWORD ||
			m_address == CONFIGURATION_MASS_PROGRAM ||
			m_address == CONFIGURATION_MASS_ERASE )
		{
			verboselog( 1, "password not required\n" );
			password_ok();
		}
		else
		{
			verboselog( 1, "send password\n" );
			m_state = STATE_LOAD_PASSWORD;
			m_byte = 0;
		}

		return;
	}

	if( ( m_configuration_registers[CONFIG_CR] & CR_RETRY_COUNTER_ENABLE_BIT ) != 0 &&
		m_configuration_registers[CONFIG_RR] == m_configuration_registers[CONFIG_RC] &&
		( m_configuration_registers[CONFIG_CR] & CR_UNAUTHORIZED_ACCESS_BITS ) != 0x80 )
	{
		// Only configuration commands are allowed
		verboselog( 1, "unauthorized access rejected\n" );
		m_state = STATE_STOP;
		m_sdar = 1;
		m_byte = 0;
		return;
	}

	int bcr = m_configuration_registers[( m_command & 1 ) ? CONFIG_BCR2 : CONFIG_BCR1];
	if( ( m_address & 0x80 ) != 0 )
	{
		bcr >>= 4;
	}

	if( ( ( m_command & 0xe0 ) == COMMAND_READ && ( bcr & BCR_Z ) != 0 && ( bcr & BCR_T ) != 0 ) ||
		( ( m_command & 0xe0 ) == COMMAND_WRITE && ( bcr & BCR_Z ) != 0 ) )
	{
		/* TODO: find out when this is really checked. */
		verboselog( 1, "command not allowed\n" );
		m_state = STATE_STOP;
		m_sdar = 1;
		m_byte = 0;
	}
	else if( ( ( m_command & 0xe0 ) == COMMAND_WRITE && ( bcr & BCR_X ) == 0 ) ||
		( ( m_command & 0xe0 ) == COMMAND_READ && ( bcr & BCR_Y ) == 0 ) )
	{
		verboselog( 1, "password not required\n" );
		password_ok();
	}
	else
	{
		verboselog( 1, "send password\n" );
		m_state = STATE_LOAD_PASSWORD;
		m_byte = 0;
	}
}

int X76F041::data_offset()
{
	int block_offset = ( ( m_command & 1 ) << 8 ) + m_address;

	// TODO: confirm block_start doesn't wrap.

	return ( block_offset & 0x180 ) | ( ( block_offset + m_byte ) & 0x7f );
}

void X76F041::write_scl(int state)
{
	if( m_scl != state )
	{
		verboselog( 2, "scl=%d\n", state );
	}

	if( m_cs == 0 )
	{
		switch( m_state )
		{
		case STATE_STOP:
			break;

		case STATE_RESPONSE_TO_RESET:
			if( m_scl != 0 && state == 0 )
			{
				m_sdar = ( m_response_to_reset[m_byte] >> m_bit ) & 1;
				verboselog( 2, "in response to reset %d (%d/%d)\n", m_sdar, m_byte, m_bit );
				m_bit++;

				if( m_bit == 8 )
				{
					m_bit = 0;
					m_byte++;

					if( m_byte == sizeof( m_response_to_reset ) )
					{
						m_byte = 0;
					}
				}
			}
			break;

		case STATE_LOAD_COMMAND:
		case STATE_LOAD_ADDRESS:
		case STATE_LOAD_PASSWORD:
		case STATE_VERIFY_PASSWORD:
		case STATE_WRITE_DATA:
		case STATE_CONFIGURATION_WRITE_DATA:
		case STATE_WRITE_CONFIGURATION_REGISTERS:
		case STATE_PROGRAM_WRITE_PASSWORD:
		case STATE_PROGRAM_READ_PASSWORD:
		case STATE_PROGRAM_CONFIGURATION_PASSWORD:
		case STATE_RESET_WRITE_PASSWORD:
		case STATE_RESET_READ_PASSWORD:
		case STATE_MASS_PROGRAM:
		case STATE_MASS_ERASE:
			// FIXME: Processing on the rising edge of the clock causes sda to change state while clock is high
			// which is not allowed. Also need to ensure that only valid device-id's and commands
			// are acknowledged.
			if( m_scl == 0 && state != 0 )
			{
				if( m_bit < 8 )
				{
					verboselog( 2, "clock\n" );
					m_shift <<= 1;

					if( m_sdaw != 0 )
					{
						m_shift |= 1;
					}

					m_bit++;
				}
				else
				{
					m_sdar = 0;

					switch( m_state )
					{
					case STATE_LOAD_COMMAND:
						m_command = m_shift;
						verboselog( 1, "-> command: %02x\n", m_command );
						/* TODO: verify command is valid? */
						m_state = STATE_LOAD_ADDRESS;
						break;

					case STATE_LOAD_ADDRESS:
						load_address();
						break;

					case STATE_LOAD_PASSWORD:
						verboselog( 1, "-> password: %02x\n", m_shift );
						m_write_buffer[m_byte++] = m_shift;

						if( m_byte == sizeof( m_write_buffer ) )
						{
							m_state = STATE_VERIFY_PASSWORD;

							// Perform the password acceptance check before verify password because
							// password verify ack is spammed and will quickly overflow the
							// retry counter.
							m_is_password_accepted = memcmp( password(), m_write_buffer, sizeof( m_write_buffer ) ) == 0;
							if( !m_is_password_accepted )
							{
								if( m_configuration_registers[CONFIG_CR] & CR_RETRY_COUNTER_ENABLE_BIT )
									m_configuration_registers[CONFIG_RC]++;
							}
						}
						break;

					case STATE_VERIFY_PASSWORD:
						verboselog( 1, "-> verify password: %02x\n", m_shift );

						/* TODO: this should probably be handled as a command */
						if( m_shift == 0xc0 )
						{
							/* TODO: this should take 10ms before it returns ok. */
							if( m_is_password_accepted )
							{
								password_ok();
							}
							else
							{
								m_sdar = 1;
							}
						}
						break;

					case STATE_WRITE_DATA:
						verboselog( 2, "-> data: %02x\n", m_shift );
						m_write_buffer[m_byte++] = m_shift;

						if( m_byte == sizeof( m_write_buffer ) )
						{
							int bcr = m_configuration_registers[( m_command & 1 ) ? CONFIG_BCR2 : CONFIG_BCR1];
							if( ( m_address & 0x80 ) != 0 )
							{
								bcr >>= 4;
							}

							if( ( bcr & ( BCR_Z | BCR_T ) ) == BCR_T )
							{
								// Bits in the data can only be set, not cleared, when in program only mode
								bool is_unauthorized_write = false;

								for( m_byte = 0; m_byte < sizeof( m_write_buffer ); m_byte++ )
								{
									int offset = data_offset();
									if( (m_write_buffer[m_byte] & m_data[offset]) != m_data[offset] )
									{
										verboselog( 1, "tried to unset bits while in program only mode\n" );
										is_unauthorized_write = true;
										break;
									}
								}

								if( is_unauthorized_write )
								{
									m_sdar = 1;
									m_byte = 0;
									break;
								}
							}

							for( m_byte = 0; m_byte < sizeof( m_write_buffer ); m_byte++ )
							{
								int offset = data_offset();
								verboselog( 1, "-> data[%03x]: %02x\n", offset, m_write_buffer[m_byte] );
								m_data[offset] = m_write_buffer[m_byte];
							}
							m_byte = 0;

							verboselog( 1, "data flushed\n" );
						}
						break;


					case STATE_CONFIGURATION_WRITE_DATA:
						// Unlike normal writes, configuration writes aren't required to be exactly 8 bytes
						// TODO: Store data in a temporary buffer until the proper end of the command before writing
						verboselog( 2, "-> data: %02x\n", m_shift );
						m_data[data_offset()] = m_shift;
						m_byte++;
						break;

					case STATE_WRITE_CONFIGURATION_REGISTERS:
						verboselog( 1, "-> configuration register[%d]: %02x\n", m_byte, m_shift );
						/* TODO: write after all bytes received? */
						m_configuration_registers[m_byte++] = m_shift;

						if( m_byte == sizeof( m_configuration_registers ) )
						{
							m_byte = 0;
						}
						break;

					case STATE_PROGRAM_WRITE_PASSWORD:
						verboselog( 1, "-> program write password[%d]: %02x\n", m_byte, m_shift );
						m_password_temp[m_byte++] = m_shift;

						if( m_byte == sizeof( m_password_temp ) )
						{
							// Read in the password twice and if the two copies match then write it to the password field
							if( memcmp( &m_password_temp[0], &m_password_temp[8], sizeof( m_write_password ) ) == 0 )
							{
								std::copy_n( std::begin( m_password_temp ), sizeof( m_write_password ), std::begin ( m_write_password ) );
							}
							else {
								m_sdar = 1;
							}

							std::fill( std::begin( m_password_temp ), std::end( m_password_temp ), 0 );

							m_byte = 0;
						}
						break;

					case STATE_PROGRAM_READ_PASSWORD:
						verboselog( 1, "-> program read password[%d]: %02x\n", m_byte, m_shift );
						m_password_temp[m_byte++] = m_shift;

						if( m_byte == sizeof( m_password_temp ) )
						{
							if( memcmp( &m_password_temp[0], &m_password_temp[8], sizeof( m_read_password ) ) == 0 )
							{
								std::copy_n( std::begin( m_password_temp ), sizeof( m_read_password ), std::begin ( m_read_password ) );
							}
							else {
								m_sdar = 1;
							}

							std::fill( std::begin( m_password_temp ), std::end( m_password_temp ), 0 );

							m_byte = 0;
						}
						break;

					case STATE_PROGRAM_CONFIGURATION_PASSWORD:
						verboselog( 1, "-> program configuration password[%d]: %02x\n", m_byte, m_shift );
						m_password_temp[m_byte++] = m_shift;

						if( m_byte == sizeof( m_password_temp ) )
						{
							if( memcmp( &m_password_temp[0], &m_password_temp[8], sizeof( m_configuration_password ) ) == 0 )
							{
								std::copy_n( std::begin( m_password_temp ), sizeof( m_configuration_password ), std::begin ( m_configuration_password ) );
							}
							else {
								m_sdar = 1;
							}

							std::fill( std::begin( m_password_temp ), std::end( m_password_temp ), 0 );

							m_byte = 0;
						}
						break;

					case STATE_RESET_WRITE_PASSWORD:
						verboselog( 1, "-> reset write password\n" );
						std::fill( std::begin( m_write_password ), std::end( m_write_password ), 0 );
						break;

					case STATE_RESET_READ_PASSWORD:
						verboselog( 1, "-> reset read password\n" );
						std::fill( std::begin( m_read_password ), std::end( m_read_password ), 0 );
						break;

					case STATE_MASS_PROGRAM:
					case STATE_MASS_ERASE:
					{
						const uint8_t fill = m_state == STATE_MASS_ERASE ? 0xff : 0;
						verboselog( 1, "-> mass erase %02x\n", fill );
						std::fill( std::begin( m_data ), std::end( m_data ), fill );
						std::fill( std::begin( m_configuration_password ), std::end( m_configuration_password ), fill );
						std::fill( std::begin( m_configuration_registers ), std::end( m_configuration_registers ), fill );
						std::fill( std::begin( m_write_password ), std::end( m_write_password ), fill );
						std::fill( std::begin( m_read_password ), std::end( m_read_password ), fill );
						break;
					}
					}

					m_bit = 0;
					m_shift = 0;
				}
			}
			break;

		case STATE_READ_DATA:
		case STATE_READ_CONFIGURATION_REGISTERS:
			// FIXME: Processing on the rising edge of the clock causes sda to change state while clock is high
			// which is not allowed.
			if( m_scl == 0 && state != 0 )
			{
				if( m_bit < 8 )
				{
					if( m_bit == 0 )
					{
						int offset;

						switch( m_state )
						{
						case STATE_READ_DATA:
							offset = data_offset();
							m_shift = m_data[offset];
							verboselog( 1, "<- data[%03x]: %02x\n", offset, m_shift );
							break;

						case STATE_READ_CONFIGURATION_REGISTERS:
							offset = m_byte & 7;
							m_shift = m_configuration_registers[offset];
							verboselog( 1, "<- configuration register[%d]: %02x\n", offset, m_shift );
							break;
						}
					}

					m_sdar = ( m_shift >> 7 ) & 1;
					m_shift <<= 1;
					m_bit++;
				}
				else
				{
					m_bit = 0;
					m_sdar = 0;

					if( m_sdaw == 0 )
					{
						verboselog( 2, "ack <-\n" );
						m_byte++;
					}
					else
					{
						verboselog( 2, "nak <-\n" );
					}
				}
			}
			break;
		}
	}

	m_scl = state;
}

void X76F041::write_sda(int state)
{
	if( m_sdaw != state )
	{
		verboselog( 2, "sdaw=%d\n", state );
	}

	if( m_cs == 0 && m_scl != 0 )
	{
		if( m_sdaw == 0 && state != 0 )
		{
			verboselog( 1, "goto stop\n" );
			m_state = STATE_STOP;
			m_sdar = 0;
		}

		if( m_sdaw != 0 && state == 0 )
		{
			switch( m_state )
			{
			case STATE_STOP:
				verboselog( 1, "goto start\n" );
				m_state = STATE_LOAD_COMMAND;
				break;

			case STATE_LOAD_PASSWORD:
				/* TODO: this will be the 0xc0 command, but it's not handled as a command yet. */
				verboselog( 1, "goto start\n" );
				break;

			case STATE_READ_DATA:
				verboselog( 1, "goto load address\n" );
				m_state = STATE_LOAD_ADDRESS;
				break;

			default:
				verboselog( 1, "skipped start (default)\n" );
				break;
			}

			m_bit = 0;
			m_byte = 0;
			m_shift = 0;
			m_sdar = 0;
		}
	}

	m_sdaw = state;
}

int X76F041::read_sda()
{
	if( m_cs != 0 )
	{
		verboselog( 2, "not selected\n" );
		return 1;
	}

	verboselog( 2, "sdar=%d\n", m_sdar );
	return m_sdar;
}


struct X76F100 {
void verboselog(int,const char*,...) {}
void device_reset();
void write_cs(int);
void write_rst(int);
void write_scl(int);
void write_sda(int);
int read_sda();
uint8_t *password();
void password_ok();
int data_offset();
	enum command_t
	{
		COMMAND_WRITE = 0x80,
		COMMAND_READ = 0x81,
		COMMAND_CHANGE_WRITE_PASSWORD = 0xfc,
		COMMAND_CHANGE_READ_PASSWORD = 0xfe,
		COMMAND_ACK_PASSWORD = 0x55
	};

	enum state_t
	{
		STATE_STOP,
		STATE_RESPONSE_TO_RESET,
		STATE_LOAD_COMMAND,
		STATE_LOAD_PASSWORD,
		STATE_VERIFY_PASSWORD,
		STATE_READ_DATA,
		STATE_WRITE_DATA
	};

	// internal state
	int m_cs;
	int m_rst;
	int m_scl;
	int m_sdaw;
	int m_sdar;
	int m_state;
	int m_shift;
	int m_bit;
	int m_byte;
	int m_command;
	int m_password_retry_counter;
	bool m_is_password_accepted;
	uint8_t m_write_buffer[8];
	uint8_t m_response_to_reset[4];
	uint8_t m_write_password[8];
	uint8_t m_read_password[8];
	uint8_t m_data[112];
};
void X76F100::device_reset()
{
	std::fill( std::begin( m_write_buffer ), std::end( m_write_buffer ), 0 );

	m_cs = 0;
	m_rst = 0;
	m_scl = 0;
	m_sdaw = 0;
	m_sdar = 0;
	m_state = STATE_STOP;
	m_shift = 0;
	m_bit = 0;
	m_byte = 0;
	m_command = 0;
	m_password_retry_counter = 0;
	m_is_password_accepted = false;
}

void X76F100::write_cs(int state)
{
	if( m_cs != state )
	{
		verboselog( 2, "cs=%d\n", state );
	}

	if( m_cs != 0 && state == 0 )
	{
		/* enable chip */
		m_state = STATE_STOP;
	}

	if( m_cs == 0 && state != 0 )
	{
		/* disable chip */
		m_state = STATE_STOP;
		/* high impendence? */
		m_sdar = 0;
	}

	m_cs = state;
}

void X76F100::write_rst(int state)
{
	if( m_rst != state )
	{
		verboselog( 2, "rst=%d\n", state );
	}

	if( m_rst == 0 && state != 0 && m_cs == 0 )
	{
		verboselog( 1, "goto response to reset\n" );
		m_state = STATE_RESPONSE_TO_RESET;
		m_bit = 0;
		m_byte = 0;
	}

	m_rst = state;
}

uint8_t *X76F100::password()
{
	if( ( m_command & 0xe1 ) == COMMAND_READ )
	{
		return m_read_password;
	}

	return m_write_password;
}

void X76F100::password_ok()
{
	m_byte = 0; // Separate password-byte cursor from authenticated data cursor.
	m_password_retry_counter = 0;

	if( ( m_command & 0x81 ) == COMMAND_READ )
	{
		m_state = STATE_READ_DATA;
	}
	else if( ( m_command & 0x81 ) == COMMAND_WRITE )
	{
		m_state = STATE_WRITE_DATA;
	}
	else
	{
		/* TODO: */
	}
}

int X76F100::data_offset()
{
	int block_offset = ( m_command >> 1 ) & 0x0f;
	int offset = ( block_offset * sizeof( m_write_buffer ) ) + m_byte;

	// Technically there are 4 bits assigned to sector values but since the data array is only 112 bytes,
	// it will try reading out of bounds when the sector is 14 (= starts at 112) or 15 (= starts at 120).
	// TODO: Verify what happens on real hardware when reading/writing sectors 14 and 15
	if( offset >= sizeof ( m_data ) )
		return -1;

	return offset;
}

void X76F100::write_scl(int state)
{
	if( m_scl != state )
	{
		verboselog( 2, "scl=%d\n", state );
	}

	if( m_cs == 0 )
	{
		switch( m_state )
		{
		case STATE_STOP:
			break;

		case STATE_RESPONSE_TO_RESET:
			if( m_scl != 0 && state == 0 )
			{
				if( m_bit == 0 )
				{
					m_shift = m_response_to_reset[m_byte];
					verboselog( 1, "<- response_to_reset[%d]: %02x\n", m_byte, m_shift );
				}

				m_sdar = m_shift & 1;
				m_shift >>= 1;
				m_bit++;

				if( m_bit == 8 )
				{
					m_bit = 0;
					m_byte++;

					if( m_byte == sizeof( m_response_to_reset ) )
					{
						m_byte = 0;
					}
				}
			}
			break;

		case STATE_LOAD_COMMAND:
		case STATE_LOAD_PASSWORD:
		case STATE_VERIFY_PASSWORD:
		case STATE_WRITE_DATA:
			// FIXME: Processing on the rising edge of the clock causes sda to change state while clock is high
			// which is not allowed. Also need to ensure that only valid device-id's and commands
			// are acknowledged.
			if( m_scl == 0 && state != 0 )
			{
				if( m_bit < 8 )
				{
					verboselog( 2, "clock\n" );
					m_shift <<= 1;

					if( m_sdaw != 0 )
					{
						m_shift |= 1;
					}

					m_bit++;
				}
				else
				{
					m_sdar = 0;

					switch( m_state )
					{
					case STATE_LOAD_COMMAND:
						m_command = m_shift;
						verboselog( 1, "-> command: %02x\n", m_command );
						/* TODO: verify command is valid? */
						m_state = STATE_LOAD_PASSWORD;
						break;

					case STATE_LOAD_PASSWORD:
						verboselog( 1, "-> password: %02x\n", m_shift );
						m_write_buffer[m_byte++] = m_shift;

						if( m_byte == sizeof( m_write_buffer ) )
						{
							m_state = STATE_VERIFY_PASSWORD;

							// Perform the password acceptance check before verify password because
							// password verify ack is spammed and will quickly overflow the
							// retry counter. This becomes an issue with System 573 games that use the
							// X76F100 as an install cartridge. The boot process first tries to use the
							// game cartridge password and if not accepted will try the install cartridge
							// password and then enter installation mode if accepted.
							m_is_password_accepted = memcmp( password(), m_write_buffer, sizeof( m_write_buffer ) ) == 0;
							if( !m_is_password_accepted )
							{
								m_password_retry_counter++;
								if( m_password_retry_counter >= 8 )
								{
									std::fill( std::begin( m_read_password ), std::end( m_read_password ), 0 );
									std::fill( std::begin( m_write_password ), std::end( m_write_password ), 0 );
									std::fill( std::begin( m_data ), std::end( m_data ), 0 );
									m_password_retry_counter = 0;
								}
							}
						}
						break;

					case STATE_VERIFY_PASSWORD:
						verboselog( 1, "-> verify password: %02x\n", m_shift );

						/* TODO: this should probably be handled as a command */
						if( m_shift == COMMAND_ACK_PASSWORD )
						{
							/* TODO: this should take 10ms before it returns ok. */
							if( m_is_password_accepted )
							{
								password_ok();
							}
							else
							{
								m_sdar = 1;
							}
						}
						break;

					case STATE_WRITE_DATA:
						verboselog( 2, "-> data: %02x\n", m_shift );
						m_write_buffer[m_byte++] = m_shift;

						if( m_byte == sizeof( m_write_buffer ) )
						{
							if( m_command == COMMAND_CHANGE_WRITE_PASSWORD )
							{
								std::copy( std::begin( m_write_buffer ), std::end( m_write_buffer ), std::begin( m_write_password ) );
							}
							else if( m_command == COMMAND_CHANGE_READ_PASSWORD )
							{
								std::copy( std::begin( m_write_buffer ), std::end( m_write_buffer ), std::begin( m_read_password ) );
							}
							else
							{
								for( m_byte = 0; m_byte < sizeof( m_write_buffer ); m_byte++ )
								{
									int offset = data_offset();

									if( offset != -1 )
									{
										verboselog( 1, "-> data[%03x]: %02x\n", offset, m_write_buffer[m_byte] );
										m_data[offset] = m_write_buffer[m_byte];
									}
									else
									{
										verboselog( 1, "-> attempted to write %02x out of bounds\n", m_write_buffer[m_byte] );
										break;
									}
								}
							}

							m_byte = 0;

							verboselog( 1, "data flushed\n" );
						}
						break;
					}

					m_bit = 0;
					m_shift = 0;
				}
			}
			break;

		case STATE_READ_DATA:
			// FIXME: Processing on the rising edge of the clock causes sda to change state while clock is high
			// which is not allowed.
			if( m_scl == 0 && state != 0 )
			{
				if( m_bit < 8 )
				{
					if( m_bit == 0 )
					{
						int offset;

						switch( m_state )
						{
						case STATE_READ_DATA:
							offset = data_offset();

							if( offset != -1 )
							{
								m_shift = m_data[offset];
								verboselog( 1, "<- data[%02x]: %02x\n", offset, m_shift );
							}
							else
							{
								m_shift = 0;
								verboselog( 1, "<- attempted to read out of bounds\n" );
							}

							break;
						}
					}

					m_sdar = ( m_shift >> 7 ) & 1;
					m_shift <<= 1;
					m_bit++;
				}
				else
				{
					m_bit = 0;
					m_sdar = 0;

					if( m_sdaw == 0 )
					{
						verboselog( 2, "ack <-\n" );
						m_byte++;
					}
					else
					{
						verboselog( 2, "nak <-\n" );
					}
				}
			}
			break;
		}
	}

	m_scl = state;
}

void X76F100::write_sda(int state)
{
	if( m_sdaw != state )
	{
		verboselog( 2, "sdaw=%d\n", state );
	}

	if( m_cs == 0 && m_scl != 0 )
	{
		if( m_sdaw == 0 && state != 0 )
		{
			verboselog( 1, "goto stop\n" );
			m_state = STATE_STOP;
			m_sdar = 0;
		}

		if( m_sdaw != 0 && state == 0 )
		{
			switch( m_state )
			{
			case STATE_STOP:
				verboselog( 1, "goto start\n" );
				m_state = STATE_LOAD_COMMAND;
				break;

			case STATE_LOAD_PASSWORD:
				/* TODO: this will be the 0xc0 command, but it's not handled as a command yet. */
				verboselog( 1, "goto start\n" );
				break;

			case STATE_READ_DATA:
				verboselog( 1, "continue reading??\n" );
//              verboselog( 1, "goto load address\n" );
//              m_state = STATE_LOAD_ADDRESS;
				break;

			default:
				verboselog( 1, "skipped start (default)\n" );
				break;
			}

			m_bit = 0;
			m_byte = 0;
			m_shift = 0;
			m_sdar = 0;
		}
	}

	m_sdaw = state;
}

int X76F100::read_sda()
{
	if( m_cs != 0 )
	{
		verboselog( 2, "not selected\n" );
		return 1;
	}

	verboselog( 2, "sdar=%d\n", m_sdar );
	return m_sdar;
}


