/* ================================================================================================== */
/* ptboot.c - bootloader for PicoTuneWH: installs images staged over Ethernet, rolls back on failure  */
/*                                                                                                    */
/*    This file is part of PicoTuneWH and is free software: you can redistribute it and/or modify     */
/*    it under the terms of the GNU General Public License as published by the Free Software          */
/*    Foundation, either version 3 of the License, or (at your option) any later version.             */
/* ================================================================================================== */
/*
	Runs from 0x10000000 after the boot2 stage, does the copying described in common/ptupdate.h,
	then starts the application at PT_APP_BASE. Every step can be interrupted by a power cut: the
	source of a copy is only released after the copy has been verified, so the next boot redoes it.
	With no runnable application and no backup it falls back to the ROM's USB BOOTSEL mode.
*/

#include <string.h>

#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/flash.h"
#include "hardware/irq.h"
#include "hardware/regs/m0plus.h"
#include "hardware/structs/scb.h"
#include "hardware/sync.h"
#include "hardware/structs/watchdog.h"

#include "../common/ptupdate.h"

#define META	((const pt_meta_t *) PT_META_BASE)

static uint8_t	buffer [PT_SECTOR] ;

// -DPTBOOT_DEBUG_LED: blink the Pico LED (GP25) n times at each step, for bring-up without a probe

#ifdef PTBOOT_DEBUG_LED
#include "hardware/gpio.h"
static void blink (int n, int ms)
{
	gpio_init (25) ;
	gpio_set_dir (25, GPIO_OUT) ;
	while (n--)
	{
		gpio_put (25, 1) ; busy_wait_ms (ms) ;
		gpio_put (25, 0) ; busy_wait_ms (ms) ;
	}
	busy_wait_ms (600) ;
}
#else
#define blink(n, ms)
#endif


static void program_meta_word (const volatile uint32_t *word)
{
	uint32_t	index = (uint32_t) word - PT_META_BASE ;

	memcpy (buffer, (const void *) PT_META_BASE, PT_PAGE) ;
	memset (buffer + index, 0, 4) ;								// flag words go from erased to 0
	flash_range_program (PT_META_BASE - XIP_BASE, buffer, PT_PAGE) ;
}


// copy and verify; the destination is erased a sector at a time

static int copy_image (uint32_t to, uint32_t from, uint32_t size, uint32_t crc)
{
	uint32_t	done ;
	uint32_t	chunk ;

	for (done = 0 ; done < size ; done += PT_SECTOR)
	{
		chunk = size - done < PT_SECTOR ? size - done : PT_SECTOR ;
		memset (buffer, 0xff, PT_SECTOR) ;
		memcpy (buffer, (const void *) (from + done), chunk) ;
		flash_range_erase   (to + done - XIP_BASE, PT_SECTOR) ;
		flash_range_program (to + done - XIP_BASE, buffer, PT_SECTOR) ;
	}
	return (pt_crc32 ((const void *) to, size) == crc) ;
}


static int image_ok (uint32_t base, uint32_t size, uint32_t crc)
{
	return (size && size <= PT_APP_MAX && pt_crc32 ((const void *) base, size) == crc) ;
}


static void update (void)
{
	const pt_meta_t	*m = META ;
	uint32_t		boots = 0 ;
	uint32_t		x ;

	if (!pt_meta_header_valid (m))
	{
		return ;
	}

// a new image waits in staging

	if (m->installed == PT_FLAG_CLEAR)
	{
		if (!image_ok (PT_STAGE_BASE, m->size, m->crc))
		{
			flash_range_erase (PT_META_BASE - XIP_BASE, PT_SECTOR) ;	// unusable: forget it
			return ;
		}
		if (m->backup_done == PT_FLAG_CLEAR && image_ok (PT_APP_BASE, m->old_size, m->old_crc))
		{
			if (copy_image (PT_BACKUP_BASE, PT_APP_BASE, m->old_size, m->old_crc))
			{
				program_meta_word (&m->backup_done) ;
			}
		}
		if (copy_image (PT_APP_BASE, PT_STAGE_BASE, m->size, m->crc))
		{
			program_meta_word (&m->installed) ;
		}
		else
		{
			return ;												// staging is intact, try again next boot
		}
	}

// a trial: count the boot, or roll back when the new image never confirmed itself

	if (m->confirmed == PT_FLAG_CLEAR && m->rolled_back == PT_FLAG_CLEAR)
	{
		for (x = 0 ; x < sizeof(m->boots) / 4 && m->boots[x] == PT_FLAG_SET ; x++)
		{
			boots++ ;
		}
		if (boots >= PT_TRIAL_BOOTS || boots >= sizeof(m->boots) / 4)
		{
			if (m->backup_done == PT_FLAG_SET && image_ok (PT_BACKUP_BASE, m->old_size, m->old_crc))
			{
				if (copy_image (PT_APP_BASE, PT_BACKUP_BASE, m->old_size, m->old_crc))
				{
					program_meta_word (&m->rolled_back) ;
				}
			}
			else
			{
				program_meta_word (&m->rolled_back) ;				// nothing to go back to: keep it
			}
		}
		else
		{
			program_meta_word (&m->boots[boots]) ;
		}
	}
}


#ifdef PTBOOT_TEST
void __attribute__((noreturn)) start_application (void) ;				// host test harness
#else
static void __attribute__((noreturn)) start_application (void)
{
	const uint32_t	*vectors = (const uint32_t *) (PT_APP_BASE + PT_VECTOR_OFFSET) ;

	irq_set_mask_enabled (0xffffffff, false) ;
	*((volatile uint32_t *) (PPB_BASE + M0PLUS_NVIC_ICPR_OFFSET)) = 0xffffffff ;	// nothing pending
	scb_hw->vtor = (uint32_t) vectors ;
	__asm volatile
	(
		"msr msp, %0\n"
		"bx  %1\n"
		: : "r" (vectors [0]), "r" (vectors [1])
	) ;
	__builtin_unreachable() ;
}
#endif


#ifdef PTBOOT_TEST
int ptboot_main (void)
#else
int main (void)
#endif
{
	const pt_meta_t	*m = META ;

	hw_clear_bits (&watchdog_hw->ctrl, WATCHDOG_CTRL_ENABLE_BITS) ;	// the app's watchdog may survive its reset
	blink (1, 400) ;												// ptboot runs
	update() ;

	if (pt_image_vectors_valid ((const uint8_t *) PT_APP_BASE, PT_APP_MAX))
	{
		blink (2, 150) ;											// jumping to the app
#ifdef PTBOOT_START_DELAY_MS
		busy_wait_ms (PTBOOT_START_DELAY_MS) ;						// bring-up experiment: cold start timing
#endif
		start_application() ;
	}

// the app slot is unusable: a power cut during a rollback, or nothing installed yet

	if
	(
		pt_meta_header_valid (m) 				&&
		m->backup_done == PT_FLAG_SET 			&&
		image_ok (PT_BACKUP_BASE, m->old_size, m->old_crc) &&
		copy_image (PT_APP_BASE, PT_BACKUP_BASE, m->old_size, m->old_crc)
	)
	{
		start_application() ;
	}

	blink (5, 80) ;													// no application: BOOTSEL
	reset_usb_boot (0, 0) ;
	while (1) ;
}
