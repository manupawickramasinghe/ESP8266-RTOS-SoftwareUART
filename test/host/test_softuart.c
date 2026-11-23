/*
 * Host-side tests for softuart.c, run against the simulator in sim.c.
 *
 * Transmit tests decode the waveform the library writes to the TX pin and
 * check its bit timing; receive tests drive UART frames onto the RX pin and
 * run the library's GPIO interrupt handler the way the SDK's ISR service would.
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "sim.h"
#include "softuart.h"

#define RX0 4
#define TX0 5
#define RX1 12
#define TX1 13
#define LATENCY_NS 3000 /* start-bit edge to handler entry */

static int failures;
static int checks;
static const char *current_test;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        checks++;                                                           \
        if (!(cond)) {                                                      \
            failures++;                                                     \
            printf("  FAIL %s:%d [%s] ", __FILE__, __LINE__, current_test); \
            printf(__VA_ARGS__);                                            \
            printf("\n");                                                   \
        }                                                                   \
    } while (0)

/* ------------------------------------------------------------ decoder */

typedef struct {
    uint8_t bytes[512];
    size_t n;
    int framing_errors;
    int glitches;
    double worst_edge_error_ns; /* distance of an edge from its ideal bit boundary */
} decoded_t;

static int level_at(const sim_edge_t *e, size_t n, uint64_t t, int before_first)
{
    int level = before_first;
    for (size_t i = 0; i < n && e[i].t_ns <= t; i++) level = e[i].level;
    return level;
}

/* Decode an 8N1 waveform recorded on a TX pin. */
static decoded_t decode_tx(int pin, uint32_t baud, bool invert)
{
    decoded_t d;
    memset(&d, 0, sizeof(d));
    const sim_edge_t *e;
    size_t n = sim_edges(pin, &e);
    const double bit = 1e9 / baud;
    const int idle = invert ? 0 : 1;

    for (size_t i = 1; i < n; i++) {
        double width = (double)(e[i].t_ns - e[i - 1].t_ns);
        if (width < bit / 2) d.glitches++;
    }

    size_t i = 0;
    while (i < n) {
        /* find the next start bit: transition away from idle */
        while (i < n && e[i].level == idle) i++;
        if (i >= n) break;
        uint64_t t0 = e[i].t_ns;
        uint8_t byte = 0;
        for (int b = 0; b < 8; b++) {
            int l = level_at(e, n, (uint64_t)(t0 + (b + 1.5) * bit), idle);
            if (l != idle) continue;
            byte |= (uint8_t)(1u << b);
        }
        if (level_at(e, n, (uint64_t)(t0 + 9.5 * bit), idle) != idle) d.framing_errors++;
        /* edges inside this frame must sit on bit boundaries */
        size_t j = i;
        while (j < n && e[j].t_ns < t0 + 9.75 * bit) {
            double rel = (double)(e[j].t_ns - t0) / bit;
            double err = fabs(rel - round(rel)) * bit;
            if (err > d.worst_edge_error_ns) d.worst_edge_error_ns = err;
            j++;
        }
        if (d.n < sizeof(d.bytes)) d.bytes[d.n++] = byte;
        i = j;
    }
    return d;
}

static void begin(const char *name, uint32_t start_us)
{
    current_test = name;
    sim_reset(start_us);
    for (uint8_t u = 0; u < SOFTUART_MAX_UARTS; u++) softuart_close(u);
    sim_reset(start_us);
}

static size_t drain(uint8_t uart, uint8_t *out, size_t max)
{
    size_t n = 0;
    while (softuart_available(uart) && n < max) out[n++] = softuart_read(uart);
    return n;
}

/* ------------------------------------------------------------- tests */

static void check_tx_n(const char *name, uint32_t baud, bool invert, uint32_t start_us, size_t count)
{
    begin(name, start_us);
    CHECK(softuart_open(0, baud, RX0, TX0, invert), "open failed");
    CHECK(sim_output_level(TX0) == (invert ? 0 : 1), "TX does not idle at the %s level",
          invert ? "low" : "high");
    sim_set_idle_level(RX0, invert ? 0 : 1);
    sim_clear_edges(TX0);

    uint8_t sent[256];
    for (size_t i = 0; i < count; i++) sent[i] = (uint8_t)(i * 0x35 + 0x5a);
    if (count == 256)
        for (size_t i = 0; i < count; i++) sent[i] = (uint8_t)i;
    CHECK(softuart_putn(0, sent, count), "putn failed");

    decoded_t d = decode_tx(TX0, baud, invert);
    CHECK(d.n == count, "decoded %zu frames, expected %zu", d.n, count);
    size_t mismatches = 0, first = 0;
    for (size_t i = 0; i < d.n && i < count; i++)
        if (d.bytes[i] != sent[i] && mismatches++ == 0) first = i;
    CHECK(mismatches == 0, "%zu bytes decoded wrongly (first: sent 0x%02x, got 0x%02x)",
          mismatches, sent[first], d.bytes[first]);
    CHECK(d.framing_errors == 0, "%d framing errors", d.framing_errors);
    CHECK(d.glitches == 0, "%d pulses shorter than half a bit", d.glitches);
    double bit = 1e9 / baud;
    CHECK(d.worst_edge_error_ns < 0.05 * bit, "edge off its bit boundary by %.0f ns (%.1f%% of a bit)",
          d.worst_edge_error_ns, 100 * d.worst_edge_error_ns / bit);
    CHECK(sim_output_level(TX0) == (invert ? 0 : 1), "TX not back at idle after sending");
}

static void check_tx(const char *name, uint32_t baud, bool invert, uint32_t start_us)
{
    check_tx_n(name, baud, invert, start_us, 256);
}

static void test_tx(void)
{
    check_tx("tx 9600 8N1", 9600, false, 1000);
    check_tx("tx 38400 8N1", 38400, false, 1000);
    check_tx("tx 9600 8N1 inverted", 9600, true, 1000);
    check_tx("tx 38400 8N1 inverted", 38400, true, 1000);
}

static void check_rx_n(const char *name, uint32_t baud, bool invert, uint32_t start_us, double gap_bits,
                       int count)
{
    begin(name, start_us);
    sim_set_idle_level(RX0, invert ? 0 : 1);
    CHECK(softuart_open(0, baud, RX0, TX0, invert), "open failed");
    CHECK(sim_intr_type(RX0) == (invert ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE),
          "RX interrupt armed on the wrong edge");

    const double bit = 1e9 / baud;
    uint8_t got[256];
    size_t n_got = 0;
    /* the bytes 0, 1, 2, ... in chunks that fit the ring buffer */
    for (int chunk = 0; chunk < count; chunk += 32) {
        int n = count - chunk < 32 ? count - chunk : 32;
        uint64_t t = sim_now_ns() + 50000;
        for (int i = 0; i < n; i++) {
            sim_queue_frame(RX0, t, (uint8_t)(chunk + i), baud, invert);
            t += (uint64_t)((10 + gap_bits) * bit);
        }
        int fired = sim_deliver_frames(RX0, LATENCY_NS);
        CHECK(fired == n, "ISR ran %d times for %d frames", fired, n);
        n_got += drain(0, got + n_got, sizeof(got) - n_got);
    }
    CHECK(n_got == (size_t)count, "received %zu bytes, expected %d", n_got, count);
    size_t mismatches = 0, first = 0;
    for (size_t i = 0; i < n_got; i++)
        if (got[i] != (uint8_t)i && mismatches++ == 0) first = i;
    CHECK(mismatches == 0, "%zu bytes wrong (first: expected 0x%02zx, got 0x%02x)", mismatches,
          first, got[first]);
}

static void check_rx(const char *name, uint32_t baud, bool invert, uint32_t start_us, double gap_bits)
{
    check_rx_n(name, baud, invert, start_us, gap_bits, 256);
}

static void test_rx(void)
{
    check_rx("rx 9600 8N1", 9600, false, 1000, 2);
    check_rx("rx 9600 8N1 back-to-back", 9600, false, 1000, 0);
    check_rx("rx 38400 8N1", 38400, false, 1000, 2);
    check_rx("rx 9600 8N1 inverted", 9600, true, 1000, 2);
    check_rx("rx 38400 8N1 inverted back-to-back", 38400, true, 1000, 0);
}

static void test_low_baud(void)
{
    /* Bit and idle delays longer than 65535 us must not be truncated. */
    check_tx_n("tx 50 baud", 50, false, 1000, 3);
    check_tx_n("tx 10 baud", 10, false, 1000, 3);
    check_rx_n("rx 10 baud", 10, false, 1000, 0, 3);
    check_rx_n("rx 10 baud inverted", 10, true, 1000, 0, 3);
}

static void test_timer_wrap(void)
{
    /*
     * esp_get_time() is a free-running 32-bit microsecond counter. Bytes in
     * flight when it crosses 2^31 or wraps at 2^32 must not be corrupted.
     * Several start offsets make the crossing land inside a frame.
     */
    static const uint32_t boundaries[] = { 0x80000000u, 0u };
    char name[64];
    for (size_t b = 0; b < 2; b++) {
        for (uint32_t k = 0; k < 8; k++) {
            uint32_t start = boundaries[b] - 10000 - k * 190;
            snprintf(name, sizeof(name), "tx across 2^%d us (offset %u)", b ? 32 : 31, k);
            check_tx(name, 9600, false, start);
            snprintf(name, sizeof(name), "rx across 2^%d us (offset %u)", b ? 32 : 31, k);
            check_rx(name, 9600, false, start - 40000, 2);
        }
    }
}

static void test_rx_buffer_full(void)
{
    begin("rx buffer overflow keeps the oldest bytes", 1000);
    CHECK(softuart_open(0, 9600, RX0, TX0, false), "open failed");
    const double bit = 1e9 / 9600;
    uint64_t t = sim_now_ns() + 50000;
    for (int i = 0; i < SOFTUART_MAX_RX_BUFF + 8; i++) {
        sim_queue_frame(RX0, t, (uint8_t)(i + 1), 9600, false);
        t += (uint64_t)(12 * bit);
    }
    sim_deliver_frames(RX0, LATENCY_NS);
    uint8_t got[SOFTUART_MAX_RX_BUFF + 8];
    size_t n = drain(0, got, sizeof(got));
    CHECK(n == SOFTUART_MAX_RX_BUFF - 1, "buffer held %zu bytes, expected %d", n,
          SOFTUART_MAX_RX_BUFF - 1);
    bool in_order = true;
    for (size_t i = 0; i < n; i++) in_order &= got[i] == (uint8_t)(i + 1);
    CHECK(in_order, "buffered bytes are not the oldest ones in order");
    CHECK(!softuart_available(0), "available() true on an empty buffer");
    CHECK(softuart_read(0) == 0, "read() on an empty buffer did not return 0");
}

static bool rx_works(uint8_t uart, int rx_pin, uint8_t value)
{
    sim_queue_frame(rx_pin, sim_now_ns() + 50000, value, 9600, false);
    sim_deliver_frames(rx_pin, LATENCY_NS);
    return softuart_available(uart) && softuart_read(uart) == value;
}

static void test_close_one_port_keeps_the_other(void)
{
    begin("closing port 1 keeps port 0 receiving", 1000);
    CHECK(softuart_open(0, 9600, RX0, TX0, false), "open 0 failed");
    CHECK(softuart_open(1, 9600, RX1, TX1, false), "open 1 failed");
    CHECK(rx_works(0, RX0, 0x41) && rx_works(1, RX1, 0x42), "both ports should receive");
    CHECK(softuart_close(1), "close 1 failed");
    CHECK(sim_isr_service_installed(), "GPIO ISR service was uninstalled while port 0 is open");
    CHECK(rx_works(0, RX0, 0x43), "port 0 stopped receiving after port 1 was closed");
    CHECK(!sim_isr_handler_registered(RX1), "port 1's RX handler is still registered");
}

static void app_button_isr(void *arg) { (void)arg; }

static void test_close_keeps_application_isr_service(void)
{
    begin("closing does not remove the application's GPIO ISR service", 1000);
    /* the application installed the service first, e.g. for a button on GPIO14 */
    CHECK(gpio_install_isr_service(0) == ESP_OK, "install failed");
    CHECK(gpio_isr_handler_add(14, app_button_isr, NULL) == ESP_OK, "add failed");
    CHECK(softuart_open(0, 9600, RX0, TX0, false), "open failed");
    CHECK(rx_works(0, RX0, 0x55), "port 0 should receive");
    CHECK(softuart_close(0), "close failed");
    CHECK(sim_isr_service_installed(), "softuart uninstalled a GPIO ISR service it did not install");
    CHECK(sim_isr_handler_registered(14), "application's GPIO handler was removed");
}

static void test_reopen(void)
{
    begin("ports can be closed and opened again", 1000);
    CHECK(softuart_open(0, 9600, RX0, TX0, false), "open failed");
    CHECK(softuart_close(0), "close failed");
    CHECK(!sim_isr_service_installed(), "ISR service left installed after the last port closed");
    CHECK(sim_intr_type(RX0) == GPIO_INTR_DISABLE, "RX interrupt still enabled after close");
    CHECK(softuart_open(0, 9600, RX0, TX0, false), "re-open failed");
    CHECK(rx_works(0, RX0, 0x5a), "re-opened port does not receive");
    CHECK(softuart_open(0, 38400, RX0, TX0, false), "open of an already open port failed");
    sim_queue_frame(RX0, sim_now_ns() + 50000, 0xa5, 38400, false);
    sim_deliver_frames(RX0, LATENCY_NS);
    CHECK(softuart_available(0) && softuart_read(0) == 0xa5, "reconfigured port does not receive");
}

static void test_reopen_discards_stale_bytes(void)
{
    begin("re-opening a port discards bytes from the previous session", 1000);
    CHECK(softuart_open(0, 9600, RX0, TX0, false), "open failed");
    sim_queue_frame(RX0, sim_now_ns() + 50000, 0x11, 9600, false);
    sim_deliver_frames(RX0, LATENCY_NS);
    CHECK(softuart_available(0), "byte not received");
    CHECK(softuart_close(0), "close failed");
    CHECK(softuart_open(0, 9600, RX0, TX0, false), "re-open failed");
    CHECK(!softuart_available(0), "stale byte from before the re-open is still buffered");
}

static void test_open_validation(void)
{
    begin("open rejects invalid arguments", 1000);
    CHECK(sim_log_count(1) == 0, "unexpected error log");
    CHECK(!softuart_open(SOFTUART_MAX_UARTS, 9600, RX0, TX0, false), "accepted port %d",
          SOFTUART_MAX_UARTS);
    CHECK(!softuart_open(0, 0, RX0, TX0, false), "accepted baud rate 0");
    CHECK(!softuart_open(0, 9600, 16, TX0, false), "accepted GPIO16 (no interrupt) as RX");
    CHECK(!softuart_open(0, 9600, 17, TX0, false), "accepted RX GPIO17");
    CHECK(!softuart_open(0, 9600, RX0, 17, false), "accepted TX GPIO17");
    CHECK(!softuart_open(0, 9600, RX0, RX0, false), "accepted the same pin for RX and TX");
    CHECK(sim_log_count(1) == 6, "expected one error log per rejected open, got %d", sim_log_count(1));
    CHECK(softuart_open(0, 9600, RX0, 16, false), "rejected GPIO16 as TX");
    CHECK(softuart_close(0), "close failed");

    CHECK(softuart_open(0, 9600, RX0, TX0, false), "open 0 failed");
    CHECK(!softuart_open(1, 9600, RX0, TX1, false), "port 1 accepted port 0's RX pin");
    CHECK(!softuart_open(1, 9600, RX1, TX0, false), "port 1 accepted port 0's TX pin");
    CHECK(!softuart_open(1, 9600, TX0, TX1, false), "port 1 accepted port 0's TX pin as RX");
    CHECK(softuart_open(1, 9600, RX1, TX1, false), "port 1 rejected free pins");

    CHECK(!softuart_put(SOFTUART_MAX_UARTS, 'x'), "put accepted an invalid port");
    CHECK(softuart_close(1), "close 1 failed");
    CHECK(!softuart_put(1, 'x'), "put accepted a closed port");
    CHECK(!softuart_available(1), "available() true on a closed port");
}

int main(void)
{
    test_tx();
    test_rx();
    test_low_baud();
    test_timer_wrap();
    test_rx_buffer_full();
    test_close_one_port_keeps_the_other();
    test_close_keeps_application_isr_service();
    test_reopen();
    test_reopen_discards_stale_bytes();
    test_open_validation();

    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
