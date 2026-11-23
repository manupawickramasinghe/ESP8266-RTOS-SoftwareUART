/*
 * Deterministic ESP8266 simulator for host-side softuart tests.
 *
 * Time is virtual and kept in nanoseconds. Every SDK call made by softuart.c
 * costs a little virtual time, so its busy-wait loops terminate and bit edges
 * land where they would on hardware, minus real interrupt jitter.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"

#define SIM_MAX_EDGES 8192
#define SIM_MAX_FRAMES 1024

typedef struct {
    uint64_t t_ns;
    int level;
} sim_edge_t;

/* Reset all simulator state. The microsecond timer starts at start_us. */
void sim_reset(uint32_t start_us);

uint64_t sim_now_ns(void);
void sim_advance_ns(uint64_t ns);
/* Advance virtual time to an absolute instant (no-op if already past it). */
void sim_advance_to_ns(uint64_t t_ns);

/* --- output side ------------------------------------------------------- */
/* Level transitions written by gpio_set_level() on a pin, oldest first. */
size_t sim_edges(int pin, const sim_edge_t **edges);
int sim_output_level(int pin);
void sim_clear_edges(int pin);

/* --- input side -------------------------------------------------------- */
/* Level of an input pin while no frame is being driven onto it. */
void sim_set_idle_level(int pin, int level);
/* Queue a UART frame (start + 8 data bits LSB first + stop) on an input pin. */
void sim_queue_frame(int pin, uint64_t start_ns, uint8_t byte, uint32_t baud, bool invert);
/*
 * Deliver every queued frame on the pin: for each start-bit edge, advance to
 * edge + latency_ns and run the registered GPIO ISR if the pin's interrupt
 * type matches that edge. Returns how many times the ISR ran.
 */
int sim_deliver_frames(int pin, uint64_t latency_ns);

/* --- interrupt controller --------------------------------------------- */
bool sim_isr_service_installed(void);
bool sim_isr_handler_registered(int pin);
gpio_int_type_t sim_intr_type(int pin);
gpio_mode_t sim_pin_mode(int pin);
