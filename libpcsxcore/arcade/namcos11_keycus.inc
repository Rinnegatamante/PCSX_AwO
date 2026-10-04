// license:BSD-3-Clause
// copyright-holders:smf

// Generated from MAME ns11prot.cpp at deb2f54fe7300e7a192690318beb860159bfd52a.

static uint16_t keycus_406_read(Namcos11Board *board, unsigned offset)
{
	if( offset == 0 && board->key_p[0] == 0x1234 && board->key_p[1] == 0x5678 && board->key_p[2] == 0x000f )
	{
		return 0x3256;
	}

	
	return keycus_unknown(board, offset);
}

static void keycus_406_write(Namcos11Board *board, unsigned offset, uint16_t data)
{
	switch( offset )
	{
	case 1:
		board->key_p[0] = data;
		return;

	case 2:
		board->key_p[1] = data;
		return;

	case 3:
		board->key_p[2] = data;
		return;
	}

	

    board->status.unknown_keycus_writes++;
}

static uint16_t keycus_409_read(Namcos11Board *board, unsigned offset)
{
	if( offset == 7 )
	{
		int a2 = (board->key_p[0] - 0x01) & 0x1f;
		int a3 = (0x20 - board->key_p[0]) & 0x1f;
		int r = (((board->key_p[1] & 0x1f) * a2) + ((board->key_p[2] & 0x1f) * a3)) / 0x1f;
		int g = ((((board->key_p[1] >> 5) & 0x1f) * a2) + (((board->key_p[2] >> 5) & 0x1f) * a3)) / 0x1f;
		int b = ((((board->key_p[1] >> 10) & 0x1f) * a2) + (((board->key_p[2] >> 10) & 0x1f) * a3)) / 0x1f;
		return r | (g << 5) | (b << 10);
	}

	
	return keycus_unknown(board, offset);
}

static void keycus_409_write(Namcos11Board *board, unsigned offset, uint16_t data)
{
	switch( offset )
	{
	case 1:
		board->key_p[0] = data;
		return;

	case 3:
		board->key_p[1] = data;
		return;

	case 7:
		board->key_p[2] = data;
		return;
	}

	

    board->status.unknown_keycus_writes++;
}

static uint16_t keycus_410_read(Namcos11Board *board, unsigned offset)
{
	if( board->key_p[1] == 0 )
	{
		uint16_t value = board->key_p[0];
		if( value == 0xfffe )
		{
			value = 410;
		}

		switch(offset)
		{
		case 1:
			return ( ( value / 1 ) % 10 );

		case 2:
			return ( ( value / 100 ) % 10 ) |
				( ( ( value / 1000 ) % 10 ) << 8 );

		case 3:
			return ( ( value / 10000 ) % 10 ) |
				( ( ( value / 10 ) % 10 ) << 8 );
		}
	}

	
	return keycus_unknown(board, offset);
}

static void keycus_410_write(Namcos11Board *board, unsigned offset, uint16_t data)
{
	switch( offset )
	{
	case 0:
		board->key_p[0] = data;
		return;

	case 2:
		board->key_p[1] = data;
		return;
	}

	

    board->status.unknown_keycus_writes++;
}

static uint16_t keycus_411_read(Namcos11Board *board, unsigned offset)
{
	if( board->key_p[1] == 0x0000 && ( ( ( board->key_p[0] == 0x0000 || board->key_p[0] == 0x100 ) && board->key_p[2] == 0xff7f ) || ( board->key_p[0] == 0x7256 ) ) )
	{
		uint16_t value = board->key_p[2];
		if( board->key_p[0] != 0x7256 )
		{
			value = 411;
		}

		switch( offset )
		{
		case 0:
			return ( ( ( value / 10 ) % 10 ) << 8 ) | ( ( value / 1 ) % 10 );

		case 2:
			return ( ( ( value / 1000 ) % 10 ) << 8 ) | ( ( value / 100 ) % 10 );

		case 8:
			return ( ( value / 10000 ) % 10 );
		}
	}

	
	return keycus_unknown(board, offset);
}

static void keycus_411_write(Namcos11Board *board, unsigned offset, uint16_t data)
{
	switch( offset )
	{
	case 2:
		board->key_p[0] = data;
		return;

	case 8:
		board->key_p[1] = data;
		return;

	case 10:
		board->key_p[2] = data;
		return;
	}

	

    board->status.unknown_keycus_writes++;
}

static uint16_t keycus_430_read(Namcos11Board *board, unsigned offset)
{
	if( board->key_p[1] == 0x0000 && ( ( board->key_p[0] == 0xbfff && board->key_p[2] == 0x0000 ) || board->key_p[2] == 0xe296 ) )
	{
		uint16_t value = board->key_p[0];

		if( board->key_p[2] != 0xe296 )
		{
			value = 430;
		}

		switch( offset )
		{
		case 1:
			return ( ( value / 10000 ) % 10 );

		case 4:
			return ( ( value / 100 ) % 10 ) |
				( ( ( value / 1000 ) % 10 ) << 8 );

		case 5:
			return ( ( value / 1 ) % 10 ) |
				( ( ( value / 10 ) % 10 ) << 8 );
		}
	}

	
	return keycus_unknown(board, offset);
}

static void keycus_430_write(Namcos11Board *board, unsigned offset, uint16_t data)
{
	switch( offset )
	{
	case 0:
		board->key_p[0] = data;
		return;

	case 1:
		board->key_p[1] = data;
		return;

	case 4:
		board->key_p[2] = data;
		return;
	}

	

    board->status.unknown_keycus_writes++;
}

static uint16_t keycus_431_read(Namcos11Board *board, unsigned offset)
{
	if( board->key_p[1] == 0x0000 && ( ( ( board->key_p[0] == 0x0000 || board->key_p[0] == 0xab50 ) && board->key_p[2] == 0x7fff ) || board->key_p[0] == 0x9e61 ) )
	{
		uint16_t value = board->key_p[2];

		if( board->key_p[0] != 0x9e61 )
		{
			value = 431;
		}

		switch( offset )
		{
		case 0:
			return  ( ( value / 1 ) % 10 ) |
				( ( ( value / 10 ) % 10 ) << 8 );
		case 4:
			return  ( ( value / 100 ) % 10 ) |
				( ( ( value / 1000 ) % 10 ) << 8 );

		case 8:
			return ( value / 10000 ) % 10;
		}
	}

	
	return keycus_unknown(board, offset);
}

static void keycus_431_write(Namcos11Board *board, unsigned offset, uint16_t data)
{
	switch( offset )
	{
	case 0:
		board->key_p[0] = data;
		return;

	case 4:
		board->key_p[1] = data;
		return;

	case 12:
		board->key_p[2] = data;
		return;
	}

	

    board->status.unknown_keycus_writes++;
}

static uint16_t keycus_432_read(Namcos11Board *board, unsigned offset)
{
	if( board->key_p[0] == 0x0000 && ( ( ( board->key_p[2] == 0x0000 || board->key_p[2] == 0x00dc ) && board->key_p[1] == 0xefff ) || board->key_p[2] == 0x2f15 ) )
	{
		uint16_t value = board->key_p[1];

		if( board->key_p[2] != 0x00002f15 )
		{
			value = 432;
		}

		switch( offset )
		{
		case 2:
			return ( ( value / 1 ) % 10 ) |
				( ( ( value / 10 ) % 10 ) << 8 );

		case 4:
			return ( ( value / 100 ) % 10 ) |
				( ( ( value / 1000 ) % 10 ) << 8 );

		case 6:
			return  ( ( value / 10000 ) % 10 ) |
				( ( ( value / 100000 ) % 10 ) << 8 );
		}
	}

	
	return keycus_unknown(board, offset);
}

static void keycus_432_write(Namcos11Board *board, unsigned offset, uint16_t data)
{
	switch( offset )
	{
	case 0:
		board->key_p[0] = data;
		return;

	case 2:
		board->key_p[1] = data;
		return;

	case 6:
		board->key_p[2] = data;
		return;
	}

	

    board->status.unknown_keycus_writes++;
}

static uint16_t keycus_442_read(Namcos11Board *board, unsigned offset)
{
	if( ( offset == 0 && board->key_p[0] == 0x0020 && board->key_p[1] == 0x0000 ) ||
		( offset == 0 && board->key_p[0] == 0x0020 && board->key_p[1] == 0x0021 ) )
	{
		return 0x0000;
	}

	if( ( offset == 1 && board->key_p[0] == 0x0020 && board->key_p[1] == 0x0020 ) ||
		( offset == 1 && board->key_p[0] == 0x0020 && board->key_p[1] == 0x3af2 ) )
	{
		return 0x0000;
	}

	if( ( offset == 1 && board->key_p[0] == 0x0020 && board->key_p[1] == 0x0021 ) )
	{
		return 0xc442;
	}

	
	return keycus_unknown(board, offset);
}

static void keycus_442_write(Namcos11Board *board, unsigned offset, uint16_t data)
{
	switch( offset )
	{
	case 0:
		board->key_p[0] = data;
		return;

	case 1:
		board->key_p[1] = data;
		return;
	}

	

    board->status.unknown_keycus_writes++;
}

static uint16_t keycus_443_read(Namcos11Board *board, unsigned offset)
{
	if( offset == 0 && board->key_p[0] == 0x0020 && ( board->key_p[1] == 0x0000 || board->key_p[1] == 0xffff || board->key_p[1] == 0xffe0 ) )
	{
		return 0x0020;
	}

	if( offset == 1 && board->key_p[0] == 0x0020 && board->key_p[1] == 0xffdf )
	{
		return 0x0000;
	}

	if( offset == 1 && board->key_p[0] == 0x0020 && ( board->key_p[1] == 0xffff || board->key_p[1] == 0xffe0 ) )
	{
		return 0xc443;
	}

	
	return keycus_unknown(board, offset);
}

static void keycus_443_write(Namcos11Board *board, unsigned offset, uint16_t data)
{
	switch( offset )
	{
	case 0:
		board->key_p[0] = data;
		return;

	case 1:
		board->key_p[1] = data;
		return;
	}

	

    board->status.unknown_keycus_writes++;
}
