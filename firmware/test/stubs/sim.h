/* host test stubs for the Pico SDK: flash simulated at the real XIP address */
#ifndef SIM_H
#define SIM_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#define XIP_BASE 0x10000000u
#define PPB_BASE 0xe0000000u
#define M0PLUS_NVIC_ICPR_OFFSET 0xe280u
typedef uint64_t absolute_time_t;
void flash_range_erase (uint32_t offset, size_t count);
void flash_range_program (uint32_t offset, const uint8_t *data, size_t count);
uint32_t save_and_disable_interrupts (void);
void restore_interrupts (uint32_t status);
void multicore_lockout_victim_init (void);
void multicore_lockout_start_blocking (void);
void multicore_lockout_end_blocking (void);
absolute_time_t get_absolute_time (void);
uint32_t to_ms_since_boot (absolute_time_t t);
void reset_usb_boot (uint32_t a, uint32_t b);
void irq_set_mask_enabled (uint32_t mask, bool enabled);
#endif
