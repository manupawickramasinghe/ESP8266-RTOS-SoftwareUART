/*
 * Deterministic ESP8266 simulator for host-side softuart tests (see sim.h).
 */
#include "sim.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/soc.h"
#include "esp8266/gpio_struct.h"
#include "esp_clk.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rom/ets_sys.h"

/* Virtual cost of SDK calls and register accesses, roughly at 160 MHz. */
#define COST_GET_TIME_NS 250
#define COST_GPIO_NS 100
#define COST_CCOUNT_NS 25
#define COST_REG_NS 25

typedef struct {
    uint64_t start_ns;
    double bit_ns;
    uint8_t byte;
    bool invert;
} sim_frame_t;

typedef struct {
    sim_edge_t edges[SIM_MAX_EDGES];
    size_t n_edges;
    int out_level;
    int idle_level;
    gpio_mode_t mode;
    gpio_int_type_t intr_type;
    uint64_t armed_ns;
    gpio_isr_t handler;
    void *handler_arg;
    sim_frame_t frames[SIM_MAX_FRAMES];
    size_t n_frames;
} sim_pin_t;

static uint64_t now_ns;
static uint32_t base_us;
static uint32_t cpu_hz = 160000000;
static uint32_t ccount_base;
static int log_counts[ESP_LOG_VERBOSE + 1];
static bool isr_service;
static int critical_depth;
static sim_delay_hook_t delay_hook;
static int delay_calls;
static int critical_entries;
static sim_pin_t pins[GPIO_PIN_COUNT];

/* Register-level view of GPIO0-15 (see mocks/esp8266/gpio_struct.h). */
static gpio_struct_t regs;
static uint64_t last_reg_access_ns;

void sim_reset(uint32_t start_us)
{
    now_ns = 0;
    base_us = start_us;
    cpu_hz = 160000000;
    /* the cycle counter wraps at the same instant as start_us * MHz would */
    ccount_base = start_us * (cpu_hz / 1000000);
    memset(log_counts, 0, sizeof(log_counts));
    isr_service = false;
    critical_depth = 0;
    delay_hook = NULL;
    delay_calls = 0;
    critical_entries = 0;
    memset(&regs, 0, sizeof(regs));
    last_reg_access_ns = 0;
    memset(pins, 0, sizeof(pins));
    for (int i = 0; i < GPIO_PIN_COUNT; i++) {
        pins[i].out_level = -1;
        pins[i].idle_level = 1;
    }
}

void sim_set_cpu_hz(uint32_t hz)
{
    /* keep the counter continuous across the change */
    uint32_t now = soc_get_ccount();
    cpu_hz = hz;
    ccount_base = now - (uint32_t)(now_ns * (cpu_hz / 1000000) / 1000);
}

uint64_t sim_now_ns(void) { return now_ns; }
void sim_advance_ns(uint64_t ns) { now_ns += ns; }

void sim_advance_to_ns(uint64_t t_ns)
{
    if (t_ns > now_ns) now_ns = t_ns;
}

static sim_pin_t *pin_or_die(int pin)
{
    if (pin < 0 || pin >= GPIO_PIN_COUNT) {
        fprintf(stderr, "sim: invalid pin %d\n", pin);
        abort();
    }
    return &pins[pin];
}

/* ------------------------------------------------------------------ SDK */

void sim_log(esp_log_level_t level, const char *tag, const char *fmt, ...)
{
    (void)tag;
    log_counts[level]++;
    if (getenv("SIM_VERBOSE")) {
        va_list ap;
        va_start(ap, fmt);
        fprintf(stderr, "[%s] ", tag);
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
        va_end(ap);
    }
}

int sim_log_count(int level) { return log_counts[level]; }

uint32_t soc_get_ccount(void)
{
    now_ns += COST_CCOUNT_NS;
    return ccount_base + (uint32_t)(now_ns * (cpu_hz / 1000000) / 1000);
}

int esp_clk_cpu_freq(void) { return (int)cpu_hz; }

void sim_enter_critical(void)
{
    critical_depth++;
    critical_entries++;
}

void sim_exit_critical(void)
{
    if (critical_depth <= 0) {
        fprintf(stderr, "sim: portEXIT_CRITICAL without portENTER_CRITICAL\n");
        abort();
    }
    critical_depth--;
}

int sim_critical_depth(void) { return critical_depth; }

TickType_t xTaskGetTickCount(void) { return (TickType_t)(now_ns / (portTICK_PERIOD_MS * 1000000ull)); }

void vTaskDelay(TickType_t ticks)
{
    if (critical_depth) {
        fprintf(stderr, "sim: vTaskDelay inside a critical section\n");
        abort();
    }
    delay_calls++;
    uint64_t tick_ns = portTICK_PERIOD_MS * 1000000ull;
    /* wake at the next tick boundary(s), like the scheduler */
    uint64_t wake = (now_ns / tick_ns + ticks) * tick_ns;
    if (delay_hook) delay_hook(wake);
    sim_advance_to_ns(wake);
}

void sim_set_delay_hook(sim_delay_hook_t hook) { delay_hook = hook; }
int sim_delay_calls(void) { return delay_calls; }
int sim_critical_entries(void) { return critical_entries; }

static void record_level(sim_pin_t *p, int pin, int level, uint64_t t)
{
    if (level == p->out_level) return;
    if (p->n_edges >= SIM_MAX_EDGES) {
        fprintf(stderr, "sim: edge buffer full on pin %d\n", pin);
        abort();
    }
    p->edges[p->n_edges++] = (sim_edge_t){ t, level };
    p->out_level = level;
}

static int input_level_at(const sim_pin_t *p, uint64_t t);

/*
 * Apply register writes made since the previous GPIO access. A write
 * happens right after the access that produced the pointer, so it takes
 * that access's timestamp.
 */
static void sync_regs(void)
{
    for (int i = 0; i < 16; i++) {
        if (regs.out_w1ts & (1u << i)) record_level(&pins[i], i, 1, last_reg_access_ns);
        if (regs.out_w1tc & (1u << i)) record_level(&pins[i], i, 0, last_reg_access_ns);
        if ((gpio_int_type_t)regs.pin[i].int_type != pins[i].intr_type) {
            pins[i].intr_type = (gpio_int_type_t)regs.pin[i].int_type;
            pins[i].armed_ns = last_reg_access_ns;
        }
    }
    regs.out_w1ts = 0;
    regs.out_w1tc = 0;
}

volatile gpio_struct_t *sim_gpio_regs(void)
{
    sync_regs();
    now_ns += COST_REG_NS;
    last_reg_access_ns = now_ns;
    uint32_t in = 0;
    for (int i = 0; i < 16; i++)
        if (input_level_at(&pins[i], now_ns)) in |= 1u << i;
    regs.in = in;
    return &regs;
}

uint32_t esp_get_time(void)
{
    now_ns += COST_GET_TIME_NS;
    /* 32-bit microsecond counter that wraps, like the WDEV timer. */
    return base_us + (uint32_t)(now_ns / 1000);
}

void os_delay_us(uint16_t us) { now_ns += (uint64_t)us * 1000; }

esp_err_t gpio_config(const gpio_config_t *cfg)
{
    sync_regs();
    for (int i = 0; i < GPIO_PIN_COUNT; i++) {
        if (!(cfg->pin_bit_mask & (1u << i))) continue;
        pins[i].mode = cfg->mode;
        if (i != 16) {
            pins[i].intr_type = cfg->intr_type;
            regs.pin[i].int_type = cfg->intr_type;
        }
    }
    return ESP_OK;
}

esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level)
{
    if (!GPIO_IS_VALID_GPIO(gpio_num)) return ESP_ERR_INVALID_ARG;
    sim_pin_t *p = pin_or_die(gpio_num);
    sync_regs();
    now_ns += COST_GPIO_NS;
    record_level(p, gpio_num, level ? 1 : 0, now_ns);
    return ESP_OK;
}

/* Physical level of bit k of a frame: 0 = start, 1..8 = data, 9 = stop. */
static int frame_bit_level(const sim_frame_t *f, uint64_t k)
{
    int logical;
    if (k == 0) logical = 0;
    else if (k <= 8) logical = (f->byte >> (k - 1)) & 1;
    else logical = 1;
    return f->invert ? !logical : logical;
}

static int frame_level(const sim_frame_t *f, uint64_t t, int *level)
{
    if (t < f->start_ns) return 0;
    uint64_t k = (uint64_t)((double)(t - f->start_ns) / f->bit_ns);
    if (k >= 10) return 0;
    *level = frame_bit_level(f, k);
    return 1;
}

static int input_level_at(const sim_pin_t *p, uint64_t t)
{
    int level;
    for (size_t i = 0; i < p->n_frames; i++)
        if (frame_level(&p->frames[i], t, &level)) return level;
    return p->idle_level;
}

int gpio_get_level(gpio_num_t gpio_num)
{
    if (!GPIO_IS_VALID_GPIO(gpio_num)) return ESP_ERR_INVALID_ARG;
    now_ns += COST_GPIO_NS;
    return input_level_at(pin_or_die(gpio_num), now_ns);
}

esp_err_t gpio_set_intr_type(gpio_num_t gpio_num, gpio_int_type_t intr_type)
{
    if (!GPIO_IS_VALID_GPIO(gpio_num) || RTC_GPIO_IS_VALID_GPIO(gpio_num)) return ESP_ERR_INVALID_ARG;
    if (intr_type >= GPIO_INTR_MAX) return ESP_ERR_INVALID_ARG;
    sim_pin_t *p = pin_or_die(gpio_num);
    sync_regs();
    now_ns += COST_GPIO_NS;
    p->intr_type = intr_type;
    p->armed_ns = now_ns;
    regs.pin[gpio_num].int_type = intr_type;
    return ESP_OK;
}

esp_err_t gpio_install_isr_service(int no_use)
{
    (void)no_use;
    if (isr_service) return ESP_FAIL; /* "GPIO isr service already installed" */
    isr_service = true;
    for (int i = 0; i < GPIO_PIN_COUNT; i++) {
        pins[i].handler = NULL;
        pins[i].handler_arg = NULL;
    }
    return ESP_OK;
}

void gpio_uninstall_isr_service(void)
{
    isr_service = false;
    for (int i = 0; i < GPIO_PIN_COUNT; i++) {
        pins[i].handler = NULL;
        pins[i].handler_arg = NULL;
    }
}

esp_err_t gpio_isr_handler_add(gpio_num_t gpio_num, gpio_isr_t isr_handler, void *args)
{
    if (!isr_service) return ESP_ERR_INVALID_STATE;
    if (!GPIO_IS_VALID_GPIO(gpio_num) || RTC_GPIO_IS_VALID_GPIO(gpio_num)) return ESP_ERR_INVALID_ARG;
    pins[gpio_num].handler = isr_handler;
    pins[gpio_num].handler_arg = args;
    return ESP_OK;
}

esp_err_t gpio_isr_handler_remove(gpio_num_t gpio_num)
{
    if (!isr_service) return ESP_ERR_INVALID_STATE;
    if (!GPIO_IS_VALID_GPIO(gpio_num) || RTC_GPIO_IS_VALID_GPIO(gpio_num)) return ESP_ERR_INVALID_ARG;
    pins[gpio_num].handler = NULL;
    pins[gpio_num].handler_arg = NULL;
    return ESP_OK;
}

/* ------------------------------------------------------------- harness */

size_t sim_edges(int pin, const sim_edge_t **edges)
{
    sync_regs();
    sim_pin_t *p = pin_or_die(pin);
    *edges = p->edges;
    return p->n_edges;
}

int sim_output_level(int pin)
{
    sync_regs();
    return pin_or_die(pin)->out_level;
}
void sim_clear_edges(int pin) { pin_or_die(pin)->n_edges = 0; }
void sim_set_idle_level(int pin, int level) { pin_or_die(pin)->idle_level = level ? 1 : 0; }

void sim_queue_frame(int pin, uint64_t start_ns, uint8_t byte, uint32_t baud, bool invert)
{
    sim_pin_t *p = pin_or_die(pin);
    if (p->n_frames >= SIM_MAX_FRAMES) {
        fprintf(stderr, "sim: frame buffer full on pin %d\n", pin);
        abort();
    }
    p->frames[p->n_frames++] = (sim_frame_t){ start_ns, 1e9 / baud, byte, invert };
}

static bool edge_matches(gpio_int_type_t type, int to_level)
{
    switch (type) {
    case GPIO_INTR_POSEDGE: return to_level == 1;
    case GPIO_INTR_NEGEDGE: return to_level == 0;
    case GPIO_INTR_ANYEDGE: return true;
    default: return false;
    }
}

/* Input transitions of the queued frames (frames must be queued in time order). */
static size_t build_transitions(const sim_pin_t *p, sim_edge_t *out)
{
    size_t n = 0;
    int level = p->idle_level;
    for (size_t i = 0; i < p->n_frames; i++) {
        const sim_frame_t *f = &p->frames[i];
        for (int k = 0; k <= 10; k++) {
            uint64_t t = f->start_ns + (uint64_t)(k * f->bit_ns);
            int l = k == 10 ? p->idle_level : frame_bit_level(f, (uint64_t)k);
            if (l != level) {
                out[n++] = (sim_edge_t){ t, l };
                level = l;
            }
        }
    }
    return n;
}

int sim_deliver_frames(int pin, uint64_t latency_ns)
{
    sim_pin_t *p = pin_or_die(pin);
    if (p->n_frames == 0) return 0;
    sync_regs();

    sim_edge_t *tr = calloc(p->n_frames * 11 + 1, sizeof(*tr));
    if (!tr) abort();
    size_t n_tr = build_transitions(p, tr);
    const sim_frame_t *last = &p->frames[p->n_frames - 1];
    uint64_t end = last->start_ns + (uint64_t)(11 * last->bit_ns);

    int fired = 0;
    uint64_t search_from = now_ns;
    size_t i = 0;
    for (;;) {
        /* Edges only latch while the interrupt is armed. */
        if (p->armed_ns > search_from) search_from = p->armed_ns;
        while (i < n_tr && (tr[i].t_ns < search_from || !edge_matches(p->intr_type, tr[i].level))) i++;
        if (i >= n_tr) break;
        uint64_t edge = tr[i++].t_ns;
        sim_advance_to_ns(edge + latency_ns);
        if (!isr_service || !p->handler) break;
        p->handler(p->handler_arg);
        sync_regs();
        fired++;
        search_from = now_ns;
    }
    free(tr);
    sim_advance_to_ns(end);
    p->n_frames = 0;
    return fired;
}

bool sim_isr_service_installed(void) { return isr_service; }
bool sim_isr_handler_registered(int pin) { return pin_or_die(pin)->handler != NULL; }
gpio_int_type_t sim_intr_type(int pin)
{
    sync_regs();
    return pin_or_die(pin)->intr_type;
}
gpio_mode_t sim_pin_mode(int pin) { return pin_or_die(pin)->mode; }
