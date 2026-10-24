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

	Core1 runs from flash and talks to the NIM, so every erase/program pauses it: Core0 raises
	pause_request, Core1 parks itself in a RAM loop with its interrupts off and answers with
	core1_parked (ota_core1_poll() in its main loop). Every wait is bounded; a timeout fails that
	flash write instead of hanging. The SDK multicore lockout used before hung right after the
	erase once the NIM was fitted (2026-10-23), and nothing here prints: printf from Core0 in the
	update path was a suspect too. TS output stops for each sector write (some tens of ms).

	Progress is written to watchdog scratch register 0, which survives a watchdog reset, so after
	one the next INFO reply says where the update stopped.
*/

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/structs/watchdog.h"
#include "socket.h"

#include "../common/ptupdate.h"
#include "ota.h"

#ifndef PTUP_KEY
#define PTUP_KEY				"picotuner"
#endif

#define OTA_SESSION_TIMEOUT_MS	30000
#define OTA_PAUSE_TIMEOUT_US	2000000				// Core1 must park within this
#define OTA_MARK_MAGIC			0x0da70000u			// in watchdog scratch[0] with a step number

enum { MARK_IDLE, MARK_PAUSE_WAIT, MARK_PARKED, MARK_ERASE, MARK_PROGRAM, MARK_RELEASED, MARK_PAUSE_TIMEOUT,
	   MARK_FLASH_DONE, MARK_IRQ_ON, MARK_XIP_SAME, MARK_XIP_FAR, MARK_PLANNED_REBOOT } ;

extern char 	__flash_binary_end ;

static volatile int	core1_running ;
static volatile int	pause_request ;
static volatile int	core1_parked ;
static void			(*idle_hook) (void) ;			// lets Core0 serve Core1's send requests while waiting
static void			(*ts_hook) (int stop) ;			// stops / restarts TS capture (PIO, DMA interrupts) around a session
static uint32_t		last_mark ;						// step the previous run stopped at, if a watchdog reset
static uint32_t		last_trace [3] ;				// Core0 / Core1 trace points and update offset before that reset
static int			after_wd ;

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

static void session_end (void)
{
	if (session.active && ts_hook)
	{
		ts_hook (0) ;												// TS capture back on
	}
	session.active = 0 ;
}


static uint8_t	sector_buffer [PT_SECTOR] ;
static uint8_t	page_buffer   [PT_PAGE] ;
static int		staged ;							// a verified image waits for ptboot
static int		confirm_checked ;
static uint32_t	confirm_retry_ms ;
static char		state_text [220] ;


static void mark (uint32_t step)
{
	watchdog_hw->scratch[0] = OTA_MARK_MAGIC | step ;
}


void ota_init (int after_watchdog, void (*hook) (void), void (*ts) (int stop))
{
	uint32_t	value = watchdog_hw->scratch[0] ;

	if (value == (OTA_MARK_MAGIC | MARK_PLANNED_REBOOT))
	{
		after_watchdog = 0 ;									// our own reboot (command, update), not a hang
	}
	after_wd = after_watchdog ;
	if (after_watchdog && (value & 0xffff0000u) == OTA_MARK_MAGIC && (value & 0xffff) != MARK_IDLE)
	{
		last_mark = value & 0xffff ;
	}
	if (after_watchdog)
	{
		last_trace [0] = watchdog_hw->scratch[1] ;
		last_trace [1] = watchdog_hw->scratch[2] ;
		last_trace [2] = watchdog_hw->scratch[3] ;
	}
	watchdog_hw->scratch[1] = 0 ;
	watchdog_hw->scratch[2] = 0 ;
	watchdog_hw->scratch[3] = 0 ;
	idle_hook = hook ;
	ts_hook   = ts ;
	mark (MARK_IDLE) ;
}


// called just before a deliberate watchdog_reboot(), so the next start does not report a hang

void ota_planned_reboot (void)
{
	mark (MARK_PLANNED_REBOOT) ;
}


// where each core is, kept over a watchdog reset (scratch 1 and 2; scratch 3: update offset)

void ota_trace (int core, uint32_t where)
{
	watchdog_hw->scratch[1 + (core & 1)] = where ;
}


void ota_core1_ready (void)
{
	core1_running = 1 ;
}


// Core1, from RAM: wait here, interrupts off, while Core0 erases or programs the flash. It must not be
// inlined: __not_in_flash_func alone let the compiler inline it into ota_core1_poll() in flash, so
// Core1 spun in XIP during the erase and hung the chip (seen on 2026-10-24 in the tuner).

static void __no_inline_not_in_flash_func (core1_park) (void)
{
	uint32_t	interrupts = save_and_disable_interrupts() ;

	core1_parked = 1 ;
	while (pause_request)
	{
		__asm volatile ("nop") ;
	}
	core1_parked = 0 ;
	restore_interrupts (interrupts) ;
}


void ota_core1_poll (void)
{
	if (pause_request)
	{
		core1_park() ;
	}
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


// erase (optionally) and program flash at an XIP address, with Core1 parked; 0 or -1 (Core1 did not park)

static int flash_write (uint32_t address, const uint8_t *data, uint32_t length, int erase)
{
	uint32_t	interrupts ;
	uint32_t	offset = address - XIP_BASE ;
	uint32_t	t0 ;

	if (core1_running)
	{
		mark (MARK_PAUSE_WAIT) ;
		pause_request = 1 ;
		t0 = time_us_32() ;
		while (!core1_parked)
		{
			if (idle_hook)
			{
				idle_hook() ;
			}
			if (time_us_32() - t0 > OTA_PAUSE_TIMEOUT_US)
			{
				pause_request = 0 ;
				mark (MARK_PAUSE_TIMEOUT) ;
				return (-1) ;
			}
		}
		mark (MARK_PARKED) ;
	}

	interrupts = save_and_disable_interrupts() ;
	if (erase)
	{
		mark (MARK_ERASE) ;
		flash_range_erase (offset, (length + PT_SECTOR - 1) & ~(PT_SECTOR - 1)) ;
	}
	if (data)
	{
		mark (MARK_PROGRAM) ;
		flash_range_program (offset, data, length) ;
	}
	mark (MARK_FLASH_DONE) ;									// still with interrupts off
	restore_interrupts (interrupts) ;
	mark (MARK_IRQ_ON) ;										// pending interrupts have run

	pause_request = 0 ;
	t0 = time_us_32() ;
	while (core1_running && core1_parked && time_us_32() - t0 < OTA_PAUSE_TIMEOUT_US)
	{
	}
	mark (MARK_RELEASED) ;
	mark (MARK_IDLE) ;
	return (0) ;
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
	ota_trace (0, 0x30) ;
	sendto (socket, buff, length, address, port) ;
	ota_trace (0, 0x31) ;
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
	if (after_wd)
	{
		snprintf (state_text + strlen (state_text), sizeof(state_text) - strlen (state_text),
				  "; after watchdog reset: core0 %02x core1 %02x offset %u",
				  (unsigned) last_trace[0], (unsigned) last_trace[1], (unsigned) last_trace[2]) ;
	}
	if (last_mark)
	{
		static const char *names [] = {"idle", "waiting for Core1", "Core1 parked", "erasing", "programming", "released", 
									   "Core1 pause timeout", "flash done, interrupts off", "interrupts back on",
									   "XIP read of the written sector", "XIP read far away"} ;
		snprintf (state_text + strlen (state_text), sizeof(state_text) - strlen (state_text), 
				  "; watchdog reset during: %s", last_mark < 11 ? names[last_mark] : "?") ;
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


static int flush_sector (uint32_t used)
{
	uint32_t	sector = (session.next - 1) / PT_SECTOR ;		// the sector that holds the last byte

	memset (sector_buffer + used, 0xff, PT_SECTOR - used) ;
	return (flash_write (PT_STAGE_BASE + sector * PT_SECTOR, sector_buffer, PT_SECTOR, 1)) ;
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
	if (used && flush_sector (used) != 0)						// the last, partly filled sector
	{
		return (PTUP_ERR_FLASH) ;
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
	if (flash_write (PT_META_BASE, page_buffer, PT_PAGE, 0) != 0 || !pt_meta_header_valid (meta()))	// erased at BEGIN
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
			if (!session.active && ts_hook)
			{
				ts_hook (1) ;											// no TS DMA interrupts while flash writes hold interrupts off
			}
			session.active = 1 ;
			if (flash_write (PT_META_BASE, NULL, PT_SECTOR, 1) != 0)	// cancel anything staged before
			{
				session_end() ;
				reply_to (socket, address, port, cmd, PTUP_ERR_FLASH, seq, 0, "Core1 did not pause") ;
				break ;
			}
			memcpy (session.host, address, 4) ;
			session.port 	= port ;
			session.size 	= count ;
			session.crc 	= get32 (buff + 12) ;
			session.next 	= 0 ;
			session.last_ms = monotime_ms_ota() ;
			reply_to (socket, session.host, session.port, cmd, PTUP_OK, seq, PTUP_MAX_DATA, NULL) ;
			break ;

		case PTUP_CMD_DATA:
			ota_trace (0, 0x20) ;
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
				watchdog_hw->scratch[3] = session.next ;
				if (used + chunk == PT_SECTOR && (ota_trace (0, 0x21), flush_sector (PT_SECTOR)) != 0)
				{
					session.next -= chunk ;							// the host resends this packet
					break ;
				}
			}
			ota_trace (0, 0x23) ;
			reply_to (socket, session.host, session.port, cmd, PTUP_OK, seq, session.next, NULL) ;
			ota_trace (0, 0x24) ;
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
				reply_to (socket, session.host, session.port, cmd, PTUP_OK, seq, session.size, "staged, rebooting") ;
			}
			else
			{
				reply_to (socket, session.host, session.port, cmd, status, seq, session.next, NULL) ;
			}
			if (status == PTUP_OK)
			{
				session.active = 0 ;									// the reboot restarts everything
			}
			else
			{
				session_end() ;
			}
			break ;

		case PTUP_CMD_ABORT:
			if (from_session)
			{
				session_end() ;
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
		session_end() ;
	}

	if (!confirm_checked && dhcp_ok && monotime_ms_ota() >= PT_CONFIRM_MS && monotime_ms_ota() >= confirm_retry_ms)
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
			if (flash_write (PT_META_BASE, page_buffer, PT_PAGE, 0) != 0)
			{
				confirm_checked  = 0 ;									// try again in 10 s
				confirm_retry_ms = monotime_ms_ota() + 10000 ;
			}
		}
	}
}
