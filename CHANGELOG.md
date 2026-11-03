# Changelog

## Unreleased

### Fixed
- **Data race on `p_cmd`**: `finish()` clears it with an atomic store outside the lock, but
  `lx_tick`, `start_txn` and the answer matcher read it with plain loads under the lock.
  ThreadSanitizer caught it in roughly 1 run in 20 of the locked-RX thread test (the CI runs
  happened to pass). Every access to `p_cmd` now goes through `LX_LOAD8`/`LX_STORE8`.
- **Shift past 31 on a bad `READ_RSP`**: a CRC-valid read response with an id of 32 or more,
  arriving while a read was in flight, evaluated `LX_BIT(id)` (undefined behaviour). On x86 the
  shift wraps, so id 34 matched a read of id 2 and ended it with `LX_EV_NACK_ID`. Such responses
  are now ignored. New test [21].
- **A refused request used up a SEQ**: every `lx_write`/`lx_read` that returned `LX_ERR_BUSY`
  still advanced the 4-bit sequence number. An app retrying while busy could start its next
  transaction with the same SEQ as the previous one, and a late or duplicate ACK of the previous
  request then ended the new one with `LX_EV_DONE`, even if the new request never arrived. The
  SEQ now advances only when a transaction starts. New test [23].
- **Buffer overflow when rebinding a variable in flight**: a batch is sized once, at `lx_write`,
  but `lx_tick` rebuilds each retransmission from the current bindings. Rebinding a variable of
  the transaction to a larger size made the retransmission overrun `tx_buf` (by 7 bytes with the
  default `LX_MAX_PAYLOAD`, more with several or larger variables, up to past the end of the
  handle). `lx_bind` now returns `LX_ERR_BUSY` for an id that is part of the transaction in
  flight. New test [22].
- **`LX_RETRIES` above 255 never timed out**: the retry counter is 8 bits, so the transaction
  was retransmitted forever, no event came and `lx_busy()` stayed true. Such values are now a
  compile-time error like the other invalid options, and `tools/run_tests.sh` checks that every
  invalid configuration is rejected.

Code size (`-Os`): Cortex-M3 −44 B, Cortex-M0+ −50 B (each section reads `p_cmd` once instead of
up to three times); ESP32 +20 B, ESP32-C3 +36 B (acquire fences). Bus and ISR-safe builds move
the same way.

## 0.1.0 — 2026-11-02

First packaged release. Wire format unchanged from the handed-over sources for point-to-point
builds; everything below is backwards compatible on the wire.

### Added
- **Multi-drop bus mode** (`LX_BUS=1`): `[DST][SRC]` after the header, broadcast address `0xFF`,
  `lx_set_addr`, `lx_publish_to`, `lx_write_to`, `lx_read_from`, `LX_ERR_ADDR`. Broadcasts are
  never answered, nodes ignore their own echo, only the addressed node can complete a
  transaction. Compiles out completely when disabled.
- **ISR-safe receive** (`LX_ISR_SAFE=1`, `LX_RXQ_SIZE`): `lx_rx` only appends to a lock-free
  single-producer queue, `lx_poll` decodes. Overflow drops bytes and discards exactly the frames
  that lost bytes (per-slot gap bitmap). `lx_poll` is an inline no-op in the default build.
- `lx_get` / `lx_set`: tear-free copy of a bound variable under the critical section.
- `st_rsp` counter (replies sent by the RX path) and `st_rx_ovf` (`LX_ISR_SAFE`).
- Platform packaging: ESP-IDF component (`idf_component.yml`, `CMakeLists.txt`, `Kconfig`,
  `portMUX` port), Arduino library (`library.properties`, two examples), Zephyr module
  (`zephyr/module.yml`, Kconfig, `k_spinlock` port), plain CMake target `lynx::lynx`.
- Examples: `extras/esp-idf/uart_sync` (FreeRTOS RX task), `extras/zephyr/uart_sync`
  (ISR-safe mode).
- On-target benchmark firmware for ESP32 (`extras/bench`), with a host simulation used as a test.
- `tools/run_tests.sh`, `tools/size_report.sh`, GitHub Actions CI, README, size report.

### Fixed (concurrency: RX path in another task or ISR than the app)
- An ACK arriving while `lx_tick` gave up could deliver **both** `DONE` and `TIMEOUT`. Answers
  and timeouts now close the transaction under the lock: exactly one event.
- A read response arriving while `lx_tick` decided to retransmit made the retransmission a
  **WRITE** of the local copy, overwriting the peer's variable. Retransmits now resend a snapshot
  of the original request.
- Answers were matched to the transaction outside the lock, so a stale answer could complete a
  newer transaction. Matching is now done under the lock (on a bus, including the sender check).
- `lx_busy()` turned false before the event callback ran, so another task could start a new
  transaction and take the old event for its own. The handle now stays busy until the callback
  has returned (one-context builds may still chain a transaction from the callback).
- `st_tx` was incremented from both paths; each counter now has a single writer.
- A NACK carrying an arbitrary reason byte (e.g. 1, the value of `LX_EV_DONE`) was passed to the
  application as the event. Only the two NACK reasons are accepted.
- `p_cmd` handover uses acquire/release byte atomics (`LX_LOAD8`/`LX_STORE8`), so ThreadSanitizer
  runs clean and weakly ordered multi-core MCUs see results before `lx_busy()` turns false.

### Changed
- `__builtin_ctz` is used only where it is a native instruction; elsewhere a binary search
  avoids libgcc's `__ctzsi2` (+314 B on ESP32-C3).
- Critical-section contract documented: `LX_ENTER_CRITICAL()` may declare a local, sections
  never nest.
- Code size: +290 B on Cortex-M3 for `lx_get`/`lx_set` and the hardening (see `docs/sizes.md`).
