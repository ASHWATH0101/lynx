# Code size: LYNX vs BAVA

Regenerate with `BAVA_DIR=/path/to/bavaprotocol tools/size_report.sh [-DLX_...]`.
BAVA is built as a black box (`src/*.c` + `include/`, bare-metal path, `-std=gnu11 -w`);
LYNX is built `-std=c99 -Wall -Wextra -Werror -Wconversion`. Both at
`-Os -ffunction-sections -fdata-sections`. `.text` includes `.rodata`; neither library has
static RAM, all state lives in the handle.

Toolchains: arm-none-eabi-gcc 14.2.1, xtensa/riscv32-esp-elf-gcc 16.1.0 (esp-16.1.0_20260609),
host gcc 15.2. BAVA at commit `b13ba07` (2026-11-01). Measured 2026-11-02 on LYNX 0.1.0.

## Default build (LYNX `LX_MAX_PAYLOAD=64`, BAVA `BAVA_MAX_PAYLOAD=32`)

| Target | LYNX .text | sizeof(lx_t) | BAVA .text | sizeof(bava_handle_t) |
|---|---:|---:|---:|---:|
| x86-64 host | 3908 | 800 | 4283 | 968 |
| Cortex-M3 | 2100 | 528 | 2086 | 556 |
| Cortex-M0+ | 2296 | 528 | 2296 | 556 |
| ESP32 (Xtensa LX6) | 2613 | 528 | 2730 | 560 |
| ESP32-S3 (Xtensa LX7) | 2617 | 528 | 2722 | 560 |
| ESP32-C3 (RV32IMC) | 2822 | 528 | 2588 | 560 |

At equal payload (`-DLX_MAX_PAYLOAD=32u`) LYNX code is the same and `sizeof(lx_t)` drops to
**432 B** on 32-bit MCUs (704 B on x86-64), against 556–560 B for BAVA.

## Bus mode (`-DLX_BUS=1`) and ISR-safe receive (`-DLX_ISR_SAFE=1`)

BAVA always includes its bus mode, so the bus row is the like-for-like comparison.

| Target | LYNX bus | sizeof | LYNX ISR-safe | sizeof | BAVA |
|---|---:|---:|---:|---:|---:|
| x86-64 host | 4539 | 808 | 4135 | 952 | 4283 |
| Cortex-M3 | 2432 | 540 | 2288 | 680 | 2086 |
| Cortex-M0+ | 2660 | 540 | 2524 | 680 | 2296 |
| ESP32 (Xtensa LX6) | 2989 | 540 | 2825 | 680 | 2730 |
| ESP32-S3 (Xtensa LX7) | 2993 | 540 | 2830 | 680 | 2722 |
| ESP32-C3 (RV32IMC) | 3162 | 540 | 3066 | 680 | 2588 |

The ISR-safe handle includes the 128-byte receive queue and its 16-byte gap bitmap.

## How the default build got here (Cortex-M3)

| Step | .text |
|---|---:|
| Handed over | 1810 |
| `ctz` fallback on cores without CLZ (no effect on M3; see below) | 1810 |
| `lx_get` / `lx_set` (dropped by `--gc-sections` when unused) | 1898 |
| Concurrency hardening | 2100 |

Hardening (+202 B on M3): ~100 B in the RX decoder (answers are matched and the transaction
closed under the lock, event delivered before the handle goes idle, NACK reasons validated),
~50 B in `lx_tick` (decide and snapshot under the lock), 9 acquire/release barriers (~36 B; on a
single-core part `LX_LOAD8`/`LX_STORE8` can be overridden with plain accesses), and small
changes elsewhere.

`LX_BUS=0` costs nothing: when bus mode was added, `lynx.o` built with `-DLX_BUS=0` was
byte-for-byte identical to the sources without it on all six targets. All bus code sits inside
`#if LX_BUS` or the `BUS_P`/`BUS_A()` parameter macros, which expand to nothing.

## What the object sizes don't show

External symbols each object pulls in at link time:

| Target | LYNX | BAVA |
|---|---|---|
| Cortex-M3 | `memcpy memset` | `memcpy memset` |
| Cortex-M0+ | `memcpy memset __gnu_thumb1_case_uqi` | `memcpy memset __gnu_thumb1_case_uqi __atomic_test_and_set` |
| ESP32 | `memcpy memset` | `memcpy memset __bswapsi2` |
| ESP32-C3 | `memcpy memset` | `memcpy memset __atomic_test_and_set` |

The handed-over LYNX used `__builtin_ctz` everywhere. On cores without a CLZ/CTZ instruction
(Cortex-M0+, RV32IMC) that is a call to libgcc's `__ctzsi2`, which on RISC-V also drags in the
256 B `__clz_tab`: **314 B of hidden flash on ESP32-C3**, 64 B on M0+. `lynx.c` now uses the
builtin only where it is a native instruction (x86, AArch64, ARMv7-M+, Xtensa with NSA, RISC-V
Zbb) and a 5-step binary search elsewhere (`LX_CTZ_BUILTIN` overrides). Net linked effect:
ESP32-C3 −258 B, Cortex-M0+ −10 B, others unchanged.

## Caveats

- Object sizes, not a linked image: a real firmware also pays for `memcpy`/`memset` (usually
  already present) and, for BAVA on M0+/C3, an `__atomic_test_and_set` implementation. Unused
  API functions of either library drop out with `--gc-sections`.
- BAVA's `ESP_PLATFORM`/Zephyr/Arduino builds add RTOS mutex/spinlock members to its handle;
  the bare-metal path is measured here because it builds without an SDK. LYNX's ESP-IDF and
  Zephyr ports add calls to the platform lock, not handle members.
