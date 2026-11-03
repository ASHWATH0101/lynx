# Code size: LYNX vs BAVA

Regenerate with `BAVA_DIR=/path/to/bavaprotocol tools/size_report.sh [-DLX_...]`.
BAVA is built as a black box (`src/*.c` + `include/`, bare-metal path, `-std=gnu11 -w`);
LYNX is built `-std=c99 -Wall -Wextra -Werror -Wconversion`, one object per file in `src/`,
sizes summed. Both at `-Os -ffunction-sections -fdata-sections`. `.text` includes `.rodata`;
neither library has static RAM, all state lives in the handle.

Toolchains: arm-none-eabi-gcc 13.2.1 (the Ubuntu 24.04 package CI uses), xtensa/riscv32-esp-elf-gcc
16.1.0 (esp-16.1.0_20260609), host gcc 13.3. BAVA at commit `b13ba07` (2026-11-01). Both
libraries measured 2026-11-03, after LYNX's sources were split into `src/` (see CHANGELOG).
Earlier editions of this page used arm-none-eabi-gcc 14.2.1 and host gcc 15.2, which give
different absolute numbers for both libraries.

## Default build (LYNX `LX_MAX_PAYLOAD=64`, BAVA `BAVA_MAX_PAYLOAD=32`)

| Target | LYNX .text | sizeof(lx_t) | BAVA .text | sizeof(bava_handle_t) |
|---|---:|---:|---:|---:|
| x86-64 host | 3839 | 800 | 3781 | 968 |
| Cortex-M3 | 1954 | 528 | 2074 | 556 |
| Cortex-M0+ | 2092 | 528 | 2272 | 556 |
| ESP32 (Xtensa LX6) | 2678 | 528 | 2730 | 560 |
| ESP32-S3 (Xtensa LX7) | 2686 | 528 | 2722 | 560 |
| ESP32-C3 (RV32IMC) | 2948 | 528 | 2588 | 560 |

At equal payload (`-DLX_MAX_PAYLOAD=32u`) LYNX code is within 10 bytes of this and
`sizeof(lx_t)` drops to **432 B** on 32-bit MCUs (704 B on x86-64), against 556–560 B for BAVA.

## Bus mode (`-DLX_BUS=1`) and ISR-safe receive (`-DLX_ISR_SAFE=1`)

BAVA always includes its bus mode, so the bus row is the like-for-like comparison.

| Target | LYNX bus | sizeof | LYNX ISR-safe | sizeof | BAVA |
|---|---:|---:|---:|---:|---:|
| x86-64 host | 4415 | 808 | 4103 | 952 | 3781 |
| Cortex-M3 | 2280 | 540 | 2138 | 680 | 2074 |
| Cortex-M0+ | 2472 | 540 | 2322 | 680 | 2272 |
| ESP32 (Xtensa LX6) | 3068 | 540 | 2883 | 680 | 2730 |
| ESP32-S3 (Xtensa LX7) | 3076 | 540 | 2891 | 680 | 2722 |
| ESP32-C3 (RV32IMC) | 3246 | 540 | 3188 | 680 | 2588 |

The ISR-safe handle includes the 128-byte receive queue and its 16-byte gap bitmap.

## How the default build got here (Cortex-M3)

| Step | .text |
|---|---:|
| Handed over | 1810 |
| `ctz` fallback on cores without CLZ (no effect on M3; see below) | 1810 |
| `lx_get` / `lx_set` (dropped by `--gc-sections` when unused) | 1898 |
| Concurrency hardening | 2100 |
| `p_cmd` race fix (one atomic load per check instead of up to three volatile loads) | 2056 |
| *Same sources with arm-none-eabi-gcc 13.2.1 (all rows below)* | *2058* |
| Sources split into `src/`, one object per file (see below) | 1954 |

Hardening (+202 B on M3): ~100 B in the RX decoder (answers are matched and the transaction
closed under the lock, event delivered before the handle goes idle, NACK reasons validated),
~50 B in `lx_tick` (decide and snapshot under the lock), 9 acquire/release barriers (~36 B; on a
single-core part `LX_LOAD8`/`LX_STORE8` can be overridden with plain accesses), and small
changes elsewhere.

`LX_BUS=0` costs nothing: when bus mode was added, `lynx.o` built with `-DLX_BUS=0` was
byte-for-byte identical to the sources without it on all six targets. All bus code sits inside
`#if LX_BUS` or the `BUS_ADDR_PARAM()`/`BUS_ADDR_ARG()` parameter macros, which expand to nothing.

## Effect of splitting the sources

0.1.0 was a single `lynx.c`; the sources are now one file per job (see
[architecture.md](architecture.md)). Behaviour is unchanged (a differential trace of 300 000
random API calls, link errors and noise gives identical output in 11 build configurations), but
the compiler now optimises each file on its own and makes different inlining choices. Default
build, same toolchains:

| Target | single `lynx.c` | `src/*.c` | |
|---|---:|---:|---:|
| x86-64 host | 3825 | 3839 | +14 |
| Cortex-M3 | 2058 | 1954 | −104 |
| Cortex-M0+ | 2230 | 2092 | −138 |
| ESP32 (Xtensa LX6) | 2633 | 2678 | +45 |
| ESP32-S3 (Xtensa LX7) | 2637 | 2686 | +49 |
| ESP32-C3 (RV32IMC) | 2858 | 2948 | +90 |

Bus and ISR-safe builds move the same way (Cortex-M3 −146 / −104 B, ESP32-C3 +8 / +90 B). On
ESP32-C3 most of the growth is the encoder: its `enc_var`/`enc_end` helpers are now inlined into
both frame builders instead of being called. Host benchmark speed is unchanged.

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
256 B `__clz_tab`: **314 B of hidden flash on ESP32-C3**, 64 B on M0+. `lynx_encode.c` uses the
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
