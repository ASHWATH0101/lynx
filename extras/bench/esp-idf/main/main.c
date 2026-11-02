/* On-target LYNX vs BAVA benchmark: ESP-IDF port of bench_core.
 *
 * Flash one board as initiator and one as responder (menuconfig), connect
 * the link UARTs (TX -> RX both ways, GND), reset both. The initiator prints
 * a markdown table on its console when done (about 30 s).
 *
 * The bench task busy-polls the UART on the last core so that round trips
 * are not quantised by the FreeRTOS tick; sdkconfig.defaults turns off the
 * idle-task watchdog check for that reason.
 *
 * MIT License (c) 2026 Ashwath */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_cpu.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "bench_core.h"

#define LINK_UART UART_NUM_1

static void p_write(void *ctx, const uint8_t *d, uint16_t n)
{
    (void)ctx;
    uart_write_bytes(LINK_UART, d, n);  /* no TX ring buffer: returns once in the FIFO */
}

static bool p_idle(void *ctx)
{
    (void)ctx;
    return uart_wait_tx_done(LINK_UART, 0) == ESP_OK;
}

static void p_baud(void *ctx, uint32_t baud)
{
    (void)ctx;
    uart_set_baudrate(LINK_UART, baud);
    uart_flush_input(LINK_UART);        /* drop bytes garbled by the switch */
}

static uint32_t p_now(void *ctx)    { (void)ctx; return (uint32_t)esp_timer_get_time(); }
static uint32_t p_cycles(void *ctx) { (void)ctx; return esp_cpu_get_cycle_count(); }
static void p_print(void *ctx, const char *s) { (void)ctx; puts(s); }

static const bench_port_t port = {
    .write = p_write, .tx_idle = p_idle, .set_baud = p_baud,
    .now_us = p_now, .cycles = p_cycles, .print = p_print,
    .cpu_hz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1000000u,
    .cpu_name = CONFIG_IDF_TARGET,
};
static const uint32_t bauds[] = { CONFIG_BENCH_BAUD_LOW, CONFIG_BENCH_BAUD_HIGH };
static bench_t bench;

static void bench_task(void *arg)
{
    (void)arg;
    const uart_config_t cfg = {
        .baud_rate  = (int)bauds[0],
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(LINK_UART, 2048, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(LINK_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(LINK_UART, CONFIG_BENCH_TX_GPIO, CONFIG_BENCH_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_set_rx_timeout(LINK_UART, 1));   /* hand bytes over after 1 idle symbol */

#ifdef CONFIG_BENCH_ROLE_RESPONDER
    puts("LYNX bench: responder ready");
    bench_init(&bench, &port, BENCH_RESPONDER, bauds, 2, 2);
#else
    vTaskDelay(pdMS_TO_TICKS(500));                        /* let the responder boot */
    bench_init(&bench, &port, BENCH_INITIATOR, bauds, 2, 2);
#endif

    uint8_t buf[256];
    for (;;) {
        size_t n = 0;
        uart_get_buffered_data_len(LINK_UART, &n);
        if (n) {
            int got = uart_read_bytes(LINK_UART, buf, n < sizeof buf ? n : sizeof buf, 0);
            if (got > 0) bench_rx(&bench, buf, (size_t)got);
        }
        if (!bench_step(&bench)) break;
    }
    puts("LYNX bench: done");
    vTaskDelete(NULL);
}

void app_main(void)
{
    xTaskCreatePinnedToCore(bench_task, "bench", 8192, NULL, 5, NULL, portNUM_PROCESSORS - 1);
}
