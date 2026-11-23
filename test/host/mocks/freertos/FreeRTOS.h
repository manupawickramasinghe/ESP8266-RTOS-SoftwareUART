/* Host-side stand-in for the FreeRTOS critical-section macros (see sim.c). */
#pragma once

void sim_enter_critical(void);
void sim_exit_critical(void);

#define portENTER_CRITICAL() sim_enter_critical()
#define portEXIT_CRITICAL() sim_exit_critical()
