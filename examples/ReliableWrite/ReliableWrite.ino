// LYNX reliable write + read: the controller (NODE 0) pushes a setpoint to
// the device (NODE 1) with ACK and auto-retry, then reads back the device's
// measured value. The event callback reports DONE / TIMEOUT / NACK.
// Flash on two boards (NODE 0 and NODE 1), cross-connect the LINK UARTs.
#include <lynx.h>

#ifndef NODE
#define NODE 0   // 0 = controller, 1 = device
#endif

#if defined(HAVE_HWSERIAL1) || defined(ARDUINO_ARCH_ESP32) || defined(ARDUINO_ARCH_RP2040) || \
    defined(ARDUINO_ARCH_SAMD) || defined(ARDUINO_ARCH_MBED) || defined(ARDUINO_ARCH_STM32)
#define LINK Serial1
#define LOG  Serial
#else
#define LINK Serial
#endif

enum { ID_SETPOINT, ID_MEASURED };

struct Setpoint {      // any POD struct up to LX_MAX_PAYLOAD bytes;
    int16_t target;    // both nodes must agree on its layout
    uint8_t mode;
};

static lx_t lynx;
static Setpoint setpoint;
static int32_t measured;

static void tx(void *ctx, const uint8_t *data, uint16_t len)
{
    (void)ctx;
    LINK.write(data, len);
}

// Called from lx_rx() (here: from loop) or lx_tick().
static void on_event(void *ctx, uint8_t ev, uint32_t mask)
{
    (void)ctx; (void)mask;
#ifdef LOG
    switch (ev) {
    case LX_EV_DONE:      LOG.println(mask == LX_BIT(ID_MEASURED) ? F("read done") : F("write ACKed")); break;
    case LX_EV_TIMEOUT:   LOG.println(F("no answer after retries")); break;
    case LX_EV_NACK_ID:   LOG.println(F("peer has no such variable")); break;
    case LX_EV_NACK_SIZE: LOG.println(F("size mismatch between nodes")); break;
    }
#else
    (void)ev;
#endif
}

void setup()
{
    LINK.begin(115200);
#ifdef LOG
    LOG.begin(115200);
#endif
    lx_init(&lynx, tx, on_event, NULL);
    lx_bind(&lynx, ID_SETPOINT, &setpoint, sizeof setpoint);
    lx_bind(&lynx, ID_MEASURED, &measured, sizeof measured);
}

void loop()
{
    while (LINK.available()) lx_rx(&lynx, (uint8_t)LINK.read());
    lx_poll(&lynx);                    // decodes here if built with LX_ISR_SAFE; no-op otherwise
    lx_tick(&lynx, millis());          // drives timeouts and retries

#if NODE == 0
    static uint32_t last;
    static uint8_t step;
    if (!lx_busy(&lynx) && millis() - last >= 1000) {
        last = millis();
        if (step++ & 1) {
            lx_read(&lynx, ID_MEASURED);
        } else {
            setpoint.target = (int16_t)(500 + (step % 10) * 50);
            setpoint.mode = 1;
            lx_write(&lynx, LX_BIT(ID_SETPOINT));
        }
    }
#ifdef LOG
    static int32_t shown = -1;
    if (measured != shown) { shown = measured; LOG.print(F("device measured ")); LOG.println(measured); }
#endif
#else
    // device: act on new setpoints, keep `measured` current for remote reads
    if (lx_take_updates(&lynx) & LX_BIT(ID_SETPOINT)) {
#ifdef LOG
        LOG.print(F("new setpoint ")); LOG.println(setpoint.target);
#endif
    }
    measured = analogRead(A0) + setpoint.target;
#endif
}
