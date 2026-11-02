# Changelog

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
