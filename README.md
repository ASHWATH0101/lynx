# LYNX

[![CI](https://github.com/ASHWATH0101/lynx/actions/workflows/ci.yml/badge.svg)](https://github.com/ASHWATH0101/lynx/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

Zero-allocation variable sync between microcontrollers, over any byte link (UART, RS-485, SPI,
BLE, Wi-Fi). Bind variables to ids on both ends; LYNX keeps them in sync.

- **Publish** (fire-and-forget) or **reliable write** (ACK, automatic retries, timeout event)
- **Batch**: any set of variables in one frame, committed atomically on the receiver
- **Read** a variable from the peer
- **Multi-drop bus** mode with node addresses and broadcast (`LX_BUS`)
- **ISR-safe receive** mode: the UART ISR only queues bytes (`LX_ISR_SAFE`)
- COBS framing + CRC-16 computed in one streaming pass; **O(1) work per received byte**
- C99, no heap, no dependencies beyond `<string.h>`; ~2.1 KB of code on a Cortex-M3
- MIT licensed. Ready-made packaging for ESP-IDF, Arduino and Zephyr.

```c
#include "lynx.h"

static lx_t lynx;
static float setpoint;
static uint32_t uptime;

static void uart_tx(void *ctx, const uint8_t *d, uint16_t n) { my_uart_write(d, n); }
static void on_event(void *ctx, uint8_t ev, uint32_t mask)
{
    if (ev == LX_EV_TIMEOUT) printf("peer did not answer\n");
}

void setup(void)
{
    lx_init(&lynx, uart_tx, on_event, NULL);
    lx_bind(&lynx, 0, &setpoint, sizeof setpoint);   /* same ids, sizes and layout on both nodes */
    lx_bind(&lynx, 1, &uptime, sizeof uptime);
}

void loop(void)
{
    while (my_uart_available()) lx_rx(&lynx, my_uart_read());
    lx_tick(&lynx, millis());                        /* timeouts and retries */

    static uint32_t last;
    if (millis() - last >= 100) {                    /* telemetry, no ACK, 10 Hz */
        last = uptime = millis();
        lx_publish(&lynx, LX_BIT(1));
    }
    if (button_pressed() && !lx_busy(&lynx)) {
        setpoint = 21.5f;
        lx_write(&lynx, LX_BIT(0));                  /* reliable: ACK + retry */
    }
    if (lx_take_updates(&lynx) & LX_BIT(0)) apply(setpoint);   /* the peer changed it */
}
```

## Contents

- [Frame format](#frame-format) · [API](#api) · [Configuration](#configuration) ·
  [Concurrency and ISRs](#concurrency-and-isrs) · [Platforms](#platforms) ·
  [Benchmarks](#benchmarks) · [Tests](#tests) · [License](#license)

## Frame format

Every frame is a raw byte string, COBS-encoded and terminated by `0x00`:

```
raw:   [HDR] [DST SRC]? [ID] [PAYLOAD 0..LX_MAX_PAYLOAD] [CRC_HI] [CRC_LO]
wire:  COBS(raw) 0x00                      (LX_LEAD_DELIM=1: 0x00 COBS(raw) 0x00)

HDR  = CMD << 4 | SEQ
CRC  = CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection) over HDR..PAYLOAD,
       big-endian, so the CRC over the whole raw frame is 0 for a valid frame
DST, SRC: only with LX_BUS=1; DST 0xFF = broadcast
```

| CMD | Name | ID | Payload | Answer |
|---:|---|---|---|---|
| 1 | WRITE | variable id | the variable's bytes | ACK or NACK |
| 2 | WRITE_M | `0xFF` | `(id, len, bytes)` repeated | ACK or NACK |
| 3 | PUB | variable id | the variable's bytes | none |
| 4 | PUB_M | `0xFF` | `(id, len, bytes)` repeated | none |
| 5 | READ | variable id | none | READ_RSP or NACK |
| 6 | READ_RSP | variable id | the variable's bytes | — |
| 7 | ACK | request's ID | none | — |
| 8 | NACK | request's ID | 1 byte: 3 = no such variable, 4 = size mismatch | — |

- **SEQ** is 0 for publishes. Each reliable write or read takes the next value mod 16 and the
  answer echoes it, so a late answer to an old request cannot complete a new one. Only one
  reliable transaction is in flight per handle; publishes can be sent at any time.
- A **batch** is validated completely (ids bound, sizes match, lengths consistent) before
  anything is committed: the receiver applies all of it or none of it.
- The receiver decodes COBS and updates the CRC as each byte arrives and never buffers more than
  one raw frame. A frame is accepted only if COBS is consistent, the length is plausible and the
  CRC residue is 0. Noise costs the frame it hits and at most the next one (with
  `LX_LEAD_DELIM=1`, never the next).
- **Bus mode**: nodes act only on frames addressed to them or to `0xFF`, and ignore frames
  carrying their own address as source (bus echo). Broadcasts are never answered: a broadcast
  WRITE is applied like a publish, a broadcast READ is ignored. Only the node a request was sent
  to can complete it.

Sizes on the wire: a 4-byte write is 10 bytes and its ACK 6 (16 per round trip); four variables
totalling 13 bytes in one batch are 27 + 6 = 33 bytes; a 4-byte publish is 10 bytes. Bus mode adds
2 bytes per frame.

## API

| Call | |
|---|---|
| `lx_init(h, tx, ev, ctx)` | `tx(ctx, data, len)` sends bytes; `ev(ctx, event, mask)` reports transaction results (may be NULL) |
| `lx_bind(h, id, ptr, size)` | bind variable `id` (0..`LX_MAX_VARS`-1) to `size` bytes at `ptr` |
| `lx_rx(h, byte)`, `lx_rx_buf(h, data, len)` | feed received bytes |
| `lx_poll(h)` | `LX_ISR_SAFE`: decode what `lx_rx` queued. No-op otherwise, so it is safe to always call |
| `lx_tick(h, now_ms)` | drives timeouts and retries; call often |
| `lx_publish(h, mask)` | send the variables in `mask` in one frame, no ACK |
| `lx_write(h, mask)` | reliable write of the variables in `mask`, one frame, atomic on the receiver |
| `lx_read(h, id)` | fetch the peer's value of `id` |
| `lx_busy(h)` | a reliable transaction is in flight (true until its event callback has returned) |
| `lx_take_updates(h)` | fetch and clear the bitmask of variables the peer changed |
| `lx_get(h, id, out)`, `lx_set(h, id, in)` | copy a bound variable under the critical section (multi-threaded use) |
| `lx_set_addr(h, self, peer)` | `LX_BUS`: this node's address, and the default destination |
| `lx_publish_to`, `lx_write_to`, `lx_read_from` | `LX_BUS`: explicit destination (publish may broadcast) |

`LX_BIT(id)` builds masks. Return codes: `LX_OK`, `LX_ERR_BUSY`, `LX_ERR_ID`, `LX_ERR_SIZE`,
`LX_ERR_ADDR`. Events: `LX_EV_DONE`, `LX_EV_TIMEOUT` (after `LX_RETRIES` retransmissions),
`LX_EV_NACK_ID`, `LX_EV_NACK_SIZE`; each transaction produces exactly one. Link statistics in the
handle: `st_rx_ok`, `st_rx_bad`, `st_tx`, `st_rsp` (replies sent), `st_retx`, `st_rx_ovf`
(`LX_ISR_SAFE`).

Variables are sent as raw bytes: both nodes must agree on each variable's size and layout
(endianness, struct padding).

## Configuration

Compile-time options; set with `-D`, or through Kconfig on ESP-IDF and Zephyr.

| Option | Default | |
|---|---|---|
| `LX_MAX_VARS` | 32 | variables per handle, ≤ 32 |
| `LX_MAX_PAYLOAD` | 64 | bytes per frame, ≤ 250 (≤ 248 with `LX_BUS`); sizes the three frame buffers in `lx_t` |
| `LX_TIMEOUT_MS` | 20 | wait for ACK / read response |
| `LX_RETRIES` | 3 | retransmissions before `LX_EV_TIMEOUT` |
| `LX_LEAD_DELIM` | 0 | also send `0x00` before each frame |
| `LX_BUS` | 0 | multi-drop bus mode; 0 compiles the bus code out entirely |
| `LX_ISR_SAFE` | 0 | `lx_rx` only queues; `lx_poll` decodes |
| `LX_RXQ_SIZE` | 128 | `LX_ISR_SAFE` queue, power of two ≤ 256 |
| `LX_ENTER_CRITICAL()` / `LX_EXIT_CRITICAL()` | no-op | critical-section hooks, see below |
| `LX_LOAD8` / `LX_STORE8` | GNU acquire/release atomics | override for other compilers, or with plain accesses on single-core parts |
| `LX_CTZ_BUILTIN` | auto | use `__builtin_ctz` (only where it is a native instruction) |

`sizeof(lx_t)` on a 32-bit MCU: 528 bytes by default, 432 with `LX_MAX_PAYLOAD=32`, +12 with
`LX_BUS`, +152 with `LX_ISR_SAFE`.

## Concurrency and ISRs

A handle has two sides: the **RX path** (`lx_rx`, or `lx_poll` with `LX_ISR_SAFE`) and the
**app path** (every other call, from one task). Pick one of three models:

1. **One loop** (typical Arduino): read the UART and call everything from `loop()`. No hooks
   needed. An event callback may start the next transaction.
2. **RX in its own task or ISR, decoding there**: define `LX_ENTER_CRITICAL`/`LX_EXIT_CRITICAL`
   (the ESP-IDF and Zephyr ports do). They guard the transaction state, the update flags and the
   bound variables; use `lx_get`/`lx_set` for variables the other side writes or sends. `tx()` is
   then called from both sides and must keep each call's bytes together; from an ISR, `tx()` and
   `ev()` must be ISR-safe. Example: [`extras/esp-idf/uart_sync`](extras/esp-idf/uart_sync).
3. **`LX_ISR_SAFE`**: the ISR calls `lx_rx`, which only appends to a lock-free single-producer
   queue (no callbacks, no locks, safe on another core). The app task calls `lx_poll`, so all
   decoding, replies and events happen in that one task and no hooks are needed. If the queue
   fills up, bytes are dropped (`st_rx_ovf`) and every frame that lost bytes is discarded.
   Example: [`extras/zephyr/uart_sync`](extras/zephyr/uart_sync).

Guarantees in every model: exactly one event per transaction, delivered before `lx_busy()` turns
false; a retransmission always repeats the original request; answers can only complete the
transaction they belong to (and, on a bus, only from the node that was asked).

## Platforms

**ESP-IDF** — the repository is a component (`idf_component.yml`, `CMakeLists.txt`, `Kconfig`).
Its port maps the critical sections to a `portMUX` spinlock (`portENTER_CRITICAL_SAFE`, usable
from tasks and ISRs on either core). Options live under *LYNX variable sync* in menuconfig.
Example with a FreeRTOS receive task: [`extras/esp-idf/uart_sync`](extras/esp-idf/uart_sync).

```yaml
# main/idf_component.yml of your project
dependencies:
  lynx:
    path: ../components/lynx        # or a git URL
```

**Arduino** — install the repository as a library (`library.properties`); examples:
[`Telemetry`](examples/Telemetry/Telemetry.ino) (two boards stream values to each other) and
[`ReliableWrite`](examples/ReliableWrite/ReliableWrite.ino) (write with ACK, read back, events).
Built in CI for Uno, Mega and ESP32.

**Zephyr** — the repository is a module (`zephyr/module.yml`): add it to your `west.yml` or to
`ZEPHYR_EXTRA_MODULES` and set `CONFIG_LYNX=y`. Its port uses a `k_spinlock`. Sample using
`CONFIG_LYNX_ISR_SAFE`: [`extras/zephyr/uart_sync`](extras/zephyr/uart_sync).

**Anything else** — compile `lynx.c` with your project (or `add_subdirectory` this repository
in CMake, target `lynx::lynx`). On a Cortex-M with an RX interrupt, either use `LX_ISR_SAFE` or:

```c
#define LX_ENTER_CRITICAL() uint32_t lx_pm = __get_PRIMASK(); __disable_irq()
#define LX_EXIT_CRITICAL()  __set_PRIMASK(lx_pm)
```

(`LX_ENTER_CRITICAL` may declare a local; LYNX never nests critical sections.)

## Benchmarks

Comparisons are against [BAVA](https://github.com/NishanthRaj707/bavaprotocol) v2, built from
source as a black box through its public API (`tools/size_report.sh`, `test/bench_vs_bava.c`,
`extras/bench`). BAVA has no batching and no un-acknowledged mode, so its "4 variables" figures
are four writes in a row, each waiting for its ACK.

**Code size** (`.text` of the object file, `-Os`, see [docs/sizes.md](docs/sizes.md) for all
targets, RAM and caveats):

| | Cortex-M3 | ESP32 | ESP32-C3 |
|---|---:|---:|---:|
| LYNX | 2100 | 2613 | 2822 |
| LYNX `LX_BUS=1` | 2432 | 2989 | 3162 |
| LYNX `LX_ISR_SAFE=1` | 2288 | 2825 | 3066 |
| BAVA (always includes bus mode) | 2086 | 2730 | 2588 |
| RAM: `sizeof(lx_t)` / `sizeof(bava_handle_t)` | 528 / 556 | 528 / 560 | 528 / 560 |

LYNX's default build is level with BAVA on Cortex-M, smaller on Xtensa and larger on RISC-V,
while also carrying publish, batch writes, reads, NACK reasons, retries and `lx_get`/`lx_set`
(unused functions drop out at link time with `--gc-sections`). The concurrency hardening in 0.1.0
cost about 200 bytes; the payload buffer size does not affect code size.

**Host benchmark** (`test/bench_vs_bava.c`: two nodes in one process, the link is a memory copy,
`gcc -O2`, Intel Core Ultra 5 125H under WSL; median of 5 runs, range in brackets):

| Case | BAVA | LYNX |
|---|---:|---:|
| Reliable 4-byte write + ACK | 230 ns [156–307], 18 B on the wire | 145 ns [129–154], 16 B |
| 4 variables (13 B), reliable | 783 ns [433–937], 69 B | 311 ns [200–323], 33 B |
| 4-byte publish, no ACK | not supported | 83 ns [53–90], 10 B |

These are host numbers: they show relative CPU cost and bytes on the wire, not MCU timings. On a
real link the wire time dominates: at 115 200 baud a 4-byte reliable write takes 1.39 ms of wire
time with LYNX and 1.56 ms with BAVA; four variables take 2.9 ms against 6.0 ms.

**On target**: [`extras/bench`](extras/bench) is a two-board benchmark (ESP32 ↔ ESP32, ported to
STM32 by implementing a small port struct) that measures cycles per encode/decode with the cycle
counter, round-trip latency and delivery rate at 115 200 and 2 000 000 baud for both protocols,
and prints a markdown table. Its harness is verified in simulation in CI. **No on-target results
yet** — they will be added here once measured on hardware.

## Tests

```sh
tools/run_tests.sh                  # gcc; CC=clang works too
BAVA_DIR=/path/to/bavaprotocol tools/run_tests.sh   # also runs the BAVA benchmark simulation
tools/size_report.sh [-DLX_BUS=1]   # cross-compile sizes for the toolchains on PATH
```

`run_tests.sh` builds `lynx.c` with `-Wall -Wextra -Werror -Wconversion -Wshadow -pedantic` and
runs [`test/test_lynx.c`](test/test_lynx.c) under ASan + UBSan in 11 configurations (default,
lead delimiter, CTZ fallback, bus mode, ISR-safe with 16/128/256-byte queues, and with a test port
that checks the critical-section contract and fires simulated interrupts at the worst moments).
Highlights: 200 000 frames through random bit flips with zero corrupted commits, 2 000 000 bytes
of noise into the parser, a three-node bus fuzz with zero misrouted commits, ACK / timeout /
read-response races, queue overflow.
[`test/test_threads.c`](test/test_threads.c) then runs both nodes in real threads under
ThreadSanitizer with a lossy link and 2 ms timeouts, and
[`extras/bench/host/sim.c`](extras/bench/host/sim.c) checks the benchmark harness against wire
time. CI also cross-compiles for six targets and builds the Arduino, ESP-IDF and Zephyr examples.

## License

MIT, see [LICENSE](LICENSE). LYNX is a clean-room implementation: it was written without
reference to BAVA's source code. BAVA (GPLv3) is never copied into this repository; the
benchmark and size scripts only compile it, from a separate checkout, for comparison.
