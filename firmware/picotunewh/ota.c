/* ================================================================================================== */
/* ota.c - firmware update over Ethernet for PicoTuneWH                                               */
/*                                                                                                    */
/*    This file is part of PicoTuneWH and is free software: you can redistribute it and/or modify     */
/*    it under the terms of the GNU General Public License as published by the Free Software          */
/*    Foundation, either version 3 of the License, or (at your option) any later version.             */
/* ================================================================================================== */
/*
	Runs on Core0 from the main loop, on the command socket (base port + 20). Packets start with
	"PTUP" so they never reach the [to@wh] command parser. The image goes to the staging area one
	sector at a time; the ptboot bootloader installs it after the reboot.

	Core1 runs from flash and talks to the NIM, so every erase/program pauses it with the SDK
	multicore lockout and runs with interrupts off. TS output stops for the length of each sector
	write (some tens of ms).
*/

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "socket.h"

#include "../common/ptupdate.h"
#include "ota.h"

#ifndef PTUP_KEY
#define PTUP_KEY				"picotuner"
#endif

#define OTA_SESSION_TIMEOUT_MS	30000

extern char 	__flash_binary_end ;

static volatile int	core1_lockout_ready ;

static uint32_t monotime_ms_ota (void)
{
	return (to_ms_since_boot (get_absolute_time())) ;
}

static struct
{
	int			active ;
	uint8_t		host [4] ;
	uint16_t	port ;
	uint32_t	size ;
	uint32_t	crc ;
	uint32_t	next ;								// next offset expected
	uint32_t	last_ms ;
}				session ;

static uint8_t	sector_buffer [PT_SECTOR] ;
static uint8_t	page_buffer   [PT_PAGE] ;
static int		staged ;							// a verified image waits for ptboot
static int		confirm_checked ;
static char		state_text [96] ;


void ota_core1_ready (void)
{
	multicore_lockout_victim_init() ;
	core1_lockout_ready = 1 ;
}


static uint32_t get32 (const uint8_t *p)
{
	uint32_t	value ;

	memcpy (&value, p, 4) ;
	return (value) ;
}


static uint16_t get16 (const uint8_t *p)
{
	uint16_t	value ;

	memcpy (&value, p, 2) ;
	return (value) ;
}


// erase (optionally) and program flash at an XIP address, with Core1 paused

static void flash_write (uint32_t address, const uint8_t *data, uint32_t length, int erase)
{
	uint32_t	interrupts ;
	uint32_t	offset = address - XIP_BASE ;

	if (core1_lockout_ready)
	{
		multicore_lockout_start_blocking() ;
	}
	interrupts = save_and_disable_interrupts() ;

	if (erase)
	{
		flash_range_erase (offset, (length + PT_SECTOR - 1) & ~(PT_SECTOR - 1)) ;
	}
	if (data)
	{
		flash_range_program (offset, data, length) ;
	}

	restore_interrupts (interrupts) ;
	if (core1_lockout_ready)
	{
		multicore_lockout_end_blocking() ;
	}
}


static void reply_to (int socket, uint8_t *address, uint16_t port, uint8_t cmd, uint8_t status, uint16_t seq, uint32_t value, const char *text)
{
	uint8_t		buff [PTUP_HEADER_SIZE + 4 + 128] ;
	uint32_t	length = PTUP_HEADER_SIZE + 4 ;

	memcpy (buff, "PTUP", 4) ;
	buff [4] = cmd | PTUP_REPLY ;
	buff [5] = status ;
	memcpy (buff + 6, &seq, 2) ;
	memcpy (buff + 8, &value, 4) ;
	if (text)
	{
		strncpy ((char *) buff + length, text, 127) ;
		length += strlen ((char *) buff + length) ;
	}
	sendto (socket, buff, length, address, port) ;
}


static const pt_meta_t* meta (void)
{
	return ((const pt_meta_t *) PT_META_BASE) ;
}


const char* ota_state_text (void)
{
	const pt_meta_t	*m = meta() ;
	uint32_t		boots = 0 ;
	uint32_t		x ;

	if (staged)
	{
		snprintf (state_text, sizeof(state_text), "staged %u bytes, rebooting", (unsigned) m->size) ;
	}
	else if (session.active)
	{
		snprintf (state_text, sizeof(state_text), "receiving %u/%u", (unsigned) session.next, (unsigned) session.size) ;
	}
	else if (!pt_meta_header_valid (m))
	{
		snprintf (state_text, sizeof(state_text), "no update") ;
	}
	else if (m->rolled_back == PT_FLAG_SET)
	{
		snprintf (state_text, sizeof(state_text), "rolled back") ;
	}
	else if (m->confirmed == PT_FLAG_SET)
	{
		snprintf (state_text, sizeof(state_text), "confirmed") ;
	}
	else if (m->installed == PT_FLAG_SET)
	{
		for (x = 0 ; x < sizeof(m->boots) / 4 ; x++)
		{
			boots += m->boots[x] == PT_FLAG_SET ;
		}
		snprintf (state_text, sizeof(state_text), "trial, boot %u of %u", (unsigned) boots, PT_TRIAL_BOOTS) ;
	}
	else
	{
		snprintf (state_text, sizeof(state_text), "pending") ;
	}
	return (state_text) ;
}


int ota_is_packet (const uint8_t *buff, int length)
{
	return (length >= PTUP_HEADER_SIZE && memcmp (buff, "PTUP", 4) == 0) ;
}


int ota_active (void)
{
	return (session.active) ;
}


int ota_reboot_wanted (void)
{
	return (staged) ;
}


static void flush_sector (uint32_t used)
{
	uint32_t	sector = (session.next - 1) / PT_SECTOR ;		// the sector that holds the last byte

	memset (sector_buffer + used, 0xff, PT_SECTOR - used) ;
	flash_write (PT_STAGE_BASE + sector * PT_SECTOR, sector_buffer, PT_SECTOR, 1) ;
}


static uint8_t finish (void)
{
	pt_meta_t		header ;
	uint32_t		used = session.next % PT_SECTOR ;
	uint32_t		old_size ;

	if (session.next != session.size)
	{
		return (PTUP_ERR_OFFSET) ;
	}
	if (used)
	{
		flush_sector (used) ;									// the last, partly filled sector
	}
	if (pt_crc32 ((const void *) PT_STAGE_BASE, session.size) != session.crc)
	{
		return (PTUP_ERR_CRC) ;
	}
	if (!pt_image_vectors_valid ((const uint8_t *) PT_STAGE_BASE, session.size))
	{
		return (PTUP_ERR_IMAGE) ;
	}

	old_size = (uint32_t) &__flash_binary_end - PT_APP_BASE ;
	memset (&header, 0xff, sizeof(header)) ;
	header.magic 		= PT_META_MAGIC ;
	header.size 		= session.size ;
	header.crc 			= session.crc ;
	header.old_size		= old_size <= PT_APP_MAX ? old_size : 0 ;
	header.old_crc		= header.old_size ? pt_crc32 ((const void *) PT_APP_BASE, header.old_size) : 0 ;
	header.header_crc	= pt_crc32 (&header, PT_META_HEADER_WORDS * 4) ;

	memset (page_buffer, 0xff, sizeof(page_buffer)) ;
	memcpy (page_buffer, &header, sizeof(header)) ;
	flash_write (PT_META_BASE, page_buffer, PT_PAGE, 0) ;		// erased at BEGIN

	if (!pt_meta_header_valid (meta()))
	{
		return (PTUP_ERR_FLASH) ;
	}
	return (PTUP_OK) ;
}


void ota_handle (int socket, const uint8_t *buff, int length, uint8_t *address, uint16_t port)
{
	uint8_t		cmd = buff [4] ;
	uint16_t	seq = get16 (buff + 6) ;
	uint32_t	offset ;
	uint32_t	count ;
	uint32_t	used ;
	uint8_t		status ;
	int			from_session = session.active && memcmp (address, session.host, 4) == 0 && port == session.port ;

	if (staged)
	{
		reply_to (socket, address, port, cmd, PTUP_ERR_STATE, seq, 0, "rebooting") ;
		return ;
	}

	switch (cmd)
	{
		case PTUP_CMD_INFO:
			reply_to (socket, address, port, cmd, PTUP_OK, seq, PT_APP_MAX, ota_state_text()) ;
			break ;

		case PTUP_CMD_BEGIN:
			if (length < PTUP_HEADER_SIZE + 9 || length < PTUP_HEADER_SIZE + 9 + buff [16])
			{
				reply_to (socket, address, port, cmd, PTUP_ERR_FORMAT, seq, 0, NULL) ;
				break ;
			}
			if (buff [16] != strlen (PTUP_KEY) || memcmp (buff + 17, PTUP_KEY, buff [16]) != 0)
			{
				reply_to (socket, address, port, cmd, PTUP_ERR_KEY, seq, 0, NULL) ;
				break ;
			}
			if (session.active && !from_session && monotime_ms_ota() - session.last_ms < OTA_SESSION_TIMEOUT_MS)
			{
				reply_to (socket, address, port, cmd, PTUP_ERR_STATE, seq, 0, "busy") ;
				break ;
			}
			count = get32 (buff + 8) ;
			if (count == 0 || count > PT_APP_MAX)
			{
				reply_to (socket, address, port, cmd, PTUP_ERR_SIZE, seq, PT_APP_MAX, NULL) ;
				break ;
			}
			flash_write (PT_META_BASE, NULL, PT_SECTOR, 1) ;			// cancel anything staged before
			memcpy (session.host, address, 4) ;
			session.port 	= port ;
			session.size 	= count ;
			session.crc 	= get32 (buff + 12) ;
			session.next 	= 0 ;
			session.last_ms = monotime_ms_ota() ;
			session.active 	= 1 ;
			printf ("Update: receiving %u bytes\r\n", (unsigned) count) ;
			reply_to (socket, session.host, session.port, cmd, PTUP_OK, seq, PTUP_MAX_DATA, NULL) ;
			break ;

		case PTUP_CMD_DATA:
			if (!from_session)
			{
				reply_to (socket, address, port, cmd, PTUP_ERR_STATE, seq, 0, NULL) ;
				break ;
			}
			session.last_ms = monotime_ms_ota() ;
			if (length < PTUP_HEADER_SIZE + 6)
			{
				reply_to (socket, session.host, session.port, cmd, PTUP_ERR_FORMAT, seq, session.next, NULL) ;
				break ;
			}
			offset = get32 (buff + 8) ;
			count  = get16 (buff + 12) ;
			if (offset + count == session.next && offset < session.next)
			{
				reply_to (socket, session.host, session.port, cmd, PTUP_OK, seq, session.next, NULL) ;		// a resend: our reply got lost
				break ;
			}
			if
			(
				count == 0 || count > PTUP_MAX_DATA || length != PTUP_HEADER_SIZE + 6 + (int) count ||
				offset + count > session.size
			)
			{
				reply_to (socket, session.host, session.port, cmd, PTUP_ERR_FORMAT, seq, session.next, NULL) ;
				break ;
			}
			if (offset != session.next)
			{
				reply_to (socket, session.host, session.port, cmd, PTUP_ERR_OFFSET, seq, session.next, NULL) ;
				break ;
			}
			while (count)												// a packet may straddle two sectors
			{
				used = session.next % PT_SECTOR ;
				uint32_t	chunk = PT_SECTOR - used < count ? PT_SECTOR - used : count ;

				memcpy (sector_buffer + used, buff + PTUP_HEADER_SIZE + 6 + (session.next - offset), chunk) ;
				session.next += chunk ;
				count 		 -= chunk ;
				if (used + chunk == PT_SECTOR)
				{
					flush_sector (PT_SECTOR) ;
				}
			}
			reply_to (socket, session.host, session.port, cmd, PTUP_OK, seq, session.next, NULL) ;
			break ;

		case PTUP_CMD_END:
			if (!from_session)
			{
				reply_to (socket, address, port, cmd, PTUP_ERR_STATE, seq, 0, NULL) ;
				break ;
			}
			status = finish() ;
			if (status == PTUP_OK)
			{
				staged = 1 ;
				printf ("Update: %u bytes staged, CRC OK, rebooting\r\n", (unsigned) session.size) ;
				reply_to (socket, session.host, session.port, cmd, PTUP_OK, seq, session.size, "staged, rebooting") ;
			}
			else
			{
				printf ("Update: failed (%d)\r\n", status) ;
				reply_to (socket, session.host, session.port, cmd, status, seq, session.next, NULL) ;
			}
			session.active = 0 ;
			break ;

		case PTUP_CMD_ABORT:
			if (from_session)
			{
				session.active = 0 ;
				printf ("Update: aborted\r\n") ;
			}
			reply_to (socket, address, port, cmd, PTUP_OK, seq, 0, NULL) ;
			break ;

		default:
			reply_to (socket, address, port, cmd, PTUP_ERR_FORMAT, seq, 0, NULL) ;
			break ;
	}
}


// called from the main loop on Core0

void ota_poll (int dhcp_ok)
{
	const pt_meta_t	*m = meta() ;

	if (session.active && monotime_ms_ota() - session.last_ms >= OTA_SESSION_TIMEOUT_MS)
	{
		session.active = 0 ;
		printf ("Update: session timed out\r\n") ;
	}

	if (!confirm_checked && dhcp_ok && monotime_ms_ota() >= PT_CONFIRM_MS)
	{
		confirm_checked = 1 ;
		if
		(
			pt_meta_header_valid (m) 			&&
			m->installed   == PT_FLAG_SET		&&
			m->confirmed   == PT_FLAG_CLEAR		&&
			m->rolled_back == PT_FLAG_CLEAR
		)
		{
			pt_meta_t	*p = (pt_meta_t *) page_buffer ;

			memcpy (page_buffer, (const void *) PT_META_BASE, PT_PAGE) ;
			p->confirmed = PT_FLAG_SET ;
			flash_write (PT_META_BASE, page_buffer, PT_PAGE, 0) ;
			printf ("Update: new firmware confirmed\r\n") ;
		}
	}
}
