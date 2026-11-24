/*
 * Host-side stand-in for ESP8266_RTOS_SDK's esp8266/gpio_struct.h.
 *
 * GPIO expands to a call into the simulator, which refreshes the input
 * register for the current virtual time and applies the previous access's
 * writes (out_w1ts/out_w1tc/int_type) with that access's timestamp.
 */
#pragma once

#include <stdint.h>

typedef struct {
    uint32_t out;
    uint32_t out_w1ts;
    uint32_t out_w1tc;
    uint32_t enable;
    uint32_t enable_w1ts;
    uint32_t enable_w1tc;
    uint32_t in;
    uint32_t status;
    uint32_t status_w1ts;
    uint32_t status_w1tc;
    union {
        struct {
            uint32_t source : 1;
            uint32_t reserved1 : 1;
            uint32_t driver : 1;
            uint32_t reserved2 : 4;
            uint32_t int_type : 3;
            uint32_t wakeup_enable : 1;
            uint32_t reserved3 : 21;
        };
        uint32_t val;
    } pin[16];
    uint32_t sigma_delta;
    uint32_t rtc_calib_sync;
    uint32_t rtc_calib_value;
} gpio_struct_t;

volatile gpio_struct_t *sim_gpio_regs(void);
#define GPIO (*sim_gpio_regs())
