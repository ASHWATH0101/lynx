/* LYNX over UART between two ESP32-family boards (ESP-IDF).
 *
 * Both boards run this firmware, one with EXAMPLE_NODE=0, the other with 1.
 * Each node publishes a counter + temperature every 100 ms (no ACK); node 0
 * also pushes a setpoint to node 1 every second with a reliable write.
 *
 * Threading: rx_task only feeds received bytes to lx_rx_buf(). Everything
 * else (lx_tick, lx_publish, lx_write, reading variables) runs in app_main.
 * The component's port maps LYNX's critical sections to a portMUX spinlock.
 *
 * MIT License (c) 2026 Ashwath */
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lynx.h"

#define LINK_UART  UART_NUM_1
#define NODE       CONFIG_EXAMPLE_NODE
#define PEER       (1 - NODE)

static const char *TAG = "lynx";

/* Same ids on both nodes: each publishes its own slot, receives the other. */
enum { ID_COUNTER_0, ID_COUNTER_1, ID_TEMP_0, ID_TEMP_1, ID_SETPOINT };

static lx_t lynx;
static uint32_t counter[2];
static float temp[2];
static int32_t setpoint;

static void tx(void *ctx, const uint8_t *data, uint16_t len)
{
    (void)ctx;
    uart_write_bytes(LINK_UART, data, len);   /* driver serialises concurrent writers */
}

/* Runs in rx_task (ACK/NACK/read response) or in app_main (timeout). */
static void on_event(void *ctx, uint8_t ev, uint32_t mask)
{
    (void)ctx;
    static const char *const name[] = { "?", "done", "TIMEOUT", "NACK id", "NACK size" };
    ESP_LOGI(TAG, "txn 0x%08" PRIx32 ": %s", mask, name[ev < 5 ? ev : 0]);
}

static void rx_task(void *arg)
{
    (void)arg;
    uint8_t buf[128];
    for (;;) {
        int n = uart_read_bytes(LINK_UART, buf, sizeof buf, pdMS_TO_TICKS(20));
        if (n > 0) lx_rx_buf(&lynx, buf, (size_t)n);
    }
}

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void app_main(void)
{
    const uart_config_t cfg = {
        .baud_rate  = CONFIG_EXAMPLE_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(LINK_UART, 1024, 1024, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(LINK_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(LINK_UART, CONFIG_EXAMPLE_TX_GPIO, CONFIG_EXAMPLE_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    lx_init(&lynx, tx, on_event, NULL);
    for (uint8_t n = 0; n < 2; n++) {
        lx_bind(&lynx, ID_COUNTER_0 + n, &counter[n], sizeof counter[n]);
        lx_bind(&lynx, ID_TEMP_0 + n, &temp[n], sizeof temp[n]);
    }
    lx_bind(&lynx, ID_SETPOINT, &setpoint, sizeof setpoint);

    xTaskCreate(rx_task, "lynx_rx", 3072, NULL, configMAX_PRIORITIES - 2, NULL);

    uint32_t t_pub = 0, t_write = 0, shown = 0;
    for (;;) {
        uint32_t t = now_ms();
        lx_tick(&lynx, t);

        if (t - t_pub >= 100) {
            t_pub = t;
            counter[NODE]++;
            temp[NODE] = 20.0f + (float)(counter[NODE] % 50) / 10.0f;
            lx_publish(&lynx, LX_BIT(ID_COUNTER_0 + NODE) | LX_BIT(ID_TEMP_0 + NODE));
        }
        if (NODE == 0 && t - t_write >= 1000 && !lx_busy(&lynx)) {
            t_write = t;
            setpoint += 10;
            lx_write(&lynx, LX_BIT(ID_SETPOINT));
        }

        uint32_t up = lx_take_updates(&lynx);
        if (up & LX_BIT(ID_SETPOINT)) {
            int32_t sp;
            lx_get(&lynx, ID_SETPOINT, &sp);
            ESP_LOGI(TAG, "setpoint from node 0: %" PRId32, sp);
        }
        if ((up & LX_BIT(ID_COUNTER_0 + PEER)) && t - shown >= 1000) {
            uint32_t c; float f;
            shown = t;
            lx_get(&lynx, ID_COUNTER_0 + PEER, &c);
            lx_get(&lynx, ID_TEMP_0 + PEER, &f);
            ESP_LOGI(TAG, "peer counter %" PRIu32 ", temp %.1f | rx ok %" PRIu32 " bad %" PRIu32
                     " | tx %" PRIu32 " retx %" PRIu32,
                     c, f, lynx.st_rx_ok, lynx.st_rx_bad, lynx.st_tx, lynx.st_retx);
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
