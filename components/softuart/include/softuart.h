/*
 * Softuart for esp-open-rtos
 *
 * Copyright (C) 2017 Ruslan V. Uss <unclerus@gmail.com>
 * Copyright (C) 2016 Bernhard Guillon <Bernhard.Guillon@web.de>
 *
 * This code is based on Softuart from here [1] and reworked to
 * fit into esp-open-rtos. For now only the RX part is ported.
 * Also the configuration of the pin is for now hardcoded.
 *
 * it fits my needs to read the GY-GPS6MV2 module with 9600 8n1
 *
 * Original Copyright:
 * Copyright (c) 2015 plieningerweb
 *
 * MIT Licensed as described in the file LICENSE
 *
 * 1 https://github.com/plieningerweb/esp8266-software-uart
 */
#ifndef SOFTUART_H_
#define SOFTUART_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#include "sdkconfig.h"

// Defaults come from menuconfig ("Software UART (softuart)"); a compiler
// definition of the same name still overrides them.
#ifndef SOFTUART_MAX_UARTS
    #ifdef CONFIG_SOFTUART_MAX_UARTS
        #define SOFTUART_MAX_UARTS CONFIG_SOFTUART_MAX_UARTS
    #else
        #define SOFTUART_MAX_UARTS 2
    #endif
#endif

#ifndef SOFTUART_MAX_RX_BUFF
    #ifdef CONFIG_SOFTUART_RX_BUFFER_SIZE
        #define SOFTUART_MAX_RX_BUFF CONFIG_SOFTUART_RX_BUFFER_SIZE
    #else
        #define SOFTUART_MAX_RX_BUFF 64 //!< Must be a power of two from 2 to 256
    #endif
#endif

/**
 * Initialize software uart and setup interrupt handler.
 * Opening a port that is already open reconfigures it; the receive buffer
 * is emptied either way. The bit period is derived from the CPU clock at
 * this point, so open the port again after changing the CPU frequency.
 * @param uart_no Software uart index, 0..SOFTUART_MAX_UARTS-1
 * @param baudrate Baudrate, e.g. 9600, 19200, etc (1..1000000)
 * @param rx_pin GPIO pin number for RX, GPIO0-15 (GPIO16 has no interrupt)
 * @param tx_pin GPIO pin number for TX, GPIO0-16, different from rx_pin
 * @param invert true for inverted (idle-low) line logic
 * @return true if no errors occured otherwise false
 */
bool softuart_open(uint8_t uart_no, uint32_t baudrate, uint32_t rx_pin, uint32_t tx_pin, bool invert);

/**
 * Deinitialize software uart. Other open ports keep working, and a GPIO ISR
 * service installed by the application is left in place.
 * @param uart_no Software uart index, 0..SOFTUART_MAX_UARTS-1
 * @return true if no errors occured otherwise false
 */
bool softuart_close(uint8_t uart_no);

/**
 * Put char to software uart. Blocks until the stop bit (plus any idle time
 * configured with CONFIG_SOFTUART_TX_EXTRA_IDLE_BITS) has been sent.
 * @param uart_no Software uart index, 0..SOFTUART_MAX_UARTS-1
 * @param c Char
 * @return true if no errors occured otherwise false
 */
bool softuart_put(uint8_t uart_no, char c);

/**
 * Put string to software uart
 * @param uart_no Software uart index, 0..SOFTUART_MAX_UARTS-1
 * @param s Null-terminated string
 * @return true if no errors occured otherwise false
 */
bool softuart_puts(uint8_t uart_no, const char *s);

/**
 * Put string with length to software uart
 * @param uart_no Software uart index, 0..SOFTUART_MAX_UARTS-1
 * @param s Bytes to send
 * @param n Number of bytes
 * @return true if no errors occured otherwise false
 */
bool softuart_putn(uint8_t uart_no, const uint8_t* s, size_t n);

/**
 * Check if data is available
 * @param uart_no Software uart index, 0..SOFTUART_MAX_UARTS-1
 * @return true if data is available otherwise false
 */
bool softuart_available(uint8_t uart_no);

/**
 * Number of received bytes waiting in the buffer.
 * @param uart_no Software uart index, 0..SOFTUART_MAX_UARTS-1
 * @return number of bytes, 0 if none or the port is not open
 */
size_t softuart_rx_count(uint8_t uart_no);

/**
 * Read current byte from internal buffer if available.
 *
 * NOTE: This call is non blocking.
 * NOTE: You have to check softuart_available() first, since a received
 *       0x00 byte and an empty buffer both return 0. softuart_read_byte()
 *       does not have this ambiguity.
 * @param uart_no Software uart index, 0..SOFTUART_MAX_UARTS-1
 * @return current byte if available otherwise 0
 */
uint8_t softuart_read(uint8_t uart_no);

/**
 * Take the oldest received byte from the buffer, without blocking.
 * @param uart_no Software uart index, 0..SOFTUART_MAX_UARTS-1
 * @param[out] byte Where to store the byte
 * @return true if a byte was read, false if the buffer is empty, the port
 *         is not open or byte is NULL
 */
bool softuart_read_byte(uint8_t uart_no, uint8_t *byte);

/** Timeout for softuart_read_bytes() that never expires. */
#define SOFTUART_WAIT_FOREVER UINT32_MAX

/**
 * Read up to len bytes, waiting up to timeout_ms for them to arrive.
 *
 * Returns as soon as len bytes have been read, the timeout expires or the
 * port is closed. While waiting it sleeps one RTOS tick at a time
 * (vTaskDelay(1)), so call it from a task, and choose a receive buffer that
 * holds at least one tick's worth of data at your baud rate.
 * @param uart_no Software uart index, 0..SOFTUART_MAX_UARTS-1
 * @param[out] buf Destination buffer
 * @param len Maximum number of bytes to read
 * @param timeout_ms 0 to only take what is already buffered,
 *        SOFTUART_WAIT_FOREVER to wait until len bytes have arrived
 * @return number of bytes read (0 on invalid arguments)
 */
size_t softuart_read_bytes(uint8_t uart_no, void *buf, size_t len, uint32_t timeout_ms);

/**
 * Discard all bytes waiting in the receive buffer.
 * @param uart_no Software uart index, 0..SOFTUART_MAX_UARTS-1
 * @return true if no errors occured otherwise false
 */
bool softuart_rx_flush(uint8_t uart_no);

/**
 * Check whether received bytes were dropped because the buffer was full,
 * and clear the flag.
 * @param uart_no Software uart index, 0..SOFTUART_MAX_UARTS-1
 * @return true if at least one byte was dropped since the previous call
 *         (or since the port was opened)
 */
bool softuart_rx_overflow(uint8_t uart_no);

/**
 * Format a string like printf() and transmit it. Output longer than 127
 * bytes is formatted into a temporary heap buffer.
 * @param uart_no Software uart index, 0..SOFTUART_MAX_UARTS-1
 * @param format printf-style format string
 * @return number of bytes sent, or -1 on error
 */
int softuart_printf(uint8_t uart_no, const char *format, ...) __attribute__((format(printf, 2, 3)));

#ifdef __cplusplus
}
#endif

#endif /* SOFTUART_H_ */
