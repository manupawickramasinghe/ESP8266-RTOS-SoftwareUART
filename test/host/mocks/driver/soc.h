/* Host-side stand-in for ESP8266_RTOS_SDK's driver/soc.h (see sim.c). */
#pragma once

#include <stdint.h>

/* CPU cycle counter (CCOUNT special register), wraps at 2^32 cycles. */
uint32_t soc_get_ccount(void);
