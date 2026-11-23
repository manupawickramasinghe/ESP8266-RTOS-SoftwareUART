/*
 * Softuart
 *
 * Copyright (C) 2020 Ivan Belokobylskiy <belokobylskij@gmail.com>
 * Copyright (C) 2017 Ruslan V. Uss <unclerus@gmail.com>
 * Copyright (C) 2016 Bernhard Guillon <Bernhard.Guillon@web.de>
 *
 * The code is slightly modified to use ESP8266_RTOS SDK
 * from Esspressif. The original code [2] is based on Softuart
 * from here [1] and reworked to fit into esp-open-rtos.
 *
 * it fits my needs to read the GY-GPS6MV2 module with 9600 8n1
 *
 * Original Copyright:
 * Copyright (c) 2015 plieningerweb
 *
 * MIT Licensed as described in the file LICENSE
 *
 * 1 https://github.com/plieningerweb/esp8266-software-uart
 * 2 https://github.com/SuperHouse/esp-open-rtos/tree/master/extras/softuart
 */

#include "softuart.h"
#include <stdint.h>
#include "driver/gpio.h"
#include "driver/soc.h"
#include "esp8266/gpio_struct.h"
#include "esp_attr.h"
#include "esp_clk.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include "rom/ets_sys.h"

// Errors are logged from task context only, never from the interrupt handler
static const char *TAG = "softuart";

_Static_assert(SOFTUART_MAX_RX_BUFF >= 2 && SOFTUART_MAX_RX_BUFF <= 256
               && (SOFTUART_MAX_RX_BUFF & (SOFTUART_MAX_RX_BUFF - 1)) == 0,
               "SOFTUART_MAX_RX_BUFF must be a power of two between 2 and 256");

// Idle bit times added after the stop bit of each transmitted byte
#ifndef CONFIG_SOFTUART_TX_EXTRA_IDLE_BITS
#define CONFIG_SOFTUART_TX_EXTRA_IDLE_BITS 0
#endif

// Highest supported baud rate (a bit is then 80 cycles at 80 MHz)
#define SOFTUART_MAX_BAUDRATE 1000000

typedef struct
{
    char receive_buffer[SOFTUART_MAX_RX_BUFF];
    uint8_t receive_buffer_tail;
    uint8_t receive_buffer_head;
    uint8_t buffer_overflow;
} softuart_buffer_t;

typedef struct
{
    uint32_t rx_pin, tx_pin;
    uint32_t baudrate;
    volatile softuart_buffer_t buffer;
    uint32_t rx_mask;    // 1 << rx_pin, for reading GPIO.in
    uint32_t tx_mask;    // 1 << tx_pin, or 0 for GPIO16 (RTC GPIO)
    uint32_t bit_cycles; // whole CPU cycles per bit: cpu_hz / baudrate
    uint32_t bit_frac;   // cpu_hz % baudrate, spread over the frame
    bool invert;
} softuart_t;

static softuart_t uarts[SOFTUART_MAX_UARTS] = { { 0 } };

// Number of open ports
static uint8_t open_uarts;
// True if softuart installed the GPIO ISR service (and so may uninstall it)
static bool isr_service_owned;

// Bit boundaries in CPU cycles since the start of a frame. The fractional
// part of the bit period is carried over (Bresenham), so a boundary is never
// more than one cycle away from its exact position, at any baud rate.
typedef struct
{
    uint32_t cycles;
    uint32_t err;
} bit_clock_t;

static inline void IRAM_ATTR bit_clock_advance(const softuart_t *uart, bit_clock_t *clk)
{
    clk->cycles += uart->bit_cycles;
    clk->err += uart->bit_frac;
    if (clk->err >= uart->baudrate)
    {
        clk->err -= uart->baudrate;
        clk->cycles++;
    }
}

// Busy-wait until `cycles` CPU cycles have passed since `start`. The unsigned
// subtraction stays correct when the cycle counter wraps around (every 26.8 s
// at 160 MHz).
static inline void IRAM_ATTR wait_cycles(uint32_t start, uint32_t cycles)
{
    while ((uint32_t)(soc_get_ccount() - start) < cycles)
    {
    }
}

// The interrupt handler and the transmit loop access the GPIO registers
// directly instead of calling the (flash-resident) GPIO driver, so a flash
// cache miss cannot delay a sample or a bit edge.
static inline void IRAM_ATTR rx_arm(const softuart_t *uart, bool enable)
{
    GPIO.pin[uart->rx_pin].int_type = !enable ? GPIO_INTR_DISABLE
                                    : uart->invert ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE;
}

static inline bool IRAM_ATTR rx_level(const softuart_t *uart)
{
    return (GPIO.in & uart->rx_mask) != 0;
}

static inline void IRAM_ATTR tx_write(const softuart_t *uart, bool level)
{
    if (!uart->tx_mask)
        gpio_set_level(uart->tx_pin, level); // GPIO16 is not in the GPIO block
    else if (level)
        GPIO.out_w1ts = uart->tx_mask;
    else
        GPIO.out_w1tc = uart->tx_mask;
}

// GPIO interrupt handler, called on the leading edge of a start bit
static void IRAM_ATTR handle_rx(void *arg)
{
    // Handler entry stands in for the time of the start-bit edge
    const uint32_t start = soc_get_ccount();
    softuart_t *uart = (softuart_t *)arg;
    if (!uart->baudrate) return;

    // Disable interrupt
    rx_arm(uart, false);

    // Sample each data bit in its middle: 1.5, 2.5, ... 8.5 bit periods
    // after the start edge
    bit_clock_t clk = { uart->bit_cycles / 2, 0 };
    uint8_t d = 0;

    for (uint8_t i = 0; i < 8; i++)
    {
        bit_clock_advance(uart, &clk);
        wait_cycles(start, clk.cycles);
        // Shift d to the right
        d >>= 1;

        // Read bit
        if (rx_level(uart) ^ uart->invert)
        {
            // If high, set msb of 8bit to 1
            d |= 0x80;
        }
    }

    // Store byte in buffer
    // If buffer full, set the overflow flag and return
    uint8_t next = (uint8_t)((uart->buffer.receive_buffer_tail + 1) % SOFTUART_MAX_RX_BUFF);
    if (next != uart->buffer.receive_buffer_head)
    {
        // save new data in buffer: tail points to where byte goes
        uart->buffer.receive_buffer[uart->buffer.receive_buffer_tail] = d; // save new byte
        uart->buffer.receive_buffer_tail = next;
    }
    else
    {
        uart->buffer.buffer_overflow = 1;
    }

    // Wait for the middle of the stop bit, then re-arm for the next start bit
    bit_clock_advance(uart, &clk);
    wait_cycles(start, clk.cycles);
    rx_arm(uart, true);
}

// Transmit one 8N1 frame. Runs from IRAM so that a flash cache miss cannot
// stretch a bit.
static void IRAM_ATTR send_frame(const softuart_t *uart, uint8_t data)
{
    // Bit 0 is the start bit (0), bits 1-8 the data LSB first, bit 9 the stop bit (1)
    const uint32_t frame = ((uint32_t)data << 1) | (1u << 9);
    bit_clock_t clk = { 0, 0 };

#ifdef CONFIG_SOFTUART_TX_CRITICAL_SECTION
    portENTER_CRITICAL();
#endif
    const uint32_t start = soc_get_ccount();
    for (uint8_t i = 0; i < 10; i++)
    {
        wait_cycles(start, clk.cycles);
        tx_write(uart, ((frame >> i) & 1) ^ uart->invert);
        bit_clock_advance(uart, &clk);
    }
#ifdef CONFIG_SOFTUART_TX_CRITICAL_SECTION
    // An interrupt during the stop bit only lengthens it, which is harmless
    portEXIT_CRITICAL();
#endif

    // Hold the stop bit, plus any configured idle time, before the next frame
    for (int i = 0; i < CONFIG_SOFTUART_TX_EXTRA_IDLE_BITS; i++)
        bit_clock_advance(uart, &clk);
    wait_cycles(start, clk.cycles);
}

static bool check_uart_no(uint8_t uart_no)
{
    if (uart_no >= SOFTUART_MAX_UARTS)
    {
        ESP_LOGE(TAG, "Invalid uart number %u, must be below %d", uart_no, SOFTUART_MAX_UARTS);
        return false;
    }

    return true;
}

static bool check_uart_enabled(uint8_t uart_no)
{
    if (!uarts[uart_no].baudrate)
    {
        ESP_LOGD(TAG, "Uart %u is not open", uart_no);
        return false;
    }

    return true;
}

// Uninstall the GPIO ISR service once no port needs it, but only if softuart
// installed it: the application may use it for its own GPIO interrupts.
static void release_isr_service(void)
{
    if (open_uarts == 0 && isr_service_owned)
    {
        gpio_uninstall_isr_service();
        isr_service_owned = false;
    }
}

///////////////////////////////////////////////////////////////////////////////
/// Public
///////////////////////////////////////////////////////////////////////////////

bool softuart_open(uint8_t uart_no, uint32_t baudrate, uint32_t rx_pin, uint32_t tx_pin, bool invert)
{
    // do some checks
    if (!check_uart_no(uart_no)) return false;
    if (baudrate == 0 || baudrate > SOFTUART_MAX_BAUDRATE)
    {
        ESP_LOGE(TAG, "Invalid baudrate %u", (unsigned)baudrate);
        return false;
    }
    // GPIO16 is an RTC GPIO without interrupt support, so it cannot receive
    if (!GPIO_IS_VALID_GPIO(rx_pin) || RTC_GPIO_IS_VALID_GPIO(rx_pin))
    {
        ESP_LOGE(TAG, "Invalid RX pin %u, must be GPIO0-15", (unsigned)rx_pin);
        return false;
    }
    if (!GPIO_IS_VALID_GPIO(tx_pin))
    {
        ESP_LOGE(TAG, "Invalid TX pin %u, must be GPIO0-16", (unsigned)tx_pin);
        return false;
    }
    if (rx_pin == tx_pin)
    {
        ESP_LOGE(TAG, "RX and TX must use different pins");
        return false;
    }
    for (uint8_t i = 0; i < SOFTUART_MAX_UARTS; i++)
        if (uarts[i].baudrate && i != uart_no
            && (uarts[i].rx_pin == rx_pin || uarts[i].tx_pin == tx_pin || uarts[i].rx_pin == tx_pin || uarts[i].tx_pin == rx_pin))
        {
            ESP_LOGE(TAG, "Cannot share pins between uarts");
            return false;
        }

    softuart_close(uart_no);

    softuart_t *uart = uarts + uart_no;

    uart->rx_pin = rx_pin;
    uart->tx_pin = tx_pin;
    uart->invert = invert;

    // Discard anything left over from a previous session
    uart->buffer.receive_buffer_head = 0;
    uart->buffer.receive_buffer_tail = 0;
    uart->buffer.buffer_overflow = 0;

    // Bit period in CPU cycles, with the remainder kept for exact timing
    const uint32_t cpu_hz = (uint32_t)esp_clk_cpu_freq();
    uart->bit_cycles = cpu_hz / baudrate;
    uart->bit_frac = cpu_hz % baudrate;
    uart->rx_mask = 1UL << rx_pin;
    uart->tx_mask = tx_pin < 16 ? 1UL << tx_pin : 0;

    // Setup Rx
    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = 1UL << rx_pin;
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 1;
    gpio_config(&io_conf);

    // Setup Tx
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = 1UL << tx_pin;
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 1;
    gpio_config(&io_conf);
    gpio_set_level(tx_pin, !uart->invert);

    // Install the GPIO ISR service unless it is already installed, either by
    // an open port or by the application (then the SDK returns ESP_FAIL).
    if (open_uarts == 0 && !isr_service_owned)
        isr_service_owned = gpio_install_isr_service(0) == ESP_OK;

    uart->baudrate = baudrate;
    if (gpio_isr_handler_add(rx_pin, handle_rx, uart) != ESP_OK)
    {
        ESP_LOGE(TAG, "Cannot attach the RX interrupt handler");
        uart->baudrate = 0;
        release_isr_service();
        return false;
    }
    open_uarts++;

    // Setup the interrupt handler to get the start bit
    gpio_set_intr_type(rx_pin, uart->invert ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE);

    ets_delay_us(1000); // TODO: not sure if it really needed

    return true;
}

bool softuart_close(uint8_t uart_no)
{
    if (!check_uart_no(uart_no)) return false;
    softuart_t *uart = uarts + uart_no;

    if (!uart->baudrate) return true;

    // Remove interrupt
    gpio_set_intr_type(uart->rx_pin, GPIO_INTR_DISABLE);
    gpio_isr_handler_remove(uart->rx_pin);
    // Mark as unused
    uart->baudrate = 0;
    open_uarts--;
    release_isr_service();

    return true;
}

bool softuart_put(uint8_t uart_no, char c)
{
    if (!check_uart_no(uart_no)) return false;
    if (!check_uart_enabled(uart_no)) return false;

    send_frame(uarts + uart_no, (uint8_t)c);

    return true;
}

bool softuart_puts(uint8_t uart_no, const char *s)
{
    if (s == NULL) return false;
    while (*s)
    {
        if (!softuart_put(uart_no, *s++))
            return false;
    }

    return true;
}
bool softuart_putn(uint8_t uart_no, const uint8_t* s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (!softuart_put(uart_no, s[i])) {
            return false;
        }
    }

    return true;
}

bool softuart_available(uint8_t uart_no)
{
    if (!check_uart_no(uart_no)) return false;
    if (!check_uart_enabled(uart_no)) return false;
    softuart_t *uart = uarts + uart_no;

    return (uart->buffer.receive_buffer_tail + SOFTUART_MAX_RX_BUFF - uart->buffer.receive_buffer_head) % SOFTUART_MAX_RX_BUFF;
}

uint8_t softuart_read(uint8_t uart_no)
{
    if (!check_uart_no(uart_no)) return 0;
    if (!check_uart_enabled(uart_no)) return 0;
    softuart_t *uart = uarts + uart_no;

    // Empty buffer?
    if (uart->buffer.receive_buffer_head == uart->buffer.receive_buffer_tail) return 0;

    // Read from "head"
    uint8_t d = uart->buffer.receive_buffer[uart->buffer.receive_buffer_head]; // grab next byte
    uart->buffer.receive_buffer_head = (uint8_t)((uart->buffer.receive_buffer_head + 1) % SOFTUART_MAX_RX_BUFF);
    return d;
}
