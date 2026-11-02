/* LYNX over UART between two Zephyr boards, ISR-safe receive mode.
 *
 * Both boards run this sample, one with CONFIG_APP_LYNX_NODE=0, the other
 * with 1. Each node publishes a counter + temperature every 100 ms; node 0
 * also pushes a setpoint to node 1 every second with a reliable write.
 *
 * CONFIG_LYNX_ISR_SAFE: the UART ISR hands bytes straight to lx_rx_buf(),
 * which only queues them. main() calls lx_poll(), so decoding, replies and
 * events all happen in this one thread: variables can be used directly and
 * tx() needs no lock.
 *
 * MIT License (c) 2026 Ashwath */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>
#include "lynx.h"

#if !LX_ISR_SAFE
#error "this sample expects CONFIG_LYNX_ISR_SAFE=y (see prj.conf)"
#endif

#define NODE CONFIG_APP_LYNX_NODE
#define PEER (1 - NODE)

#if !DT_NODE_HAS_STATUS(DT_ALIAS(lynx_uart), okay)
#error "add a 'lynx-uart' alias to an enabled UART (see boards/*.overlay)"
#endif
static const struct device *const uart = DEVICE_DT_GET(DT_ALIAS(lynx_uart));

enum { ID_COUNTER_0, ID_COUNTER_1, ID_TEMP_0, ID_TEMP_1, ID_SETPOINT };

static lx_t lynx;
static uint32_t counter[2];
static float temp[2];
static int32_t setpoint;

static void uart_isr(const struct device *dev, void *user_data)
{
    ARG_UNUSED(user_data);
    while (uart_irq_update(dev) && uart_irq_rx_ready(dev)) {
        uint8_t buf[32];
        int n = uart_fifo_read(dev, buf, sizeof buf);
        if (n <= 0) break;
        lx_rx_buf(&lynx, buf, (size_t)n);   /* lock-free enqueue, nothing else */
    }
}

static void tx(void *ctx, const uint8_t *data, uint16_t len)
{
    ARG_UNUSED(ctx);
    for (uint16_t i = 0; i < len; i++) uart_poll_out(uart, data[i]);
}

static void on_event(void *ctx, uint8_t ev, uint32_t mask)
{
    ARG_UNUSED(ctx);
    static const char *const name[] = { "?", "done", "TIMEOUT", "NACK id", "NACK size" };
    printk("txn 0x%08x: %s\n", mask, name[ev < 5 ? ev : 0]);
}

int main(void)
{
    if (!device_is_ready(uart)) {
        printk("lynx-uart not ready\n");
        return 0;
    }
    lx_init(&lynx, tx, on_event, NULL);
    for (uint8_t n = 0; n < 2; n++) {
        lx_bind(&lynx, ID_COUNTER_0 + n, &counter[n], sizeof counter[n]);
        lx_bind(&lynx, ID_TEMP_0 + n, &temp[n], sizeof temp[n]);
    }
    lx_bind(&lynx, ID_SETPOINT, &setpoint, sizeof setpoint);

    uart_irq_callback_user_data_set(uart, uart_isr, NULL);
    uart_irq_rx_enable(uart);

    uint32_t t_pub = 0, t_write = 0, shown = 0;
    for (;;) {
        uint32_t t = k_uptime_get_32();
        lx_poll(&lynx);                     /* decode, reply, fire events */
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
        if (up & LX_BIT(ID_SETPOINT)) printk("setpoint from node 0: %d\n", setpoint);
        if ((up & LX_BIT(ID_COUNTER_0 + PEER)) && t - shown >= 1000) {
            shown = t;
            printk("peer counter %u | rx ok %u bad %u ovf %u | tx %u retx %u\n",
                   counter[PEER], lynx.st_rx_ok, lynx.st_rx_bad, lynx.st_rx_ovf,
                   lynx.st_tx, lynx.st_retx);
        }
        k_msleep(2);    /* the 256-byte queue covers ~22 ms at 115200 baud */
    }
}
