/* ioLibrary's sendto(), renamed so it does not clash with the C library's */
#include <stdint.h>
#define sendto wiz_sendto
int32_t wiz_sendto (uint8_t sn, uint8_t *buf, uint16_t len, uint8_t *addr, uint16_t port);
