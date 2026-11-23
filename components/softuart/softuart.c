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
#include "esp_log.h"

#include "rom/ets_sys.h"

// Errors are logged from task context only, never from the interrupt handler
static const char *TAG = "softuart";

_Static_assert(SOFTUART_MAX_RX_BUFF >= 2 && SOFTUART_MAX_RX_BUFF <= 256
               && (SOFTUART_MAX_RX_BUFF & (SOFTUART_MAX_RX_BUFF - 1)) == 0,
               "SOFTUART_MAX_RX_BUFF must be a power of two between 2 and 256");

// Highest baud rate whose bit time is still at least 1 us
#define SOFTUART_MAX_BAUDRATE 1000000

// Free-running 32-bit microsecond timer (wraps every ~71.6 minutes)
extern uint32_t esp_get_time(void);

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
    uint32_t bit_time;
    bool invert;
} softuart_t;

static softuart_t uarts[SOFTUART_MAX_UARTS] = { { 0 } };

// Number of open ports
static uint8_t open_uarts;
// True if softuart installed the GPIO ISR service (and so may uninstall it)
static bool isr_service_owned;

// Busy-wait until `us` microseconds have passed since `start`. The unsigned
// subtraction stays correct when the timer wraps around.
static inline void wait_until(uint32_t start, uint32_t us)
{
    while ((uint32_t)(esp_get_time() - start) < us)
    {
    }
}

// Busy-wait for `us` microseconds. Unlike ets_delay_us(), which is the ROM's
// os_delay_us(uint16_t), this does not truncate delays above 65535 us.
static inline void delay_us(uint32_t us)
{
    wait_until(esp_get_time(), us);
}

// GPIO interrupt handler, called on the leading edge of a start bit
static void handle_rx(void *arg)
{
    softuart_t *uart = (softuart_t *)arg;
    if (!uart->baudrate) return;

    // Disable interrupt
    gpio_set_intr_type(uart->rx_pin, GPIO_INTR_DISABLE);

    // Wait till start bit is half over so we can sample the next one in the center
    delay_us(uart->bit_time / 2);

    // Now sample bits
    uint8_t d = 0;
    uint32_t start_time = esp_get_time();

    for (uint8_t i = 0; i < 8; i++)
    {
        wait_until(start_time, uart->bit_time * (i + 1));
        // Shift d to the right
        d >>= 1;

        // Read bit
        if (gpio_get_level(uart->rx_pin) ^ uart->invert)
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

    // Wait for stop bit
    delay_us(uart->bit_time);

    // Done, reenable interrupt
    gpio_set_intr_type(uart->rx_pin, uart->invert ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE);
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

    // Calculate bit_time
    uart->bit_time = (1000000 / baudrate);
    if (((100000000 / baudrate) - (100 * uart->bit_time)) > 50) uart->bit_time++;

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
    softuart_t *uart = uarts + uart_no;
    const uint8_t data = (uint8_t)c;

    // Start bit
    uint32_t start_time = esp_get_time();
    gpio_set_level(uart->tx_pin, uart->invert);

    // Data bits, LSB first
    for (uint8_t i = 0; i < 8; i++)
    {
        wait_until(start_time, uart->bit_time * (i + 1));
        gpio_set_level(uart->tx_pin, ((data >> i) & 1) ^ uart->invert);
    }

    // Stop bit, followed by idle time before the next byte
    wait_until(start_time, uart->bit_time * 9);
    gpio_set_level(uart->tx_pin, !uart->invert);
    delay_us(uart->bit_time * 6);

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
