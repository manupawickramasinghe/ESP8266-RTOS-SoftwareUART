/* Host-side stand-in for FreeRTOS task.h (see sim.c). */
#pragma once

#include "freertos/FreeRTOS.h"

TickType_t xTaskGetTickCount(void);
/* Advances virtual time by whole ticks and runs the simulator's delay hook. */
void vTaskDelay(TickType_t ticks);
