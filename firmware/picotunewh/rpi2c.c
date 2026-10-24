/* -------------------------------------------------------------------------------------------------- */
/*																									  */
/* BATC WinterHill project																			  */
/* This file, rpi2c.c, was originally ftdi.c, Copyright 2019 Heather Lomond                           */
/*																									  */
/* i2c is now controlled by the Raspberry Pi /dev/i2c-1 											  */
/* References to USB have been removed																  */		
/* The transport streams are conveyed by PICs to SPI ports on a Raspberry Pi     	          		  */
/* The PICs are also on the i2c bus        															  */
/*																									  */
/* -------------------------------------------------------------------------------------------------- */
/*
    This file is part of longmynd

    Longmynd is software: you can redistribute it
    and/or modify it under the terms of the GNU General Public License 
    as published by the Free Software Foundation, either version 3 of the 
    License, or (at your option) any later version.

    Longmynd is distributed in the hope that it will 
    be useful, but WITHOUT ANY WARRANTY; without even the implied 
    warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  
    See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along 
    with longmynd.  If not, see <https://www.gnu.org/licenses/>.

    WinterHill December 2020:

    Parts modified for the BATC WinterHill 4 channel receiver project - terms as above
    Copyright Brian Jordan, G4EWJ
*/


/* -------------------------------------------------------------------------------------------------- */
/* ----------------- INCLUDES ----------------------------------------------------------------------- */
/* -------------------------------------------------------------------------------------------------- */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
///////#include <unistd.h>
#include "rpi2c.h"
#include "errors.h"
#include "nim.h"

#include <string.h>				
#include <fcntl.h>
#include <errno.h>
///#include <sys/ioctl.h>
///#include <linux/i2c-dev.h>

#include "globals.h"

/* -------------------------------------------------------------------------------------------------- */
/* ----------------- DEFINES ------------------------------------------------------------------------ */
/* -------------------------------------------------------------------------------------------------- */


#undef 	PICO_DEFAULT_I2C_SCL_PIN 	
#undef 	PICO_DEFAULT_I2C_SDA_PIN	
#undef	i2c_default

#define PICO_DEFAULT_I2C_SCL_PIN 	I2C1CLK
#define PICO_DEFAULT_I2C_SDA_PIN	I2C1DAT
#define i2c_default					&i2c1_inst							// i2c-1


static	int			i2cfd ;									// file descriptor for i2C
static	char		i2cname 	  [] = {"/dev/i2c-1"} ;		// system file name for I2C		 

		uint8_t     NIMI2CADDRESS [] = {0,NIM_DEMOD_ADDR_A,NIM_DEMOD_ADDR_B} ;



/* -------------------------------------------------------------------------------------------------- */
/* ----------------- ROUTINES ----------------------------------------------------------------------- */
/* -------------------------------------------------------------------------------------------------- */

uint rpi2c_init (uint baudrate)
{
	gpio_init 		(BOARDRESET);
    gpio_set_dir 	(BOARDRESET, GPIO_OUT);
    gpio_put 		(BOARDRESET, 0) ;   								// active low
	sleep_ms (100) ;
    gpio_put 		(BOARDRESET, 1) ;   								
	sleep_ms (1500) ;
	
	i2c_init			(i2c_default, baudrate) ;
    gpio_set_function	(PICO_DEFAULT_I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function	(PICO_DEFAULT_I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up		(PICO_DEFAULT_I2C_SDA_PIN);
    gpio_pull_up		(PICO_DEFAULT_I2C_SCL_PIN);


	return (0) ;
}


/* -------------------------------------------------------------------------------------------------- */
/* read an 8 bit value from a 16 bit address			         									  */
/*																									  */	
/*   addr: the 16 bit address of the i2c register to access 										  */
/*    reg: the 8 biti2c register to read                                                              */
/*   *val: the return value for the register we have read                                             */
/* return: error code                                                                                 */
/* -------------------------------------------------------------------------------------------------- */

// Bounded I2C transfers (0v3e-ota17): the SDK's *_blocking calls wait for ever on a stuck bus, which
// stopped Core1 until the watchdog reset, over and over, with no clue why. A transfer of up to three
// bytes takes about 0.3 ms at 100 kHz; failures are counted in i2c_errors ($41).

#define	I2C_TIMEOUT_US		10000

volatile	uint32_t	i2c_errors ;

static int i2c_write_bounded (uint8_t i2caddr, const uint8_t *data, size_t length)
{
	int		status = i2c_write_timeout_us (i2c_default, i2caddr >> 1, data, length, 0, I2C_TIMEOUT_US) ;

	if (status < 0)
	{
		i2c_errors++ ;
	}
	return (status) ;
}

static int i2c_read_bounded (uint8_t i2caddr, uint8_t *data, size_t length)
{
	int		status = i2c_read_timeout_us (i2c_default, i2caddr >> 1, data, length, 0, I2C_TIMEOUT_US) ;

	if (status < 0)
	{
		i2c_errors++ ;
	}
	return (status) ;
}

int i2c_read_reg16 (uint8_t i2caddr, uint16_t reg, uint8_t *val) 
{
    int 		status ;
	uint8_t		datax [2] ;									

	datax [0] = (reg >> 8) & 0xff ;
	datax [1] = (reg >> 0) & 0xff ;

	status = i2c_write_bounded (i2caddr, datax, 2) ;						// release bus at end
	if (status >= 0)
	{
		status = i2c_read_bounded (i2caddr, datax, 1) ;
	}
	if (status >= 0)
	{
		*val = datax [0] ;
	}

	if (status >= 0)
	{
		return (0) ;
	}
	else
	{
		return (status) ;
	}
	
} 

/* -------------------------------------------------------------------------------------------------- */
/* write an 8 bit value into a 16 bit register                                                        */
/*   addr: the i2c bus address to access                                                              */
/*    reg: the i2c register to write to                                                               */
/*    val: the value to write into the register                                                       */
/* return: error code                                                                                 */
/* -------------------------------------------------------------------------------------------------- */

int i2c_write_reg16 (uint8_t i2caddr, uint16_t reg, uint8_t val) 
{
	uint8_t		datax [3] ;									
	int			status ;

///	printf ("i2c16w: %02X %04X %02X\r\n",i2caddr,reg,val) ; ///////////////	    

	datax [0] = (reg >> 8) & 0xff ;
	datax [1] = (reg >> 0) & 0xff ;
	datax [2] = val ;

	status = i2c_write_bounded (i2caddr, datax, 3) ;							// release bus at end

///	printf ("i2cw16 exit\r\n") ; /////////	

	if (status >= 0)
	{
		return (0) ;
	}
	else
	{
		return (status) ;
	}
    
}

/* -------------------------------------------------------------------------------------------------- */
/* read an i2c 8 bit value from an 8 bit register address                                             */
/*   addr: the i2c bus address to access                                                              */
/*    reg: the 8 bit address of the i2c register to read                                              */
/*   *val: the return value for the register we have read                                             */
/* return: error code                                                                                 */
/* -------------------------------------------------------------------------------------------------- */

int i2c_read_reg8 (uint8_t i2caddr, uint8_t reg, uint8_t *val) 
{
    int 		status ;
	uint8_t		datax [3] ;									

	datax [0] = reg ;

	status = i2c_write_bounded (i2caddr, datax, 1) ;						// release bus at end
	if (status >= 0)
	{
		status = i2c_read_bounded (i2caddr, datax, 1) ;
	}
	if (status >= 0)
	{
		*val = datax [0] ;
	}

	if (status >= 0)
	{
		return (0) ;
	}
	else
	{
		return (status) ;
	}
	
}

/* -------------------------------------------------------------------------------------------------- */
/* -------------------------------------------------------------------------------------------------- */
/* write an 8 bit value to an i2c 8 bit register address in the nim                                   */
/*   addr: the i2c bus address to access                                                              */
/*    reg: the i2c register to write to                                                               */
/*    val: the value to write into the register                                                       */
/* return: error code                                                                                 */
/* -------------------------------------------------------------------------------------------------- */

int i2c_write_reg8 (uint8_t i2caddr, uint8_t reg, uint8_t val) 
{
	char		buff  [8] ;
	uint8_t		datax [3] ;									
	int			status ;

	datax [0] = reg ;
	datax [1] = val ;

	status = i2c_write_bounded (i2caddr, datax, 2) ;							// release bus at end

	if (status >= 0)
	{
		return (0) ;
	}
	else
	{
		return (status) ;
	}

///	printf ("i2c: %02X %02X %02X\r\n",addr,reg,val) ; ///////////////	    

}


// send I2C address/write + function address + 2 bytes to a PIC

int i2c_write_pic16 (uint8_t i2caddr, uint8_t val1, uint8_t val2, uint8_t val3) 
{
	uint8_t		err = 0 ;
	
	err |= nim_set_stv0910_repeaters (false) ;
	err |= i2c_write_reg16 (i2caddr, ((((uint16_t)val1) << 8)) | val2, val3) ;

	return (err) ;
}


// send I2C address/write + function address + 1 byte to a PIC

int i2c_write_pic8 (uint8_t i2caddr, uint8_t val1, uint8_t val2) 
{
	int		err = 0 ;
	
	err |= nim_set_stv0910_repeaters (false) ;
	err |= i2c_write_reg8 (i2caddr, val1, val2) ;

	return (err) ;
}


// send I2C address/read and function address, then read 1 byte from a PIC

int i2c_read_pic8 (uint8_t i2caddr, uint8_t functionaddr, uint8_t* buff) 
{
	uint8_t		err = 0 ;
	uint8_t		val ;

	err |= nim_set_stv0910_repeaters (false) ;
	err |= i2c_read_reg8 (i2caddr, functionaddr, &val) ;
	*buff = val ;

	return (err) ;
}

