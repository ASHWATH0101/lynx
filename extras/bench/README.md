# On-target benchmark: LYNX vs BAVA

Two boards, one link UART, one harness for both protocols. The initiator prints a markdown table:

- **CPU cycles** to encode / decode a reliable 4-byte write, its ACK, a 4-variable update and a
  publish (local loopback on the initiator, median of 201 runs, cycle-counter overhead subtracted);
- **round-trip latency** of 1000 reliable 4-byte writes and 1000 4-variable updates at each baud
  rate (default 115200 and 2 000 000): min / median / p99 / max, and how many were acknowledged;
- **delivery rate** of 1000 back-to-back 4-byte publishes (LYNX only: BAVA has no un-ACKed mode).

BAVA does not batch, so its "4 variables" figures are four writes in a row, each waiting for its
ACK — which is how a BAVA user would send them.

## Layout

| File | |
|---|---|
| `bench_core.[ch]` | platform-neutral harness (non-blocking state machines, report) |
| `bench_bava.[ch]` | BAVA behind a type-free adapter; public `bava.h` API only |
| `esp-idf/` | ESP-IDF firmware: UART driver, `esp_cpu_get_cycle_count()`, `esp_timer` |
| `host/sim.c` | the same harness over a simulated UART, used as a test |

## Running on two ESP32 boards

Wiring: initiator TX → responder RX, initiator RX ← responder TX, GND ↔ GND. Keep the wires short
for 2 Mbaud. Default pins: ESP32 TX 17 / RX 16, ESP32-S3 TX 17 / RX 18, ESP32-C3/C6 TX 4 / RX 5.

```sh
cd extras/bench/esp-idf
export BAVA_DIR=/path/to/bavaprotocol    # optional; omit for LYNX-only
idf.py set-target esp32
idf.py menuconfig        # "LYNX benchmark": Role = Responder
idf.py -p PORT_B flash
idf.py menuconfig        # Role = Initiator
idf.py -p PORT_A flash monitor
```

Reset the responder first, then the initiator. The run takes about 30 s at the default settings.
BAVA is compiled from `$BAVA_DIR/src/*.c` as a black box; it is GPLv3 and is not part of this
repository, so a firmware image built with it is a GPLv3 combined work: keep it for local
measurement only. By default BAVA is built with its own ESP-IDF integration (FreeRTOS mutex);
`BENCH_BAVA_BAREMETAL` measures its bare-metal path instead.

Both protocols get identical treatment: same compiler flags (`-O2`), same UART settings
(RX timeout of 1 symbol), the same busy-polling loop, and every received byte is fed to both
decoders in turn on both boards.

## STM32 (or any other MCU)

Implement `bench_port_t` and call `bench_rx()` / `bench_step()` from your main loop. For the cycle
counter on Cortex-M3/M4/M7:

```c
CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
DWT->CYCCNT = 0;
DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;
static uint32_t p_cycles(void *ctx) { (void)ctx; return DWT->CYCCNT; }
```

`set_baud` is only called once `tx_idle` reports the last byte has left the shift register.

## Host simulation (runs in CI)

`host/sim.c` joins an initiator and a responder with a simulated UART in virtual time: each byte
takes 10 bit times at the sender's baud, and bytes that cross a baud switch arrive as garbage.
The simulated CPU is infinitely fast, so every round trip must equal pure wire time; the run fails
if one doesn't, or if anything is lost on a clean line. `sim 0.001` drops 0.1 % of bytes to
exercise retries (LYNX retries; BAVA reports failures).

```sh
tools/run_tests.sh                      # includes the simulation
BAVA_DIR=/path/to/bavaprotocol tools/run_tests.sh
```

Simulated 115200 baud, clean line: LYNX 4-byte write round trip 1391 µs (wire time 1388 µs,
16 bytes), BAVA 1564 µs (1562 µs, 18 bytes); 4 variables: LYNX 2866 µs (33 bytes), BAVA 5996 µs
(69 bytes). These are wire-time checks of the harness, not measurements: run the firmware for
real numbers.

## Results

Not measured on hardware yet. Paste the initiator's table here, with board, IDF version and
wiring, when you run it.
