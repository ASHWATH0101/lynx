# LYNX — handoff brief for Claude Code

## What this is
LYNX is a clean-room, MIT-licensed, zero-allocation variable-sync protocol for MCU-to-MCU links
(UART/SPI/BLE/Wi-Fi). It was written from scratch as a faster, leaner alternative to BAVA
(github.com/NishanthRaj707/bavaprotocol, GPLv3).

**HARD RULE: never copy, paraphrase, or read BAVA's src/ files.** LYNX must stay clean-room
so it can be MIT-licensed. BAVA may only be used as a black box through its public header
for benchmarking (test/bench_vs_bava.c).

## Current state (all passing)
- lynx.h / lynx.c: C99, -Wall -Wextra -Werror -Wconversion clean, ~3.4 KB at -Os (x86).
- Frame: COBS([HDR=(CMD<<4)|SEQ][ID][PAYLOAD][CRC16 BE]) + 0x00. CRC-16/CCITT-FALSE, table-free.
- CRC + COBS done in ONE streaming pass on TX; on RX, COBS decode + CRC run per byte,
  valid iff CRC residue == 0 (no second pass).
- Features: publish (no-ACK) and reliable write, batch (many vars, one frame, atomic commit),
  read, ACK/NACK with reasons, 4-bit SEQ matching, auto-retry + timeout, wraparound-safe clock,
  bitmask update flags (lx_take_updates), separate RX reply buffer, link stats.
- test/test_lynx.c: 13 tests incl. 200k-frame bit-flip fuzz (0 bad commits) and 2M-byte noise
  fuzz, under ASan+UBSan. Run: `gcc -std=c99 -O2 -fsanitize=address,undefined -I. lynx.c test/test_lynx.c && ./a.out`

## Host benchmark vs BAVA v2 (x86, same compiler, public APIs only)
| Case | BAVA | LYNX |
|---|---|---|
| Reliable 4B write + ACK | 243 ns, 18 B on wire | 96 ns, 16 B |
| 4 vars reliable | 1050 ns, 69 B | 247 ns, 33 B |
| 4B telemetry, no ACK | not supported | 64 ns, 10 B |
| Handle RAM | 968 B (32 B payload) | 792 B (64 B payload) |
These are HOST numbers. They prove relative efficiency, not MCU timings.

## Your tasks (in order)
1. **Cross-compile sizes**: build with arm-none-eabi-gcc (-mcpu=cortex-m3 -Os) and xtensa/riscv
   ESP32 toolchains; report .text/.bss for LYNX vs BAVA.
2. **Multi-drop bus mode** (BAVA v2 has this; LYNX doesn't yet): optional compile flag
   LX_BUS, 1-byte dst + 1-byte src after HDR, broadcast address 0xFF, broadcasts never ACKed.
   Zero cost when disabled. Add tests.
3. **Platform examples**: ESP-IDF component (idf_component.yml, CMakeLists, FreeRTOS RX task,
   portENTER_CRITICAL spinlock), Arduino library (library.properties, examples/), Zephyr module.
4. **On-target benchmark firmware**: ESP32 <-> ESP32 (or STM32) at 115200 and 2 Mbaud, measuring
   cycles per encode/decode with the cycle counter (DWT->CYCCNT / esp_cpu_get_cycle_count),
   round-trip latency, and delivery rate. Same harness for BAVA. Output a markdown table.
5. **Hardening**: review every critical-section path for ISR safety; consider an optional
   LX_ISR_SAFE build where lx_rx only queues and lx_poll dispatches.
6. README.md (spec, API, frame format, honest benchmark table), MIT LICENSE, GitHub Actions CI
   running tests under sanitizers + cross-compile size check.

Do not weaken any existing test. Keep zero heap, O(1) per byte, C99.

## Status after the 2026-11-02 session (all six tasks done; see CHANGELOG.md)
- Build/test env: WSL Ubuntu (gcc 15, clang 21, arm-none-eabi 14.2, Espressif 16.1 toolchains in
  /opt/esp, arduino-cli + AVR core in /opt/arduino, BAVA checkout in /root/bava).
  Run `tools/run_tests.sh` (11 ASan/UBSan configs + 4 TSan thread configs + bench simulation)
  and `tools/size_report.sh`. Host numbers above are from another machine; README has fresh ones.
- Verified locally: all tests (gcc and clang), cross-compiles for 6 targets, Arduino Uno/Mega
  builds, ESP-IDF/Zephyr port headers through real cross compilers with stub SDK headers.
- NOT verified: real ESP-IDF and Zephyr builds (no SDK installed: disk space), the CI workflow
  (never run on GitHub), and no on-target benchmark numbers (no hardware). Expect to fix small
  things in .github/workflows/ci.yml on its first run.
- Placeholders to fill: `url=` in library.properties (GitHub URL), CI badge in README.
- Bus mode costs nothing when off: verified byte-identical disassembly when it was added.
