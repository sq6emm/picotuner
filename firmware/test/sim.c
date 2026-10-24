/* Host simulation of PicoTuneWH updates: ota.c and ptboot.c built for x86-64 against a 2 MB
   NOR-flash model mapped at the real XIP address 0x10000000.

     sim mkflash FLASH APP.bin        new flash with APP in the app slot (as installed over USB)
     sim serve FLASH PORT             ota.c on a UDP port until an image is staged (ptupdate.py)
     sim boot FLASH                   one ptboot run: prints "app CRC" or "bootsel"
     sim confirm FLASH                ota_poll() after 61 s with an IP address
     sim selftest                     update, rollback and power cuts at every flash operation

   The old application's end (__flash_binary_end) is fixed at link time: OLD_APP_SIZE. */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include "sim.h"
#include "../common/ptupdate.h"
#include "../picotunewh/ota.h"

#define FLASH_SIZE		(2u * 1024 * 1024)
#define FLASH			((uint8_t *) (uintptr_t) XIP_BASE)

int ptboot_main (void) ;

static jmp_buf	boot_exit ;
static long		ops_until_cut = -1 ;					// power cut after this many flash operations
static long		flash_ops ;
static uint32_t	now_ms ;
static int		udp = -1 ;
static volatile int	core1_in_poll ;					// the simulated Core1 is inside ota_core1_poll()
static volatile int	core1_stop ;
static pthread_t	core1_thread ;

enum { EXIT_APP = 1, EXIT_BOOTSEL, EXIT_POWER_CUT } ;

// ---- flash model: erase sets 0xff, program can only clear bits; a cut leaves the op half done

static void flash_op (uint8_t *start, size_t count, const uint8_t *data)
{
	size_t	n = count ;
	size_t	i ;
	int		cut = ops_until_cut == 0 ;

	if (cut)
	{
		n = count / 2 ;
	}
	for (i = 0 ; i < n ; i++)
	{
		start [i] = data ? start [i] & data [i] : 0xff ;
	}
	flash_ops++ ;
	if (ops_until_cut >= 0)
	{
		ops_until_cut-- ;
	}
	if (cut)
	{
		longjmp (boot_exit, EXIT_POWER_CUT) ;
	}
}

void flash_range_erase (uint32_t offset, size_t count)
{
	if (offset % PT_SECTOR || count % PT_SECTOR || offset + count > FLASH_SIZE)
	{
		fprintf (stderr, "bad erase %#x %#zx\n", offset, count) ; exit (2) ;
	}
	if (offset < PT_BOOT_SIZE)
	{
		fprintf (stderr, "erase of the boot area %#x\n", offset) ; exit (2) ;
	}
	if (core1_thread && !core1_in_poll)
	{
		fprintf (stderr, "flash erased with Core1 running\n") ; exit (2) ;
	}
	flash_op (FLASH + offset, count, NULL) ;
}

void flash_range_program (uint32_t offset, const uint8_t *data, size_t count)
{
	if (offset % PT_PAGE || count % PT_PAGE || offset + count > FLASH_SIZE || offset < PT_BOOT_SIZE)
	{
		fprintf (stderr, "bad program %#x %#zx\n", offset, count) ; exit (2) ;
	}
	if (core1_thread && !core1_in_poll)
	{
		fprintf (stderr, "flash written with Core1 running\n") ; exit (2) ;
	}
	flash_op (FLASH + offset, count, data) ;
}

uint32_t save_and_disable_interrupts (void)			{ return (0) ; }
void restore_interrupts (uint32_t status)			{ (void) status ; }
absolute_time_t get_absolute_time (void)			{ return (now_ms) ; }
uint32_t to_ms_since_boot (absolute_time_t t)		{ return ((uint32_t) t) ; }
void irq_set_mask_enabled (uint32_t m, bool e)		{ (void) m ; (void) e ; }
watchdog_hw_t sim_watchdog ;
uint32_t time_us_32 (void)
{
	struct timespec	t ;

	clock_gettime (CLOCK_MONOTONIC, &t) ;
	return ((uint32_t) (t.tv_sec * 1000000u + t.tv_nsec / 1000)) ;
}

// Core1 as a thread: its main loop calls ota_core1_poll() between other work

static void *core1_loop (void *arg)
{
	(void) arg ;
	while (!core1_stop)
	{
		core1_in_poll = 1 ;
		ota_core1_poll() ;
		core1_in_poll = 0 ;
		usleep (2000) ;										// the rest of its loop: I2C, status
	}
	return (NULL) ;
}

static void core1_start (void)
{
	ota_core1_ready() ;
	core1_stop = 0 ;
	pthread_create (&core1_thread, NULL, core1_loop, NULL) ;
}

static void core1_halt (void)							// Core1 stops answering (stuck elsewhere)
{
	core1_stop = 1 ;
	pthread_join (core1_thread, NULL) ;
	core1_thread = 0 ;
}
void reset_usb_boot (uint32_t a, uint32_t b)		{ (void) a ; (void) b ; longjmp (boot_exit, EXIT_BOOTSEL) ; }
void start_application (void)						{ longjmp (boot_exit, EXIT_APP) ; }

static int		last_status = -1 ;							// status byte of the last PTUP reply

int32_t wiz_sendto (uint8_t sn, uint8_t *buf, uint16_t len, uint8_t *addr, uint16_t port)
{
	struct sockaddr_in	to = { .sin_family = AF_INET, .sin_port = htons (port) } ;

	(void) sn ;
	if (len > 5)
	{
		last_status = buf [5] ;
	}
	memcpy (&to.sin_addr, addr, 4) ;
	return ((int32_t) sendto (udp, buf, len, 0, (struct sockaddr *) &to, sizeof(to))) ;
}

// ---- helpers

static void map_flash (void)
{
	if (mmap (FLASH, FLASH_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != FLASH)
	{
		perror ("mmap at 0x10000000") ; exit (2) ;
	}
	memset (FLASH, 0xff, FLASH_SIZE) ;
}

static void load (const char *path)
{
	FILE	*f = fopen (path, "rb") ;

	if (!f || fread (FLASH, 1, FLASH_SIZE, f) != FLASH_SIZE) { perror (path) ; exit (2) ; }
	fclose (f) ;
}

static void save (const char *path)
{
	FILE	*f = fopen (path, "wb") ;

	if (!f || fwrite (FLASH, 1, FLASH_SIZE, f) != FLASH_SIZE) { perror (path) ; exit (2) ; }
	fclose (f) ;
}

static int boot (void)									// one ptboot run
{
	int		result = setjmp (boot_exit) ;

	if (result == 0)
	{
		ptboot_main() ;
		return (EXIT_BOOTSEL) ;
	}
	return (result) ;
}

static uint32_t slot_crc (uint32_t base, uint32_t size)
{
	return (pt_crc32 ((const void *) (uintptr_t) base, size)) ;
}

// a synthetic application: random bytes with a vector table for the app slot

static void make_image (uint8_t *image, uint32_t size, unsigned seed)
{
	uint32_t	vectors [2] = { 0x20042000u, PT_APP_BASE + 0x1000 + 1 } ;
	uint32_t	i ;

	srand (seed) ;
	for (i = 0 ; i < size ; i++)
	{
		image [i] = (uint8_t) rand() ;
	}
	memcpy (image + PT_VECTOR_OFFSET, vectors, 8) ;
}

// stage an image the way ota.c's finish() does (selftest only)

static void stage (const uint8_t *image, uint32_t size, uint32_t old_size)
{
	pt_meta_t	header ;

	memset (FLASH + (PT_META_BASE - XIP_BASE), 0xff, PT_SECTOR) ;
	memset (FLASH + (PT_STAGE_BASE - XIP_BASE), 0xff, PT_STAGE_MAX) ;
	memcpy (FLASH + (PT_STAGE_BASE - XIP_BASE), image, size) ;
	memset (&header, 0xff, sizeof(header)) ;
	header.magic 		= PT_META_MAGIC ;
	header.size 		= size ;
	header.crc 			= pt_crc32 (image, size) ;
	header.old_size 	= old_size ;
	header.old_crc 		= slot_crc (PT_APP_BASE, old_size) ;
	header.header_crc 	= pt_crc32 (&header, PT_META_HEADER_WORDS * 4) ;
	memcpy (FLASH + (PT_META_BASE - XIP_BASE), &header, sizeof(header)) ;
}

static const pt_meta_t *meta_ (void)	{ return ((const pt_meta_t *) (uintptr_t) PT_META_BASE) ; }

static int failures ;

static void expect (int ok, const char *what)
{
	printf ("  %-60s %s\n", what, ok ? "ok" : "FAIL") ;
	failures += !ok ;
}

// boot until the app starts; power cuts are retried

static int boot_until_app (int limit)
{
	int		n ;

	for (n = 0 ; n < limit ; n++)
	{
		int r = boot() ;
		if (r == EXIT_APP)		return (1) ;
		if (r == EXIT_BOOTSEL)	return (0) ;
	}
	return (0) ;
}

static void selftest (void)
{
	static uint8_t	old [OLD_APP_SIZE] ;
	static uint8_t	new [0x21345] ;
	static uint8_t	saved [FLASH_SIZE] ;
	uint32_t		old_crc ;
	uint32_t		new_crc ;
	long			total ;
	long			cut ;
	int				bad ;
	int				n ;

	map_flash() ;
	make_image (old, sizeof(old), 1) ;
	make_image (new, sizeof(new), 2) ;
	old_crc = pt_crc32 (old, sizeof(old)) ;
	new_crc = pt_crc32 (new, sizeof(new)) ;

	printf ("empty flash\n") ;
	expect (boot() == EXIT_BOOTSEL, "no application: USB BOOTSEL") ;

	printf ("plain boot, no update\n") ;
	memcpy (FLASH + (PT_APP_BASE - XIP_BASE), old, sizeof(old)) ;
	flash_ops = 0 ;
	expect (boot() == EXIT_APP && flash_ops == 0, "starts the app, no flash writes") ;

	printf ("update\n") ;
	stage (new, sizeof(new), sizeof(old)) ;
	memcpy (saved, FLASH, FLASH_SIZE) ;
	flash_ops = 0 ;
	expect (boot() == EXIT_APP, "first boot starts the app") ;
	total = flash_ops ;
	expect (slot_crc (PT_APP_BASE, sizeof(new)) == new_crc, "app slot holds the new image") ;
	expect (slot_crc (PT_BACKUP_BASE, sizeof(old)) == old_crc, "backup holds the old image") ;
	expect (meta_()->installed == PT_FLAG_SET && meta_()->boots[0] == PT_FLAG_SET, "installed, trial boot 1 counted") ;
	printf ("  (%ld flash operations)\n", total) ;

	printf ("power cut at each of the %ld flash operations of the install\n", total) ;
	bad = 0 ;
	for (cut = 0 ; cut < total ; cut++)
	{
		memcpy (FLASH, saved, FLASH_SIZE) ;
		ops_until_cut = cut ;
		if (boot() != EXIT_POWER_CUT)			{ bad++ ; continue ; }
		ops_until_cut = -1 ;
		if (!boot_until_app (3))				{ bad++ ; continue ; }
		if (slot_crc (PT_APP_BASE, sizeof(new)) != new_crc || meta_()->installed != PT_FLAG_SET)
		{
			bad++ ;
		}
	}
	expect (bad == 0, "always ends with the new image installed") ;

	printf ("no confirmation: rollback\n") ;
	memcpy (FLASH, saved, FLASH_SIZE) ;
	for (n = 1 ; n <= PT_TRIAL_BOOTS ; n++)
	{
		boot() ;
	}
	expect (slot_crc (PT_APP_BASE, sizeof(new)) == new_crc, "new image runs for 3 boots") ;
	memcpy (saved, FLASH, FLASH_SIZE) ;
	flash_ops = 0 ;
	expect (boot() == EXIT_APP, "4th boot starts the app") ;
	total = flash_ops ;
	expect (slot_crc (PT_APP_BASE, sizeof(old)) == old_crc, "app slot holds the old image again") ;
	expect (meta_()->rolled_back == PT_FLAG_SET, "rolled back") ;
	flash_ops = 0 ;
	boot() ;
	expect (flash_ops == 0 && slot_crc (PT_APP_BASE, sizeof(old)) == old_crc, "later boots: old image, no writes") ;

	printf ("power cut at each of the %ld flash operations of the rollback\n", total) ;
	bad = 0 ;
	for (cut = 0 ; cut < total ; cut++)
	{
		memcpy (FLASH, saved, FLASH_SIZE) ;
		ops_until_cut = cut ;
		if (boot() != EXIT_POWER_CUT)			{ bad++ ; continue ; }
		ops_until_cut = -1 ;
		if (!boot_until_app (3) || slot_crc (PT_APP_BASE, sizeof(old)) != old_crc)
		{
			bad++ ;
		}
	}
	expect (bad == 0, "always ends with the old image") ;

	printf ("confirmed\n") ;
	memcpy (FLASH + (PT_APP_BASE - XIP_BASE), old, sizeof(old)) ;
	stage (new, sizeof(new), sizeof(old)) ;
	boot() ;
	now_ms = 61000 ;
	core1_start() ;
	ota_poll (1) ;
	core1_halt() ;
	expect (meta_()->confirmed == PT_FLAG_SET, "ota_poll() confirms after 60 s, Core1 parked") ;

	printf ("Core1 never parks\n") ;
	{
		uint8_t		begin [32] = "PTUP" ;
		uint8_t		host [4] = {127, 0, 0, 1} ;
		uint32_t	size = sizeof(new) ;
		uint32_t	crc = pt_crc32 (new, sizeof(new)) ;
		uint32_t	t0 ;
		uint8_t		meta_before [PT_PAGE] ;

		memcpy (meta_before, (const void *) (uintptr_t) PT_META_BASE, PT_PAGE) ;
		begin [4] = PTUP_CMD_BEGIN ;
		memcpy (begin + 8, &size, 4) ;
		memcpy (begin + 12, &crc, 4) ;
		begin [16] = 9 ;
		memcpy (begin + 17, "picotuner", 9) ;
		ota_core1_ready() ;										// says it runs, but never polls
		t0 = time_us_32() ;
		ota_handle (0, begin, 26, host, 1234) ;
		expect (time_us_32() - t0 >= 1900000 && time_us_32() - t0 < 4000000, "BEGIN gives up after ~2 s, no hang") ;
		expect (memcmp (meta_before, (const void *) (uintptr_t) PT_META_BASE, PT_PAGE) == 0, "metadata not erased") ;
		expect (!ota_active(), "no session started") ;
	}
	for (n = 0 ; n < 6 ; n++)
	{
		boot() ;
	}
	expect (slot_crc (PT_APP_BASE, sizeof(new)) == new_crc, "6 more boots: still the new image") ;

	printf ("corrupt staging\n") ;
	memcpy (FLASH + (PT_APP_BASE - XIP_BASE), old, sizeof(old)) ;
	stage (new, sizeof(new), sizeof(old)) ;
	FLASH [PT_STAGE_BASE - XIP_BASE + 5000] ^= 1 ;
	expect (boot() == EXIT_APP && slot_crc (PT_APP_BASE, sizeof(old)) == old_crc, "ignored, old image runs") ;
	expect (!pt_meta_header_valid (meta_()), "metadata cleared") ;

	printf ("BEGIN while on trial\n") ;							// 0v3e-ota17: refused, so the trial is not lost
	{
		uint8_t		begin [32] = "PTUP" ;
		uint8_t		host [4] = {127, 0, 0, 1} ;
		uint32_t	size = sizeof(new) ;
		uint32_t	crc = pt_crc32 (new, sizeof(new)) ;

		memcpy (FLASH + (PT_APP_BASE - XIP_BASE), old, sizeof(old)) ;
		stage (new, sizeof(new), sizeof(old)) ;
		boot() ;
		begin [4] = PTUP_CMD_BEGIN ;
		memcpy (begin + 8, &size, 4) ;
		memcpy (begin + 12, &crc, 4) ;
		begin [16] = 9 ;
		memcpy (begin + 17, "picotuner", 9) ;
		core1_start() ;
		ota_handle (0, begin, 26, host, 1234) ;
		expect (!ota_active(), "refused during the trial") ;
		expect (meta_()->installed == PT_FLAG_SET && meta_()->confirmed != PT_FLAG_SET, "trial kept") ;
		((pt_meta_t *) (FLASH + (PT_META_BASE - XIP_BASE)))->confirmed = PT_FLAG_SET ;	// as ota_poll() would after 60 s
		ota_handle (0, begin, 26, host, 1234) ;
		expect (ota_active(), "accepted once confirmed") ;

		printf ("malformed packets\n") ;							// the session above is still open
		{
			uint8_t		other [4] = {10, 0, 0, 9} ;
			uint8_t		data [PTUP_HEADER_SIZE + 6 + 64] = "PTUP" ;
			uint32_t	off = 0 ;
			uint16_t	cnt = 64 ;

			ota_handle (0, begin, 26, other, 1234) ;
			expect (last_status == PTUP_ERR_STATE, "BEGIN from another host during a session: busy") ;
			data [4] = PTUP_CMD_DATA ;
			memcpy (data + 8, &off, 4) ;
			memcpy (data + 12, &cnt, 2) ;
			ota_handle (0, data, sizeof(data) - 1, host, 1234) ;
			expect (last_status == PTUP_ERR_FORMAT, "DATA shorter than its count: format error") ;
			off = 4096 ;
			memcpy (data + 8, &off, 4) ;
			ota_handle (0, data, sizeof(data), host, 1234) ;
			expect (last_status == PTUP_ERR_OFFSET, "DATA at the wrong offset: offset error") ;
			off = 0xfffffff0u ;
			memcpy (data + 8, &off, 4) ;
			ota_handle (0, data, sizeof(data), host, 1234) ;
			expect (last_status != PTUP_OK, "DATA with an offset that wraps: refused") ;
			cnt = 2000 ;
			off = 0 ;
			memcpy (data + 8, &off, 4) ;
			memcpy (data + 12, &cnt, 2) ;
			ota_handle (0, data, sizeof(data), host, 1234) ;
			expect (last_status == PTUP_ERR_FORMAT, "DATA count beyond the packet: format error") ;
			ota_handle (0, data, sizeof(data), other, 1234) ;
			expect (last_status == PTUP_ERR_STATE, "DATA from another host: refused") ;
			expect (ota_active(), "the session survives all of it") ;
		}
		core1_halt() ;
	}

	printf ("malformed packets without a session\n") ;
	{
		uint8_t		host [4] = {127, 0, 0, 1} ;
		uint8_t		shortbegin [12] = "PTUP" ;
		uint8_t		badkey [32] = "PTUP" ;
		uint32_t	size = 1000 ;

		expect (!ota_is_packet ((const uint8_t *) "PTUP", 4) && !ota_is_packet ((const uint8_t *) "XXXXXXXXXXXX", 12),
				"too short or not PTUP: not taken as an update packet") ;
		shortbegin [4] = PTUP_CMD_BEGIN ;
		ota_handle (0, shortbegin, sizeof(shortbegin), host, 5678) ;
		expect (last_status == PTUP_ERR_FORMAT || last_status == PTUP_ERR_STATE, "BEGIN without size and key: refused") ;
		badkey [4] = PTUP_CMD_BEGIN ;
		memcpy (badkey + 8, &size, 4) ;
		badkey [16] = 9 ;
		memcpy (badkey + 17, "wrong-key", 9) ;
		ota_handle (0, badkey, 26, host, 5678) ;
		expect (last_status == PTUP_ERR_KEY || last_status == PTUP_ERR_STATE, "BEGIN with a wrong key: refused") ;
	}

	printf ("%s\n", failures ? "FAILED" : "all passed") ;
	exit (failures != 0) ;
}

// ---- OTA server for ptupdate.py

static void serve (const char *path, int port)
{
	struct sockaddr_in	me = { .sin_family = AF_INET, .sin_port = htons (port), .sin_addr.s_addr = htonl (INADDR_LOOPBACK) } ;
	struct sockaddr_in	from ;
	socklen_t			fromlen ;
	uint8_t				buff [1400] ;
	ssize_t				n ;

	core1_start() ;
	udp = socket (AF_INET, SOCK_DGRAM, 0) ;
	if (bind (udp, (struct sockaddr *) &me, sizeof(me)) < 0) { perror ("bind") ; exit (2) ; }
	printf ("listening on 127.0.0.1:%d\n", port) ;
	fflush (stdout) ;
	while (!ota_reboot_wanted())
	{
		fromlen = sizeof(from) ;
		n = recvfrom (udp, buff, sizeof(buff), 0, (struct sockaddr *) &from, &fromlen) ;
		now_ms += 10 ;
		if (n > 0 && ota_is_packet (buff, (int) n))
		{
			ota_handle (0, buff, (int) n, (uint8_t *) &from.sin_addr, ntohs (from.sin_port)) ;
		}
		ota_poll (1) ;
	}
	usleep (200000) ;
	save (path) ;
	printf ("staged: %s\n", ota_state_text()) ;
}

int main (int argc, char **argv)
{
	if (argc == 2 && !strcmp (argv[1], "selftest"))
	{
		selftest() ;
	}
	map_flash() ;
	if (argc == 4 && !strcmp (argv[1], "mkflash"))
	{
		FILE	*f = fopen (argv[3], "rb") ;
		size_t	n = f ? fread (FLASH + (PT_APP_BASE - XIP_BASE), 1, PT_APP_MAX, f) : 0 ;

		if (n != OLD_APP_SIZE) { fprintf (stderr, "%s: need %d bytes, got %zu\n", argv[3], OLD_APP_SIZE, n) ; return (2) ; }
		save (argv[2]) ;
		return (0) ;
	}
	if (argc == 4 && !strcmp (argv[1], "serve"))
	{
		load (argv[2]) ;
		serve (argv[2], atoi (argv[3])) ;
		return (0) ;
	}
	if (argc == 3 && !strcmp (argv[1], "boot"))
	{
		int r ;

		load (argv[2]) ;
		r = boot() ;
		save (argv[2]) ;
		if (r == EXIT_APP)
		{
			const pt_meta_t	*m = meta_() ;
			uint32_t		size = pt_meta_header_valid (m) && m->rolled_back != PT_FLAG_SET ? m->size : OLD_APP_SIZE ;

			printf ("app %08x (%u bytes) - %s\n", slot_crc (PT_APP_BASE, size), size, ota_state_text()) ;
		}
		else
		{
			printf ("bootsel\n") ;
		}
		return (0) ;
	}
	if (argc == 3 && !strcmp (argv[1], "confirm"))
	{
		load (argv[2]) ;
		now_ms = 61000 ;
		core1_start() ;
		ota_poll (1) ;
		core1_halt() ;
		save (argv[2]) ;
		printf ("%s\n", ota_state_text()) ;
		return (0) ;
	}
	fprintf (stderr, "usage: see the top of sim.c\n") ;
	return (2) ;
}
