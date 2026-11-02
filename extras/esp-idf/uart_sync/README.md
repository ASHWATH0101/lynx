# LYNX UART sync (ESP-IDF)

Two ESP32-family boards keep a counter, a temperature and a setpoint in sync over UART1.

Wiring: board A TX → board B RX, A RX ← B TX, GND ↔ GND (defaults: ESP32 TX 17 / RX 16,
ESP32-S3 TX 17 / RX 18, ESP32-C3/C6 TX 4 / RX 5; change in menuconfig).

```sh
idf.py set-target esp32          # or esp32s3, esp32c3, ...
idf.py menuconfig                # "LYNX UART example": node 0 on one board, 1 on the other
idf.py -p PORT flash monitor
```

The LYNX component itself is configured under "LYNX variable sync" (payload size, timeout,
retries, bus mode). `main/idf_component.yml` points the component manager at this repository.

Threading: `rx_task` only calls `lx_rx_buf()`; `app_main` does everything else. The component's
port maps LYNX's critical sections to a `portMUX` spinlock, and the example reads peer
variables with `lx_get()` so a commit from `rx_task` can never be seen half-written.
