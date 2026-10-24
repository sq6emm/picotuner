/* ================================================================================================== */
/* ptupdate.h - flash layout and update metadata shared by the ptboot bootloader and PicoTuneWH       */
/*                                                                                                    */
/*    This file is part of PicoTuneWH and is free software: you can redistribute it and/or modify     */
/*    it under the terms of the GNU General Public License as published by the Free Software          */
/*    Foundation, either version 3 of the License, or (at your option) any later version.             */
/* ================================================================================================== */
/*
	2 MB flash, all addresses XIP:

	0x10000000	  32 KiB	ptboot (boot2 + bootloader)
	0x10008000	 480 KiB	application slot, linked here (its own boot2 copy at the start is unused,
							vector table at +0x100)
	0x10080000	 512 KiB	unused (room to grow the slots; the application is ~135 KiB)
	0x10100000	 480 KiB	staging: a new image received over Ethernet
	0x10178000	 508 KiB	backup: the previous application, kept for a rollback
	0x101f7000	   4 KiB	update metadata (one sector)
	0x101f8000	  32 KiB	FLASH0..7, reserved by the original linker script, unused

	Metadata words only ever go from 0xffffffff to 0 after the sector has been written once, so
	every state change is a page program, never an erase. The sequence is:

	app:		erase meta, erase + write staging, write header (state "pending")
	ptboot:		app -> backup (backup_done), staging -> app (installed), count trial boots
	app:		after 60 s with an IP address: confirmed
	ptboot:		3 trial boots without confirmed: backup -> app (rolled_back)
*/

#ifndef PTUPDATE_H
#define PTUPDATE_H

#include <stdint.h>

#define PT_FLASH_BASE			0x10000000u
#define PT_SECTOR				4096u
#define PT_PAGE					256u

#define PT_BOOT_BASE			0x10000000u
#define PT_BOOT_SIZE			0x00008000u
#define PT_APP_BASE				0x10008000u
#define PT_APP_MAX				0x00078000u			// 480 KiB, also the linker limit for the app
#define PT_STAGE_BASE			0x10100000u
#define PT_STAGE_MAX			0x00078000u
#define PT_BACKUP_BASE			0x10178000u
#define PT_BACKUP_MAX			0x0007f000u
#define PT_META_BASE			0x101f7000u

#define PT_VECTOR_OFFSET		0x100u				// boot2 is 256 bytes, vectors follow
#define PT_RAM_BASE				0x20000000u
#define PT_RAM_END				0x20042000u

#define PT_META_MAGIC			0x50545550u			// "PTUP"
#define PT_FLAG_SET				0u					// a programmed flag word
#define PT_FLAG_CLEAR			0xffffffffu			// erased
#define PT_TRIAL_BOOTS			3					// boots allowed before a rollback
#define PT_CONFIRM_MS			60000u				// uptime with an IP address before confirming

typedef struct
{
	uint32_t	magic ;								// PT_META_MAGIC
	uint32_t	size ;								// new image size in bytes
	uint32_t	crc ;								// CRC-32 (zlib) of the new image
	uint32_t	old_size ;							// size of the application it replaces
	uint32_t	old_crc ;							// CRC-32 of the application it replaces
	uint32_t	header_crc ;						// CRC-32 of the five words above
	uint32_t	reserved [2] ;
	uint32_t	backup_done ;						// PT_FLAG_SET: old application copied to backup
	uint32_t	installed ;							// PT_FLAG_SET: staging copied to the app slot, trial started
	uint32_t	confirmed ;							// PT_FLAG_SET: the new application confirmed itself
	uint32_t	rolled_back ;						// PT_FLAG_SET: ptboot restored the backup
	uint32_t	reserved2 [4] ;
	uint32_t	boots [16] ;						// one word programmed per trial boot
} pt_meta_t ;

#define PT_META_HEADER_WORDS	5

static inline uint32_t pt_crc32_update (uint32_t crc, const uint8_t *data, uint32_t length)
{
	crc = ~crc ;
	while (length--)
	{
		crc ^= *data++ ;
		for (int bit = 0 ; bit < 8 ; bit++)
		{
			crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u))) ;
		}
	}
	return (~crc) ;
}

static inline uint32_t pt_crc32 (const void *data, uint32_t length)
{
	return (pt_crc32_update (0, (const uint8_t *) data, length)) ;
}

static inline int pt_meta_header_valid (const pt_meta_t *m)
{
	return
	(
		m->magic == PT_META_MAGIC 		&&
		m->size  != 0 					&&
		m->size  <= PT_APP_MAX			&&
		m->old_size <= PT_APP_MAX		&&
		m->header_crc == pt_crc32 (m, PT_META_HEADER_WORDS * 4)
	) ;
}

// an image is runnable from the app slot if its vector table points into RAM and into the image

static inline int pt_image_vectors_valid (const uint8_t *image, uint32_t size)
{
	const uint32_t	*vectors = (const uint32_t *) (image + PT_VECTOR_OFFSET) ;
	uint32_t		sp ;
	uint32_t		pc ;

	if (size < PT_VECTOR_OFFSET + 8)
	{
		return (0) ;
	}
	sp = vectors [0] ;
	pc = vectors [1] ;
	return
	(
		sp >  PT_RAM_BASE 							&&
		sp <= PT_RAM_END 							&&
		(pc & 1) 									&&
		(pc & ~1u) >= PT_APP_BASE + PT_VECTOR_OFFSET	&&
		(pc & ~1u) <  PT_APP_BASE + size
	) ;
}

// UDP protocol on the command port (base port + 20, normally 9920)

#define PTUP_CMD_INFO			1		// -> state text
#define PTUP_CMD_BEGIN			2		// u32 size, u32 crc, u8 keylen, key
#define PTUP_CMD_DATA			3		// u32 offset, u16 length, data (sequential, <= PTUP_MAX_DATA)
#define PTUP_CMD_END			4		// verify, write the metadata, reboot
#define PTUP_CMD_ABORT			5
#define PTUP_REPLY				0x80	// reply: cmd | 0x80, u8 status, u16 seq, u32 value, text

#define PTUP_OK					0
#define PTUP_ERR_STATE			1		// no session, or a session with another host
#define PTUP_ERR_KEY			2
#define PTUP_ERR_SIZE			3
#define PTUP_ERR_OFFSET			4		// value = the next offset expected
#define PTUP_ERR_CRC			5
#define PTUP_ERR_IMAGE			6		// not linked for PT_APP_BASE
#define PTUP_ERR_FLASH			7
#define PTUP_ERR_FORMAT			8

#define PTUP_MAX_DATA			1024
#define PTUP_HEADER_SIZE		8		// "PTUP", cmd, 0, seq

#endif
