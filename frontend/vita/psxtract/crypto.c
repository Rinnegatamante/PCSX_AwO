// Copyright (C) 2014       Hykem <hykem@hotmail.com>
// Licensed under the terms of the GNU GPL, version 3
// http://www.gnu.org/licenses/gpl-3.0.txt

/*
	Copyright (c) 2005  adresd
	Copyright (c) 2005  Marcus R. Brown
	Copyright (c) 2005  James Forshaw
	Copyright (c) 2005  John Kelley
	Copyright (c) 2005  Jesper Svennevid
	All rights reserved.

	Redistribution and use in source and binary forms, with or without
	modification, are permitted provided that the following conditions
	are met:
	1. Redistributions of source code must retain the above copyright
	notice, this list of conditions and the following disclaimer.
	2. Redistributions in binary form must reproduce the above copyright
	notice, this list of conditions and the following disclaimer in the
	documentation and/or other materials provided with the distribution.
	3. The names of the authors may not be used to endorse or promote products
	derived from this software without specific prior written permission.

	THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY EXPRESS OR
	IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
	OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
	IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY DIRECT, INDIRECT,
	INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
	NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
	DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
	THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
	(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
	THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "crypto.h"

static unsigned char dnas_key1A90[] = {0xED, 0xE2, 0x5D, 0x2D, 0xBB, 0xF8, 0x12, 0xE5, 0x3C, 0x5C, 0x59, 0x32, 0xFA, 0xE3, 0xE2, 0x43};
static unsigned char dnas_key1AA0[] = {0x27, 0x74, 0xFB, 0xEB, 0xA4, 0xA0, 0x01, 0xD7, 0x02, 0x56, 0x9E, 0x33, 0x8C, 0x19, 0x57, 0x83};

static int is_empty_key(const unsigned char *key)
{
    if (!key)
        return 1;
    for (int i = 0; i < 16; ++i)
        if (key[i])
            return 0;
    return 1;
}

int decrypt_pgd(unsigned char* pgd_data, int pgd_size, int flag, unsigned char* key)
{
	int result;
	PGD_HEADER PGD[sizeof(PGD_HEADER)];
	MAC_KEY mkey;
	CIPHER_KEY ckey;
	unsigned char*fkey;

	// Read in the PGD header parameters.
	memset(PGD, 0, sizeof(PGD_HEADER));

	PGD->buf = pgd_data;
	PGD->key_index = *(u32*)(pgd_data + 4);
	PGD->drm_type  = *(u32*)(pgd_data + 8);

	// Set the hashing, crypto and open modes.
	if (PGD->drm_type == 1)
	{
		PGD->mac_type = 1;
		flag |= 4;

		if(PGD->key_index > 1)
		{
			PGD->mac_type = 3;
			flag |= 8;
		}
		PGD->cipher_type = 1;
	}
	else
	{
		PGD->mac_type = 2;
		PGD->cipher_type = 2;
	}
	PGD->open_flag = flag;

	// Get the fixed DNAS key.
	fkey = NULL;
	if((flag & 0x2) == 0x2)
		fkey = dnas_key1A90;
	if((flag & 0x1) == 0x1)
		fkey = dnas_key1AA0;

	if (fkey == NULL)
	{
		printf("PGD: Invalid DNAS flag! %08x\n", flag);
		return -1;
	}

	// Test MAC hash at 0x80 (DNAS hash).
	sceDrmBBMacInit(&mkey, PGD->mac_type);
	sceDrmBBMacUpdate(&mkey, pgd_data, 0x80);
	result = sceDrmBBMacFinal2(&mkey, pgd_data + 0x80, fkey);

	if (result)
	{
		printf("PGD: Invalid 0x80 MAC hash!\n");
		return -1;
	}

	// Test MAC hash at 0x70 (key hash).
	sceDrmBBMacInit(&mkey, PGD->mac_type);
	sceDrmBBMacUpdate(&mkey, pgd_data, 0x70);

	// If a key was provided, check it against MAC 0x70.
	if (!is_empty_key(key))
	{
		result = sceDrmBBMacFinal2(&mkey, pgd_data + 0x70, key);
		if (result)
		{
			printf("PGD: Invalid 0x70 MAC hash!\n");
			return -1;
		}
		else
		{
			memcpy(PGD->vkey, key, 16);
		}
	}
	else
	{
		// Generate the key from MAC 0x70.
		bbmac_getkey(&mkey, pgd_data + 0x70, PGD->vkey);
	}

	// Decrypt the PGD header block (0x30 bytes).
	sceDrmBBCipherInit(&ckey, PGD->cipher_type, 2, pgd_data + 0x10, PGD->vkey, 0);
	sceDrmBBCipherUpdate(&ckey, pgd_data + 0x30, 0x30);
	sceDrmBBCipherFinal(&ckey);

	// Get the decryption parameters from the decrypted header.
	PGD->data_size   = *(u32*)(pgd_data + 0x44);
	PGD->block_size  = *(u32*)(pgd_data + 0x48);
	PGD->data_offset = *(u32*)(pgd_data + 0x4c);

	// Additional size variables.
	PGD->align_size = (PGD->data_size + 15) &~ 15;
	PGD->table_offset = PGD->data_offset + PGD->align_size;
	PGD->block_nr = (PGD->align_size + PGD->block_size - 1) &~ (PGD->block_size - 1);
	PGD->block_nr = PGD->block_nr / PGD->block_size;

	if ((PGD->align_size + PGD->block_nr * 16) > pgd_size)
	{
		printf("PGD: Invalid data size!\n");
		return -1;
	}

	// Test MAC hash at 0x60 (table hash).
	sceDrmBBMacInit(&mkey, PGD->mac_type);
	sceDrmBBMacUpdate(&mkey, pgd_data + PGD->table_offset, PGD->block_nr * 16);
	result = sceDrmBBMacFinal2(&mkey, pgd_data + 0x60, PGD->vkey);

	if(result)
	{
		printf("PGD: Invalid 0x60 MAC hash!\n");
		return -1;
	}

	// Decrypt the data.
	sceDrmBBCipherInit(&ckey, PGD->cipher_type, 2, pgd_data + 0x30, PGD->vkey, 0);
	sceDrmBBCipherUpdate(&ckey, pgd_data + 0x90, PGD->align_size);
	sceDrmBBCipherFinal(&ckey);

	return PGD->data_size;
}
