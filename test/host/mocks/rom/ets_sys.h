/* Host-side stand-in for ESP8266_RTOS_SDK's rom/ets_sys.h (see sim.c). */
#pragma once

#include <stdint.h>

/* As in the SDK: ets_delay_us() is the ROM's os_delay_us(), which takes a
 * 16-bit argument, so delays above 65535 us are truncated. */
void os_delay_us(uint16_t us);
#define ets_delay_us os_delay_us
