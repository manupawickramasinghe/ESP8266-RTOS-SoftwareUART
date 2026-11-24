/*
 * Software UART example for ESP8266_RTOS_SDK.
 *
 * Opens software UART port 0 on the pins and baud rate chosen in menuconfig
 * ("Software UART example"), sends a greeting, echoes every received byte
 * back and logs each received line on the console. A heartbeat line is sent
 * every 10 seconds so the link can be checked without typing.
 *
 * Connect a 3.3 V USB-serial adapter: adapter TX -> RX GPIO, adapter RX <-
 * TX GPIO, GND <-> GND, then open it at the configured baud rate, 8N1.
 */
#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "softuart.h"

#define PORT 0
#define HEARTBEAT_PERIOD_MS 10000

#ifdef CONFIG_EXAMPLE_SOFTUART_INVERT
#define INVERT true
#else
#define INVERT false
#endif

static const char *TAG = "softuart-example";

void app_main(void)
{
    ESP_LOGI(TAG, "software UART %d: RX GPIO%d, TX GPIO%d, %d baud 8N1%s", PORT,
             CONFIG_EXAMPLE_SOFTUART_RX_GPIO, CONFIG_EXAMPLE_SOFTUART_TX_GPIO,
             CONFIG_EXAMPLE_SOFTUART_BAUD_RATE, INVERT ? ", inverted" : "");

    if (!softuart_open(PORT, CONFIG_EXAMPLE_SOFTUART_BAUD_RATE, CONFIG_EXAMPLE_SOFTUART_RX_GPIO,
                       CONFIG_EXAMPLE_SOFTUART_TX_GPIO, INVERT)) {
        ESP_LOGE(TAG, "softuart_open() failed, check the pins in menuconfig");
        return;
    }

    softuart_puts(PORT, "\r\nHello from Soft UART! Type something and it is echoed back.\r\n");

    char line[80];
    size_t len = 0;
    unsigned heartbeat = 0;
    TickType_t last_heartbeat = xTaskGetTickCount();

    while (true) {
        uint8_t byte;
        while (softuart_read_byte(PORT, &byte)) {
            char c = (char)byte;

            if (c == '\r' || c == '\n') {
                softuart_puts(PORT, "\r\n");
                if (len > 0) {
                    line[len] = '\0';
                    ESP_LOGI(TAG, "received: %s", line);
                    len = 0;
                }
                continue;
            }

            softuart_put(PORT, c); // echo
            if (len < sizeof(line) - 1) {
                line[len++] = c;
            }
        }

        if (softuart_rx_overflow(PORT)) {
            ESP_LOGW(TAG, "receive buffer overflowed, some bytes were dropped");
        }

        if (xTaskGetTickCount() - last_heartbeat >= pdMS_TO_TICKS(HEARTBEAT_PERIOD_MS)) {
            last_heartbeat = xTaskGetTickCount();
            softuart_printf(PORT, "[heartbeat %u]\r\n", ++heartbeat);
        }

        // Yield so the idle task runs and feeds the task watchdog
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
