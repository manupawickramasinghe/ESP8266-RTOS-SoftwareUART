/* Host-side stand-in for FreeRTOS.h as configured by ESP8266_RTOS_SDK
 * (100 Hz tick, 32-bit TickType_t); behaviour lives in sim.c. */
#pragma once

#include <stdint.h>

typedef uint32_t TickType_t;
#define configTICK_RATE_HZ 100
#define portTICK_PERIOD_MS ((TickType_t)1000 / configTICK_RATE_HZ)
#define portMAX_DELAY ((TickType_t)0xffffffffUL)
#define pdMS_TO_TICKS(ms) ((TickType_t)(((TickType_t)(ms) * (TickType_t)configTICK_RATE_HZ) / (TickType_t)1000))

void sim_enter_critical(void);
void sim_exit_critical(void);

#define portENTER_CRITICAL() sim_enter_critical()
#define portEXIT_CRITICAL() sim_exit_critical()
