// LYNX telemetry: two boards stream values to each other, no ACKs.
// Flash this sketch on both boards, with NODE 0 on one and NODE 1 on the
// other, and cross-connect TX/RX (plus GND) of the LINK UARTs.
#include <lynx.h>

#ifndef NODE
#define NODE 0   // 0 or 1
#endif

// Boards with a spare hardware UART use Serial1 for the link and keep Serial
// for logging. Single-UART boards (Uno, Nano) use Serial for the link only.
#if defined(HAVE_HWSERIAL1) || defined(ARDUINO_ARCH_ESP32) || defined(ARDUINO_ARCH_RP2040) || \
    defined(ARDUINO_ARCH_SAMD) || defined(ARDUINO_ARCH_MBED) || defined(ARDUINO_ARCH_STM32)
#define LINK Serial1
#define LOG  Serial
#else
#define LINK Serial
#endif

// The same ids on both nodes: each node publishes its own and receives the other's.
enum { ID_UPTIME_0, ID_UPTIME_1, ID_ANALOG_0, ID_ANALOG_1 };

static lx_t lynx;
static uint32_t uptime[2];   // [NODE] is ours, [!NODE] is the peer's
static uint16_t analog[2];

static void tx(void *ctx, const uint8_t *data, uint16_t len)
{
    (void)ctx;
    LINK.write(data, len);
}

void setup()
{
    LINK.begin(115200);
#ifdef LOG
    LOG.begin(115200);
#endif
    lx_init(&lynx, tx, NULL, NULL);
    for (uint8_t n = 0; n < 2; n++) {
        lx_bind(&lynx, ID_UPTIME_0 + n, &uptime[n], sizeof uptime[n]);
        lx_bind(&lynx, ID_ANALOG_0 + n, &analog[n], sizeof analog[n]);
    }
}

void loop()
{
    while (LINK.available()) lx_rx(&lynx, (uint8_t)LINK.read());
    lx_poll(&lynx);                    // decodes here if built with LX_ISR_SAFE; no-op otherwise

    static uint32_t last;
    if (millis() - last >= 100) {
        last = millis();
        uptime[NODE] = last;
        analog[NODE] = (uint16_t)analogRead(A0);
        // both variables in one frame
        lx_publish(&lynx, LX_BIT(ID_UPTIME_0 + NODE) | LX_BIT(ID_ANALOG_0 + NODE));
    }

    uint32_t got = lx_take_updates(&lynx);
    (void)got;
#ifdef LOG
    if (got & LX_BIT(ID_UPTIME_0 + !NODE)) {
        LOG.print(F("peer uptime "));  LOG.print(uptime[!NODE]);
        LOG.print(F(" ms, analog "));  LOG.print(analog[!NODE]);
        LOG.print(F(", frames ok/bad ")); LOG.print(lynx.st_rx_ok);
        LOG.print('/');                   LOG.println(lynx.st_rx_bad);
    }
#endif
}
