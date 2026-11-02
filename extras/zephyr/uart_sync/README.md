# LYNX UART sync (Zephyr)

Two boards keep a counter, a temperature and a setpoint in sync over the UART aliased
`lynx-uart` (see `boards/*.overlay`; add one for your board).

```sh
west build -b nrf52840dk/nrf52840 path/to/lynx/extras/zephyr/uart_sync -- -DCONFIG_APP_LYNX_NODE=0
west flash
```

Flash the second board with `-DCONFIG_APP_LYNX_NODE=1`. The sample's `CMakeLists.txt` adds this
repository as a Zephyr module via `ZEPHYR_EXTRA_MODULES`; to use LYNX in your own application,
add the repository to your `west.yml` (or `ZEPHYR_EXTRA_MODULES`) and set `CONFIG_LYNX=y`.
Options live under `CONFIG_LYNX_*` (payload size, timeout, retries, bus mode).

Threading: built with `CONFIG_LYNX_ISR_SAFE=y`. The UART ISR passes bytes to `lx_rx_buf()`,
which only appends them to LYNX's lock-free queue; `main()` calls `lx_poll()`, so decoding,
replies and events all run in `main()`. Variables can therefore be used directly and `tx()` needs
no lock. (The ESP-IDF example shows the other model: a receive task decoding concurrently,
protected by the port's critical sections.)
