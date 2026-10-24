/* ================================================================================================== */
/* ota.h - firmware update over Ethernet for PicoTuneWH (see common/ptupdate.h)                       */
/*                                                                                                    */
/*    This file is part of PicoTuneWH and is free software: you can redistribute it and/or modify     */
/*    it under the terms of the GNU General Public License as published by the Free Software          */
/*    Foundation, either version 3 of the License, or (at your option) any later version.             */
/* ================================================================================================== */

#ifndef OTA_H
#define OTA_H

#include <stdint.h>

void		ota_init			(int after_watchdog, void (*idle_hook) (void)) ;	// early in main()
void		ota_core1_ready		(void) ;										// called first thing on Core1
void		ota_core1_poll		(void) ;										// Core1 main loop: park for a flash write
int			ota_is_packet		(const uint8_t *buff, int length) ;
void		ota_handle			(int socket, const uint8_t *buff, int length, uint8_t *address, uint16_t port) ;
int			ota_active			(void) ;										// a session is in progress: poll fast
int			ota_reboot_wanted	(void) ;										// a new image is staged
void		ota_poll			(int dhcp_ok) ;									// timeouts, confirmation
const char*	ota_state_text		(void) ;

#endif
